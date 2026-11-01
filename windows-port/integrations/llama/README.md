# Experimental CUDA expert paging

This backend connects GreenBoost's file residency library to llama.cpp b12019.
It keeps ordinary GGUF files and upstream CUDA inference kernels. The CUDA
router's actual expert IDs determine which weight regions are loaded before
each expert matrix operation. Dense weights, KV cache and computation remain
on the selected GPU.

It is a separate source build. The portable allocation shim, beta 3 package,
Ollama and LM Studio do not acquire this capability by being launched through
greenboost.exe.

## Build and run

Install Visual Studio C++ Build Tools, CMake, Ninja and a CUDA Toolkit. The
November 1 build used MSVC 19.44, CUDA 13.3 and an RTX 3060. Build from the
repository root:

~~~powershell
.\windows-port\tools\build_moe.ps1 -CudaRoot 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3' -Architectures 86
.\windows-port\tools\run_moe.ps1 -LlamaDirectory '.\windows-port\tools\backends\greenboost-moe-b12019\build\bin' -Model 'D:\models\model.gguf' -Gpu 0 -ExpertVramMiB 1024 -ExpertRamMiB 4096
~~~

Use the CUDA ordinal from the backend's device listing. The build helper
downloads pinned b12019 source and applies the included adapter. It preserves
the upstream licence. It also accepts an unmodified b12019 source directory
with `-SourceDirectory`. The stock backend remains separate. Use a fresh
build directory when updating the patch script.

The architecture list can include 75, 86, 89 and 120 for RTX 2000, 3000, 4000
and 5000 respectively, provided the installed Toolkit supports those targets.
Only SM86 execution is qualified by the new receipts. Building another target
does not prove its hardware behaviour.

## Memory budgets

ExpertVramMiB bounds the physical pool used for paged experts. Dense weights,
activations, KV cache and CUDA runtime allocations need additional VRAM.
ExpertRamMiB bounds the pinned read cache; zero disables caching and uses two
2 MiB staging buffers. The cache can retain bytes that are also resident in
VRAM. Windows may independently keep file data in its ordinary file cache.

The unit of residency is a 2 MiB chunk. Requested spans are combined before
eviction, so all chunks needed by an operation must fit the expert pool.
Increase the pool or reduce MicroBatch if that working set is too large.
An oversized working set produces an error.

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

Use the resident backend when the model fits. This opt-in path is for testing
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
