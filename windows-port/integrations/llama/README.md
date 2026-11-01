# Configure model offloading with GreenBoost

GreenBoost can keep a supported MoE model's expert weights in its GGUF file
and bring the required sections through system RAM into a limited amount of
GPU memory. MoE models divide part of their work among specialist sections
called experts. Only the selected experts are needed for each operation.

**You choose this mode when launching the model. There is no automatic switch
from the ordinary launcher when GPU memory runs out.** Use normal loading
when the complete workload fits. Expert paging is an experimental option for
reducing the memory occupied by expert weights, with additional transfer costs.
Useful speed on models larger than the card has not been established.

This guide covers the source on `main`. The published 2.4.0 beta 3 ZIP does
not contain this backend. Build it separately using the instructions below.

## Choose the loading mode

| Your situation | What to use |
| --- | --- |
| The model and your chosen context fit in GPU memory | Use the [ordinary model launcher](../../../README.md#run-your-first-model). Keep expert paging off. |
| A supported quantised MoE model needs less memory for its experts | Build this backend and launch with `run_moe.ps1`, setting an expert VRAM pool and RAM cache. |
| A dense model needs more memory | This adapter does not page dense weights. Reduce the context or use a smaller model or quantisation. |
| You use Ollama, LM Studio or an existing inference server | This adapter is not integrated with those applications. Injecting `greenboost_cuda.dll` does not enable it. |

The expert pool is separate from the portable shim's extra capacity allowance.
`-ExtraMiB` and `GREENBOOST_EXTRA_MB` change reported capacity for the ordinary
shim. They do not configure expert paging. This mode uses `-ExpertVramMiB`
and `-ExpertRamMiB` and launches the compiled backend directly.

## Requirements

- Windows x64 and an NVIDIA CUDA GPU with driver support for CUDA virtual
  memory management. Model inference in this revision was exercised on an
  RTX 3060; see [evidence](#evidence) for the exact limits.
- Visual Studio C++ Build Tools with the C++ workload and Windows SDK,
  CMake and Ninja available on `PATH`, and a CUDA Toolkit that supports your
  GPU and installed compiler. The November 1 build used MSVC 19.44 and CUDA 13.3.
- A normal quantised MoE GGUF in one file on a local drive. Keep that file
  available and unchanged for the entire run. The scripts do not download a
  model. The recorded model is Granite 3.1 1B A400M Q4_K_M.
- Enough GPU memory for the expert pool **plus** ordinary model weights,
  attention memory (the KV cache), temporary calculation buffers and CUDA.
  System RAM must accommodate the pinned cache and the rest of the application.

This separate build does not require Microsoft Detours, `greenboost.exe`,
`greenboost_cuda.dll` or the legacy GreenBoost kernel driver. It needs its own
compiled CUDA libraries, which the build helper places beside the executable.

## 1. Get the source and build the backend

For a new checkout:

```powershell
git clone https://github.com/denoflore/greenboost-windows.git
Set-Location .\greenboost-windows
```

If you already have the source, open PowerShell in the repository root.
The following example builds for an RTX 3000 GPU. Replace the Toolkit path
with your installed version and choose the architecture for your card:

```powershell
.\windows-port\tools\build_moe.ps1 `
    -CudaRoot 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3' `
    -Architectures 86
```

| GeForce family | `-Architectures` value |
| --- | --- |
| RTX 2000 | `75` |
| RTX 3000 | `86` |
| RTX 4000 | `89` |
| RTX 5000 | `120` |

These are build targets, not a hardware qualification list. The installed
Toolkit must support each requested target. Only SM86 model execution is
covered by the new receipts. To include several targets, quote the list:
`-Architectures '75;86;89;120'`. Omitting the option requests that full list.

The helper downloads official llama.cpp **b12019**, applies the adapter and
builds a separate backend. It preserves upstream licence notices. On success,
it prints the directory containing `llama-completion.exe`, `llama-bench.exe`
and their DLLs. Keep the directory together.

Optional build settings:

| Setting | Use |
| --- | --- |
| `-Directory 'D:\builds\greenboost-moe'` | Put the downloaded source and compiled backend in a different directory. Its backend will be `D:\builds\greenboost-moe\build\bin`. |
| `-SourceDirectory 'D:\src\llama.cpp-b12019'` | Copy an existing, unmodified b12019 source directory instead of downloading it. |
| `-Jobs 4` | Use fewer compiler processes if the default eight use too much RAM. |

When updating the adapter, use a fresh `-Directory` and point the launcher
at its new `build\bin` folder. Reusing a prepared source directory does not
reapply an updated patch. An arbitrary newer llama.cpp version is unsupported.

## 2. Choose the GPU

In a fresh PowerShell window at the repository root, set the backend path.
Clear inherited device filters in this shell so the listing uses the same
numbering as the launcher:

```powershell
$backend = (Resolve-Path '.\windows-port\tools\backends\greenboost-moe-b12019\build\bin').Path
$env:CUDA_VISIBLE_DEVICES = $null
$env:CUDA_DEVICE_ORDER = $null
& "$backend\llama-completion.exe" --list-devices
```

Use the number in that CUDA listing. `CUDA2` means `-Gpu 2` when launching.
`nvidia-smi` can number cards differently. The launcher exposes only your
chosen card, so the running model will call it `CUDA0` internally.

## 3. Set the memory budgets and launch

This is a configuration example, not a promise that every model fits these
limits. It gives expert weights a 1 GiB GPU pool and a 4 GiB pinned RAM cache:

```powershell
.\windows-port\tools\run_moe.ps1 `
    -LlamaDirectory $backend `
    -Model 'D:\models\your-moe-model.gguf' `
    -Gpu 0 `
    -ExpertVramMiB 1024 `
    -ExpertRamMiB 4096 `
    -Context 2048 `
    -MicroBatch 8 `
    -Tokens 128 `
    -Prompt 'Explain how a telescope works.'
```

Replace the model path and GPU number. The script runs one prompt and exits.
It restores the environment settings it changed when it finishes. Each launch
creates a new GPU pool and RAM cache; they do not persist between processes.

### How to choose the limits

All memory sizes below are **MiB**: 1,024 MiB is 1 GiB.

| Setting | Default | What it controls |
| --- | --- | --- |
| `-ExpertVramMiB` | `1024` | Physical GPU memory reserved for paged expert weights. It does not cap the process's total VRAM. |
| `-ExpertRamMiB` | `4096` | Pinned RAM cache for file data, meaning RAM Windows cannot page out while allocated. `0` disables the cache and uses two 2 MiB staging buffers. |
| `-Context` | `2048` | Maximum context length. The KV cache and other context allocations still use GPU memory. |
| `-MicroBatch` | `8` | Tokens processed together. Larger batches can require more experts at once and larger temporary buffers. The helper uses this for both batch settings. |
| `-Tokens` | `128` | Maximum generated tokens for this completion. |
| `-Gpu` | `0` | CUDA device number from the unfiltered listing above. |
| `-Prompt` | An explanation of GPU memory and RAM | Text to send to the model. |
| `-VerboseBackend` | Off | Additional loader and backend messages. |

`-Model` and `-LlamaDirectory` are required. Context and output must fit the
model's supported context. `run_moe.ps1` has no `-ExtraMiB`, `-CacheType`,
`-AllocationPolicy` or automatic fitting option.

**Give the expert pool as much free VRAM as you can spare after the rest of
the workload and other applications are accounted for.** A smaller pool
forces more transfers. The 32 MiB pool used in the development receipt
demonstrates bounded residency; it is not a recommended general setting.

For illustration, if 9 GiB is available and the rest of the model needs
3 GiB, leaving 1 GiB spare would leave 5 GiB for the expert pool
(`-ExpertVramMiB 5120`). Those figures are an example, not measured requirements
for your model. The GGUF file size alone cannot tell you how much space the
other weights, context and calculation buffers need.

The RAM cache can avoid repeated file reads. It still transfers missing
pages into the GPU pool. It may duplicate bytes already in VRAM, and Windows
can also retain its own file cache. Leave RAM for Windows, other applications
and model loading. This mode does not apply the allocation shim's RAM reserve
for you. Use `-ExpertRamMiB 0` when you want only staging memory for this path;
other model and process memory still applies.

Both budgets are rounded down to whole 2 MiB chunks and capped at the file's
rounded size. Use multiples of 2 MiB. A nonzero RAM budget smaller than one
chunk behaves like cache disabled. The wrapper requires at least 2 MiB for
the expert pool, and a model's actual minimum may be much larger.

The unit of residency is a 2 MiB chunk. Requested spans are combined before
eviction, so all chunks needed by an operation must fit the expert pool.
If they do not fit, increase `-ExpertVramMiB` when there is room, or reduce
`-MicroBatch`, down to `1` if needed. Reducing the batch cannot make an
individual required expert operation fit an arbitrarily small pool.

## 4. Recognise an active offload run

When the backend binds paged expert weights, its startup log includes a line
beginning `GreenBoost pid=` with `CUDA=`, `PCI=`, `expert_pool=` and
`host_cache=`. The pool and cache values in this line are **bytes**. On a
normal exit, `GreenBoost residency:` reports file bytes read, bytes uploaded,
cache hits and evictions.

`uploaded` can be much larger than the model file because pages may be loaded
repeatedly. An eviction means the GPU pool reused space for another page.
The startup line confirms that the paging backend was engaged; memory usage
alone does not prove where calculations ran or establish a speed benefit.

If that line is absent, do not assume paging is active. Check that the chosen
backend was built with `build_moe.ps1`, the model contains supported routed
experts, and you launched through `run_moe.ps1`. A dense model has no expert
weights for this mechanism to page.

## 5. Return to normal loading

To use normal loading, choose the [ordinary backend and launcher](../../../README.md#run-your-first-model).
`run_moe.ps1` restores its environment on exit, so it does not permanently
enable expert paging for future shells or applications.

You can also run the compiled backend directly with expert paging disabled.
Use the `$backend` path from step 2, set the selected GPU number, and clear
any paging settings you set manually:

```powershell
$env:GREENBOOST_EXPERT_VRAM_MB = $null
$env:GREENBOOST_EXPERT_RAM_MB = $null
$env:GGML_CUDA_DISABLE_FUSION = $null
$env:CUDA_DEVICE_ORDER = $null
$env:CUDA_VISIBLE_DEVICES = '0'
& "$backend\llama-completion.exe" `
    -m 'D:\models\your-moe-model.gguf' `
    -ngl 999 -dev CUDA0 -sm none -mg 0 -ot '.*=CUDA0' `
    -fa on -t 1 -b 8 -ub 8 --no-host -fit off `
    -c 2048 -n 128 -p 'Explain how a telescope works.' `
    --backend-sampling --samplers temperature --temp 0 --no-conversation
```

This requests normal GPU loading and requires the workload to fit. The
compiled experimental backend still has CUDA graphs disabled at build time.
Use the ordinary backend for its usual optimisation settings. Passing
`-ExpertVramMiB 0` to `run_moe.ps1` is invalid; that script always enables paging.

## Troubleshooting

| Message or symptom | Meaning and action |
| --- | --- |
| `Requested span union exceeds the resident VRAM budget`, or `Requested span exceeds the resident VRAM budget` | One operation needs more pages than the pool holds. Increase the expert pool if VRAM is available, or lower `-MicroBatch`. |
| CUDA out of memory while loading or creating the context | The pool plus ordinary weights, KV cache and temporary buffers may exceed available memory. Reduce the pool or context, close other GPU workloads, or use a smaller model. A larger RAM cache does not fix these GPU allocations. |
| Pinned host allocation fails | The requested RAM cache may be too large for available host memory. Reduce `-ExpertRamMiB`, or set it to `0`. |
| `expert tensors require quantisation and rows aligned...`, `unsupported CUDA router layout`, or `unsupported consumer of paged expert weights` | The model or operation is outside this adapter's supported layout. Use normal loading or a supported model. Increasing budgets cannot add operator support. |
| `split GGUF expert buffers are not supported` | This build expects one GGUF file. Split model files are outside the supported scope. |
| `CUDA virtual memory management is unavailable on this device` | The selected device and driver do not expose the mapping feature this adapter needs. Use normal loading on that configuration. |
| `Set -CudaRoot to an installed CUDA Toolkit`, or an unknown CUDA architecture | Check the Toolkit path and select an architecture supported by that installation and your card. |
| `Pinned b12019 anchor is absent or ambiguous` or `Unprepared source already exists` | Use untouched b12019 source and a fresh build directory. Do not apply the patch twice or to another llama.cpp release. |
| Very slow generation with frequent evictions | Repeated transfers and mapping work can dominate inference. Increase the expert pool if there is spare VRAM. A larger RAM cache may reduce file reads but cannot remove GPU uploads. Use normal loading when the workload fits. |

## How the loading works

The CUDA router's actual expert IDs determine which weight regions are loaded
before each expert matrix operation. Dense weights, KV cache and computation
remain on the selected GPU. The GGUF weights and upstream CUDA inference
kernels keep their existing formats and calculation paths.

Each chunk keeps its CUDA virtual address when reloaded. The library uses
overlapped Windows reads and pinned transfers, then marks resident mappings
read-only. A CUDA event protects every consumer before its physical memory
can be reused. Completely resident requests require no residency barrier.

This path uses Windows file I/O through RAM. Direct storage-to-GPU transfers
and automatic GPU page-fault handling are not implemented.

## Current scope

- One GPU, one GGUF file, one owner thread and CUDA context.
- Quantised MoE expert matrices with rows aligned to llama.cpp's CUDA
  MATRIX_ROW_PADDING. The tested model is Granite 3.1 1B A400M Q4_K_M.
- Actual router selection and expert matrix multiplication execute on CUDA.
  CPU work is file I/O, launch orchestration and bounded request metadata.
- Graph capture and fusion are disabled for paged execution so they cannot
  bypass residency acquisition. Supported upstream CUDA matrix paths are
  required; unsupported operations fail.
- Split GGUF expert buffers, paged LoRA operations, multiple concurrent model
  contexts, CPU validation of paged tensors and server integration are not
  qualified. The supplied command runs a completion and exits.
- Model quality, long contexts, long sessions and models exceeding physical
  card capacity remain unqualified by this small-model test.

## Evidence

The [November 1 receipts](../../STREAMING_RESULTS_20261101.json) include the
commands, output, process identities, GPU samples and profiler summaries.

The new receipts distinguish storage integrity from model execution.
The same-build resident and paged Granite runs generated identical text for
the initial 64-token test. The expert pool was 32 MiB. Adapter-wide peak
memory readings were approximately 2,484 MiB resident and 1,820 MiB paged,
including existing desktop memory; these are not isolated allocation totals.

Paging was slower for this model, which fits in VRAM. The resident run
reported 128.77 decode tokens/s. Initial paged runs reported 3.86 tokens/s
with a 128 MiB RAM cache and 5.79 with 768 MiB. These are single exploratory
runs, not repeated performance qualification.

The final launcher build also matched the resident backend's generated text
for two different prompts (64 and 32 output tokens). A deliberately undersized
2 MiB pool failed with the explicit resident-budget diagnostic. Storage
integrity additionally passed on the RTX 5090; model inference on that card
has not been checked in this revision.

Nsight Systems observed PID 67396 running 75,329 CUDA kernels on the RTX 3060
at PCI 0000:09:00.0, including 4,749 routing-request kernels. It recorded
about 195 ms of kernel activity and 7.26 seconds of host-to-device transfers.
The 2 MiB page granularity uploaded about 45.9 GB over this small-model run.
This identifies transfer and mapping costs; it does not establish a speedup
over resident inference or a cold-SSD benchmark.

Use the resident backend when the model fits. This experimental path supplies
explicit bounded residency. A large-model throughput claim needs a matching
large-model receipt.

## Storage API

`FileTensor::acquire_many(spans)` protects the union of noncontiguous spans.
Submit consumers on `stream()`, or call `record_use(consumer_stream)`
after submitting them and before the next acquisition. The original
`acquire(offset, bytes)` API remains supported.

Build the existing test_storage and the new tests/test_residency.c with
GB_BUILD_STORAGE=ON. The latter reuses the original fixture's CUDA verifier
and checks scattered spans, overlapping spans, an oversized union, a partial
last chunk, a separate consumer stream and cache-disabled behaviour.

The original tests passed unchanged. The deliberate wrong-seed control failed
with GPU mismatches, as required. Logicka's causal report remains inconclusive
because the imported summaries do not establish executed-source lineage and
its completed-run contract rejects a nonzero expected-failure control.
No independent review has been recorded. These limitations do not turn the
captured CUDA results into general production qualification.
