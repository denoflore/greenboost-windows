# GreenBoost — 3-Tier GPU Memory Extension for Linux

**Author:** Ferran Duarri
**License:** GPL v2 (open-source)
**Status:** First public release — after several months of testing on real hardware

---

I have been quietly working on this for a while and figured it was time to share it with the
community. This is a Linux kernel module + CUDA userspace shim that transparently extends
GPU VRAM using system DDR4 RAM and NVMe storage, so you can run large language models that
exceed your GPU memory without modifying the inference software at all.

**Important:** GreenBoost does not replace or modify the official NVIDIA kernel drivers
(`nvidia.ko`, `nvidia-uvm.ko`). It loads as a completely independent kernel module
alongside them and works at the CUDA allocation layer, not the driver layer.

---

## The problem I was trying to solve

I have an RTX 5070 with 12 GB VRAM and I wanted to run `glm-4.7-flash:q8_0`, which is a
31.8 GB model. The standard options are:

- **Offload layers to CPU** — works, but drops token/s by 5–10× because CPU RAM has no
  CUDA coherence. You end up waiting.
- **Use a smaller quantization** — you lose quality. At q4_0 the model is noticeably worse
  on reasoning tasks.
- **Buy a bigger GPU** — not realistic for consumer hardware. A 48 GB card costs more than
  a complete workstation.

None of those felt right, so I built an alternative: route the overflow memory to DDR4 via
DMA-BUF, which gives the GPU direct access to system RAM over PCIe 4.0 without a CPU
copy involved.

My system: i9-14900KF, RTX 5070 12 GB (Blackwell, compute capability 12.0), 64 GB
DDR4-3600, Samsung 990 EVO Plus 4 TB NVMe, Ubuntu 26.04, kernel 6.19, NVIDIA driver 580.x.

---

## How it works

Two components cooperate:

**1. Kernel module (`greenboost.ko`)**

Allocates pinned DDR4 pages using the buddy allocator (2 MB compound pages for efficiency)
and exports them as DMA-BUF file descriptors. The GPU can then import these pages as
CUDA external memory via `cudaImportExternalMemory`. From CUDA's perspective, those pages
look like device-accessible memory — it doesn't know they live in system RAM. The PCIe 4.0
x16 link handles the actual data movement (~32 GB/s).

A sysfs interface (`/sys/class/greenboost/greenboost/pool_info`) lets you monitor usage
live. A watchdog kernel thread monitors RAM and NVMe pressure and signals userspace before
things get dangerous.

**2. CUDA shim (`libgreenboost_cuda.so`, injected via `LD_PRELOAD`)**

Intercepts `cudaMalloc`, `cudaMallocAsync`, `cuMemAllocAsync`, `cudaFree`, and `cuMemFree`.
Small allocations (< 256 MB) pass straight through to the real CUDA runtime. Large ones
(KV cache, model weights overflowing VRAM) are redirected to the kernel module and imported
back as CUDA device pointers.

There is one tricky part worth mentioning: Ollama resolves GPU symbols via `dlopen` +
`dlsym` internally, which bypasses LD_PRELOAD on those symbols. To handle this, the shim
also intercepts `dlsym` itself (using `dlvsym` with the GLIBC version tag to bootstrap a
real pointer without recursion) and returns hooked versions of `cuDeviceTotalMem_v2` and
`nvmlDeviceGetMemoryInfo`. Without this, Ollama sees only 12 GB and puts layers on the CPU.

---

## Memory tiers

| Tier | Device | Capacity | Bandwidth | Used for |
|------|--------|----------|-----------|---------|
| T1 | RTX 5070 VRAM | 12 GB | ~336 GB/s | Hot layers, active compute |
| T2 | DDR4 pool | 51 GB | ~32 GB/s via PCIe 4.0 | KV cache, cold weights |
| T3 | NVMe swap | 64 GB | ~1.8 GB/s | Safety overflow (rarely hit) |

For the 32 GB model: T1 holds the hot layers, T2 holds the rest. T3 exists as a safety
net but in practice the model + KV cache at 32K context fits comfortably in T1+T2.

---

## What this code is based on 

 NVIDIA's open-source kernel module sourcecode had been explored
(`nvidia/open-gpu-kernel-modules`) to understand how UVM page fault handling works and how
CUDA external memory imports are expected to behave. That code was invaluable for getting
the DMA-BUF import path right. I want to be clear that I didn't copy any of it — I studied
the architecture and wrote my own implementation, but the public NVIDIA driver source was
a critical reference.

Also check at how `llama.cpp` handles CPU offload, how ExLlamaV2/V3 structures its KV
cache, and how vLLM's paged attention allocates memory. Understanding those patterns helped
figure out where in the allocation lifecycle to intercept.

---

## Bundled inference libraries

To get the most out of the extended memory, GreenBoost bundles several open-source
libraries and integrates them through a unified `optimize-model` command:

- **ExLlamaV3** — high-performance inference engine with a native GreenBoost KV cache
  layer. KV tensors are allocated directly from `/dev/greenboost` as zero-copy
  mmap → numpy → PyTorch tensors. No CUDA shim involved for cache I/O.

- **kvpress** — runtime KV cache compression (ExpAttn, SnapKV, KnormPress). Applies at
  inference time, no retraining needed. Reduces DDR4 pressure significantly at long context.

- **NVIDIA ModelOpt** — post-training quantization (FP8, INT4-AWQ, NVFP4). No retraining,
  only a calibration pass (~512 samples). Cuts the 32 GB model to ~16 GB (FP8) or ~8 GB
  (INT4-AWQ), which changes everything for memory tier usage.

- **TensorRT-Edge-LLM** — TRT engine export for desktop RTX GPUs.

- **Unsloth + LoRA** — parameter-efficient fine-tuning that fits a 30B model into 12 GB
  VRAM via 4-bit base quantization and rank-16 adapters. Needs your own training data.

---

## Installation

```bash
git clone https://gitlab.com/IsolatedOctopi/nvidia_greenboost.git
cd nvidia_greenboost

# Auto-detects GPU VRAM, RAM size, CPU P/E-core topology, NVMe capacity
# and computes optimal parameters at install time:
sudo ./greenboost_setup.sh full-install

# After reboot, verify everything is working:
sudo ./greenboost_setup.sh diagnose
```

The installer builds the kernel module and shim, sets up the NVMe swap file, configures
Ollama's systemd environment, and applies sysctl tuning for PCIe and memory performance.

---

## Quick usage examples

```bash
# Run Ollama as normal — GreenBoost is transparent
ollama run glm4:latest

# ExLlamaV3 with GreenBoost DDR4 KV cache offload:
./tools/greenboost-exllama.sh --model /opt/models/glm-4.7-flash-exl3

# Convert a HuggingFace model to EXL3 format (4bpw → ~16 GB):
./tools/convert_to_exl3.sh --model THUDM/glm-4.7-flash-hf --bpw 4.0

# FP8 quantization (32 GB → ~16 GB, no retraining):
python tools/greenboost-ptq.py --model THUDM/glm-4.7-flash-hf --quant fp8

# Runtime KV cache compression benchmark:
./tools/greenboost-kvpress.sh --model THUDM/glm-4.7-flash-hf --benchmark

# Monitor memory tiers live:
watch -n1 'cat /sys/class/greenboost/greenboost/pool_info'
```

---

## Observed performance (glm-4.7-flash:q8_0, RTX 5070)

| Setup | Decode tok/s | TTFT |
|-------|-------------|------|
| Ollama + GreenBoost shim (baseline) | 2–5 | 5–15s |
| + kvpress 50% KV compression | 4–8 | 3–10s |
| ExLlamaV3 + GreenBoost cache | 8–20 | 2–8s |
| ModelOpt FP8 (16 GB model) | 10–25 | 1–5s |
| ExLlamaV3 EXL3 2bpw (8 GB, full VRAM) | 25–60 | 0.5–2s |

The PCIe 4.0 link (~32 GB/s) is the bottleneck when the model overflows VRAM. The best
strategy is to shrink the model until it fits — either with EXL3 quantization or ModelOpt
PTQ — and use GreenBoost's DDR4 pool for KV cache only.

---

## What GreenBoost is not

- It is **not** a replacement for the NVIDIA driver. `nvidia.ko`, `nvidia-uvm.ko`, and all
  NVIDIA official modules continue to run exactly as normal. GreenBoost loads beside them.
- It is **not** a virtual GPU. It doesn't expose a new GPU device or change how compute
  works. It only affects how CUDA memory allocations are routed.
- It is **not** a hack around driver restrictions. The DMA-BUF + external memory import
  path it uses is a documented CUDA feature.
- It does **not** work without the NVIDIA driver installed.

---

## Known limitations

- Tested on kernel 6.19 (Ubuntu 26.04). Earlier kernels may lack some exported symbols
  the module relies on.
- The dlsym hook is specific to how Ollama resolves CUDA symbols. Other inference engines
  (llama.cpp, vllm) may need different handling — contributions welcome.
- T3 NVMe tier is slow for random access. If a workload thrashes T3, performance degrades
  significantly. This is a fundamental NVMe latency constraint, not a software issue.
- Blackwell (compute capability 12.0) is the primary tested GPU. Ada Lovelace and Ampere
  should work, but have not been tested as extensively.

---

## Repository structure

```
greenboost.c              — kernel module (char device, DMA-BUF, IOCTL, watchdog)
greenboost_cuda_shim.c    — CUDA LD_PRELOAD shim (allocation intercept, dlsym hook)
greenboost_ioctl.h        — shared IOCTL definitions (kernel + userspace)
greenboost_setup.sh       — full installer (hardware detection, build, configure, diagnose)
Makefile                  — builds kernel module + shim

tools/
  greenboost-exllama.sh   — ExLlamaV3 launcher with GreenBoost cache
  convert_to_exl3.sh      — HuggingFace → EXL3 quantization
  greenboost-kvpress.sh   — runtime KV cache compression (kvpress)
  greenboost-ptq.py       — post-training quantization (NVIDIA ModelOpt)
  greenboost-lora-train.py — LoRA fine-tuning via Unsloth

libraries/
  exllamav3/              — ExLlamaV3 with GreenBoost cache patches
  kvcompress/             — kvpress and related KV compression tools
  Model-Optimizer/        — NVIDIA ModelOpt PTQ library
  TensorRT-Edge-LLM/      — TensorRT engine export
  LoRA/                   — loralib (Unsloth uses this under the hood)

architecture.md           — detailed technical architecture reference
plan.md                   — integration plan and implementation status
```

---

## License

GPL v2 — open-source. Attribution to Ferran Duarri is required in all forks, derivatives,
and any documentation that references this work.

A commercial license is available for proprietary products or integrations that cannot
comply with GPL v2. Contact information is in the repository.

```
Copyright (C) 2024-2026 Ferran Duarri
```

---

If you try this and run into issues, open an issue on the repository. I am particularly
interested in feedback from people running different GPU generations (Ada Lovelace, Ampere)
or different inference engines beyond Ollama and ExLlamaV3.
