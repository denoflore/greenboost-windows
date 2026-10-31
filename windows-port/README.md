# GreenBoost Windows 2.5.0 experimental

The main branch builds this experimental version. The latest published binary
package remains 2.4.0 beta 3 and lacks these experimental changes. Keep
`greenboost.exe` and `greenboost_cuda.dll` together. Windows 10/11 x64 and a
compatible NVIDIA CUDA driver are required. No GreenBoost kernel driver,
administrator rights, CUDA SDK or test-signing mode is required to run it.

```powershell
.\greenboost-info.exe
.\install_llama.ps1 -Cuda 13.4
.\run_llama.ps1 -ListDevices
.\run_llama.ps1 -Model 'D:\models\model.gguf' -Gpu 0 -CacheType q8_0
```

The installer supplies official llama.cpp b12019 in a new local folder.
`-Cuda 12.4` on both scripts selects the older runtime build, including RTX 2000
code. CUDA 13.4 includes native RTX 3000/4000/5000 kernels. RTX 2000 hardware
execution and older drivers remain untested. Existing applications keep their
own backends. See `RTX_KERNELS.md` for upstream changes and architecture limits.

The helper uses one CUDA GPU, Flash Attention and supported GPU sampling.
It accepts `-AllocationPolicy adaptive|managed`, `-CacheType f16|q8_0|q4_0`,
`-Context`, `-Batch`, `-MicroBatch`, `-Mode Benchmark` and `-VerboseBackend`.
f16 is the default cache. Quantization saves memory and can change output.
Use the CUDA ordinal printed by `greenboost-info`, which may differ from
`nvidia-smi`. Environment settings are restored after the helper exits.

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

Sizes are MiB. These process settings override compatible legacy registry
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

This is a developer API, not a llama.cpp scheduler integration or transparent
paging for arbitrary CUDA kernels. Optional NVIDIA hardware memory compression
is lossless but does not increase capacity or compress the file.
See `EXPERIMENTAL.md` for API use, tests and exact limits.

`EXPERIMENTAL.md` contains this branch's measurements. `VALIDATION.md` and
`COMPATIBILITY.md` retain earlier beta measurements. LM Studio, Ollama,
mixed-architecture child trees and long production runs remain unqualified.

Source: [denoflore/greenboost-windows](https://github.com/denoflore/greenboost-windows).
Original GreenBoost by [Ferran Duarri](https://gitlab.com/IsolatedOctopi/greenboost).
Windows port by Chris Zuger / denoflore. GPL v2; Microsoft Detours is MIT licensed.
