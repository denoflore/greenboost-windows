# GreenBoost 2.5.0 experimental

Version 2.5.0 experimental, October 31, 2026. Developed in the isolated
`experiment/fuck-it-we-ball-20261031` worktree from 2.4.0-beta.3, this code is
now part of main. The latest published binary remains 2.4.0-beta.3. Merging
the source does not update installed applications or qualify the file API
for inference model use.

## Memory allocation

The default `GREENBOOST_ALLOC_POLICY=adaptive` first requests native CUDA device
memory. Only CUDA out-of-memory permits a managed-memory attempt for an eligible
allocation. `GREENBOOST_ALLOC_POLICY=managed` preserves the previous large
allocation policy. `GREENBOOST_EXTRA_MB=0` disables the extension.

The active CUDA context determines managed-memory capability. A failed early
allocation does not permanently disable later valid contexts. Other CUDA
errors propagate to the caller.

The unqualified KMDF path requires explicit `GREENBOOST_LEGACY_DRIVER=1`.
The portable default does not start its prefetch worker.

Run `greenboost-info.exe` to see CUDA ordinals, physical capacity, architecture,
managed-memory limits, virtual-memory support and hardware compression support.
Reported CUDA capacity remains an allowance, not a reservation or an
oversubscription guarantee.

## Inference controls

```powershell
.\install_llama.ps1 -Cuda 13.4
.\run_llama.ps1 -Model C:\models\model.gguf -Gpu 0 -AllocationPolicy adaptive -CacheType q8_0
```

`-Cuda 12.4` chooses the corresponding installed backend. `-CacheType` accepts
`f16`, `q8_0`, or `q4_0`; the default remains f16. Quantized caches can change
model output. `-Batch` and `-MicroBatch` control prefill memory usage.
The supplied profile keeps all model tensors and supported sampling on CUDA.

## Experimental file residency API

Build with Visual Studio C++, Detours, and CUDA SDK headers/import library:

```powershell
.\windows-port\build.ps1 -DetoursRoot C:\deps\Detours -StaticRuntime -Storage -CudaRoot 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3' -Package
```

`greenboost::FileTensor` reserves one CUDA virtual address range for a read-only
file and maps a bounded pool of physical VRAM chunks into it. Two pinned host
buffers stage Windows file reads and CUDA transfers. Repeated resident reads
avoid transfers. Evictions reuse the physical allocation while preserving
the virtual address of each file region.

```cpp
// A CUDA context must already be current on this thread.
greenboost::FileTensor weights(L"C:\\models\\weights.bin", 256ull<<20, 8ull<<20);
CUdeviceptr region = weights.acquire(offset, bytes);
// Submit the consuming CUDA kernel on weights.stream().
// Acquire the next span only after submitting every consumer of this span.
```

The caller must schedule residency with `acquire` before a kernel uses a region.
Every consumer must use the object's stream, original context and owner thread.
A span must fit the resident budget including chunk alignment. File contents
are immutable while open; CUDA mappings become read-only after upload.
An I/O or CUDA failure poisons the object and is reported, never substituted.

The optional fourth constructor argument requests NVIDIA's lossless hardware
memory compression. The library checks the actual allocation properties and
fails if CUDA does not grant it. This does not increase the allocated VRAM
capacity and does not compress the backing file.

This library is a developer component. It is not connected to llama.cpp's
tensor scheduler and cannot transparently page arbitrary CUDA kernels.
File reads currently use the Windows file cache; elapsed file-transfer times
are not direct NVMe bandwidth measurements. No system paging, drivers or
installed applications are reconfigured.

The original CUDA context must outlive the object. Destroy the object before
releasing the context. The API is read-only, single owner and single stream.
It currently uses FIFO slot reuse and synchronous residency acquisition;
transfer staging can overlap file reading, but a cache miss waits for previous
consumers. It does not predict expert selection or compress model weights.

From the storage package, create a new disposable fixture, then read it again:

```powershell
.\test_storage.exe C:\temp\greenboost-fixture.bin 0 0 create 12345
.\test_storage.exe C:\temp\greenboost-fixture.bin 0 1 verify 12345
```

The create command refuses to overwrite an existing file. The fixture contains
256 MiB plus 252 bytes, produced by CUDA. Verification runs on CUDA. The test
uses 32 MiB of resident VRAM and 16 MiB of pinned staging RAM. `1` requests
hardware compression and fails if unsupported. The test requires Turing or
newer because its embedded PTX targets SM 7.5. The library queries capabilities
at runtime; this experiment was built with SDK 13.3.

## Measurements and retained evidence

Windows 11 Pro build 26200, NVIDIA 610.88, one workstation. No driver replacement
or CPU tensor computation was used. Raw process outputs, error outputs, actual
exit receipts and sampled device utilisation are retained in the worktree's
local `.validation` directory. [EXPERIMENTAL_RESULTS.json](EXPERIMENTAL_RESULTS.json)
contains the extracted observations, process identities, UTC times and telemetry
sample counts. Device utilisation was sampled with `nvidia-smi` about every
500 ms during each owned process; it is device-level telemetry, not a profiler's
per-process measurement. CUDA placement is established separately by the
test/backend's device and PCI output.

| CUDA ordinal | GPU | PCI bus | SM | Native / managed 512 MiB | File test, plain / compressed |
| --- | --- | --- | --- | --- | --- |
| 0 | RTX 4090 | 03:00.0 | 8.9 | pass / pass | pass / pass |
| 1 | RTX 5090 | 01:00.0 | 12.0 | pass / pass | pass / pass |
| 2 | RTX 3090 | 05:00.0 | 8.6 | pass / pass | pass / pass |
| 3 | RTX 5060 Ti | 08:00.0 | 12.0 | pass / pass | pass / pass |
| 4 | RTX 3060 | 09:00.0 | 8.6 | pass / pass | pass / pass |

All five reported managed memory, CUDA VMM and generic hardware compression
support, with concurrent managed access and pageable access both zero.
RTX 2000 hardware, older NVIDIA drivers, AMD and Intel were not tested.

### Allocation and compatibility

Forty runtime cases passed: CUDA 11.8, 12.0, 12.4, 12.9, 13.0, 13.1, 13.3 and
13.4, each with native, adaptive, managed, 8 MiB below-threshold and disabled
extension cases on the RTX 3060. Every case used a real runtime DLL in a fresh
process, GPU memset, scalar readback and free. Peak observed device utilisation
was 99% in each five-process batch. This establishes older-runtime compatibility
on driver 610.88; it does not qualify every older driver or every new CUDA API.

The driver tests independently checked both policies on all five cards.
The resolver test passed 34 real NVIDIA lookup outcomes. Capacity accounting
passed on the 3060 and 5090. CreateProcessA/W child tests passed with actual
managed allocation and GPU writes in child PIDs 26396 and 54684. A repeated free
returned NVIDIA's invalid-value error rather than hiding it.

The early-initialization regression first returned CUDA error 3 before cuInit,
then completed a managed allocation and GPU write after a valid context was
created. The old binary retained native allocation in that case. Both old
failure receipts and candidate receipts remain available.

An actual overflow control requested 12,544 MiB on the 12,287 MiB RTX 3060:
native allocation returned CUDA out-of-memory in PID 3020. Adaptive PID 28444
logged native exhaustion, returned a managed allocation, filled it on CUDA,
read back 305419896 and freed it successfully. Observed utilisation reached
100% at PCI 09:00.0. This was a mixed GPU Windows configuration; it does not
promise the same oversubscription behaviour with only one GPU visible.

### Inference performance

All comparisons used the same official llama.cpp b12019 CUDA 13.4 binary and
`Qwen3.5-4B-SFT-Claude-Opus-Reasoning-Unsloth.i1-Q6_K.gguf` (4.21B parameters,
3.22 GiB), all 33 layers and model tensors on the selected GPU, Flash Attention,
one CPU orchestration thread and no host tensor offload. CPU control/I/O remains
ordinary CPU work. These are short isolated benchmarks, not production service
latency measurements.

With all five GPUs visible, CUDA4 selected, 128 prompt tokens, 8 decode tokens
and two repetitions per process:

| Allocation / run | llama PID | Prefill tokens/s | Decode tokens/s | Peak utilisation on PCI 09:00.0 |
| --- | --- | --- | --- | --- |
| Beta 3 managed, first | 49288 | 159.04 | 1.39 | 100% |
| Adaptive, first | 43036 | 2425.42 | 68.67 | 93% |
| Adaptive, second | 38624 | 2410.72 | 69.03 | 93% |
| Beta 3 managed, final clean control | 11000 | 162.21 | 1.44 | 100% |

The model buffer was 3293.12 MiB on CUDA4 in every run. Old allocation logs
showed UVM; candidate logs showed native allocation. The difference is real in
this configuration. NVIDIA documents Windows mapped system-memory placement
when visible GPUs lack compatible peer mappings. That is consistent with the
observations; physical residency itself was not traced here. See
[NVIDIA's Windows multi-GPU rules](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/unified-memory.html).

With only one GPU visible, 512 prompt tokens, 128 decode tokens, three
repetitions per process and managed/adaptive/adaptive/managed run order:

| GPU | Managed decode tokens/s, two run means | Adaptive decode tokens/s, two run means | Peak observed GPU utilisation |
| --- | --- | --- | --- |
| RTX 3060 | 73.61, 73.59 | 73.79, 73.71 | 98% to 100% |
| RTX 4090 | 170.91, 171.65 | 170.86, 169.78 | 96% to 97% |
| RTX 5090 | 287.45, 290.94 | 291.97, 292.67 | 95% to 96% |

These results do not establish a broad speedup for an already resident model.
The 4090's second managed prefill run was noisy and is retained in the JSON.
A setup using an invalid main GPU index failed before computation. A longer
managed benchmark exceeded the harness's 60-second window and has no completed
exit receipt. Another managed repeat overlapped a cache smoke test. All three
are preserved locally and excluded from the performance comparison above.

### KV cache compression

The actual PowerShell 5.1 helper loaded all 33 layers onto CUDA, used backend
greedy sampling and repeated the requested inspection code `IVORY-627`.
At context 32,768 on the RTX 3060:

| Cache | CUDA KV buffer | llama PID | Peak GPU utilisation |
| --- | --- | --- | --- |
| f16 | 1024 MiB | 27480 | 67% |
| q8_0 | 544 MiB | 31576 | 66% |
| q4_0 | 288 MiB | 55128 | 67% |

The q8_0 helper also passed on RTX 5090/CUDA 13.4 (PID 39788) and RTX 3060/CUDA
12.4 (PID 11484). This checks plumbing and a short response, not perplexity,
long-context accuracy or equivalence across models. f16 remains the default.

### File residency and hardware compression

Ten runs passed, one plain and one compressed on each of the five GPUs. Every
run used the same 268,435,708-byte file with a 33,554,432-byte physical pool,
two 8 MiB staging buffers, forward and reverse sweeps, eviction/reload, a final
partial chunk, an unaligned request and rejected out-of-range/over-budget spans.
Every returned region retained its expected virtual address. CUDA compared
every requested element against the generated pattern; all ten had zero
mismatches. The pattern is 75% zeros with seeded remaining values, not model
weights. Compression was verified through actual CUDA handle properties.

| GPU | Plain PID / peak GPU utilisation | Compressed PID / peak GPU utilisation |
| --- | --- | --- |
| RTX 3060 | 28636 / 94% | 15348 / 93% |
| RTX 4090 | 39396 / 77% | 39228 / 76% |
| RTX 5090 | 49976 / 80% | 36276 / 79% |
| RTX 3090 | 2664 / 89% | 44480 / 89% |
| RTX 5060 Ti | 25136 / 81% | 8392 / 84% |

A changed seed produced GPU mismatches and exit 1 (PID 41336). A missing file
produced Windows error 2 and exit 1 (PID 52848). Neither was silently accepted.
The file fits in the Windows cache, so this is residency/data integrity evidence,
not cold SSD throughput. Hardware compression produced no established speedup;
it remains opt-in. No inference model has used this file API yet.

### Build and extracted packages

Both portable and optional storage builds passed with Visual Studio C++ and
static Microsoft runtime. The storage library uses the CUDA driver import
library and SDK 13.3 headers. Both ZIPs were extracted into directories with
spaces. Launcher help, missing-application failure, PowerShell parsing,
installer overwrite refusal and PowerShell 5.1 default backend paths passed.

The extracted portable package passed five CUDA 13.4 cases on the RTX 5090
(PIDs 55180, 17700, 50116, 51716, 11884). Its actual inference helper completed
on that GPU in PID 40280, restored every environment setting it changed and
rejected a microbatch larger than its batch. The extracted storage executable
passed compressed GPU data verification on the RTX 3060 in PID 26760.
The packaged benchmark helper also completed on RTX 5060 Ti with q8_0 key/value
caches, 64 prompt tokens, 32 decode tokens and two repetitions. This verifies
the benchmark argument path; it is not a before/after performance comparison.
These are local package checks recorded before publication. Current portable
build checks are available in
[GitHub Actions](https://github.com/denoflore/greenboost-windows/actions/workflows/windows-build.yml).
No 2.5.0 binary release has been published.

### Causal review boundary

The two allocator regressions were recorded prospectively in Logicka. Their
semantic outcomes passed and predicted allocation changes matched. The first
recording incorrectly treated allocation type, GPU data and exit status as a
single carried value. A successor explicitly invalidated that mechanism claim.
The current report is **semantic PASS, prediction MATCHED, attribution
INCONCLUSIVE, mechanism UNOBSERVED**, scope isolated. Executed-source/pointer
lineage and an independent review are missing. The measured outputs above
remain observations; no formal causal qualification or live deployment is
claimed. Frozen graders and failed baseline receipts were retained unchanged.

## Public research used

Only public techniques informed this branch. No NSCA, svjet or LumenFold code
or design was imported.

- [NVIDIA CUDA VMM](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/virtual-memory-management.html): stable virtual reservations, reusable physical mappings, access permissions and opt-in compressible allocations. The implementation queries both device support and actual handle properties.
- [NVIDIA oversubscription analysis](https://developer.nvidia.com/blog/improving-gpu-memory-oversubscription-performance/): placement and access pattern matter. The allocator comparison tests those consequences on Windows instead of assuming managed memory is free capacity.
- [LLM in a flash](https://arxiv.org/abs/2312.11514): contiguous transfers and reuse of resident data informed the chunked file API. Its neuron prediction and row-column bundling algorithm are not implemented here.
- [FlexGen](https://arxiv.org/abs/2303.06865): explicit scheduling across memory tiers and compression are useful design principles. Its CPU tensor execution is not adopted.
- [KIVI](https://arxiv.org/abs/2402.02750): motivates measuring cache compression with quality limits. This branch exposes upstream q8_0/q4_0 caches; it does not implement KIVI's asymmetric 2-bit method.
- [llama.cpp expert cache PR 29887](https://github.com/ggml-org/llama.cpp/pull/29887) and [disk streaming PR 25294](https://github.com/ggml-org/llama.cpp/pull/25294): reviewed as concrete scheduler precedents. They are not wired into this API; their offload/CPU routing assumptions need separate work under this project's CUDA computation requirement.

Reddit was used to discover and cross-check current community concerns:
[expert caching discussion](https://www.reddit.com/r/LocalLLaMA/comments/1vfhns3/a_llamacpp_pr_caches_hot_moe_experts_on_the_gpu/),
[KV cache discussion](https://www.reddit.com/r/LocalLLaMA/comments/1sz9v9z/qwen_3635ba3b_kv_cache_part_2_ppl_kl_divergence/) and
[SSD inference discussion](https://www.reddit.com/r/LocalLLM/comments/1v7zpq1/running_inference_from_ssd_while_caching_experts/).
Community speed claims are not used as GreenBoost benchmark results.
