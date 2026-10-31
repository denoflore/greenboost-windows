# GreenBoost Windows 2.5.0 experimental

GreenBoost starts a CUDA application with a library that changes selected
memory allocation calls. This version uses GPU memory (VRAM) first and can
retry eligible large allocations through NVIDIA managed memory when VRAM is
exhausted. The driver controls where managed memory lives.

**This guide describes 2.5.0 experimental, built from main.** The latest
published download is still 2.4.0 beta 3. It uses the previous allocation
policy and lacks the new cache controls, GPU inspection tool and file library.

## Run your first model

You need Windows 10/11 x64, a compatible NVIDIA driver and a GGUF model file
you supply. Running this package requires no administrator rights, GreenBoost
kernel driver or CUDA Toolkit.

Extract the ZIP and open PowerShell in that folder. Keep `greenboost.exe`
and `greenboost_cuda.dll` together. For RTX 3000, 4000 and 5000:

```powershell
.\install_llama.ps1 -Cuda 13.4
.\run_llama.ps1 -ListDevices
.\run_llama.ps1 -Model 'D:\models\model.gguf' -Gpu 0 -Prompt 'Explain why the sky is blue.'
```

Replace the model path and GPU number. The helper runs one prompt and exits.
It does not download a model. The installer supplies official llama.cpp
b12019 and CUDA libraries in a new local folder.
`-Cuda 12.4` on both scripts selects the older runtime build, including RTX 2000
code. CUDA 13.4 includes native RTX 3000/4000/5000 kernels. RTX 2000 hardware
execution and older drivers remain untested. Existing applications keep their
own backends. See `RTX_KERNELS.md` for upstream changes and architecture limits.

## Adjust memory use

The helper uses one CUDA GPU, Flash Attention and supported GPU sampling.
It defaults to a 4,096 token context, 128 output tokens and a 4,096 MiB extra
capacity allowance. The allowance does not reserve RAM or guarantee that a
larger model fits.
It accepts `-AllocationPolicy adaptive|managed`, `-CacheType f16|q8_0|q4_0`,
`-Context`, `-Batch`, `-MicroBatch`, `-Mode Benchmark` and `-VerboseBackend`.
The KV cache holds information about previous tokens. f16 is the default;
q8_0 and q4_0 use less memory and can change output. For example:

```powershell
.\run_llama.ps1 -Model 'D:\models\model.gguf' -Gpu 0 -CacheType q8_0 -Context 32768
```

The model must support that context length and the workload must fit the
memory available to CUDA. `greenboost-info.exe` shows GPU memory capabilities.
Use the CUDA GPU number printed by `-ListDevices`, which may differ from
`nvidia-smi`. Environment settings are restored after the helper exits.

## Launch another application

For another x64 CUDA application:

```powershell
.\greenboost.exe 'C:\application\app.exe' arguments
```

The launcher creates a fresh process, propagates injection through x64
CreateProcessA/W children, waits and returns the application's exit code.
It does not modify an already running background server.

## Allocation policy

The default `GREENBOOST_ALLOC_POLICY=adaptive` requests native VRAM first.
Only native out-of-memory permits a managed attempt for an eligible allocation.
`managed` retains the previous large-allocation policy. A failed allocation
before CUDA initialization no longer disables later managed requests.

| Environment variable | Default | Meaning |
| --- | --- | --- |
| `GREENBOOST_ALLOC_POLICY` | `adaptive` | `adaptive` or `managed`. |
| `GREENBOOST_EXTRA_MB` | Half of available RAM, capped by reserve | Extra reported capacity allowance. `0` disables extension. |
| `GREENBOOST_RESERVE_MB` | `8192` | RAM excluded from the allowance. |
| `GREENBOOST_THRESHOLD_MB` | `256` | Minimum allocation size eligible for managed memory. |
| `GREENBOOST_DEBUG` | `0` | `1` prints allocation and launch diagnostics. |
| `GREENBOOST_LEGACY_DRIVER` | `0` | `1` opts into the separately installed, unqualified KMDF path. |

Sizes are MiB. These settings apply to generic launches. The model helper
sets its own allowance and allocation policy from `-ExtraMiB` and
`-AllocationPolicy`. Process settings override compatible legacy registry
values. Capacity reporting does not reserve RAM or guarantee larger allocations.
Windows managed memory has platform limits; NVML and `nvidia-smi` continue to
show physical VRAM. No system paging settings are changed.

## Verify on your GPU

```powershell
.\test_cuda.exe .\greenboost_cuda.dll 2 0 512 0
.\test_compatibility.ps1 -RuntimePath 'C:\my-app\cudart64_13.dll' -Device 0
.\test_lookup.exe .\greenboost_cuda.dll
```

`test_cuda` arguments are DLL, lookup mode (0=export, 1=older resolver,
2=CUDA 12+ resolver), CUDA ordinal, MiB and expected managed allocation.
Adaptive should return native memory for 512 MiB when it fits. The runtime
script tests native, adaptive, managed, small and extension-disabled allocations
in five fresh processes, with GPU writes, readback and free. Use an idle GPU,
512 MiB free VRAM and sufficient free RAM above the 8 GiB reserve. Keep each CUDA
runtime with its matching application.

The shim extends synchronous driver allocation/free and capacity queries,
including runtime entry-point lookup. Explicit managed allocations, stream
ordered pools and CUDA VMM retain NVIDIA's own implementation.

## Experimental file residency

The storage build includes `greenboost_storage.lib`, `file_tensor.h` and
`test_storage.exe`. It keeps file offsets at stable CUDA virtual addresses,
using a bounded VRAM pool and two pinned RAM staging buffers. The caller must
request residency before a kernel consumes the region.

**This API is not connected to llama.cpp's model scheduler.** Running the
model helper does not stream model weights or MoE experts from SSD through
this library. File reads use the Windows cache and RAM staging buffers.
Optional NVIDIA hardware memory compression is lossless; it does not increase
capacity or compress the file.
See `EXPERIMENTAL.md` for API use, tests and exact limits.

`EXPERIMENTAL.md` records the October 31, 2026 measurements of this version.
`VALIDATION.md` and `COMPATIBILITY.md` retain earlier beta measurements. LM Studio, Ollama,
mixed-architecture child trees and long production runs remain unqualified.

Source: [denoflore/greenboost-windows](https://github.com/denoflore/greenboost-windows).
Original GreenBoost by [Ferran Duarri](https://gitlab.com/IsolatedOctopi/greenboost).
Windows port by Chris Zuger / denoflore. GPL v2; Microsoft Detours is MIT licensed.
