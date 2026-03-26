# CC BUILD INSTRUCTIONS: GreenBoost UVM Allocation Path
## Wire UvmAvailable into the actual allocation/free/prefetch paths
## Generated: 2026-03-26 | /fiwb Phase 2
## Repo: denoflore/GreenBoost-Windows | Branch: main
## Predecessor: Merge #6 (deferred UVM probe) -- UvmProbed/UvmAvailable flags exist but are unused

---

## EXECUTION PATTERN

1. Read `.nsca/repo_profile.md` for repo orientation
2. Read a section of this spec, build it, run the smoke test
3. Smoke test MUST PASS before continuing
4. Read this spec AGAIN to find next section
5. After all build phases: execute the Reconciliation Review phase
6. After reconciliation passes: regenerate repo profile

---

## REPO CONSCIOUSNESS BRIEF

### What This Repo IS

GreenBoost is a three-tier GPU memory expansion system. A KMDF kernel driver manages DDR4 RAM pages. A CUDA shim DLL (Detours-based) intercepts `cudaMalloc` calls and routes large allocations through the driver or UVM. After Merge #6, the shim probes UVM capability on first interception and sets `gb_config.UvmAvailable`, but then ignores it -- every allocation still goes through the slow driver+cuMemHostRegister path (PCIe-bound, ~31x bandwidth penalty vs VRAM).

This build wires UvmAvailable into the actual allocation, free, and prefetch paths.

### Protected Zones (DO NOT MODIFY OR DELETE)

- Everything outside `windows-port/shim/` and `windows-port/tests/` -- do not touch
- `windows-port/driver/` -- no driver changes
- `gb_alloc_from_driver()` -- do NOT modify or delete this function. It is the fallback path.
- `gb_probe_uvm_once()` -- do NOT modify. Already correct from Merge #6.
- `gb_shim_init()` / `gb_shim_cleanup()` init flow -- do NOT add CUDA API calls to init. The cleanup loop WILL be modified.

### Active Build Zone

Modifies:
- `windows-port/shim/greenboost_cuda_shim_win.h` -- add typedefs for cuMemPrefetchAsync, cuCtxGetDevice; add GpuDevice and UvmAllocatedBytes to config
- `windows-port/shim/greenboost_cuda_shim_win.c` -- add gb_alloc_uvm(), modify hooked_cudaMalloc/cuMemAlloc_v2/async variants, fix gb_free_buffer(), fix cleanup loop, extend prefetch worker, resolve new symbols

Creates:
- `windows-port/tests/test_uvm.c` -- standalone UVM validation test

### How This Build Connects

The shim hooks CUDA alloc/free calls via Detours. After this build: large allocs go through `gb_alloc_uvm()` (UVM + prefetch) when available, falling back to `gb_alloc_from_driver()` (driver IOCTL + cuMemHostRegister) when not. Free path branches on hash table `is_managed` field. Prefetch worker supports both CUDA device prefetch and Win32 RAM prefetch.

---

## REQUIRED READING

Read these files BEFORE writing any code:

1. `windows-port/shim/greenboost_cuda_shim_win.h` -- 169 lines. Note existing: `pfn_cuMemAllocManaged` typedef (line ~73), `is_managed` field in `gb_ht_entry_t` (line ~108), `UvmProbed`/`UvmAvailable` in config (lines 141-142).
2. `windows-port/shim/greenboost_cuda_shim_win.c` -- 1099 lines. Note existing:
   - `real_cuMemAllocManaged_drv` (line 53) -- resolved, used by probe only
   - `gb_alloc_from_driver()` (line 327) -- THE FALLBACK, do not modify
   - `gb_free_buffer()` (line 414) -- MUST be modified (currently ignores `is_managed`)
   - `gb_probe_uvm_once()` (line 450) -- DO NOT modify
   - `hooked_cudaMalloc` (line 498) -- probe exists, needs UVM alloc path added
   - `hooked_cuMemAlloc_v2` (line 533) -- same
   - `hooked_cudaMallocAsync` (line 522) -- needs probe + UVM path
   - `hooked_cuMemAllocAsync` (line 557) -- needs probe + UVM path
   - `prefetch_req_t` (line 77) -- only has ptr+size, needs is_managed+to_device
   - `enqueue_prefetch` (line 124) -- only takes ptr+size, needs extending
   - Cleanup loop (line 1046) -- unconditionally calls cuMemHostUnregister, needs is_managed branch
3. `windows-port/driver/greenboost_ioctl_win.h` -- unchanged, read for IOCTL structure context

---

## CRITICAL ARCHITECTURE DECISIONS

### Decision: UVM-first, driver-fallback, in every hook

Every allocation hook follows this pattern:
```
if (above_threshold && initialized) {
    if (!UvmProbed) gb_probe_uvm_once();
    if (UvmAvailable) { try gb_alloc_uvm(); if success return; }
    try gb_alloc_from_driver(); if success return;
    log("both paths failed, falling through");
}
return real_cuda_function(...);
```

### Anti-Patterns -- READ THESE CAREFULLY

- **Do NOT hook `cuMemAllocManaged`.** It is the REAL function `gb_alloc_uvm` calls. Hooking it = infinite recursion = stack overflow = crash.
- **Do NOT call `cuMemHostUnregister` on UVM pointers.** UVM pointers were never host-registered. Check `is_managed` FIRST.
- **Do NOT call `gb_free_driver_buf` for UVM allocations.** There is no driver buffer (buf_id=0). Check `is_managed` FIRST.
- **Do NOT modify `gb_alloc_from_driver()`.** It is the working fallback and must remain unchanged.
- **Do NOT modify `gb_probe_uvm_once()`.** It is correct as merged.
- **Do NOT add CUDA API calls to `gb_shim_init()`.** No CUDA context exists at DLL_PROCESS_ATTACH.

---

## Phase 1: Header Additions

**Goal:** Add missing typedefs and config fields.

**File:** `windows-port/shim/greenboost_cuda_shim_win.h`

### 1a: Add new function pointer typedefs

After the existing `pfn_cuMemHostGetDevicePointer` typedef (around line 84), add:

```c
/* UVM prefetch and device query */
typedef CUresult    (*pfn_cuMemPrefetchAsync)(CUdeviceptr, size_t, CUdevice, CUstream);
typedef CUresult    (*pfn_cuCtxGetDevice)(CUdevice *);
```

### 1b: Add config fields

After `UvmAvailable` (line 142), add:

```c
    CUdevice GpuDevice;                /* GPU ordinal for prefetch target  */
    volatile LONG64 UvmAllocatedBytes; /* shim-side UVM accounting         */
```

### SMOKE TEST:
```
Header compiles cleanly with no new warnings. All existing types resolve.
```

After completing Phase 1, read this document again.

---

## Phase 2: Symbol Resolution

**Goal:** Resolve cuMemPrefetchAsync and cuCtxGetDevice from nvcuda.dll.

**File:** `windows-port/shim/greenboost_cuda_shim_win.c`

### 2a: Add static function pointers

After `real_cuMemAllocManaged_drv` (line 53), add:

```c
static pfn_cuMemPrefetchAsync     real_cuMemPrefetchAsync    = NULL;
static pfn_cuCtxGetDevice         real_cuCtxGetDevice         = NULL;
```

### 2b: Resolve in `gb_resolve_symbols()`

After the line `real_cuMemAllocManaged_drv = ... GetProcAddress(hCudaDrv, "cuMemAllocManaged");` (around line 761-762), add:

```c
    real_cuMemPrefetchAsync = (pfn_cuMemPrefetchAsync)
        GetProcAddress(hCudaDrv, "cuMemPrefetchAsync");
    real_cuCtxGetDevice = (pfn_cuCtxGetDevice)
        GetProcAddress(hCudaDrv, "cuCtxGetDevice");
```

### 2c: Update the log line

The log line at ~804 currently shows cuMemAllocManaged. Extend it:

```c
    gb_log("symbols resolved: cuMemAlloc=%p cudaMalloc=%p cuMemHostRegister=%p "
           "cuMemAllocManaged=%p cuMemPrefetchAsync=%p",
           (void*)real_cuMemAlloc_v2, (void*)real_cudaMalloc,
           (void*)real_cuMemHostRegister, (void*)real_cuMemAllocManaged_drv,
           (void*)real_cuMemPrefetchAsync);
```

### 2d: Initialize new config fields

In `gb_shim_init()`, right after the existing `gb_config.UvmAvailable = FALSE;` init (which is inside the struct zero-init or wherever CC put it -- if it's not explicit, add it near the existing UvmProbed init):

```c
    gb_config.GpuDevice = 0;
    gb_config.UvmAllocatedBytes = 0;
```

### SMOKE TEST:
```
Build. On load, log shows cuMemPrefetchAsync address (non-NULL on CUDA 8.0+).
```

After completing Phase 2, read this document again.

---

## Phase 3: UVM Allocation Function

**Goal:** Create `gb_alloc_uvm()` that allocates via UVM and prefetches to GPU.

**File:** `windows-port/shim/greenboost_cuda_shim_win.c`

### Add this function BETWEEN `gb_probe_uvm_once()` and `hooked_cudaMalloc`

Insert at line ~492 (after the closing brace of `gb_probe_uvm_once`, before the CUDA hook implementations comment block):

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

    /* Get GPU device ordinal if not yet known */
    if (gb_config.GpuDevice == 0 && real_cuCtxGetDevice) {
        CUdevice dev = 0;
        if (real_cuCtxGetDevice(&dev) == CUDA_SUCCESS)
            gb_config.GpuDevice = dev;
    }

    ret = real_cuMemAllocManaged_drv(&dptr, size, CU_MEM_ATTACH_GLOBAL);
    if (ret != CUDA_SUCCESS) {
        gb_log("cuMemAllocManaged failed: %d for %zuMB", ret, size >> 20);
        return ret;
    }

    /* Prefetch to GPU -- bring pages into VRAM proactively.
     * Uses NULL stream (default): subsequent CUDA work on this stream
     * will wait for prefetch to complete. Correct for weight loading. */
    if (real_cuMemPrefetchAsync) {
        CUresult pfRet = real_cuMemPrefetchAsync(dptr, size,
                                                  gb_config.GpuDevice, NULL);
        if (pfRet != CUDA_SUCCESS)
            gb_log("cuMemPrefetchAsync hint failed: %d (non-fatal)", pfRet);
    }

    *devPtr = (void *)(uintptr_t)dptr;

    /* Track: is_managed=1, buf_id=0, mapped_ptr=NULL (no driver buffer) */
    if (!ht_insert(dptr, size, 1, 0, NULL)) {
        gb_log_err("hash table full -- freeing UVM allocation");
        real_cuMemFree_v2(dptr);
        return CUDA_ERROR_OUT_OF_MEMORY;
    }

    InterlockedAdd64(&gb_config.UvmAllocatedBytes, (LONG64)size);

    gb_log("UVM alloc: %zuMB -> ptr %p (prefetched to device %d)",
           size >> 20, *devPtr, gb_config.GpuDevice);

    return CUDA_SUCCESS;
}
```

### SMOKE TEST:
```
Compiles. Function signature matches how it will be called in Phase 4.
gb_alloc_uvm takes (void**, size_t), returns CUresult. Consistent with gb_alloc_from_driver.
```

After completing Phase 3, read this document again.

---

## Phase 4: Wire UVM Into Allocation Hooks

**Goal:** Modify all four allocation hooks to try UVM before driver fallback.

**File:** `windows-port/shim/greenboost_cuda_shim_win.c`

### 4a: Replace `hooked_cudaMalloc` body

Current code (lines ~498-513) calls `gb_alloc_from_driver` only. Replace the ENTIRE function body:

```c
static cudaError_t CUDAAPI hooked_cudaMalloc(void **devPtr, size_t size)
{
    /* Deferred UVM probe -- runs once on first interception */
    if (!gb_config.UvmProbed)
        gb_probe_uvm_once();

    /* Only intercept allocations above threshold */
    if (size >= gb_config.ThresholdBytes && gb_config.Initialized) {
        /* Try UVM first */
        if (gb_config.UvmAvailable) {
            CUresult ret = gb_alloc_uvm(devPtr, size);
            if (ret == CUDA_SUCCESS)
                return (cudaError_t)CUDA_SUCCESS;
            gb_log("UVM alloc failed (%d), trying driver path", ret);
        }
        /* Fall back to driver + HostRegister */
        {
            CUresult ret = gb_alloc_from_driver(devPtr, size);
            if (ret == CUDA_SUCCESS)
                return (cudaError_t)CUDA_SUCCESS;
        }
        gb_log("GreenBoost alloc failed, falling through to real cudaMalloc");
    }
    return real_cudaMalloc(devPtr, size);
}
```

### 4b: Replace `hooked_cudaMallocAsync` body

Current code (lines ~522-531). Replace entire function:

```c
static cudaError_t CUDAAPI hooked_cudaMallocAsync(
    void **devPtr, size_t size, cudaStream_t stream)
{
    if (!gb_config.UvmProbed)
        gb_probe_uvm_once();

    if (size >= gb_config.ThresholdBytes && gb_config.Initialized) {
        if (gb_config.UvmAvailable) {
            CUresult ret = gb_alloc_uvm(devPtr, size);
            if (ret == CUDA_SUCCESS)
                return (cudaError_t)CUDA_SUCCESS;
        }
        {
            CUresult ret = gb_alloc_from_driver(devPtr, size);
            if (ret == CUDA_SUCCESS)
                return (cudaError_t)CUDA_SUCCESS;
        }
    }
    return real_cudaMallocAsync(devPtr, size, stream);
}
```

### 4c: Replace `hooked_cuMemAlloc_v2` body

Current code (lines ~533-548). Replace entire function:

```c
static CUresult CUDAAPI hooked_cuMemAlloc_v2(CUdeviceptr *dptr, size_t bytesize)
{
    if (!gb_config.UvmProbed)
        gb_probe_uvm_once();

    if (bytesize >= gb_config.ThresholdBytes && gb_config.Initialized) {
        void *ptr = NULL;
        if (gb_config.UvmAvailable) {
            CUresult ret = gb_alloc_uvm(&ptr, bytesize);
            if (ret == CUDA_SUCCESS) {
                *dptr = (CUdeviceptr)(uintptr_t)ptr;
                return CUDA_SUCCESS;
            }
        }
        {
            CUresult ret = gb_alloc_from_driver(&ptr, bytesize);
            if (ret == CUDA_SUCCESS) {
                *dptr = (CUdeviceptr)(uintptr_t)ptr;
                return CUDA_SUCCESS;
            }
        }
    }
    return real_cuMemAlloc_v2(dptr, bytesize);
}
```

### 4d: Replace `hooked_cuMemAllocAsync` body

Apply same pattern:

```c
static CUresult CUDAAPI hooked_cuMemAllocAsync(
    CUdeviceptr *dptr, size_t bytesize, CUstream hStream)
{
    if (!gb_config.UvmProbed)
        gb_probe_uvm_once();

    if (bytesize >= gb_config.ThresholdBytes && gb_config.Initialized) {
        void *ptr = NULL;
        if (gb_config.UvmAvailable) {
            CUresult ret = gb_alloc_uvm(&ptr, bytesize);
            if (ret == CUDA_SUCCESS) {
                *dptr = (CUdeviceptr)(uintptr_t)ptr;
                return CUDA_SUCCESS;
            }
        }
        {
            CUresult ret = gb_alloc_from_driver(&ptr, bytesize);
            if (ret == CUDA_SUCCESS) {
                *dptr = (CUdeviceptr)(uintptr_t)ptr;
                return CUDA_SUCCESS;
            }
        }
    }
    return real_cuMemAllocAsync(dptr, bytesize, hStream);
}
```

### SMOKE TEST:
```
Build the DLL. Inject into a CUDA program that does:
  cudaMalloc(&ptr, 512 * 1024 * 1024);  // 512MB, above 256MB threshold
Check stderr for EITHER:
  "[GreenBoost] UVM alloc: 512MB -> ptr 0x..." (UVM path)
  OR
  "[GreenBoost] driver mapped 512MB at ..." (driver fallback)
Both are correct. The key test: UVM path is ATTEMPTED first when UvmAvailable is TRUE.
```

After completing Phase 4, read this document again.

---

## Phase 5: Fix Free Path

**Goal:** Branch free logic on `is_managed` to correctly handle UVM vs driver allocations.

**File:** `windows-port/shim/greenboost_cuda_shim_win.c`

### 5a: Replace `gb_free_buffer()` (lines ~414-434)

Replace the ENTIRE function:

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
        /* UVM allocation: free via CUDA directly */
        gb_log("freeing UVM buffer: ptr=%p size=%zuMB",
               (void *)(uintptr_t)ptr, size >> 20);

        if (real_cuMemFree_v2) {
            CUresult ret = real_cuMemFree_v2(ptr);
            if (ret != CUDA_SUCCESS)
                gb_log_err("cuMemFree failed for UVM ptr %p: %d",
                           (void *)(uintptr_t)ptr, ret);
        }

        InterlockedAdd64(&gb_config.UvmAllocatedBytes, -(LONG64)size);
    } else {
        /* Driver allocation: unregister from CUDA, then free via IOCTL */
        gb_log("freeing driver buffer: ptr=%p size=%zuMB buf_id=%d",
               (void *)(uintptr_t)ptr, size >> 20, buf_id);

        if (mapped_ptr && real_cuMemHostUnregister)
            real_cuMemHostUnregister(mapped_ptr);

        gb_free_driver_buf(buf_id);
    }

    return 1;
}
```

### 5b: Fix cleanup loop in `gb_shim_cleanup()` (lines ~1046-1057)

Replace the cleanup for-loop:

```c
    /* Free any remaining tracked allocations */
    for (i = 0; i < HT_SIZE; i++) {
        gb_ht_entry_t *e = &gb_htable[i];
        if (e->ptr != HT_EMPTY && e->ptr != HT_TOMBSTONE) {
            gb_log("cleanup: freeing leaked buffer ptr=%p size=%zu is_managed=%d buf_id=%d",
                   (void*)(uintptr_t)e->ptr, e->size, e->is_managed, e->gb_buf_id);

            if (e->is_managed) {
                /* UVM: free directly */
                if (real_cuMemFree_v2)
                    real_cuMemFree_v2(e->ptr);
            } else {
                /* Driver: unregister + IOCTL free */
                if (e->mapped_ptr && real_cuMemHostUnregister)
                    real_cuMemHostUnregister(e->mapped_ptr);
                gb_free_driver_buf(e->gb_buf_id);
            }
        }
    }
```

### SMOKE TEST:
```
Build. Inject into CUDA program:
  cudaMalloc(&ptr, 512MB);   // intercepted
  cudaFree(ptr);             // must free correctly

Check stderr:
  If UVM: "freeing UVM buffer: ptr=... size=512MB"
    Must NOT see "cuMemHostUnregister" in the log for this free.
    Must NOT see "gb_free_driver_buf" for this free.
  If driver: "freeing driver buffer: ptr=... size=512MB buf_id=N"
    MUST see cuMemHostUnregister happening.

No crashes. No leaks.
```

After completing Phase 5, read this document again.

---

## Phase 6: Extend Prefetch Worker

**Goal:** Support CUDA device prefetch (UVM) alongside Win32 RAM prefetch (driver path).

**File:** `windows-port/shim/greenboost_cuda_shim_win.c`

### 6a: Extend `prefetch_req_t` struct (line ~77)

Replace:
```c
typedef struct {
    void  *ptr;
    size_t size;
} prefetch_req_t;
```

With:
```c
typedef struct {
    void  *ptr;
    size_t size;
    int    is_managed;    /* 1 = use cuMemPrefetchAsync, 0 = PrefetchVirtualMemory */
    int    to_device;     /* 1 = prefetch to GPU, 0 = demote to CPU */
} prefetch_req_t;
```

### 6b: Update `prefetch_worker` loop body

Replace the body inside the while loop (after dequeueing `req`) with:

```c
        if (req.is_managed && real_cuMemPrefetchAsync) {
            /* CUDA UVM prefetch to GPU or CPU */
            CUdevice target = req.to_device
                ? gb_config.GpuDevice
                : (CUdevice)-1;            /* CU_DEVICE_CPU = -1 */
            CUresult pfRet = real_cuMemPrefetchAsync(
                (CUdeviceptr)(uintptr_t)req.ptr,
                req.size, target, NULL);
            gb_log("prefetch: cuMemPrefetchAsync %p size=%zu to %s -> %s",
                   req.ptr, req.size,
                   req.to_device ? "GPU" : "CPU",
                   pfRet == CUDA_SUCCESS ? "OK" : "FAIL");
        } else if (pPrefetchVirtualMemory && req.ptr && req.size > 0) {
            /* Win32 RAM prefetch for driver-path allocations */
            WIN32_MEMORY_RANGE_ENTRY range;
            range.VirtualAddress = req.ptr;
            range.NumberOfBytes = req.size;
            pPrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
            gb_log("prefetch: PrefetchVirtualMemory on %p size=%zu",
                   req.ptr, req.size);
        }
```

### 6c: Update `enqueue_prefetch` signature and body (line ~124)

Replace:
```c
static void enqueue_prefetch(void *ptr, size_t size)
{
    if (!ptr || !prefetch_running)
        return;

    EnterCriticalSection(&prefetch_cs);
    int next_head = (prefetch_head + 1) % PREFETCH_QUEUE_SIZE;
    if (next_head != prefetch_tail) {
        prefetch_queue[prefetch_head].ptr = ptr;
        prefetch_queue[prefetch_head].size = size;
        prefetch_head = next_head;
        WakeConditionVariable(&prefetch_cv);
    }
    LeaveCriticalSection(&prefetch_cs);
}
```

With:
```c
static void enqueue_prefetch(void *ptr, size_t size, int is_managed, int to_device)
{
    if (!ptr || !prefetch_running)
        return;

    EnterCriticalSection(&prefetch_cs);
    int next_head = (prefetch_head + 1) % PREFETCH_QUEUE_SIZE;
    if (next_head != prefetch_tail) {
        prefetch_queue[prefetch_head].ptr = ptr;
        prefetch_queue[prefetch_head].size = size;
        prefetch_queue[prefetch_head].is_managed = is_managed;
        prefetch_queue[prefetch_head].to_device = to_device;
        prefetch_head = next_head;
        WakeConditionVariable(&prefetch_cv);
    }
    LeaveCriticalSection(&prefetch_cs);
}
```

### 6d: Update the ONE existing caller of `enqueue_prefetch`

In `gb_alloc_from_driver()` (around line 401-402), the call:
```c
    enqueue_prefetch(mappedPtr, (size_t)resp.size);
```
becomes:
```c
    enqueue_prefetch(mappedPtr, (size_t)resp.size, 0, 1);
```
(0 = not managed, 1 = to device -- this is the driver path, uses PrefetchVirtualMemory to bring pages into RAM)

### SMOKE TEST:
```
Build. Grep for "enqueue_prefetch" -- every call site must pass 4 arguments.
There should be exactly ONE call: in gb_alloc_from_driver.
(gb_alloc_uvm does its own inline cuMemPrefetchAsync, no enqueue needed at alloc time.)
```

After completing Phase 6, read this document again.

---

## Phase 7: Test Program

**Goal:** Standalone test validating UVM allocation through the shim.

**File:** `windows-port/tests/test_uvm.c` (NEW FILE)

```c
/* SPDX-License-Identifier: GPL-2.0-only
 * GreenBoost v2.3 -- UVM allocation test
 *
 * Build: cl /O2 test_uvm.c /link cuda.lib
 * Run:   withdll.exe /d:greenboost_cuda.dll test_uvm.exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <cuda_runtime.h>

#define TEST_SMALL   (64  * 1024 * 1024)
#define TEST_LARGE   (512 * 1024 * 1024)
#define TEST_MEDIUM  (256 * 1024 * 1024)

static int n_run = 0, n_pass = 0;

#define RUN(name)  do { n_run++; printf("Test %d: %s ... ", n_run, name); fflush(stdout); } while(0)
#define PASS()     do { n_pass++; printf("PASS\n"); } while(0)
#define FAIL(f,...)do { printf("FAIL: " f "\n", ##__VA_ARGS__); } while(0)

int main(void)
{
    void *ptr = NULL;
    cudaError_t err;

    printf("=== GreenBoost UVM Tests ===\n\n");

    /* 1: Small alloc -- below threshold, passthrough */
    RUN("small alloc passthrough");
    err = cudaMalloc(&ptr, TEST_SMALL);
    if (err == cudaSuccess && ptr) { cudaFree(ptr); ptr = NULL; PASS(); }
    else FAIL("err=%d", err);

    /* 2: Large alloc -- above threshold, intercepted */
    RUN("large alloc interception");
    err = cudaMalloc(&ptr, TEST_LARGE);
    if (err == cudaSuccess && ptr) PASS();
    else FAIL("err=%d", err);

    /* 3: GPU memset on intercepted buffer */
    if (ptr) {
        RUN("GPU memset on intercepted buffer");
        err = cudaMemset(ptr, 0xAB, TEST_LARGE);
        if (err == cudaSuccess) {
            err = cudaDeviceSynchronize();
            if (err == cudaSuccess) PASS();
            else FAIL("sync err=%d", err);
        } else FAIL("memset err=%d", err);
    }

    /* 4: Free intercepted buffer */
    if (ptr) {
        RUN("free intercepted buffer");
        err = cudaFree(ptr);
        if (err == cudaSuccess) PASS();
        else FAIL("err=%d", err);
        ptr = NULL;
    }

    /* 5: Multiple large allocs */
    RUN("4x 256MB allocs");
    {
        void *p[4] = {0};
        int i, ok = 1;
        for (i = 0; i < 4; i++) {
            if (cudaMalloc(&p[i], TEST_MEDIUM) != cudaSuccess) { ok = 0; break; }
        }
        if (ok) {
            for (i = 0; i < 4; i++) cudaMemset(p[i], (unsigned char)i, TEST_MEDIUM);
            cudaDeviceSynchronize();
            PASS();
        } else FAIL("alloc %d failed", i);
        for (i = 0; i < 4; i++) if (p[i]) cudaFree(p[i]);
    }

    /* 6: VRAM spoofing active */
    RUN("VRAM spoofing reports extended memory");
    {
        size_t fr = 0, tot = 0;
        err = cudaMemGetInfo(&fr, &tot);
        if (err == cudaSuccess) {
            printf("(total=%zuMB free=%zuMB) ", tot >> 20, fr >> 20);
            if (tot > (size_t)24ULL * 1024 * 1024 * 1024) PASS();
            else FAIL("total not spoofed -- shim may not be active");
        } else FAIL("err=%d", err);
    }

    printf("\n=== %d/%d passed ===\n", n_pass, n_run);
    return (n_pass == n_run) ? 0 : 1;
}
```

### SMOKE TEST:
```
Compiles with CUDA toolkit. All 6 tests pass with shim injected.
Test 6 fails WITHOUT shim (confirming shim is active).
```

After completing Phase 7, read this document again.

---

## Phase 8: Reconciliation Review

Before declaring complete, verify:

### Code Consistency
1. Every call to `enqueue_prefetch` passes 4 arguments (grep to confirm)
2. `gb_alloc_uvm` uses `real_cuMemAllocManaged_drv` (NOT `real_cudaMallocManaged` which is runtime API)
3. `gb_free_buffer` checks `is_managed` BEFORE calling any cleanup function
4. Cleanup loop in `gb_shim_cleanup` checks `is_managed` BEFORE calling any cleanup function
5. No CUDA API calls exist in `gb_shim_init()`
6. `gb_alloc_from_driver()` is UNCHANGED from the version that was there before this build
7. `gb_probe_uvm_once()` is UNCHANGED from the version that was there before this build

### Data Pipeline Trace
8. UVM alloc path: `hooked_cudaMalloc` -> probe check -> `gb_alloc_uvm` -> `real_cuMemAllocManaged_drv` -> `cuMemPrefetchAsync` -> ht_insert(is_managed=1, buf_id=0, mapped_ptr=NULL) -> return
9. UVM free path: `hooked_cudaFree` -> `gb_free_buffer` -> ht_remove -> is_managed=1 -> `real_cuMemFree_v2` -> InterlockedAdd64 accounting -> return
10. Driver alloc path: UNCHANGED. `hooked_cudaMalloc` -> UVM unavailable or failed -> `gb_alloc_from_driver` -> IOCTL -> cuMemHostRegister -> ht_insert(is_managed=0) -> return
11. Driver free path: UNCHANGED. `gb_free_buffer` -> is_managed=0 -> `cuMemHostUnregister` -> `gb_free_driver_buf` -> return

### Verify NO recursion
12. grep for `cuMemAllocManaged` in the Detours attach list. It must NOT be there. If `hooked_cuMemAllocManaged` or any hook for that function exists, REMOVE IT.

### Verdict
Issues found -> fix before committing.
All clear -> commit and update profile.

---

## Phase 9: Finalize

Commit message:
```
Wire UVM into allocation/free/prefetch paths (Phase 3-7 of #5)

- gb_alloc_uvm(): UVM allocation with cuMemPrefetchAsync to GPU
- All 4 allocation hooks: UVM-first with driver fallback
- gb_free_buffer(): branch on is_managed (UVM: cuMemFree, driver: unregister+IOCTL)
- Cleanup loop: is_managed-aware leak cleanup
- Prefetch worker: CUDA device prefetch for UVM, Win32 prefetch for driver path
- test_uvm.c: 6-test validation suite
```

Regenerate profile:
```
python3 repo_profile_gen.py --owner denoflore --repo GreenBoost-Windows --token $GH_TOKEN --format both --push
```

---

## WHY THIS MATTERS

Before this build: every GPU access to GreenBoost memory traverses PCIe (~32 GB/s). After: NVIDIA's UVM driver migrates hot pages to VRAM (~1008 GB/s on 4090). For inference workloads where model weights fit in physical VRAM, this is the difference between 11s/it and 38s/it. The driver fallback ensures older GPUs and edge cases still work.
