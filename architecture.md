# GreenBoost — Architecture Reference
**Author:** Ferran Duarri
**Version:** 2.3
**License:** GPL v2 (open-source) / Commercial (contact author)
**Kernel:** Linux 6.19 | **GPU:** NVIDIA RTX 5070 (GB205, Blackwell) | **CUDA:** 13

---

## Problem Statement

Running `glm-4.7-flash:q8_0` (31.8 GB, 30B parameters, 198K context) on a workstation with
12 GB GPU VRAM requires transparent memory extension. GreenBoost creates a 3-tier virtual
GPU memory pool that CUDA applications use automatically, without any code changes.

Total addressable memory: **T1 (12 GB) + T2 (51 GB) + T3 (64 GB) = 127 GB**

---

## 3-Tier Memory Hierarchy

| Tier | Device | Capacity | Bandwidth | Latency | Role |
|------|--------|----------|-----------|---------|------|
| **T1** | RTX 5070 GDDR7 | 12 GB | ~336 GB/s | ~100 ns | Hot layers — native GPU compute |
| **T2** | DDR4-3600 pool | 51 GB | ~57.6 GB/s local / ~32 GB/s via PCIe 4.0 | ~50 ns | Cold layers — pinned DMA-BUF pages |
| **T3** | NVMe swap | 64 GB | ~1.8 GB/s | ~100 µs | Frozen pages — kernel swap subsystem |

**Design principle:** A 30B model's active layer fits in T1. The rest of the KV cache and
cold weight tensors live in T2 over PCIe 4.0 x16 (~32 GB/s DMA bandwidth). T3 is a safety
net — it is rarely touched during normal inference.

---

## Component Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                    CUDA Application (Ollama / ExLlamaV3)            │
│               cudaMalloc / cudaFree / cuMemAllocAsync               │
└─────────────────────────────┬───────────────────────────────────────┘
                              │  LD_PRELOAD intercept
                              ▼
┌─────────────────────────────────────────────────────────────────────┐
│              libgreenboost_cuda.so  (CUDA shim v2.3)               │
│  • Intercepts: cudaMalloc, cudaMallocAsync, cuMemAllocAsync,       │
│                cudaFree, cuMemFree, dlsym                           │
│  • Hooks: cuDeviceTotalMem_v2, nvmlDeviceGetMemoryInfo             │
│    → reports virtual VRAM (63 GB) instead of physical (12 GB)      │
│  • Small allocs (< 256 MB): pass-through to real CUDA              │
│  • Large allocs (≥ 256 MB): routed to /dev/greenboost IOCTL       │
│  • Hash map (131 072 slots): devPtr → gb_buf for O(1) free lookup  │
│  • libcudart.so located at runtime (Ollama bundles it separately)  │
└─────────────────────────────┬───────────────────────────────────────┘
                              │  GB_IOCTL_ALLOC / GB_IOCTL_FREE
                              ▼
┌─────────────────────────────────────────────────────────────────────┐
│              /dev/greenboost  (greenboost.ko v2.3)                  │
│  • Char device, IOCTL interface                                     │
│  • GB_IOCTL_ALLOC    — allocate pinned DDR4 pages, return DMA-BUF │
│  • GB_IOCTL_GET_INFO — pool statistics                             │
│  • GB_IOCTL_MADVISE  — hint eviction priority (FROZEN / KV_CACHE) │
│  • GB_IOCTL_EVICT    — force spill of a buffer to T3 NVMe         │
│  • GB_IOCTL_POLL_FD  — eventfd for watchdog pressure signals       │
│  • IDR allocator tracks live gb_buf objects                        │
│  • Mutex + spinlock protect the pool                               │
│  • sysfs: pool_info, hw_info, active_buffers                       │
└──────────┬──────────────────┬───────────────────────────────────────┘
           │                  │
     DMA-BUF FD          swappable pages
           │                  │
           ▼                  ▼
┌──────────────────┐  ┌───────────────────┐  ┌───────────────────────┐
│  Tier 1: T1      │  │  Tier 2: T2        │  │  Tier 3: T3           │
│  RTX 5070 VRAM   │  │  DDR4 pool         │  │  NVMe swap            │
│  12 GB GDDR7     │◄─┤  51 GB pinned      │  │  64 GB (/swap_nvme)  │
│  336 GB/s        │  │  2 MB hugepages    │  │  kernel swap handles  │
│  Native CUDA     │  │  cudaImportExt.Mem │  │  eviction to NVMe     │
└──────────────────┘  └───────────────────┘  └───────────────────────┘
```

---

## Kernel Module — `greenboost.c` → `greenboost.ko`

### Memory Allocation (Tier 2)

Pinned DDR4 pages are allocated with:
```c
alloc_pages(GFP_KERNEL | __GFP_COMP, order)  // order=9 → 2 MB compound pages
```
This uses the buddy allocator directly — **not** the HugeTLB pool (which would lock RAM and
trigger the OOM guard). The compound pages are physically contiguous, enabling efficient DMA.

Each allocation produces a DMA-BUF file descriptor exported via the kernel's DMA-BUF framework.
The CUDA shim imports this FD as `cudaExternalMemoryHandleTypeOpaqueFd`, then maps it as a
contiguous CUDA device pointer via `cudaExternalMemoryGetMappedBuffer`.

### Tier 3 — NVMe Swap

Normal 4K swappable pages are allocated for T3. The kernel swap subsystem handles eviction
to `/swap_nvme.img` automatically. GreenBoost does not compress T3 pages — `lz4` swap
compression is enabled system-wide via sysctl.

### Watchdog Kthread

- Monitors `nr_free_pages` and `nr_swap_pages`
- Pinned to P-cores (CPU 0–15 on i9-14900KF) via `pcores_only=1`
- Signals userspace via `eventfd` (`GB_IOCTL_POLL_FD`) before OOM
- Triggers safe eviction before `safety_reserve_gb` floor is breached

### Key Module Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `physical_vram_gb` | 12 | RTX 5070 actual VRAM |
| `virtual_vram_gb` | 51 | DDR4 pool capacity (T2) |
| `safety_reserve_gb` | 12 | Min free RAM never touched |
| `nvme_swap_gb` | 64 | T3 NVMe swap size (auto-detected on install) |
| `nvme_pool_gb` | 58 | T3 soft cap |
| `use_hugepages` | 1 | 2 MB compound pages for T2 |
| `pcores_only` | 1 | Pin watchdog to P-cores |
| `debug_mode` | 0 | Verbose dmesg output |

**Known kernel constraints (Linux 6.19):**
- `total_swap_pages` is NOT exported — use `nvme_swap_gb` module param instead
- `si_swapinfo()` is NOT exported — use `nr_swap_pages` atomic
- `get_nr_swap_pages()` is an exported inline — safe to call

---

## CUDA Shim — `greenboost_cuda_shim.c` → `libgreenboost_cuda.so`

### Allocation Flow

```
cudaMalloc(size) called by Ollama/ExLlamaV3
    │
    ├─ size < 256 MB  → real_cudaMalloc() (pass-through)
    │
    └─ size ≥ 256 MB  → open("/dev/greenboost")
                         ioctl(GB_IOCTL_ALLOC, {size, GB_ALLOC_KV_CACHE})
                         cudaImportExternalMemory(dma_buf_fd)
                         cudaExternalMemoryGetMappedBuffer()
                         → return devPtr (valid CUDA pointer to DDR4 pages)
```

### Virtual VRAM Reporting (dlsym hook)

Ollama uses `dlopen` + `dlsym` to resolve CUDA/NVML symbols at runtime. This bypasses
`LD_PRELOAD` hooks on the symbols directly. The shim intercepts `dlsym` itself:

```c
// Priority-101 constructor runs before gb_shim_init, bootstraps real dlsym:
real_dlsym = dlvsym(RTLD_DEFAULT, "dlsym", "GLIBC_2.0");

// Our dlsym override returns hooked pointers for key symbols:
//   cuDeviceTotalMem_v2 → returns physical_vram + virtual_vram (63 GB)
//   nvmlDeviceGetMemoryInfo → same
//   cudaMalloc → hooked allocation path
// RTLD_NEXT lookups are passed through (prevents infinite recursion)
```

Without this hook, Ollama sees only 12 GB VRAM and schedules model layers to CPU
instead of the T2 DDR4 pool.

### Allocation Flags (from `greenboost_ioctl.h`)

| Flag | Value | Meaning |
|------|-------|---------|
| `GB_ALLOC_WEIGHTS` | `1 << 0` | Model weights (hint: prefer T2, frozen) |
| `GB_ALLOC_KV_CACHE` | `1 << 1` | KV cache (hint: hot, don't evict) |
| `GB_ALLOC_ACTIVATIONS` | `1 << 2` | Activations (short-lived) |
| `GB_ALLOC_FROZEN` | `1 << 3` | Pin in T2 — never evict while frozen |
| `GB_ALLOC_NO_HUGEPAGE` | `1 << 4` | Force 4K pages (fragmented regions) |

---

## ExLlamaV3 Integration — `libraries/exllamav3/`

GreenBoost patches ExLlamaV3 with a native KV cache layer that allocates directly from
`/dev/greenboost`, bypassing the CUDA shim entirely for cache tensors.

**File:** `libraries/exllamav3/exllamav3/cache/greenboost.py`

### Zero-copy mmap → PyTorch bridge

```python
# DMA-BUF pages mmap'd into userspace:
mm = mmap.mmap(dma_buf_fd, size, access=mmap.ACCESS_WRITE)

# Zero-copy numpy array (no allocation, shares DMA-BUF backing):
arr = np.frombuffer(mm, dtype=np.float16, count=n).reshape(shape)

# Zero-copy PyTorch tensor (no allocation, shares numpy backing):
tensor = torch.from_numpy(arr)
# tensor.data_ptr() points directly into pinned DDR4 pages
```

This avoids any CPU copy when ExLlamaV3 reads/writes the KV cache.

### IOCTL constants

```python
_GB_ALLOC_REQ_FMT = "=QQii"          # 24 bytes: size, flags, fd_out, padding
_GB_IOCTL_ALLOC   = 0xC0184701       # _IOWR('G', 1, 24)
_GB_ALLOC_KV_CACHE = 1 << 1
```

### Usage

```bash
# Chat with GLM-4.7-Flash via ExLlamaV3 + GreenBoost DDR4 KV cache:
./tools/greenboost-exllama.sh --model /opt/models/glm-4.7-flash-exl3

# Convert HF model to EXL3 (4 bpw → ~16 GB; 2 bpw → ~8 GB):
./tools/convert_to_exl3.sh --model THUDM/glm-4.7-flash-hf --bpw 4.0
```

---

## Inference Optimization Tools

### Runtime (no retraining required)

| Tool | Library | Effect |
|------|---------|--------|
| `tools/greenboost-exllama.sh` | ExLlamaV3 | EXL3 4bpw: model fits in VRAM → 3–5× faster |
| `tools/greenboost-kvpress.sh` | kvpress | 50% KV cache compression → less T2 pressure |
| `tools/greenboost-ptq.py` | NVIDIA ModelOpt | FP8 PTQ: 32 GB → 16 GB, 2× smaller |

### With calibration / retraining

| Tool | Library | Effect |
|------|---------|--------|
| `tools/greenboost-ptq.py --quant int4_awq` | NVIDIA ModelOpt | INT4 AWQ: 32 GB → 8 GB, 4× smaller |
| `tools/greenboost-lora-train.py` | Unsloth + LoRA | Domain adaptation, merges into GGUF |

### Model optimization command

```bash
# Show available libraries and model stats:
sudo ./greenboost_setup.sh optimize-model --model /path/to/model

# Apply runtime KV cache compression:
sudo ./greenboost_setup.sh optimize-model --model /path/to/model --strategy kvcompress

# Post-training FP8 quantization:
sudo ./greenboost_setup.sh optimize-model --model THUDM/glm-4.7-flash-hf --strategy modelopt-ptq

# Fine-tune with LoRA:
sudo ./greenboost_setup.sh optimize-model --model THUDM/glm-4.7-flash-hf \
    --strategy lora --data /path/to/data.jsonl
```

---

## Installation — Hardware Auto-Detection

`full-install` detects hardware at install time and computes optimal module parameters:

```bash
# Auto-detect GPU, RAM, CPU topology, NVMe:
sudo ./greenboost_setup.sh full-install

# Hardcoded optimal preset for owner's workstation:
sudo ./greenboost_setup.sh full-install --owner-workstation
```

**Detected parameters:**
- `GB_PHYS` — GPU VRAM from `nvidia-smi`
- `GB_VIRT` — 80% of system RAM from `/proc/meminfo`
- `GB_NVME_SWAP` — 2× model size from NVMe capacity via `lsblk`
- `GB_PCORES_MAX`, `GB_GOLDEN_MIN/MAX` — P/E core topology from `/sys/devices/system/cpu/`
- `GB_OLLAMA_CTX` — largest context that fits in T1+T2

---

## Ollama Integration

The shim is injected into Ollama's systemd environment:

```ini
# /etc/systemd/system/ollama.service.d/greenboost.conf
[Service]
Environment="LD_PRELOAD=/usr/local/lib/libgreenboost_cuda.so"
Environment="OLLAMA_FLASH_ATTENTION=1"
Environment="OLLAMA_KV_CACHE_TYPE=q8_0"
Environment="OLLAMA_NUM_CTX=131072"
```

After install, Ollama's `inference compute` log should show `total=">20 GiB"` confirming
virtual VRAM is visible. Run `sudo ./greenboost_setup.sh diagnose` to verify.

---

## Monitoring

```bash
# Live pool stats (T1/T2/T3 usage, alloc count, evictions):
watch -n1 'cat /sys/class/greenboost/greenboost/pool_info'

# Shim activity in Ollama logs:
journalctl -u ollama -f | grep GreenBoost

# Kernel messages:
dmesg | grep greenboost | tail -20

# Swap pressure:
watch -n1 'swapon --show'
```

---

## Expected Performance (glm-4.7-flash:q8_0, 32 GB model)

| Configuration | Tok/s (decode) | TTFT | Notes |
|---------------|----------------|------|-------|
| Ollama + GreenBoost shim | 2–5 | 5–15s | Baseline — PCIe 4.0 bound |
| + kvpress 50% compression | 4–8 | 3–10s | Less PCIe traffic |
| + ExLlamaV3 + GreenBoost cache | 8–20 | 2–8s | Native DDR4 cache offload |
| + ModelOpt FP8 → 16 GB | 10–25 | 1–5s | Model fits better in T1 |
| ExLlamaV3 EXL3 2bpw → 8 GB | 25–60 | 0.5–2s | Full VRAM fit, maximum speed |

PCIe 4.0 x16 (~32 GB/s) is the main bottleneck when the model exceeds VRAM.
Reducing model size (EXL3 or PTQ) has the largest impact on decode speed.

---

## Libraries Integrated

| Library | Source | Role |
|---------|--------|------|
| **ExLlamaV3** | `libraries/exllamav3/` | High-performance inference engine, GreenBoost KV cache |
| **kvpress** | PyPI / `libraries/kvcompress/` | Runtime KV cache compression (ExpAttn, SnapKV, KnormPress) |
| **NVIDIA ModelOpt** | `libraries/Model-Optimizer/` | Post-training quantization (FP8, INT4-AWQ, NVFP4, INT8) |
| **TensorRT-Edge-LLM** | `libraries/TensorRT-Edge-LLM/` | TRT engine export for edge/desktop GPUs |
| **Unsloth + LoRA** | PyPI / `libraries/LoRA/` | Parameter-efficient fine-tuning, fits 30B in 12 GB VRAM |

**Reference code studied (not copied):**
- `nvidia/open-gpu-kernel-modules` — CUDA driver architecture, UVM page fault handling
- `huggingface/transformers` — model loading patterns
- `ggerganov/llama.cpp` — GGUF format, CPU offload strategies
- `turboderp/exllamav2` — ExLlamaV2 cache design (predecessor to ExLlamaV3)

---

## What GreenBoost Does NOT Do

- Does **not** replace `nvidia.ko`, `nvidia-uvm.ko`, or any official NVIDIA driver module
- Does **not** modify GPU firmware or hardware
- Does **not** intercept GPU compute — only memory allocation routing
- Does **not** require patching Ollama, ExLlamaV3, or any application

GreenBoost loads as an independent kernel module alongside NVIDIA's official drivers.
The CUDA shim intercepts only allocation calls — all GPU compute goes through the
official NVIDIA CUDA runtime unchanged.

---

## License

**GPL v2** — open-source, mandatory attribution to Ferran Duarri in all forks and derivatives.

**Commercial license** available for proprietary products. Contact: see repository.

```
Copyright (C) 2024-2026 Ferran Duarri
SPDX-License-Identifier: GPL-2.0-only
```
