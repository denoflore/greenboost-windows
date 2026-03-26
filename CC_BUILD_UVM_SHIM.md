# CC BUILD INSTRUCTIONS: GreenBoost UVM Shim Migration
## Replace cuMemHostRegister with CUDA Unified Virtual Memory for GPU-local bandwidth
## Generated: 2026-03-26 | /fiwb chain
## Repo: denoflore/GreenBoost-Windows | Branch: main

---

## EXECUTION PATTERN

1. Read `.nsca/repo_profile.md` for repo orientation (generate it first if missing)
2. Read a section of this spec, build it, run the smoke test
3. Smoke test MUST PASS before continuing
4. Read this spec AGAIN to find next section
5. After all build phases: execute the Reconciliation Review phase
6. After reconciliation passes: regenerate repo profile

---

## REPO CONSCIOUSNESS BRIEF

### What This Repo IS

GreenBoost is a three-tier GPU memory expansion system (VRAM > DDR4 RAM > NVMe pagefile) that makes AI inference engines believe they have more VRAM than physically exists. It consists of a Linux kernel module (the original, production-quality), a Windows KMDF driver port, a CUDA interception shim DLL, and framework-specific patches (ExLlamaV3). The shim is the critical bridge -- it hooks every CUDA allocation call via Microsoft Detours, intercepts large allocations, and routes them through the driver's pinned DDR4 pages instead of GPU VRAM.

### What Success Looks Like

The shim intercepts `cudaMalloc` for large allocations (default >= 256MB), allocates via CUDA Unified Virtual Memory (`cuMemAllocManaged`), prefetches pages to the GPU, and returns a unified pointer. The NVIDIA driver transparently migrates pages between VRAM and RAM based on access patterns. Inference frameworks (ComfyUI, ExLlama, etc.) see expanded VRAM and run at near-native speed for weights that fit in physical VRAM, with graceful degradation to PCIe bandwidth only for overflow. The driver continues to monitor RAM pressure, the shim continues to spoof VRAM sizes, and the fallback path (driver MDL + cuMemHostRegister) still works for GPUs without UVM support.

### Protected Zones (DO NOT MODIFY OR DELETE)

- `greenboost.c`, `greenboost.h`, `greenboost_ioctl.h` -- Linux kernel module source. Do not touch.
- `greenboost.ko`, `greenboost.o`, `greenboost.mod.*` -- Compiled Linux kernel objects. Do not touch.
- `Makefile`, `Kbuild` -- Linux build system. Do not touch.
- `tools/` -- Linux userspace tools. Do not touch.
- `patches/` -- Framework integration patches (Linux-specific). Do not touch.
- `windows-port/driver/` -- The KMDF driver. This build does NOT modify any driver code.
- `windows-port/tests/test_ioctl.c` -- Existing tests. Do not break. May extend.
- `windows-port/tools/` -- Build/install scripts from PR #4. Do not touch.
- `.nsca/` -- Repo profile. Only regenerate via script.

### Active Build Zone

This build modifies:
- `windows-port/shim/greenboost_cuda_shim_win.h` -- Add new CUDA typedefs and a UVM mode flag
- `windows-port/shim/greenboost_cuda_shim_win.c` -- Core changes: new UVM alloc path, updated free path, symbol resolution, prefetch worker, init/cleanup

This build creates:
- `windows-port/tests/test_uvm.c` -- Standalone UVM allocation test

### How This Build Connects

The shim DLL is loaded into a CUDA process via Detours (`withdll.exe /d:greenboost_cuda.dll app.exe`). It intercepts `cudaMalloc`, `cuMemAlloc_v2`, `cudaFree`, `cuMemFree_v2`, and their async variants, plus memory info queries (`cudaMemGetInfo`, NVML). The driver provides configuration (registry), RAM pressure monitoring (named event), and buffer management (IOCTLs). After this build, the shim's hot path bypasses the driver for memory allocation (using UVM instead) but still uses the driver for configuration, pressure signaling, and VRAM info spoofing context.

---

## REQUIRED READING

Before writing any code, read these files IN ORDER:

1. `windows-port/shim/greenboost_cuda_shim_win.h` -- All typedefs, hash table structure, config struct. Note: `is_managed` field already exists in hash table entries. `pfn_cuMemAllocManaged` typedef already exists.
2. `windows-port/shim/greenboost_cuda_shim_win.c` -- The entire file (1031 lines). Pay close attention to:
   - `gb_alloc_from_driver()` (line ~326) -- THIS IS THE FUNCTION BEING REPLACED
   - `gb_free_buffer()` (line ~413) -- Free path needs branching on `is_managed`
   - `gb_resolve_symbols()` (line ~661) -- Where CUDA symbols are loaded
   - `gb_shim_init()` (line ~906) -- Init validation and UVM capability check goes here
   - `gb_shim_cleanup()` (line ~960) -- Leak cleanup needs UVM awareness
   - `prefetch_worker()` (line ~92) -- Will be extended for CUDA prefetch
3. `windows-port/driver/greenboost_ioctl_win.h` -- IOCTL interface structures (unchanged, but need to understand for fallback path)

---

## CRITICAL ARCHITECTURE DECISIONS

### Decision 1: UVM-first with driver fallback

The shim tries `cuMemAllocManaged` first. If it succeeds, the allocation is tracked as `is_managed=1` with `gb_buf_id=0` (no driver buffer). If UVM fails (old GPU, driver issue, out of address space), fall back to the existing driver IOCTL + cuMemHostRegister path.

**Do NOT** remove the driver allocation path. It must remain functional as a fallback.

### Decision 2: Synchronous prefetch on alloc, async prefetch worker for background

When a UVM allocation succeeds, immediately call `cuMemPrefetchAsync(ptr, size, device, NULL)` with the NULL stream. This is synchronous-ish -- it queues the prefetch and returns, but using the default stream means subsequent CUDA work will wait for it. For weight loading this is correct: weights must be resident before inference starts.

The existing prefetch worker thread is repurposed: instead of `PrefetchVirtualMemory` (RAM-only), it calls `cuMemPrefetchAsync` for UVM allocations. This handles background promotion/demotion when pressure events fire.

### Decision 3: No driver accounting for UVM allocations

UVM allocations bypass the driver entirely. The driver's `PoolAllocated` counter and `ActiveBufs` count will not reflect UVM allocations. This is acceptable because:
- VRAM spoofing uses `ReportedTotalVram` from config, not allocation counters
- GET_INFO accuracy is nice-to-have but not critical
- Adding a new IOCTL for UVM accounting would complicate the driver for no functional benefit

The shim maintains its own `uvm_allocated_bytes` counter for logging.

### Anti-Patterns

- **Do NOT hook `cudaMallocManaged` or `cuMemAllocManaged`.** These are the REAL functions we're calling. Hooking them would cause infinite recursion.
- **Do NOT call `cuMemHostUnregister` on UVM pointers.** UVM pointers were never host-registered. Check `is_managed` before unregistering.
- **Do NOT call `gb_free_driver_buf` for UVM allocations.** There is no driver buffer. Check `is_managed` and `gb_buf_id`.
- **Do NOT remove `real_cuMemHostRegister` from symbol resolution.** It's still needed for the fallback path.
- **Do NOT change any function signatures or IOCTL structures.** This build only changes internal implementation of existing functions.

---

## Phase 1: Header Updates

**Goal:** Add new CUDA API typedefs and UVM state to the header.

**File:** `windows-port/shim/greenboost_cuda_shim_win.h`

### Changes:

1. Add new function pointer typedefs after the existing `pfn_cuMemHostGetDevicePointer` line:

```c
/* UVM prefetch and device query */
typedef CUresult    (*pfn_cuMemPrefetchAsync)(CUdeviceptr, size_t, CUdevice, CUstream);
typedef CUresult    (*pfn_cuCtxGetDevice)(CUdevice *);
```

Note: `pfn_cuMemAllocManaged` (driver API, takes CUdeviceptr*) ALREADY exists in the header -- do NOT add a duplicate. The runtime version `pfn_cudaMallocManaged` (takes void**) also exists. We only need `pfn_cuMemPrefetchAsync` and `pfn_cuCtxGetDevice` which are new. The driver API variable will be `real_cuMemAllocManaged` (not to be confused with `real_cudaMallocManaged` which is the runtime version).

2. Add a UVM mode flag and counter to `gb_shim_config_t`:

```c
    /* UVM state */
    BOOL    UvmAvailable;      /* cuMemAllocManaged resolved and working */
    BOOL    UvmProbed;         /* FALSE until first alloc probes UVM capability */
    CUdevice GpuDevice;        /* GPU device ordinal for prefetch target */
    volatile LONG64 UvmAllocatedBytes;  /* shim-side UVM accounting */
```

Add these after the existing `BOOL Initialized;` field.

### SMOKE TEST:

```
Compile the header standalone:
  cl /c /D_USERMODE /I"windows-port/driver" windows-port/shim/greenboost_cuda_shim_win.h
Must produce zero errors. (If cl not available, visual inspection: no syntax errors, all types resolve.)
```

After completing Phase 1, read this document again.

---

## Phase 2: Symbol Resolution and Init

**Goal:** Resolve new CUDA symbols and detect UVM capability at startup.

**File:** `windows-port/shim/greenboost_cuda_shim_win.c`

### 2a: Add new static function pointers

After the existing `static pfn_cuMemHostGetDevicePointer real_cuMemHostGetDevicePointer = NULL;` line (around line 58), add:

```c
static pfn_cuMemPrefetchAsync     real_cuMemPrefetchAsync    = NULL;
static pfn_cuCtxGetDevice         real_cuCtxGetDevice         = NULL;
static pfn_cuMemAllocManaged  real_cuMemAllocManaged  = NULL;
```

### 2b: Resolve new symbols in `gb_resolve_symbols()`

After the line that resolves `cuMemHostGetDevicePointer_v2` (around line 701), add:

```c
    /* UVM and prefetch symbols */
    real_cuMemAllocManaged = (pfn_cuMemAllocManaged)
        GetProcAddress(hCudaDrv, "cuMemAllocManaged");
    real_cuMemPrefetchAsync = (pfn_cuMemPrefetchAsync)
        GetProcAddress(hCudaDrv, "cuMemPrefetchAsync");
    real_cuCtxGetDevice = (pfn_cuCtxGetDevice)
        GetProcAddress(hCudaDrv, "cuCtxGetDevice");
```

After the log line "symbols resolved: cuMemAlloc=%p cudaMalloc=%p cuMemHostRegister=%p", add:

```c
    gb_log("UVM symbols: cuMemAllocManaged=%p cuMemPrefetchAsync=%p cuCtxGetDevice=%p",
           (void*)real_cuMemAllocManaged, (void*)real_cuMemPrefetchAsync,
           (void*)real_cuCtxGetDevice);
```

### 2c: UVM capability detection -- DEFERRED to first allocation

**CRITICAL: Do NOT call any CUDA functions during gb_shim_init().**

`gb_shim_init()` runs at `DLL_PROCESS_ATTACH` time, BEFORE the application has called `cuInit()` or created a CUDA context. Any CUDA API call (including `cuMemAllocManaged`) will fail with `CUDA_ERROR_NOT_INITIALIZED`. The existing shim correctly avoids CUDA calls at init -- it only resolves function pointers via GetProcAddress.

The UVM probe must be deferred to the first intercepted allocation, when a CUDA context is guaranteed to exist.

**In `gb_shim_init()`, replace the validation block:**
```c
    /* Validate we have the minimum required symbols */
    if (!real_cuMemHostRegister || !real_cuMemHostGetDevicePointer) {
        gb_log_err("critical CUDA symbols not found -- shim disabled");
        gb_config.Initialized = FALSE;
        return;
    }
```

**With:**
```c
    /* Initialize UVM state -- actual probe deferred to first allocation */
    gb_config.UvmAvailable = FALSE;
    gb_config.UvmProbed = FALSE;
    gb_config.GpuDevice = 0;
    gb_config.UvmAllocatedBytes = 0;

    if (real_cuMemAllocManaged && real_cuMemPrefetchAsync) {
        gb_log("UVM symbols resolved -- will probe on first allocation");
    } else {
        gb_log("UVM symbols not found -- will use driver+HostRegister path");
    }

    /* Validate fallback path is available */
    if (!real_cuMemAllocManaged && !real_cuMemHostRegister) {
        gb_log_err("neither UVM nor HostRegister symbols found -- shim disabled");
        gb_config.Initialized = FALSE;
        return;
    }
```

**Add a new `UvmProbed` field to `gb_shim_config_t` in the header** (Phase 1), right after `UvmAvailable`:
```c
    BOOL    UvmProbed;         /* FALSE until first alloc probes UVM */
```

**Add the lazy probe function** as a new static function in the .c file, before `gb_alloc_uvm()`:

```c
/*
 * Probe UVM capability on first intercepted allocation.
 * Called exactly once, when the CUDA context is known to exist.
 * Thread-safe via InterlockedCompareExchange on UvmProbed.
 */
static void gb_probe_uvm_once(void)
{
    /* Atomic check-and-set: only one thread probes */
    if (InterlockedCompareExchange((volatile LONG*)&gb_config.UvmProbed, TRUE, FALSE))
        return;  /* Already probed by another thread */

    if (!real_cuMemAllocManaged || !real_cuMemPrefetchAsync) {
        gb_log("UVM probe: symbols not available");
        return;
    }

    /* Get current GPU device */
    if (real_cuCtxGetDevice) {
        CUresult devRet = real_cuCtxGetDevice(&gb_config.GpuDevice);
        if (devRet != CUDA_SUCCESS) {
            gb_log("cuCtxGetDevice failed (%d), defaulting to device 0", devRet);
            gb_config.GpuDevice = 0;
        }
    }

    /* Probe: try a small UVM allocation */
    {
        CUdeviceptr testPtr = 0;
        CUresult uvmRet = real_cuMemAllocManaged(&testPtr, 4096,
                                                  CU_MEM_ATTACH_GLOBAL);
        if (uvmRet == CUDA_SUCCESS && testPtr) {
            real_cuMemFree_v2(testPtr);
            gb_config.UvmAvailable = TRUE;
            gb_log("UVM probe SUCCESS -- managed memory active (device %d)",
                   gb_config.GpuDevice);
        } else {
            gb_log("UVM probe FAILED (%d) -- using driver+HostRegister fallback",
                   uvmRet);
        }
    }
}
```

### SMOKE TEST:

```
Build the shim DLL. On load (DLL_PROCESS_ATTACH), check stderr:
  "[GreenBoost] UVM symbols resolved -- will probe on first allocation"
  (NO CUDA calls at this point -- just symbol resolution)

Then on first cudaMalloc above threshold:
  "[GreenBoost] UVM probe SUCCESS -- managed memory active (device 0)"
  or
  "[GreenBoost] UVM probe FAILED (...) -- using driver+HostRegister fallback"

The probe must NOT happen at DLL load time. It must happen at first interception.
```

After completing Phase 2, read this document again.

---

## Phase 3: UVM Allocation Path

**Goal:** Replace the core allocation function with UVM-first, driver-fallback logic.

**File:** `windows-port/shim/greenboost_cuda_shim_win.c`

### 3a: New UVM allocation function

Add this NEW function BEFORE `gb_alloc_from_driver()`:

```c
/*
 * Allocate via CUDA Unified Virtual Memory.
 * The NVIDIA driver manages page migration between VRAM and RAM.
 * Returns CUDA_SUCCESS on success, error code on failure.
 */
static CUresult gb_alloc_uvm(void **devPtr, size_t size)
{
    CUdeviceptr dptr = 0;
    CUresult ret;

    ret = real_cuMemAllocManaged(&dptr, size, CU_MEM_ATTACH_GLOBAL);
    if (ret != CUDA_SUCCESS) {
        gb_log("cuMemAllocManaged failed: %d for %zuMB", ret, size >> 20);
        return ret;
    }

    /* Prefetch to GPU immediately -- bring pages into VRAM proactively */
    if (real_cuMemPrefetchAsync) {
        CUresult pfRet = real_cuMemPrefetchAsync(dptr, size,
                                                   gb_config.GpuDevice, NULL);
        if (pfRet != CUDA_SUCCESS) {
            gb_log("cuMemPrefetchAsync hint failed: %d (non-fatal)", pfRet);
            /* Non-fatal: UVM will page-fault on first access instead */
        }
    }

    *devPtr = (void *)(uintptr_t)dptr;

    /* Track in hash table: is_managed=1, buf_id=0, mapped_ptr=NULL */
    if (!ht_insert(dptr, size, 1, 0, NULL)) {
        gb_log_err("hash table full -- freeing UVM allocation");
        real_cuMemFree_v2(dptr);
        return CUDA_ERROR_OUT_OF_MEMORY;
    }

    /* Update shim-side accounting */
    InterlockedAdd64(&gb_config.UvmAllocatedBytes, (LONG64)size);

    gb_log("UVM alloc: %zuMB -> ptr %p (prefetched to device %d)",
           size >> 20, *devPtr, gb_config.GpuDevice);

    return CUDA_SUCCESS;
}
```

### 3b: Modify `hooked_cudaMalloc` to try UVM first

Replace the body of `hooked_cudaMalloc`:

```c
static cudaError_t CUDAAPI hooked_cudaMalloc(void **devPtr, size_t size)
{
    /* Only intercept allocations above threshold */
    if (size >= gb_config.ThresholdBytes && gb_config.Initialized) {
        /* Lazy UVM probe on first interception */
        if (!gb_config.UvmProbed)
            gb_probe_uvm_once();

        /* Try UVM first */
        if (gb_config.UvmAvailable) {
            CUresult ret = gb_alloc_uvm(devPtr, size);
            if (ret == CUDA_SUCCESS)
                return (cudaError_t)CUDA_SUCCESS;
            gb_log("UVM alloc failed, trying driver path");
        }
        /* Fall back to driver + HostRegister */
        {
            CUresult ret = gb_alloc_from_driver(devPtr, size);
            if (ret == CUDA_SUCCESS)
                return (cudaError_t)CUDA_SUCCESS;
        }
        /* Both paths failed, fall through to real CUDA */
        gb_log("GreenBoost alloc failed, falling through to real cudaMalloc");
    }
    return real_cudaMalloc(devPtr, size);
}
```

### 3c: Apply same pattern to `hooked_cuMemAlloc_v2` and async variants

For `hooked_cuMemAlloc_v2`, replace:

```c
static CUresult CUDAAPI hooked_cuMemAlloc_v2(CUdeviceptr *dptr, size_t bytesize)
{
    if (bytesize >= gb_config.ThresholdBytes && gb_config.Initialized) {
        void *ptr = NULL;
        /* Lazy UVM probe on first interception */
        if (!gb_config.UvmProbed)
            gb_probe_uvm_once();

        /* Try UVM first */
        if (gb_config.UvmAvailable) {
            CUresult ret = gb_alloc_uvm(&ptr, bytesize);
            if (ret == CUDA_SUCCESS) {
                *dptr = (CUdeviceptr)(uintptr_t)ptr;
                return CUDA_SUCCESS;
            }
        }
        /* Fall back to driver */
        {
            CUresult ret = gb_alloc_from_driver(&ptr, bytesize);
            if (ret == CUDA_SUCCESS) {
                *dptr = (CUdeviceptr)(uintptr_t)ptr;
                return CUDA_SUCCESS;
            }
        }
        gb_log("GreenBoost alloc failed, falling through to real cuMemAlloc");
    }
    return real_cuMemAlloc_v2(dptr, bytesize);
}
```

For `hooked_cudaMallocAsync` and `hooked_cuMemAllocAsync`: apply the same UVM-first pattern. Each must include the lazy probe check:
```c
        if (!gb_config.UvmProbed)
            gb_probe_uvm_once();
```
before checking `gb_config.UvmAvailable`. The UVM alloc itself is synchronous but the returned pointer works in any stream context.

### SMOKE TEST:

```
Build the shim DLL. Inject into a simple CUDA program that calls:
  cudaMalloc(&ptr, 512 * 1024 * 1024);  // 512MB, above default 256MB threshold
Check stderr output for:
  "[GreenBoost] UVM alloc: 512MB -> ptr 0x... (prefetched to device 0)"
Verify the returned pointer is usable:
  cudaMemset(ptr, 0, 512 * 1024 * 1024);  // must not crash
  cudaFree(ptr);  // must succeed
```

After completing Phase 3, read this document again.

---

## Phase 4: Free Path Fix

**Goal:** Branch the free path based on `is_managed` to handle UVM vs driver allocations correctly.

**File:** `windows-port/shim/greenboost_cuda_shim_win.c`

### 4a: Fix `gb_free_buffer()`

Replace the current `gb_free_buffer()` implementation:

```c
static int gb_free_buffer(CUdeviceptr ptr)
{
    size_t size;
    int is_managed;
    void *mapped_ptr;
    int buf_id;

    if (!ht_remove(ptr, &size, &is_managed, &mapped_ptr, &buf_id))
        return 0;  /* Not our allocation */

    if (is_managed) {
        /* UVM allocation: just free via CUDA */
        gb_log("freeing UVM buffer: ptr=%p size=%zuMB",
               (void *)(uintptr_t)ptr, size >> 20);

        if (real_cuMemFree_v2) {
            CUresult ret = real_cuMemFree_v2(ptr);
            if (ret != CUDA_SUCCESS)
                gb_log_err("cuMemFree failed for UVM ptr %p: %d",
                           (void *)(uintptr_t)ptr, ret);
        }

        /* Update shim-side accounting */
        InterlockedAdd64(&gb_config.UvmAllocatedBytes, -(LONG64)size);
    } else {
        /* Driver allocation: unregister from CUDA, then free via driver */
        gb_log("freeing driver buffer: ptr=%p size=%zuMB buf_id=%d",
               (void *)(uintptr_t)ptr, size >> 20, buf_id);

        if (mapped_ptr && real_cuMemHostUnregister)
            real_cuMemHostUnregister(mapped_ptr);

        gb_free_driver_buf(buf_id);
    }

    return 1;
}
```

### 4b: Fix `gb_shim_cleanup()` leak cleanup

In the `gb_shim_cleanup()` function, replace the loop that frees remaining tracked allocations:

```c
    /* Free any remaining tracked allocations */
    for (i = 0; i < HT_SIZE; i++) {
        gb_ht_entry_t *e = &gb_htable[i];
        if (e->ptr != HT_EMPTY && e->ptr != HT_TOMBSTONE) {
            gb_log("cleanup: freeing leaked buffer ptr=%p size=%zu is_managed=%d buf_id=%d",
                   (void*)(uintptr_t)e->ptr, e->size, e->is_managed, e->gb_buf_id);

            if (e->is_managed) {
                /* UVM: just free */
                if (real_cuMemFree_v2)
                    real_cuMemFree_v2(e->ptr);
            } else {
                /* Driver path: unregister + driver free */
                if (e->mapped_ptr && real_cuMemHostUnregister)
                    real_cuMemHostUnregister(e->mapped_ptr);
                gb_free_driver_buf(e->gb_buf_id);
            }
        }
    }
```

### SMOKE TEST:

```
Inject shim into a CUDA program that:
  1. Allocates 512MB via cudaMalloc (intercepted as UVM)
  2. Calls cudaFree on the returned pointer
Check stderr:
  "[GreenBoost] UVM alloc: 512MB -> ptr 0x..."
  "[GreenBoost] freeing UVM buffer: ptr=0x... size=512MB"
Must NOT see "cuMemHostUnregister" or "gb_free_driver_buf" for UVM allocations.
No crashes. No leaks.
```

After completing Phase 4, read this document again.

---

## Phase 5: Prefetch Worker Enhancement

**Goal:** Extend the prefetch worker to support CUDA device prefetch for UVM allocations, not just Win32 PrefetchVirtualMemory.

**File:** `windows-port/shim/greenboost_cuda_shim_win.c`

### 5a: Extend prefetch_req_t

Change the `prefetch_req_t` struct to include a managed flag:

```c
typedef struct {
    void  *ptr;
    size_t size;
    int    is_managed;    /* 1 = use cuMemPrefetchAsync, 0 = use PrefetchVirtualMemory */
    int    to_device;     /* 1 = prefetch to GPU, 0 = prefetch to CPU (demote) */
} prefetch_req_t;
```

### 5b: Update prefetch_worker loop

Replace the body of the while loop in `prefetch_worker()`:

```c
        /* Dispatch based on allocation type */
        if (req.is_managed && real_cuMemPrefetchAsync) {
            /* CUDA UVM prefetch */
            CUdevice target = req.to_device
                ? gb_config.GpuDevice                 /* promote to GPU VRAM */
                : (CUdevice)-1;                       /* -1 = CPU in CUDA API */
            /* Note: CU_DEVICE_CPU is typically -1 */
            CUresult pfRet = real_cuMemPrefetchAsync(
                (CUdeviceptr)(uintptr_t)req.ptr,
                req.size, target, NULL);
            gb_log("prefetch: cuMemPrefetchAsync %p size=%zu to %s -> %s",
                   req.ptr, req.size,
                   req.to_device ? "GPU" : "CPU",
                   pfRet == CUDA_SUCCESS ? "OK" : "FAIL");
        } else if (pPrefetchVirtualMemory && req.ptr && req.size > 0) {
            /* Win32 RAM prefetch (driver-path allocations) */
            WIN32_MEMORY_RANGE_ENTRY range;
            range.VirtualAddress = req.ptr;
            range.NumberOfBytes = req.size;
            pPrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
            gb_log("prefetch: PrefetchVirtualMemory on %p size=%zu", req.ptr, req.size);
        }
```

### 5c: Update enqueue_prefetch signature

```c
static void enqueue_prefetch(void *ptr, size_t size, int is_managed, int to_device)
```

Update the function body to store the new fields. Update ALL callers:
- In `gb_alloc_from_driver()`: change `enqueue_prefetch(mappedPtr, (size_t)resp.size)` to `enqueue_prefetch(mappedPtr, (size_t)resp.size, 0, 1)`
- In `gb_alloc_uvm()`: the initial prefetch is done inline via `cuMemPrefetchAsync`, so NO enqueue_prefetch call is needed at alloc time. The prefetch worker is for background promotion/demotion.

### 5d: Add pressure-response prefetch (future-ready hook)

Add a new function that can be called when the pressure event fires:

```c
/*
 * Demote UVM allocations to CPU when under memory pressure.
 * Called from pressure monitoring thread (future: pressure event watcher).
 * Walks the hash table and prefetches cold UVM buffers to CPU.
 */
static void gb_demote_cold_uvm(void)
{
    UINT i;
    int demoted = 0;

    for (i = 0; i < HT_SIZE && demoted < 4; i++) {
        gb_ht_entry_t *e = &gb_htable[i];
        if (e->ptr != HT_EMPTY && e->ptr != HT_TOMBSTONE && e->is_managed) {
            enqueue_prefetch((void *)(uintptr_t)e->ptr, e->size, 1, 0);
            demoted++;
        }
    }

    if (demoted > 0)
        gb_log("pressure response: demoting %d UVM buffers to CPU", demoted);
}
```

This function is defined but not wired to the pressure event yet -- that's Phase 2 of the issue (#5). It's here so CC has the interface ready.

### SMOKE TEST:

```
Build successfully. The enqueue_prefetch signature change must not break any callers.
Grep for "enqueue_prefetch" -- every call site must pass 4 arguments.
```

After completing Phase 5, read this document again.

---

## Phase 6: UVM Allocation Test

**Goal:** Create a standalone test program that validates UVM allocation through the shim.

**File:** `windows-port/tests/test_uvm.c` (NEW FILE)

```c
/* SPDX-License-Identifier: GPL-2.0-only
 * GreenBoost v2.3 -- UVM allocation test
 *
 * Tests that the shim correctly routes large allocations through
 * CUDA Unified Virtual Memory and that the returned pointers are
 * usable for GPU compute.
 *
 * Build: cl /O2 test_uvm.c /link cuda.lib
 * Run:   withdll.exe /d:greenboost_cuda.dll test_uvm.exe
 *
 * Expected: all tests PASS when shim is injected with UVM support.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cuda_runtime.h>

#define TEST_SIZE_SMALL  (64 * 1024 * 1024)      /* 64MB -- below threshold */
#define TEST_SIZE_LARGE  (512 * 1024 * 1024)     /* 512MB -- above threshold */
#define TEST_SIZE_MEDIUM (256 * 1024 * 1024)     /* 256MB -- at threshold */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST_START(name) do { \
    tests_run++; \
    printf("Test %d: %s ... ", tests_run, name); \
    fflush(stdout); \
} while(0)

#define TEST_PASS() do { \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)

#define TEST_FAIL(fmt, ...) do { \
    printf("FAIL: " fmt "\n", ##__VA_ARGS__); \
} while(0)

int main(void)
{
    void *ptr = NULL;
    cudaError_t err;

    printf("=== GreenBoost UVM Allocation Tests ===\n\n");

    /* Test 1: Small allocation (should NOT be intercepted) */
    TEST_START("small alloc passthrough");
    err = cudaMalloc(&ptr, TEST_SIZE_SMALL);
    if (err == cudaSuccess && ptr) {
        cudaMemset(ptr, 0, TEST_SIZE_SMALL);
        cudaFree(ptr);
        ptr = NULL;
        TEST_PASS();
    } else {
        TEST_FAIL("cudaMalloc failed: %d", err);
    }

    /* Test 2: Large allocation (should be intercepted, UVM or driver) */
    TEST_START("large alloc interception");
    err = cudaMalloc(&ptr, TEST_SIZE_LARGE);
    if (err == cudaSuccess && ptr) {
        TEST_PASS();
    } else {
        TEST_FAIL("cudaMalloc failed: %d", err);
    }

    /* Test 3: GPU memset on intercepted allocation */
    if (ptr) {
        TEST_START("GPU memset on intercepted buffer");
        err = cudaMemset(ptr, 0xAB, TEST_SIZE_LARGE);
        if (err == cudaSuccess) {
            err = cudaDeviceSynchronize();
            if (err == cudaSuccess)
                TEST_PASS();
            else
                TEST_FAIL("sync failed: %d", err);
        } else {
            TEST_FAIL("memset failed: %d", err);
        }
    }

    /* Test 4: Free intercepted allocation */
    if (ptr) {
        TEST_START("free intercepted buffer");
        err = cudaFree(ptr);
        if (err == cudaSuccess) {
            TEST_PASS();
        } else {
            TEST_FAIL("cudaFree failed: %d", err);
        }
        ptr = NULL;
    }

    /* Test 5: Multiple large allocations */
    TEST_START("multiple large allocs");
    {
        void *ptrs[4] = {0};
        int i, alloc_ok = 1;
        for (i = 0; i < 4; i++) {
            err = cudaMalloc(&ptrs[i], TEST_SIZE_MEDIUM);
            if (err != cudaSuccess) {
                printf("(alloc %d failed: %d) ", i, err);
                alloc_ok = 0;
                break;
            }
        }
        if (alloc_ok) {
            /* Verify all are usable */
            for (i = 0; i < 4; i++) {
                cudaMemset(ptrs[i], (unsigned char)i, TEST_SIZE_MEDIUM);
            }
            cudaDeviceSynchronize();
            TEST_PASS();
        } else {
            TEST_FAIL("could not allocate all 4 buffers");
        }
        /* Cleanup */
        for (i = 0; i < 4; i++) {
            if (ptrs[i]) cudaFree(ptrs[i]);
        }
    }

    /* Test 6: Memory info spoofing still works */
    TEST_START("memory info reports extended VRAM");
    {
        size_t free_mem = 0, total_mem = 0;
        err = cudaMemGetInfo(&free_mem, &total_mem);
        if (err == cudaSuccess) {
            printf("(total=%zuMB free=%zuMB) ", total_mem >> 20, free_mem >> 20);
            /* With shim active, total should be > physical VRAM */
            if (total_mem > (size_t)24 * 1024 * 1024 * 1024ULL) {
                TEST_PASS();  /* More than 24GB means spoofing is active */
            } else {
                TEST_FAIL("total VRAM not spoofed (shim may not be active)");
            }
        } else {
            TEST_FAIL("cudaMemGetInfo failed: %d", err);
        }
    }

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
```

### SMOKE TEST:

```
Compile test_uvm.c. Run with shim injected. All 6 tests pass.
Run WITHOUT shim injected. Tests 1 should pass. Tests 2-5 should pass (native CUDA).
Test 6 should FAIL (no spoofing without shim). This confirms the shim is actually intercepting.
```

After completing Phase 6, read this document again.

---

## Phase 7: Reconciliation Review

Before declaring the build complete, verify what you built:

### Code Consistency
1. Read `.nsca/repo_profile.md` (or generate it)
2. For every file you created or modified:
   - Variable names consistent with naming conventions in related files?
   - Imports resolve to actual modules?
   - Function usage matches actual signatures in existing code?
3. Grep for key variable/function names you introduced:
   - `gb_alloc_uvm` -- no collisions?
   - `UvmAvailable`, `UvmProbed`, `UvmAllocatedBytes`, `GpuDevice` -- no collisions with existing config fields?
   - `gb_demote_cold_uvm` -- no collisions?
   - `real_cuMemPrefetchAsync`, `real_cuCtxGetDevice`, `real_cuMemAllocManaged` -- no collisions?

### Data Pipeline Integrity
4. Trace the UVM alloc path end-to-end:
   - `hooked_cudaMalloc` receives `(void **devPtr, size_t size)`
   - Checks `!gb_config.UvmProbed` -- if first call, runs `gb_probe_uvm_once()`
   - `gb_probe_uvm_once()` atomically sets UvmProbed=TRUE, probes cuMemAllocManaged(4096), sets UvmAvailable
   - If `UvmAvailable`: calls `gb_alloc_uvm(devPtr, size)`
   - `gb_alloc_uvm` calls `real_cuMemAllocManaged(&dptr, size, CU_MEM_ATTACH_GLOBAL)`
   - Returns `CUdeviceptr` via `*devPtr = (void*)(uintptr_t)dptr`
   - Hash table entry: `ptr=dptr, size=size, is_managed=1, gb_buf_id=0, mapped_ptr=NULL`
   - Later: `hooked_cudaFree(devPtr)` -> `gb_free_buffer((CUdeviceptr)devPtr)`
   - `ht_remove` returns `is_managed=1` -> calls `real_cuMemFree_v2(ptr)`
   - Does NOT call `cuMemHostUnregister` or `gb_free_driver_buf`
   - **CRITICAL: The probe MUST happen inside a hook function, never in gb_shim_init(), because no CUDA context exists at DLL_PROCESS_ATTACH time.**
   - **Verify this trace is complete and correct.**

5. Trace the fallback (driver) path -- MUST STILL WORK:
   - `hooked_cudaMalloc` -> UVM fails -> `gb_alloc_from_driver(devPtr, size)`
   - IOCTL_ALLOC -> driver maps pages -> cuMemHostRegister -> cuMemHostGetDevicePointer
   - Hash table: `is_managed=0, gb_buf_id=N, mapped_ptr=userVA`
   - Free: `is_managed=0` -> `cuMemHostUnregister(mapped_ptr)` + `gb_free_driver_buf(buf_id)`
   - **Verify the driver path is unchanged.**

### Blast Radius
6. Files modified: only `greenboost_cuda_shim_win.h` and `greenboost_cuda_shim_win.c`
   - Neither is a dependency hotspot in the profile
   - No other files import from the shim
   - The driver code is NOT modified

7. Run existing test_ioctl tests -- they must still pass (driver path unchanged)

### Verdict
- Issues found: Fix before proceeding, document in commit message
- All clear: Proceed to profile update

---

## Phase 8: Finalize

1. Commit all changes with message:
   ```
   Shim: UVM-first allocation with driver fallback (closes #5 Phase 1)

   - cuMemAllocManaged replaces cuMemHostRegister as primary alloc path
   - cuMemPrefetchAsync stages pages to GPU VRAM on allocation
   - Fallback to driver+HostRegister if UVM unavailable
   - Free path branches on is_managed (UVM: cuMemFree, driver: unregister+IOCTL)
   - Prefetch worker extended for CUDA device prefetch
   - UVM capability probed at init with small test allocation
   - New test_uvm.c validates UVM path end-to-end
   ```

2. Generate and push repo profile:
   ```
   python3 repo_profile_gen.py --owner denoflore --repo GreenBoost-Windows \
     --token $GH_TOKEN --format both --push
   ```

---

## ARCHITECTURAL NOTES

### Why UVM over explicit staging?

UVM is the minimal-code-change path that lets the NVIDIA driver handle page migration. For inference workloads (sequential weight reads, mostly-read access patterns), NVIDIA's UVM heuristics are well-optimized. The driver pre-fetches pages the GPU is about to access and evicts cold pages back to RAM. This matches the GreenBoost use case almost exactly.

Explicit staging (Option B from issue #5) gives more control but requires model-level knowledge of which layers are hot. That's framework-specific and belongs in the ExLlama/ComfyUI integration patches, not the generic shim.

### Why keep the driver?

Even with UVM handling allocation, the driver provides:
1. **Registry configuration** -- threshold, pool sizes, debug mode
2. **Pressure monitoring** -- watchdog thread tracks RAM and pagefile pressure
3. **Named event signaling** -- shim can watch for pressure and demote UVM pages
4. **VRAM size context** -- physical vs virtual VRAM for spoofing calculations
5. **PIN_USER_PTR** -- external buffer pinning can't use UVM

### Expected performance impact

For weights that fit in physical VRAM: near-native speed. NVIDIA's UVM driver will migrate hot pages to VRAM and keep them there. First-access latency is ~5-15us per 64KB page (page fault + migration), but after initial load, subsequent reads hit VRAM directly.

For weights that overflow VRAM: the NVIDIA driver manages the eviction policy. Hot layers stay in VRAM, cold layers spill to RAM. This is the same PCIe bandwidth as before, but only for overflow -- not for everything.

Worst case (VRAM thrashing): if the working set oscillates between two sets of pages both larger than VRAM, UVM will thrash. This is unlikely for inference (sequential layer access) but possible for training. GreenBoost targets inference.

### CU_DEVICE_CPU constant

CUDA uses -1 (or `CU_DEVICE_CPU` which equals -1) as the device ordinal for "prefetch to CPU". This is how `gb_demote_cold_uvm` tells the NVIDIA driver to move pages back to system RAM. The `(CUdevice)-1` cast is intentional.
