# GreenBoost - routed weight residency

Chris authorised this implementation on November 1, 2026. It is separate from
the parked capacity-policy and Linux-upstream decisions. Existing receipts,
EXPERIMENTAL_RESULTS.json and COMPATIBILITY.md remain unchanged.

## Intended operation

An opt-in llama.cpp CUDA backend reads ordinary GGUF expert weights on demand.
Dense weights and all tensor computation remain on CUDA. The router's actual
selected experts determine residency. A bounded physical VRAM pool and an
explicitly bounded pinned RAM cache replace allocating every expert in VRAM.
Missing data, unsupported operations and insufficient working-set capacity
must produce errors. They must never produce zero weights or CPU computation.

## Implementation and acceptance

- [x] Extend FileTensor with batched spans, consumer-stream fences, bounded
  pinned host caching, and overlapped Windows reads. Preserve acquire().
- [x] Connect GGUF file offsets to CUDA expert tensor storage and resolve
  actual CUDA router selections before the existing matrix kernels run.
- [x] Provide a reproducible opt-in build and launcher. Preserve the stock
  backend and original model files.
- [x] Run original storage integrity tests and focused CUDA residency checks.
- [x] Compare real MoE inference against its resident CUDA baseline, including
  generated output, memory samples and owning-process CUDA kernel activity.
  Windows cache was not flushed; cold storage throughput remains unmeasured.
- [x] Document measured results and remaining limits. Do not claim a speedup
  or larger-model capability without the corresponding new receipt.

Transferable methods: exact demand paging, retaining useful pages, bounded RAM
reuse, batched reads, pinned transfer staging and explicit ordering of consumers.
The Windows implementation uses CUDA events and Win32 I/O. GPU spin-wait
doorbells are unsuitable as a default for this WDDM target. A learned expert
predictor requires model-specific training and independent accuracy evidence;
it cannot replace actual routing or serve as a correctness dependency.

Tests retain the original fixture and binaries. New receipts identify the exact
build, model, commands, CUDA device, process, measured activity and cache policy.

## November 1 bounded result

The source build and shipped launcher ran Granite 3.1 1B A400M Q4_K_M on the
RTX 3060. Resident and paged output matched for both tested prompts. A 32 MiB
expert VRAM pool and 768 MiB pinned cache were used. Paging was slower than
resident execution for this model, which fits in VRAM. The profiler observed
the routing kernel and upstream CUDA matrix kernels in PID 67396 on PCI09:00.0.
Uploads and mapping changes dominate this small-model paged run.

Original storage integrity, compressed allocation and the wrong-seed rejection
were retained. The new scattered-span/external-stream test passed on the 3060
and 5090. No larger-model throughput, RTX2000 execution or production-session
claim follows from these results.

The Logicka regression comparison remains inconclusive. Its storage-role
divergence prediction was superseded because final integrity values should
remain unchanged. Imported summaries lack executed-source lineage; the expected
nonzero negative control also cannot satisfy its completed-run contract.
No independent review was performed. These are recorded limits, not a causal
closure claim.

Evidence: [new receipts](../../windows-port/STREAMING_RESULTS_20261101.json).
Usage: [opt-in backend](../../windows-port/integrations/llama/README.md).
The capacity-policy and Linux-upstream decisions remain parked.
