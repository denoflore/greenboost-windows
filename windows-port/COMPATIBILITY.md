# CUDA compatibility

Checked October 31, 2026 for GreenBoost Windows 2.4.0 beta 2. The launcher and shim use the same runtime mechanism as beta 1; this update adds compatibility checks, packages the test tools and documents the measured scope.

## Verified runtime matrix

Windows 11 Pro build 26200, NVIDIA driver 610.88, RTX 3060, CUDA device 4, PCI `0000:09:00.0`, physical CUDA capacity 12,287 MiB. Each row used the actual NVIDIA runtime DLL in four fresh processes. No driver replacement, mocked CUDA implementation or CPU computation path was used.

| CUDA runtime | Source | Native / extended / small / disabled | Extended-process PID |
| --- | --- | --- | --- |
| 11.8 | NVIDIA cudart 11.8.89 redistributable | 4/4 pass | 17640 |
| 12.0 | NVIDIA cudart 12.0.146 redistributable | 4/4 pass | 21648 |
| 12.4 | Official llama.cpp b12018 CUDA 12.4 dependency package | 4/4 pass | 17052 |
| 12.9 | NVIDIA cudart 12.9.79 redistributable | 4/4 pass | 53092 |
| 13.0 | NVIDIA cudart 13.0.96 redistributable | 4/4 pass | 52972 |
| 13.1 | Installed NVIDIA Toolkit 13.1 | 4/4 pass | 41996 |
| 13.3 | Installed NVIDIA Toolkit 13.3 | 4/4 pass | 35784 |
| 13.4 | Toolkit 13.4.3, cudart 13.4.92 redistributable | 4/4 pass | 8292 |

Every process printed its PID, CUDA ordinal and PCI address, wrote the allocation using `cudaMemset`, read back the final scalar `1515870810`, and freed the allocation successfully. GPU samples taken during each four-process sequence reached 99% on the selected RTX 3060. This verifies the bounded operations; it is not a throughput comparison.

The extracted beta 2 ZIP also passed the four CUDA 13.4 cases on an RTX 5090 (CUDA device 1, PCI `0000:01:00.0`) from a directory containing spaces. Native/extended/small/disabled process IDs were 48080/54904/52504/35400. Physical and extended totals were 32,606 and 33,630 MiB; GPU utilisation samples reached 98%. This adds one Blackwell runtime/GPU combination, for 36 successful runtime cases in total.

The four cases were:

| Case | Allocation | Expected result | Observed CUDA total |
| --- | --- | --- | --- |
| Native application without GreenBoost | 512 MiB | Native device memory | 12,287 MiB |
| GreenBoost, 1,024 MiB allowance | 512 MiB | Managed memory | 13,311 MiB |
| GreenBoost, below the 256 MiB threshold | 8 MiB | Native device memory | 13,311 MiB |
| GreenBoost, allowance set to zero | 512 MiB | Native device memory | 12,287 MiB |

The real driver lookup check also passed 34 cases across the four-argument and five-argument `cuGetProcAddress` interfaces. It compares NVIDIA's results before and after loading the shim: redirected memory functions, recursive resolver lookup, stream flags, old ABI requests, invalid versions, missing symbols, and unchanged managed/async/VMM entry points. Pointer preservation checks do not claim execution coverage of those async or VMM operations.

## What backward compatibility means here

The verified scope is older runtime DLLs on the tested newer driver. It does not establish compatibility with older drivers, every intermediate CUDA release, all CUDA libraries, or every GPU architecture. The shim resolves available driver exports dynamically and keeps NVIDIA's failure results and unsupported APIs intact.

NVIDIA documents backward compatibility for older CUDA applications on newer drivers. Its minor-version compatibility permits some newer runtimes on older drivers within the same major family, subject to feature and PTX limitations. CUDA 13.4 corresponds to R615; new 13.4 features can require that branch even though these existing memory operations passed on 610.88. See [NVIDIA's compatibility guide](https://docs.nvidia.com/deploy/cuda-compatibility/minor-version-compatibility.html) and [release notes](https://docs.nvidia.com/cuda/cuda-toolkit-release-notes/index.html).

CUDA 13 removed offline compilation and library support for Maxwell, Pascal and Volta. Applications targeting those GPUs need suitable CUDA 12.x or earlier builds and a driver supporting their hardware. GreenBoost cannot restore architectures removed from an application's libraries. Those older GPUs have not been qualified here. [NVIDIA CUDA 13.0 architecture notes](https://docs.nvidia.com/cuda/archive/13.0.0/cuda-toolkit-release-notes/index.html#deprecated-architectures).

The latest toolkit checked was [CUDA 13.4.3 in NVIDIA's archive](https://developer.nvidia.com/cuda-toolkit-archive). Future releases need another measured compatibility pass.

## Reproduce with your application

Extract the beta ZIP and run from PowerShell:

```powershell
.\test_compatibility.ps1 -RuntimePath "C:\my-app\cudart64_13.dll" -Device 0
.\test_lookup.exe .\greenboost_cuda.dll
```

`RuntimePath` also accepts multiple DLL paths in a PowerShell array. Each runtime stays in its original application directory; do not mix libraries from different CUDA major releases. The package includes no NVIDIA runtime DLLs. The script changes only its process environment and restores those settings on completion or failure.

Use an otherwise idle GPU with at least 512 MiB free VRAM, and enough free system RAM to leave the 8 GiB reserve plus the requested 1 GiB allowance. The runtime check uses the same `test_runtime.exe` production-driver probe as the original beta validation.

The hosted Windows CI job proves compilation, packaging and launcher behaviour without a GPU. The runtime matrix above comes from separate real GPU execution. Large-model inference, performance, older drivers, other runtime/GPU combinations, LM Studio/Ollama and long-duration reliability remain unqualified.
