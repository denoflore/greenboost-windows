# Windows beta validation

Date: October 31, 2026. NVIDIA driver: 610.88. Scope: isolated Windows x64 processes on one workstation, not a production deployment or a cross-hardware compatibility claim.

## Beta 2 compatibility checks

The [runtime matrix](COMPATIBILITY.md) adds CUDA 11.8, 12.0, 12.4, 12.9, 13.0, 13.1, 13.3 and 13.4 from Toolkit 13.4.3. All 32 runtime cases passed: native allocation, extension, below-threshold allocation and extension disabled, each with GPU memset, scalar readback and free. Actual process/device identifiers and observed GPU utilisation are recorded in that document. The shim implementation was unchanged from beta 1.

The final extracted package also passed the four CUDA 13.4 cases on an RTX 5090 from a path containing spaces, bringing the total to 36 successful runtime cases. This qualifies that additional combination; it does not extend every matrix row to every GPU.

`test_lookup` passed 34 real-driver comparisons covering both resolver ABIs, recursive lookup, stream flags, old/invalid versions, unknown symbols and unchanged APIs outside the extension. Beta 2 packages `test_runtime`, `test_lookup`, `test_compatibility.ps1` and `COMPATIBILITY.md` so these checks can be repeated with an application's own runtime.

## Observed cases

| Case | Before | Candidate |
| --- | --- | --- |
| Documented build from repository root | CMake looked in the wrong source directory | Script resolves its own source directory and builds the portable targets |
| Driver resolver v1/v2, 512 MiB | Native allocation bypassed the shim | Managed allocation, GPU fill/readback/free |
| Actual CUDA 13.3 runtime, 512 MiB | Managed attribute 0 | Managed attribute 1, GPU memset/readback/free |
| CUDA 13.3 runtime, 8 MiB | Native allocation | Native allocation retained below threshold |
| CreateProcessW/A child | Parent injected, child native | Both APIs launched an injected CUDA child |
| Capacity on RTX 3060 and RTX 4090 | Both reported the hardcoded 64,512 MiB | Each physical total plus exactly 1,024 or 2,048 MiB; zero setting restored physical totals |
| Real invalid free after native release | NVIDIA returned CUDA 1; shim returned success | Shim returns CUDA 1 |
| Managed allocation larger than VRAM | Direct-export path could allocate | 12,544 MiB allocation on a 12,287 MiB RTX 3060 completed GPU fill/readback/free through resolver v2 |
| llama.cpp b12018, CUDA 12.4, Qwen3.5 4B Q6_K | Not used as a failure baseline | All 33 layers offloaded; 3,293 MiB model allocation intercepted; prompt processing and generation completed |

The oversized-allocation probe PID was 52116, CUDA device 4, PCI 0000:09:00.0. GPU samples reached 100% during its kernel. The CUDA 13.3 runtime probe PID 35008 reached 96% on the same GPU. These establish that the bounded writes ran on CUDA. They do not establish an inference speed improvement.

The llama.cpp check used a model that fits physical VRAM. It establishes application integration with intercepted memory, not successful inference of a 40 GB model. The large allocation test is a separate memory/kernel check.

The extracted static-runtime ZIP was also tested from a directory containing spaces. Its driver probe PID 40884 and CUDA 13.3 runtime probe PID 51036 passed. The final llama.cpp process PID 52568 used CUDA_VISIBLE_DEVICES=4, with tensor override `.*=CUDA0`, all 33 layers on CUDA, normal CUDA graph settings, pp32/tg8 and one repetition. Its RTX 3060 GPU samples reached 64%; no CPU model buffer was reported. This is a bounded functional run, not a comparative benchmark.

The portable launcher imports KERNEL32.dll; the shim imports KERNEL32.dll and ADVAPI32.dll. NVIDIA application libraries are supplied by the application. Beta 2 contains the launcher, shim, three probes, the compatibility script and five documentation/licence files.

## Reproduce

From a built or extracted package, replace device 0 with the intended CUDA ordinal:

```powershell
.\test_cuda.exe .\greenboost_cuda.dll 2 0 512 1
.\test_cuda.exe .\greenboost_cuda.dll 2 0 8 0
```

Run a larger allocation only when you have sufficient available RAM and have selected an otherwise idle GPU. The test writes the full allocation on the GPU, verifies the final scalar and frees it.

The source also contains:
- `test_runtime.exe <cudart DLL path> <device> <MiB> <expected managed>`, launched through GreenBoost.
- `test_child.exe A|W "child command line"`, launched through GreenBoost.
- `test_capacity.exe <shim path> <device A> <device B> <expected extra MiB>`, with `GREENBOOST_EXTRA_MB` set before launch.
- `test_free_error.exe <shim path>`, run directly; use `CUDA_VISIBLE_DEVICES` to select its device.

## Evidence limits

The structured debugging recorder reports semantic PASS for the runtime, child and capacity observations. Causal attribution remains INCONCLUSIVE and its mechanism verdict is UNOBSERVED: the imported records lack executed-source binding and a complete carried-value trace. Runtime prediction matched; child/capacity prediction records remain inconclusive. These records are retained as bounded evidence, not formal causal closure or independent review.

Unqualified: the legacy kernel driver, Linux changes, LM Studio/Ollama, mixed-architecture process trees, multi-process RAM contention, long-duration reliability, other GPUs/drivers and large-model throughput. The beta is intentionally limited to the observed portable CUDA path.
