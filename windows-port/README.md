# GreenBoost Windows portable beta

Version: 2.4.0 beta 2. [Download](https://github.com/denoflore/greenboost-windows/releases/tag/v2.4.0-beta.2).

Keep `greenboost.exe` and `greenboost_cuda.dll` in the same extracted folder.

```powershell
.\greenboost.exe "C:\llama\llama-server.exe" -m "D:\models\model.gguf" -ngl 999 -c 2048
```

This starts a new application with GreenBoost and propagates injection through its x64 CreateProcessA/W children. The launcher waits and returns the application's exit code. It does not attach to an existing background server.

Windows 10/11 x64 and an NVIDIA driver with managed-memory support are required. Your application still needs its normal CUDA libraries. No GreenBoost kernel driver, administrator rights, test-signing mode or CUDA SDK is needed for this package.

## Check your GPU

```powershell
.\test_cuda.exe .\greenboost_cuda.dll 2 0 512 1
```

Arguments are DLL path, lookup mode (0=export, 1=older resolver, 2=CUDA 12+ resolver), CUDA device, allocation MiB and expected interception (1=yes, 0=native). This writes the allocation on the GPU and verifies a scalar readback and successful free. Use an otherwise idle GPU and expect `PASS`.

## Check your CUDA runtime

```powershell
.\test_compatibility.ps1 -RuntimePath "C:\my-app\cudart64_13.dll" -Device 0
```

Use your application's actual runtime path; CUDA 11 commonly uses `cudart64_110.dll`, CUDA 12 uses `cudart64_12.dll`, and CUDA 13 uses `cudart64_13.dll`. Keep each runtime with its matching application libraries. GreenBoost does not supply or upgrade NVIDIA libraries.

The script checks native allocation, a 512 MiB extended allocation, an 8 MiB native allocation and disabled extension. Every case performs a GPU write, scalar readback and free in a fresh process. It restores its process environment afterwards. Allow 512 MiB free VRAM and enough free RAM above the 8 GiB reserve.

CUDA 11.8, 12.0, 12.4, 12.9, 13.0, 13.1, 13.3 and 13.4 passed these checks on an RTX 3060 with driver 610.88. CUDA 13.4, supplied by Toolkit 13.4.3, also passed on an RTX 5090. Older drivers and unlisted combinations have not been verified. See `COMPATIBILITY.md` for the matrix, driver requirements and GPU-generation limits.

`test_lookup.exe .\greenboost_cuda.dll` checks NVIDIA resolver results, including unsupported requests and APIs outside GreenBoost's extension. It does not replace the GPU tests.

## Settings

```powershell
$env:GREENBOOST_EXTRA_MB = "4096"
$env:GREENBOOST_DEBUG = "1"
.\greenboost.exe "C:\path\application.exe" arguments
```

- `GREENBOOST_EXTRA_MB`: extra RAM allowance. Default is half of available RAM, capped to leave the reserve. `0` disables extension.
- `GREENBOOST_RESERVE_MB`: RAM excluded from the allowance, default `8192`.
- `GREENBOOST_THRESHOLD_MB`: redirect individual allocations at or above this size, default `256`.
- `GREENBOOST_DEBUG`: `1` prints diagnostics, including the launched PID.

All sizes above are MiB. Settings affect this process tree. Environment values override compatible legacy registry settings.

CUDA sees each GPU's physical capacity plus the allowance. Free capacity is limited by remaining allowance, tracked managed allocations and current available RAM. This is planning information, not a reservation or a promise that every allocation can succeed. Multiple processes/GPUs compete for system RAM.

## Scope

This beta extends synchronous CUDA allocations through the driver boundary. CUDA runtime entry-point lookup is intercepted recursively. Explicit managed allocations, asynchronous memory pools and CUDA virtual-memory APIs keep NVIDIA's implementation. NVML and nvidia-smi show physical GPU memory.

The tests in `VALIDATION.md` cover real CUDA kernels, the runtime compatibility matrix, child processes and llama.cpp with a Qwen3.5 4B model on an RTX 3060. Performance and compatibility depend on hardware, driver, model and application. LM Studio, Ollama, mixed 32/64-bit process trees and large production deployments are not yet qualified.

The package contains no kernel driver. Legacy driver installation, signing and diagnostics scripts in the source tree are separate development tools; they are not the portable setup procedure.

Source, build instructions and issue reporting: [denoflore/greenboost-windows](https://github.com/denoflore/greenboost-windows).

Original GreenBoost by [Ferran Duarri](https://gitlab.com/IsolatedOctopi/greenboost). Windows port by Chris Zuger / denoflore. GPL v2; Microsoft Detours is MIT licensed.
