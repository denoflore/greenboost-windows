# RTX kernels for GreenBoost

Updated October 31, 2026. Beta 3 integrates the official Windows x64 **llama.cpp b12019** backend, commit `1052f7e1d`, with a choice of CUDA 13.4 or CUDA 12.4. GreenBoost manages memory; llama.cpp supplies the inference kernels. Updating this backend does not update kernels inside an existing Ollama or LM Studio installation.

## Install and use

From the extracted GreenBoost beta 3 folder:

```powershell
.\install_llama.ps1
.\run_llama.ps1 -ListDevices
.\run_llama.ps1 -Model "D:\models\model.gguf" -Gpu 0 -Tokens 128
```

The installer downloads both official release archives into a new local directory. It installs no system driver or CUDA Toolkit, changes no PATH setting and refuses to overwrite an existing directory. Allow about 600 MB of downloads for CUDA 13.4 and additional space for extraction. If a download fails, the script reports the retained staging directory. Choose a fresh destination when retrying.

`-Gpu` uses the ordinal printed by `-ListDevices`. CUDA and `nvidia-smi` can number the same cards differently. The selected card becomes `CUDA0` inside the child process. Settings are restored when the script returns.

The completion profile enables Flash Attention, all model layers and tensors on the selected CUDA device, and greedy GPU token sampling. Greedy sampling always selects the highest scoring token; it is deterministic and differs from ordinary temperature sampling. The script does not enable grammar constraints or reasoning budgets, which can disable upstream GPU sampling. It sets a 4,096 MiB additional RAM allowance by default; change `-ExtraMiB` to suit available memory. `-ExtraMiB 0` disables the extension. Allocation or application errors remain failures.

Benchmark your own model through the same profile:

```powershell
.\run_llama.ps1 -Mode Benchmark -Model "D:\models\model.gguf" -Gpu 0 -PromptTokens 512 -Tokens 128 -Repetitions 3
```

Benchmark mode measures model evaluation and does not exercise token sampling. Its verbose log should show the selected GPU, all model layers offloaded and a CUDA model buffer. The completion log marks GPU samplers with `+`, for example `+temp-ext -> +greedy`. Host buffers used for input/output can still appear in the log.

This profile uses one GPU and upstream's default fused kernels and CUDA graph chunking. Concurrent QKV streams (`GGML_CUDA_GRAPH_OPT=1`) remain outside this profile because their benefit depends on the model. MMQ/cuBLAS selection remains with the upstream architecture dispatcher. Existing generic `greenboost.exe application.exe ...` usage is unchanged.

## GPU generation and backward compatibility

| GeForce family | Architecture | Backend choice | Verified scope |
| --- | --- | --- | --- |
| RTX 2000 | Turing, compute capability 7.5 | CUDA 12.4 for older drivers; 13.4 with a driver that supports its PTX | Both release binaries contain `sm_75` intermediate code; no RTX 20 hardware test |
| RTX 3000 | Ampere, 8.6 | CUDA 13.4 or 12.4 | Both contain native `sm_86` code; RTX 3060 measured, RTX 3090 package check passed |
| RTX 4000 | Ada, 8.9 | CUDA 13.4 or 12.4 | Both contain native `sm_89` code; RTX 4090 measured |
| RTX 5000 | Blackwell, 12.0 | CUDA 13.4 | Native `sm_120a` code verified; RTX 5090 measured, RTX 5060 Ti package check passed |

These are GeForce generations. Workstation names such as RTX A2000 and RTX 2000 Ada identify different architectures; use [NVIDIA's compute capability table](https://developer.nvidia.com/cuda/gpus).

For the older runtime option:

```powershell
.\install_llama.ps1 -Cuda 12.4
.\run_llama.ps1 -LlamaDirectory .\backends\llama-b12019-cuda12.4 -Model "D:\models\model.gguf" -Gpu 0
```

Both choices use the same current llama.cpp source and its applicable kernel fixes. CUDA 12.4 does not contain native Blackwell FP4 instructions. CUDA 13.4's `sm_75` PTX requires a driver capable of compiling that PTX version. Passing a CUDA allocation test on an older driver does not prove that PTX compilation works. NVIDIA documents this restriction under [minor version compatibility](https://docs.nvidia.com/deploy/cuda-compatibility/minor-version-compatibility.html). Consult the [CUDA 13.4 release notes](https://docs.nvidia.com/cuda/cuda-toolkit-release-notes/index.html) for its matching driver. No driver upgrade is performed by these scripts.

Binary architecture inspection used NVIDIA `cuobjdump --list-elf` and `--list-ptx`. CUDA 13.4 includes native 8.6, 8.9, 12.0a and 12.1a code, plus 7.5, 8.0 and 9.0 PTX. The original memory shim retains its separately verified CUDA 11.8 through 13.4 runtime compatibility; see `COMPATIBILITY.md`.

## Recent changes included

The pinned release contains these merged upstream changes. Their source commits were checked as ancestors of b12019:

| Change | Merged | Relevance |
| --- | --- | --- |
| [Fused feed-forward matrix multiplication and activation, #29948](https://github.com/ggml-org/llama.cpp/pull/29948) | October 25 | Q4_K, Q5_K, Q6_K, Q8_0 and IQ4_XS on Turing and newer; combines eligible prompt processing work into one kernel |
| [Fused MoE feed-forward kernels, #31188](https://github.com/ggml-org/llama.cpp/pull/31188) | October 28 | Extends that fusion to compatible mixture-of-experts models |
| [Chunked CUDA graphs, #30804](https://github.com/ggml-org/llama.cpp/pull/30804) | October 23 | Reduces graph launch overhead during token generation |
| [Flash Attention shared-memory race fix, #30573](https://github.com/ggml-org/llama.cpp/pull/30573) | October 19 | Adds the missing warp barrier before shared storage is reused |
| [Chunked gated delta network kernel, #29353](https://github.com/ggml-org/llama.cpp/pull/29353) | October 14 | Applicable Qwen hybrid model prompt processing at larger batches |

Blackwell native FP4 and GPU sampling are also present. FP4 acceleration requires a compatible FP4 model; installing the backend does not convert Q6_K or other weights. [NVIDIA describes the upstream RTX work](https://developer.nvidia.com/blog/open-source-ai-tool-upgrades-speed-up-llm-and-diffusion-models-on-nvidia-rtx-pcs/). Upstream benchmark percentages are not GreenBoost measurements.

## Local measurements

On October 31, 2026, Windows 11 and NVIDIA driver 610.88, both official b12019 builds ran the same Qwen3.5 4B Q6_K model through the beta 2 shim. Model size was 3.22 GiB; all 33 layers and all model tensors were placed on CUDA. Each process used 512 prompt tokens, 128 generated tokens, three repetitions, Flash Attention, one GPU, one orchestration thread, and 4,096 MiB additional allowance. CUDA graph optimization was `0`. The comparison order was 12.4, 13.4, 13.4, 12.4 per GPU. The initial cold 5090 run was retained separately and excluded from steady-state averages.

| GPU | Prompt tokens/s, 12.4 -> 13.4 | Generated tokens/s, 12.4 -> 13.4 | Interpretation |
| --- | --- | --- | --- |
| RTX 3060 | 3,194 -> 3,211 | 73.69 -> 73.67 | No material change |
| RTX 4090 | 15,588 -> 15,473 | 171.82 -> 172.55 | No demonstrated improvement beyond run variation |
| RTX 5090 | 17,279 -> 17,696 | 281.32 -> 288.49 | Observed +2.4% prompt and +2.6% generation; small differences with overlapping sample variation |

Each cell averages six timed samples across two processes. These are comparisons of complete CUDA builds, including their libraries and architecture targets. They do not isolate individual kernel changes or establish statistical significance. Both builds already contain the October source improvements. This model fits VRAM; these results do not measure oversubscribed inference, FP4 or a large MoE model.

Process and device receipts for those runs:

| GPU / PCI bus | CUDA 12.4 child PIDs | CUDA 13.4 child PIDs | Observed peak GPU utilisation |
| --- | --- | --- | --- |
| RTX 3060 / `09:00.0` | 52716, 16144 | 52056, 27136 | 98 to 99% |
| RTX 4090 / `03:00.0` | 5128, 35668 | 4032, 24692 | 96 to 97% |
| RTX 5090 / `01:00.0` | 52988, 47924 | 44224, 52824 | 90 to 96% |

The completion helper also ran on the RTX 3060 in process 54076: CUDA allocation, GPU sampling (`+temp-ext -> +greedy`), 32 generated tokens, exit 0 and observed GPU utilisation up to 98%. Both installers downloaded and extracted their actual upstream archives into paths containing spaces. The 12.4 runtime libraries were byte-identical to the libraries used in the comparison.

The extracted beta 3 helper subsequently completed `pp32` and `tg32`, one repetition each, on all five available cards with all 33 layers and the 3,293 MiB model buffer on CUDA:

| GPU / PCI bus | Child PID | Observed peak GPU utilisation | Exit |
| --- | --- | --- | --- |
| RTX 4090 / `03:00.0` | 52884 | 66% | 0 |
| RTX 5090 / `01:00.0` | 17148 | 11% | 0 |
| RTX 3090 / `05:00.0` | 42256 | 66% | 0 |
| RTX 5060 Ti / `08:00.0` | 54460 | 98% | 0 |
| RTX 3060 / `09:00.0` | 53664 | 83% | 0 |

These short runs establish package operation, not comparative throughput. Sampling at 500 ms can miss peaks in short GPU work. Invalid CUDA ordinal 31 returned exit 1 before model evaluation. Existing destination preservation and process environment restoration were also checked.

Windows PowerShell 5.1 `-File` initially failed while evaluating default path parameters. The old script's fresh process still returned 1; the updated script enumerated CUDA devices with exit 0; a nonexistent backend still returned 1. CI now exercises default path resolution under PowerShell 5.1. The separate Logicka record (`848e8743-cf59-4156-92e0-6bfa41ca136b`) remains inconclusive for formal causal attribution: its capture lacks executed source binding and carried path lineage, and no independent review was performed. The actual process results above do not imply formal causal closure.

RTX 20 execution, long-context stability, large oversubscribed models and independent kernel attribution remain unverified. No universal speedup is promised.
