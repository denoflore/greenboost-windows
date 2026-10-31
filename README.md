![GreenBoost Windows](assets/header.png)

# GreenBoost for Windows

GreenBoost redirects large CUDA allocations to NVIDIA managed memory so supported applications can use system RAM when their GPU memory is insufficient. This Windows fork builds on [Ferran Duarri's GreenBoost](https://gitlab.com/IsolatedOctopi/greenboost), under GPL v2. Windows maintenance by [Chris Zuger / denoflore](https://github.com/denoflore).

**The portable Windows beta includes a launcher and DLL. No GreenBoost kernel driver, administrator access, WDK or test-signing mode is required.**

## Download and run

1. Open [Releases](https://github.com/denoflore/greenboost-windows/releases) and download `GreenBoost-Windows-2.4.0-beta.1-x64.zip`.
2. Extract it to a local folder. Keep `greenboost.exe` and `greenboost_cuda.dll` together.
3. Start a fresh instance of your CUDA application through the launcher:

```powershell
.\greenboost.exe "C:\llama\llama-server.exe" -m "D:\models\model.gguf" -ngl 999 -c 2048
```

Close an already running application first. Injecting a new client does not modify an existing background server.

Requirements: Windows 10/11 x64, an NVIDIA GPU with managed-memory support, a compatible NVIDIA driver and enough free system RAM. Your CUDA application keeps its own runtime requirements. This beta's launcher and shim use the static Microsoft C++ runtime.

For a quick GPU check, run this from the extracted folder. Change `0` if needed to select your CUDA device:

```powershell
.\test_cuda.exe .\greenboost_cuda.dll 2 0 512 1
```

It allocates 512 MiB through the shim, runs a GPU write, reads the final scalar and frees the allocation. It must finish with `PASS`; a larger displayed memory total alone is not evidence that interception works.

## Configuration

Settings apply to the launched process and its children:

```powershell
$env:GREENBOOST_EXTRA_MB = "4096"
$env:GREENBOOST_DEBUG = "1"
.\greenboost.exe "C:\llama\llama-server.exe" -m "D:\models\model.gguf" -ngl 999
```

| Variable | Default | Meaning |
| --- | --- | --- |
| `GREENBOOST_EXTRA_MB` | Half of currently available RAM, limited by the reserve below | Additional memory allowance reported to CUDA applications. Explicit values are also capped by available RAM. Set `0` to disable extension. |
| `GREENBOOST_RESERVE_MB` | `8192` | Free system RAM excluded from that allowance. |
| `GREENBOOST_THRESHOLD_MB` | `256` | Minimum individual allocation size redirected to managed memory. |
| `GREENBOOST_DEBUG` | `0` | Set `1` to print hook, allocation and launch diagnostics. |

The allowance helps applications plan allocations. It does not reserve RAM or guarantee every allocation will succeed. Multiple processes and GPUs share the same physical system RAM. Use one workload first and size its allowance conservatively.

Existing registry threshold/debug/RAM-cap settings are read for compatibility. Environment settings take precedence. The old configured physical VRAM value is ignored; each GPU's real capacity is queried.

## What this beta supports

- Synchronous CUDA driver allocation/free and capacity queries, including runtime entry-point lookup through `cuGetProcAddress` and its CUDA 12+ ABI.
- CUDA runtimes that obtain those driver entry points, including the tested CUDA 12.4 llama.cpp build and CUDA 13.3 runtime.
- Child processes created through Windows `CreateProcessA` and `CreateProcessW`.
- NVIDIA's physical memory telemetry remains unchanged in NVML and `nvidia-smi`.

Stream-ordered memory pools, CUDA virtual-memory allocations and explicit managed-memory calls retain NVIDIA's own behaviour. They are not extended by this shim. This package targets x64 applications and children; protected applications and mixed-architecture process trees are not qualified.

## Verified scope and limits

The October 31, 2026 checks used an RTX 3060 with 12,287 MiB physical CUDA capacity. A 12,544 MiB managed allocation completed a GPU fill, readback and free. A real llama.cpp Qwen3.5 4B run completed with its model buffer allocated through GreenBoost. [Validation details](windows-port/VALIDATION.md) distinguish those two tests.

This does not establish that a 40 GB model will work on every Windows GPU, or that system RAM matches VRAM speed. Other drivers, larger models, long runs, LM Studio and Ollama still need application-specific testing. NVMe capacity is not advertised as GPU memory.

## Build and contribute

See [Windows build instructions](windows-port/BUILDING.md), [portable usage](windows-port/README.md) and [troubleshooting](windows-port/TROUBLESHOOTING.md).

The legacy KMDF driver and installer remain in `windows-port/driver` and `windows-port/tools` for development. They are not included in this portable release and were not qualified by these tests. Do not enable Windows test-signing mode for the portable package.

The Linux sources are retained from upstream. This maintenance release changes the Windows port; it does not claim to fix the Linux invalid-context issue.

License: [GPL v2](LICENSE). Microsoft Detours is MIT licensed; its notice ships in the ZIP.
