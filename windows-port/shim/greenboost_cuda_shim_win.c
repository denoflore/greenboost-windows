/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2024-2026 Ferran Duarri. Dual-licensed: GPL v2 + Commercial.
 *
 * GreenBoost v2.3 — Windows CUDA Shim DLL
 *
 * Intercepts CUDA memory allocations and routes large allocations to the
 * GreenBoost KMDF driver, which provides system RAM (DDR4) and NVMe
 * pagefile as extended GPU memory. Exposes combined VRAM + DDR4 pool
 * to CUDA applications without code changes.
 *
 * Injection methods:
 *   1. Microsoft Detours (recommended): withdll.exe /d:greenboost_cuda.dll app.exe
 *   2. AppInit_DLLs registry key (development)
 *   3. LM Studio/Ollama custom launcher script
 *
 * Port of the Linux greenboost_cuda_shim.c. Replaces:
 *   - LD_PRELOAD + dlsym → Detours DetourAttach (or manual IAT patching)
 *   - /dev/greenboost → \\.\GreenBoost via CreateFileW
 *   - DMA-BUF fd + mmap -> driver MDL map via MmMapLockedPagesSpecifyCache
 *   - eventfd → Named event (GreenBoostPressure)
 *   - pthread_mutex_t → CRITICAL_SECTION
 *   - pthread_cond_t → CONDITION_VARIABLE
 *   - madvise(MADV_WILLNEED) → PrefetchVirtualMemory
 *
 * Author  : Ferran Duarri
 * License : GPL v2 (open-source) / Commercial — see LICENSE
 */

#include "greenboost_cuda_shim_win.h"
#include <memoryapi.h>    /* PrefetchVirtualMemory */
#include <psapi.h>

#if GB_USE_DETOURS
#  include <detours.h>
#endif

/* ================================================================== */
/*  Globals                                                             */
/* ================================================================== */

/* DeviceHandle must be INVALID_HANDLE_VALUE before gb_read_config.
 * Zero-init sets it to 0 (a valid handle on Windows = stdin).
 * gb_read_config() corrects it, but guard against any use in between. */
gb_shim_config_t  gb_config = { 0 };
gb_ht_entry_t     gb_htable[HT_SIZE];
CRITICAL_SECTION  ht_locks[HT_LOCKS];

/* Real function pointers (resolved at init) */
static pfn_cudaMalloc           real_cudaMalloc          = NULL;
static pfn_cudaFree             real_cudaFree            = NULL;
static pfn_cudaMallocAsync      real_cudaMallocAsync     = NULL;
static pfn_cudaMallocManaged    real_cudaMallocManaged   = NULL;
static pfn_cudaMemGetInfo       real_cudaMemGetInfo      = NULL;
static pfn_cuMemAlloc_v2        real_cuMemAlloc_v2       = NULL;
static pfn_cuMemFree_v2         real_cuMemFree_v2        = NULL;
static pfn_cuMemAllocManaged    real_cuMemAllocManaged_drv = NULL;
static pfn_cuMemPrefetchAsync     real_cuMemPrefetchAsync    = NULL;
static pfn_cuCtxGetDevice         real_cuCtxGetDevice         = NULL;
static pfn_cuMemAllocAsync      real_cuMemAllocAsync     = NULL;
static pfn_cuMemFreeAsync       real_cuMemFreeAsync      = NULL;
static pfn_cuMemGetInfo         real_cuMemGetInfo        = NULL;
static pfn_cuDeviceTotalMem_v2  real_cuDeviceTotalMem_v2 = NULL;
static pfn_cuMemHostRegister    real_cuMemHostRegister   = NULL;
static pfn_cuMemHostUnregister  real_cuMemHostUnregister = NULL;
static pfn_cuMemHostGetDevicePointer real_cuMemHostGetDevicePointer = NULL;
static pfn_nvmlDeviceGetMemoryInfo   real_nvmlDeviceGetMemoryInfo   = NULL;
static pfn_nvmlDeviceGetMemoryInfo_v2 real_nvmlDeviceGetMemoryInfo_v2 = NULL;

/* CUDA DLL handles */
static HMODULE hCudaRT  = NULL;   /* cudart64_*.dll */
static HMODULE hCudaDrv = NULL;   /* nvcuda.dll */
static HMODULE hNvml    = NULL;   /* nvml.dll */

/* ================================================================== */
/*  Async prefetch worker (port of Linux prefetch_worker)               */
/* ================================================================== */

#define PREFETCH_QUEUE_SIZE 256

typedef struct {
    void  *ptr;
    size_t size;
    int    is_managed;    /* 1 = use cuMemPrefetchAsync, 0 = PrefetchVirtualMemory */
    int    to_device;     /* 1 = prefetch to GPU, 0 = demote to CPU */
} prefetch_req_t;

static prefetch_req_t    prefetch_queue[PREFETCH_QUEUE_SIZE];
static volatile int      prefetch_head = 0;
static volatile int      prefetch_tail = 0;
static CRITICAL_SECTION  prefetch_cs;
static CONDITION_VARIABLE prefetch_cv;
static HANDLE            prefetch_thread_handle = NULL;
static volatile BOOL     prefetch_running = FALSE;

/* PrefetchVirtualMemory function pointer (Win8+) */
typedef BOOL (WINAPI *pfn_PrefetchVirtualMemory)(
    HANDLE, ULONG_PTR, PWIN32_MEMORY_RANGE_ENTRY, ULONG);
static pfn_PrefetchVirtualMemory pPrefetchVirtualMemory = NULL;

static DWORD WINAPI prefetch_worker(LPVOID arg)
{
    (void)arg;

    while (prefetch_running) {
        prefetch_req_t req;

        EnterCriticalSection(&prefetch_cs);
        while (prefetch_head == prefetch_tail && prefetch_running) {
            SleepConditionVariableCS(&prefetch_cv, &prefetch_cs, 1000);
        }
        if (!prefetch_running) {
            LeaveCriticalSection(&prefetch_cs);
            break;
        }
        req = prefetch_queue[prefetch_tail];
        prefetch_tail = (prefetch_tail + 1) % PREFETCH_QUEUE_SIZE;
        LeaveCriticalSection(&prefetch_cs);

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
    }
    return 0;
}

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

/* ================================================================== */
/*  Hash table — open-addressed with tombstone deletion                 */
/* ================================================================== */

static inline CRITICAL_SECTION *ht_lock_for(uint32_t h)
{
    return &ht_locks[h & (HT_LOCKS - 1u)];
}

/*
 * Insert an entry. Returns 1 on success, 0 if table is full.
 *
 * Can reuse tombstone slots, which fixes the probe-chain bug from
 * the Linux version (which used memset to zero, breaking chains).
 */
static int ht_insert(CUdeviceptr ptr, size_t size, int is_managed,
                     int gb_buf_id, void *mapped_ptr)
{
    uint32_t h = ht_hash(ptr);
    uint32_t i;

    for (i = 0; i < HT_SIZE; i++) {
        uint32_t idx = (h + i) & HT_MASK;
        gb_ht_entry_t *e = &gb_htable[idx];
        CRITICAL_SECTION *lk = ht_lock_for(idx);

        EnterCriticalSection(lk);
        if (e->ptr == HT_EMPTY || e->ptr == HT_TOMBSTONE) {
            e->ptr            = ptr;
            e->size           = size;
            e->is_managed     = is_managed;
            e->gb_buf_id      = gb_buf_id;
            e->mapped_ptr     = mapped_ptr;
            LeaveCriticalSection(lk);
            return 1;
        }
        LeaveCriticalSection(lk);
    }
    return 0;  /* table full */
}

/*
 * Remove an entry. Returns 1 if found, fills output params.
 * Uses tombstone marker instead of zeroing to preserve probe chains.
 */
static int ht_remove(CUdeviceptr ptr, size_t *out_size, int *out_managed,
                     void **out_mapped_ptr, int *out_buf_id)
{
    uint32_t h = ht_hash(ptr);
    uint32_t i;

    for (i = 0; i < HT_SIZE; i++) {
        uint32_t idx = (h + i) & HT_MASK;
        gb_ht_entry_t *e = &gb_htable[idx];
        CRITICAL_SECTION *lk = ht_lock_for(idx);

        EnterCriticalSection(lk);

        if (e->ptr == HT_EMPTY) {
            /* Empty slot -- key not present in table */
            LeaveCriticalSection(lk);
            break;
        }

        if (e->ptr == ptr) {
            /* Found it */
            if (out_size)       *out_size       = e->size;
            if (out_managed)    *out_managed     = e->is_managed;
            if (out_mapped_ptr) *out_mapped_ptr  = e->mapped_ptr;
            if (out_buf_id)     *out_buf_id      = e->gb_buf_id;

            /* Mark as tombstone -- probing continues past this slot */
            e->ptr            = HT_TOMBSTONE;
            e->size           = 0;
            e->is_managed     = 0;
            e->gb_buf_id      = -1;
            e->mapped_ptr     = NULL;

            LeaveCriticalSection(lk);
            return 1;
        }

        LeaveCriticalSection(lk);

        /* Tombstone -- keep probing */
    }
    return 0;
}

/* Lookup without removal */
static int ht_lookup(CUdeviceptr ptr, gb_ht_entry_t *out)
{
    uint32_t h = ht_hash(ptr);
    uint32_t i;

    for (i = 0; i < HT_SIZE; i++) {
        uint32_t idx = (h + i) & HT_MASK;
        gb_ht_entry_t *e = &gb_htable[idx];
        CRITICAL_SECTION *lk = ht_lock_for(idx);

        EnterCriticalSection(lk);

        if (e->ptr == HT_EMPTY) {
            LeaveCriticalSection(lk);
            break;
        }
        if (e->ptr == ptr) {
            if (out) *out = *e;
            LeaveCriticalSection(lk);
            return 1;
        }

        LeaveCriticalSection(lk);
    }
    return 0;
}

/* ================================================================== */
/*  Driver communication                                                */
/* ================================================================== */

static HANDLE gb_open_device(void)
{
    if (gb_config.DeviceHandle != INVALID_HANDLE_VALUE)
        return gb_config.DeviceHandle;

    gb_config.DeviceHandle = CreateFileW(
        L"\\\\.\\GreenBoost",
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (gb_config.DeviceHandle == INVALID_HANDLE_VALUE) {
        gb_log("failed to open \\\\.\\GreenBoost (err=%lu)", GetLastError());
    } else {
        gb_log("opened \\\\.\\GreenBoost");
    }

    return gb_config.DeviceHandle;
}

static void gb_close_device(void)
{
    if (gb_config.DeviceHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(gb_config.DeviceHandle);
        gb_config.DeviceHandle = INVALID_HANDLE_VALUE;
    }
}

/*
 * Tell the driver to free a buffer by ID. Used on error cleanup
 * and on cudaFree. The driver unmaps the user VA and frees pages.
 */
static void gb_free_driver_buf(int buf_id)
{
    struct gb_free_req_win freq = { 0 };
    DWORD bytesReturned;

    if (buf_id <= 0)
        return;

    if (gb_config.DeviceHandle == INVALID_HANDLE_VALUE)
        return;

    freq.buf_id = buf_id;
    DeviceIoControl(gb_config.DeviceHandle, GB_IOCTL_FREE,
                    &freq, sizeof(freq),
                    NULL, 0,
                    &bytesReturned, NULL);
}

/*
 * Allocate memory from the GreenBoost driver and register it with CUDA.
 *
 * Flow: DeviceIoControl(ALLOC) -- driver maps pages into our process
 *       via MmMapLockedPagesSpecifyCache -- returns user_va --
 *       cuMemHostRegister(DEVICEMAP) -- cuMemHostGetDevicePointer
 *
 * The driver does the mapping directly (no MapViewOfFile needed).
 * This is the Windows equivalent of Linux mmap(dma_buf_fd).
 *
 * On any failure, returns non-zero and the caller falls through to
 * real CUDA allocation.
 */
static CUresult gb_alloc_from_driver(void **devPtr, size_t size)
{
    HANDLE hDev;
    struct gb_alloc_req_win req = { 0 };
    struct gb_alloc_req_win resp = { 0 };
    DWORD bytesReturned;
    BOOL ok;
    PVOID mappedPtr;
    CUresult ret;
    CUdeviceptr dptr;

    hDev = gb_open_device();
    if (hDev == INVALID_HANDLE_VALUE)
        return CUDA_ERROR_NOT_SUPPORTED;

    req.size = (gb_u64)size;
    req.flags = GB_ALLOC_WEIGHTS;

    ok = DeviceIoControl(hDev, GB_IOCTL_ALLOC,
                         &req, sizeof(req),
                         &resp, sizeof(resp),
                         &bytesReturned, NULL);
    if (!ok) {
        gb_log("IOCTL_ALLOC failed (err=%lu) for %zuMB",
               GetLastError(), size >> 20);
        return CUDA_ERROR_NOT_SUPPORTED;
    }

    /*
     * The driver mapped the physical pages directly into our process
     * via MmMapLockedPagesSpecifyCache(UserMode). The returned user_va
     * is already valid in our address space -- no MapViewOfFile needed.
     */
    mappedPtr = (PVOID)(ULONG_PTR)resp.user_va;
    if (!mappedPtr) {
        gb_log_err("driver returned NULL user_va for %zuMB", size >> 20);
        gb_free_driver_buf(resp.buf_id);
        return CUDA_ERROR_NOT_SUPPORTED;
    }

    gb_log("driver mapped %lluMB at %p (buf_id=%d)",
           (unsigned long long)resp.size >> 20, mappedPtr, resp.buf_id);

    /* Register with CUDA as device-mappable host memory */
    if (!real_cuMemHostRegister) {
        gb_log_err("cuMemHostRegister not resolved -- cannot register");
        gb_free_driver_buf(resp.buf_id);
        return CUDA_ERROR_NOT_SUPPORTED;
    }

    ret = real_cuMemHostRegister(mappedPtr, (size_t)resp.size,
                                 CU_MEMHOSTREGISTER_DEVICEMAP);
    if (ret != CUDA_SUCCESS) {
        gb_log_err("cuMemHostRegister failed: %d", ret);
        gb_free_driver_buf(resp.buf_id);
        return ret;
    }

    /* Get the device-visible pointer */
    ret = real_cuMemHostGetDevicePointer(&dptr, mappedPtr, 0);
    if (ret != CUDA_SUCCESS) {
        gb_log_err("cuMemHostGetDevicePointer failed: %d", ret);
        real_cuMemHostUnregister(mappedPtr);
        gb_free_driver_buf(resp.buf_id);
        return ret;
    }

    *devPtr = (void *)(uintptr_t)dptr;

    /* Track in hash table for cleanup on cudaFree */
    if (!ht_insert(dptr, (size_t)resp.size, 0, resp.buf_id, mappedPtr)) {
        gb_log_err("hash table full -- leaking allocation");
    }

    /* Enqueue prefetch to bring pages into RAM proactively */
    enqueue_prefetch(mappedPtr, (size_t)resp.size, 0, 1);

    gb_log("GreenBoost alloc: %zuMB -> device ptr %p (buf_id=%d)",
           size >> 20, *devPtr, resp.buf_id);

    return CUDA_SUCCESS;
}

/*
 * Free a GreenBoost-allocated buffer. Unregisters from CUDA,
 * then tells the driver to unmap and free the pages.
 */
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

/* ================================================================== */
/*  Deferred UVM probe                                                  */
/*                                                                      */
/*  MUST NOT run at DLL_PROCESS_ATTACH (gb_shim_init) — no CUDA         */
/*  context exists yet, so cuMemAllocManaged would fail with            */
/*  CUDA_ERROR_NOT_INITIALIZED, permanently disabling UVM fallback      */
/*  even on capable hardware.                                           */
/*                                                                      */
/*  Instead, probe once on the first intercepted allocation, where      */
/*  the calling application has already created a CUDA context.         */
/* ================================================================== */

static void gb_probe_uvm_once(void)
{
    CUdeviceptr probe_ptr = 0;
    CUresult ret;
    pfn_cuMemFree_v2 free_fn;

    /* Double-checked locking via Initialized gate + UvmProbed flag */
    if (gb_config.UvmProbed)
        return;

    /* Serialize concurrent first-call races */
    EnterCriticalSection(&ht_locks[0]);
    if (gb_config.UvmProbed) {
        LeaveCriticalSection(&ht_locks[0]);
        return;
    }

    gb_config.UvmAvailable = FALSE;

    if (!real_cuMemAllocManaged_drv) {
        gb_log("UVM probe: cuMemAllocManaged not found — UVM unavailable");
        gb_config.UvmProbed = TRUE;
        LeaveCriticalSection(&ht_locks[0]);
        return;
    }

    /* Tiny probe allocation — 4 bytes, just to test capability */
    ret = real_cuMemAllocManaged_drv(&probe_ptr, 4, CU_MEM_ATTACH_GLOBAL);
    if (ret == CUDA_SUCCESS) {
        gb_config.UvmAvailable = TRUE;
        gb_log("UVM probe: managed memory available (probe alloc succeeded)");

        /* Clean up the probe allocation */
        free_fn = real_cuMemFree_v2;
        if (free_fn)
            free_fn(probe_ptr);
    } else {
        gb_log("UVM probe: cuMemAllocManaged returned %d — UVM unavailable", ret);
    }

    gb_config.UvmProbed = TRUE;
    LeaveCriticalSection(&ht_locks[0]);
}

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

/* ================================================================== */
/*  CUDA hook implementations                                           */
/* ================================================================== */

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

static cudaError_t CUDAAPI hooked_cudaFree(void *devPtr)
{
    if (devPtr && gb_free_buffer((CUdeviceptr)(uintptr_t)devPtr))
        return (cudaError_t)CUDA_SUCCESS;
    return real_cudaFree(devPtr);
}

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

static cudaError_t CUDAAPI hooked_cudaMallocManaged(
    void **devPtr, size_t size, unsigned int flags)
{
    if (!gb_config.UvmProbed)
        gb_probe_uvm_once();

    /* Intercept large managed allocations the same way as cudaMalloc */
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
    return real_cudaMallocManaged(devPtr, size, flags);
}

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

static CUresult CUDAAPI hooked_cuMemFree_v2(CUdeviceptr dptr)
{
    if (dptr && gb_free_buffer(dptr))
        return CUDA_SUCCESS;
    return real_cuMemFree_v2(dptr);
}

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

static CUresult CUDAAPI hooked_cuMemFreeAsync(CUdeviceptr dptr, CUstream hStream)
{
    if (dptr && gb_free_buffer(dptr))
        return CUDA_SUCCESS;
    return real_cuMemFreeAsync(dptr, hStream);
}

/* ================================================================== */
/*  VRAM spoofing hooks — report extended memory                        */
/* ================================================================== */

static CUresult CUDAAPI hooked_cuDeviceTotalMem_v2(size_t *bytes, CUdevice dev)
{
    CUresult ret = real_cuDeviceTotalMem_v2(bytes, dev);
    if (ret == CUDA_SUCCESS && gb_config.Initialized) {
        size_t original = *bytes;
        *bytes = gb_config.ReportedTotalVram;
        gb_log("cuDeviceTotalMem: %zuMB → %zuMB (spoofed)",
               original >> 20, *bytes >> 20);
    }
    return ret;
}

static cudaError_t CUDAAPI hooked_cudaMemGetInfo(size_t *free, size_t *total)
{
    cudaError_t ret = real_cudaMemGetInfo(free, total);
    if (ret == (cudaError_t)CUDA_SUCCESS && gb_config.Initialized) {
        size_t orig_total = *total;
        size_t orig_free = *free;
        *total = gb_config.ReportedTotalVram;
        /* Add the DDR4 pool available to free, guard against underflow
         * when GPU has more VRAM than our config expects */
        if (gb_config.ReportedTotalVram > orig_total)
            *free = orig_free + (gb_config.ReportedTotalVram - orig_total);
        /* else: leave *free as-is (real GPU has more than configured) */
        gb_log("cudaMemGetInfo: total %zuMB→%zuMB free %zuMB→%zuMB",
               orig_total >> 20, *total >> 20,
               orig_free >> 20, *free >> 20);
    }
    return ret;
}

static CUresult CUDAAPI hooked_cuMemGetInfo(size_t *free, size_t *total)
{
    CUresult ret = real_cuMemGetInfo(free, total);
    if (ret == CUDA_SUCCESS && gb_config.Initialized) {
        size_t orig_total = *total;
        size_t orig_free = *free;
        *total = gb_config.ReportedTotalVram;
        if (gb_config.ReportedTotalVram > orig_total)
            *free = orig_free + (gb_config.ReportedTotalVram - orig_total);
        gb_log("cuMemGetInfo: total %zuMB→%zuMB free %zuMB→%zuMB",
               orig_total >> 20, *total >> 20,
               orig_free >> 20, *free >> 20);
    }
    return ret;
}

/* NVML spoofing */
static nvmlReturn_t hooked_nvmlDeviceGetMemoryInfo(
    nvmlDevice_t device, nvmlMemory_t *memory)
{
    nvmlReturn_t ret = real_nvmlDeviceGetMemoryInfo(device, memory);
    if (ret == NVML_SUCCESS && gb_config.Initialized) {
        memory->total = (unsigned long long)gb_config.ReportedTotalVram;
        memory->free = memory->total - memory->used;
        gb_log("nvmlDeviceGetMemoryInfo: total=%lluMB",
               memory->total >> 20);
    }
    return ret;
}

static nvmlReturn_t hooked_nvmlDeviceGetMemoryInfo_v2(
    nvmlDevice_t device, nvmlMemory_v2_t *memory)
{
    nvmlReturn_t ret = real_nvmlDeviceGetMemoryInfo_v2(device, memory);
    if (ret == NVML_SUCCESS && gb_config.Initialized) {
        memory->total = (unsigned long long)gb_config.ReportedTotalVram;
        memory->free = memory->total - memory->used;
        gb_log("nvmlDeviceGetMemoryInfo_v2: total=%lluMB",
               memory->total >> 20);
    }
    return ret;
}

/* ================================================================== */
/*  Configuration — read from registry                                  */
/* ================================================================== */

static void gb_read_config(void)
{
    HKEY hKey;
    DWORD type, size, value;
    LONG result;

    /* Defaults */
    gb_config.PhysicalVramGb = 12;
    gb_config.VirtualVramGb  = 51;
    gb_config.ThresholdMb    = 256;
    gb_config.DebugMode      = 0;

    /* Check environment variables first (override registry) */
    {
        const char *env;
        env = getenv("GREENBOOST_DEBUG");
        if (env && env[0] == '1')
            gb_config.DebugMode = 1;

        env = getenv("GREENBOOST_THRESHOLD_MB");
        if (env) {
            int v = atoi(env);
            if (v > 0)
                gb_config.ThresholdMb = (ULONG)v;
        }
    }

    /* Read from registry — try the driver's service key first (canonical
     * path written by install.ps1 and config.ps1), fall back to the
     * SOFTWARE hive for backwards compatibility. */
    result = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                           "SYSTEM\\CurrentControlSet\\Services\\GreenBoost\\Parameters",
                           0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) {
        result = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\GreenBoost",
                               0, KEY_READ, &hKey);
    }
    if (result != ERROR_SUCCESS)
        goto done;

    size = sizeof(value);
    if (RegQueryValueExA(hKey, "PhysicalVramGb", NULL, &type,
                         (LPBYTE)&value, &size) == ERROR_SUCCESS && type == REG_DWORD)
        gb_config.PhysicalVramGb = value;

    size = sizeof(value);
    if (RegQueryValueExA(hKey, "VirtualVramGb", NULL, &type,
                         (LPBYTE)&value, &size) == ERROR_SUCCESS && type == REG_DWORD)
        gb_config.VirtualVramGb = value;

    size = sizeof(value);
    if (RegQueryValueExA(hKey, "ThresholdMb", NULL, &type,
                         (LPBYTE)&value, &size) == ERROR_SUCCESS && type == REG_DWORD)
        gb_config.ThresholdMb = value;

    size = sizeof(value);
    if (RegQueryValueExA(hKey, "DebugMode", NULL, &type,
                         (LPBYTE)&value, &size) == ERROR_SUCCESS && type == REG_DWORD)
        gb_config.DebugMode = value;

    RegCloseKey(hKey);

done:
    gb_config.ThresholdBytes = (size_t)gb_config.ThresholdMb * 1024ULL * 1024ULL;
    gb_config.ReportedTotalVram =
        ((size_t)gb_config.PhysicalVramGb + (size_t)gb_config.VirtualVramGb)
        * (1ULL << 30);
    gb_config.DeviceHandle = INVALID_HANDLE_VALUE;

    gb_log("config: physVRAM=%luGB virtVRAM=%luGB threshold=%luMB debug=%lu",
           gb_config.PhysicalVramGb, gb_config.VirtualVramGb,
           gb_config.ThresholdMb, gb_config.DebugMode);
}

/* ================================================================== */
/*  Symbol resolution — load real CUDA functions                        */
/* ================================================================== */

static void gb_resolve_symbols(void)
{
    /*
     * Load the CUDA driver API (nvcuda.dll) and runtime (cudart64_*.dll).
     * We load by name — the system DLL search order will find NVIDIA's
     * DLLs in their standard install location.
     */
    hCudaDrv = LoadLibraryA("nvcuda.dll");
    if (!hCudaDrv) {
        gb_log_err("failed to load nvcuda.dll");
        return;
    }

    hCudaRT = LoadLibraryA("cudart64_12.dll");
    if (!hCudaRT) {
        /* Try without version suffix */
        hCudaRT = LoadLibraryA("cudart64.dll");
    }

    hNvml = LoadLibraryA("nvml.dll");

    /* Resolve driver API functions */
    real_cuMemAlloc_v2 = (pfn_cuMemAlloc_v2)
        GetProcAddress(hCudaDrv, "cuMemAlloc_v2");
    real_cuMemFree_v2 = (pfn_cuMemFree_v2)
        GetProcAddress(hCudaDrv, "cuMemFree_v2");
    real_cuMemAllocAsync = (pfn_cuMemAllocAsync)
        GetProcAddress(hCudaDrv, "cuMemAllocAsync");
    real_cuMemFreeAsync = (pfn_cuMemFreeAsync)
        GetProcAddress(hCudaDrv, "cuMemFreeAsync");
    real_cuMemGetInfo = (pfn_cuMemGetInfo)
        GetProcAddress(hCudaDrv, "cuMemGetInfo_v2");
    real_cuDeviceTotalMem_v2 = (pfn_cuDeviceTotalMem_v2)
        GetProcAddress(hCudaDrv, "cuDeviceTotalMem_v2");
    real_cuMemAllocManaged_drv = (pfn_cuMemAllocManaged)
        GetProcAddress(hCudaDrv, "cuMemAllocManaged");
    real_cuMemPrefetchAsync = (pfn_cuMemPrefetchAsync)
        GetProcAddress(hCudaDrv, "cuMemPrefetchAsync");
    real_cuCtxGetDevice = (pfn_cuCtxGetDevice)
        GetProcAddress(hCudaDrv, "cuCtxGetDevice");
    real_cuMemHostRegister = (pfn_cuMemHostRegister)
        GetProcAddress(hCudaDrv, "cuMemHostRegister_v2");
    real_cuMemHostUnregister = (pfn_cuMemHostUnregister)
        GetProcAddress(hCudaDrv, "cuMemHostUnregister");
    real_cuMemHostGetDevicePointer = (pfn_cuMemHostGetDevicePointer)
        GetProcAddress(hCudaDrv, "cuMemHostGetDevicePointer_v2");

    /* Resolve runtime API functions */
    if (hCudaRT) {
        real_cudaMalloc = (pfn_cudaMalloc)
            GetProcAddress(hCudaRT, "cudaMalloc");
        real_cudaFree = (pfn_cudaFree)
            GetProcAddress(hCudaRT, "cudaFree");
        real_cudaMallocAsync = (pfn_cudaMallocAsync)
            GetProcAddress(hCudaRT, "cudaMallocAsync");
        real_cudaMallocManaged = (pfn_cudaMallocManaged)
            GetProcAddress(hCudaRT, "cudaMallocManaged");
        real_cudaMemGetInfo = (pfn_cudaMemGetInfo)
            GetProcAddress(hCudaRT, "cudaMemGetInfo");
    }

    /* Resolve NVML functions */
    if (hNvml) {
        real_nvmlDeviceGetMemoryInfo = (pfn_nvmlDeviceGetMemoryInfo)
            GetProcAddress(hNvml, "nvmlDeviceGetMemoryInfo");
        real_nvmlDeviceGetMemoryInfo_v2 = (pfn_nvmlDeviceGetMemoryInfo_v2)
            GetProcAddress(hNvml, "nvmlDeviceGetMemoryInfo_v2");
    }

    /* Resolve PrefetchVirtualMemory (Win8+) */
    {
        HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
        if (hKernel32) {
            pPrefetchVirtualMemory = (pfn_PrefetchVirtualMemory)
                GetProcAddress(hKernel32, "PrefetchVirtualMemory");
        }
    }

    gb_log("symbols resolved: cuMemAlloc=%p cudaMalloc=%p cuMemHostRegister=%p "
           "cuMemAllocManaged=%p cuMemPrefetchAsync=%p",
           (void*)real_cuMemAlloc_v2, (void*)real_cudaMalloc,
           (void*)real_cuMemHostRegister, (void*)real_cuMemAllocManaged_drv,
           (void*)real_cuMemPrefetchAsync);
}

/* ================================================================== */
/*  Hook installation                                                   */
/* ================================================================== */

#if GB_USE_DETOURS

static void gb_install_hooks(void)
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (real_cudaMalloc)
        DetourAttach((PVOID*)&real_cudaMalloc, hooked_cudaMalloc);
    if (real_cudaFree)
        DetourAttach((PVOID*)&real_cudaFree, hooked_cudaFree);
    if (real_cudaMallocAsync)
        DetourAttach((PVOID*)&real_cudaMallocAsync, hooked_cudaMallocAsync);
    if (real_cuMemAlloc_v2)
        DetourAttach((PVOID*)&real_cuMemAlloc_v2, hooked_cuMemAlloc_v2);
    if (real_cuMemFree_v2)
        DetourAttach((PVOID*)&real_cuMemFree_v2, hooked_cuMemFree_v2);
    if (real_cuMemAllocAsync)
        DetourAttach((PVOID*)&real_cuMemAllocAsync, hooked_cuMemAllocAsync);
    if (real_cuMemFreeAsync)
        DetourAttach((PVOID*)&real_cuMemFreeAsync, hooked_cuMemFreeAsync);
    if (real_cudaMallocManaged)
        DetourAttach((PVOID*)&real_cudaMallocManaged, hooked_cudaMallocManaged);
    if (real_cuDeviceTotalMem_v2)
        DetourAttach((PVOID*)&real_cuDeviceTotalMem_v2, hooked_cuDeviceTotalMem_v2);
    if (real_cudaMemGetInfo)
        DetourAttach((PVOID*)&real_cudaMemGetInfo, hooked_cudaMemGetInfo);
    if (real_cuMemGetInfo)
        DetourAttach((PVOID*)&real_cuMemGetInfo, hooked_cuMemGetInfo);
    if (real_nvmlDeviceGetMemoryInfo)
        DetourAttach((PVOID*)&real_nvmlDeviceGetMemoryInfo,
                     hooked_nvmlDeviceGetMemoryInfo);
    if (real_nvmlDeviceGetMemoryInfo_v2)
        DetourAttach((PVOID*)&real_nvmlDeviceGetMemoryInfo_v2,
                     hooked_nvmlDeviceGetMemoryInfo_v2);

    LONG err = DetourTransactionCommit();
    if (err != NO_ERROR) {
        gb_log_err("DetourTransactionCommit failed: %ld", err);
    } else {
        gb_log("hooks installed via Detours");
    }
}

static void gb_remove_hooks(void)
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (real_cudaMalloc)
        DetourDetach((PVOID*)&real_cudaMalloc, hooked_cudaMalloc);
    if (real_cudaFree)
        DetourDetach((PVOID*)&real_cudaFree, hooked_cudaFree);
    if (real_cudaMallocAsync)
        DetourDetach((PVOID*)&real_cudaMallocAsync, hooked_cudaMallocAsync);
    if (real_cuMemAlloc_v2)
        DetourDetach((PVOID*)&real_cuMemAlloc_v2, hooked_cuMemAlloc_v2);
    if (real_cuMemFree_v2)
        DetourDetach((PVOID*)&real_cuMemFree_v2, hooked_cuMemFree_v2);
    if (real_cuMemAllocAsync)
        DetourDetach((PVOID*)&real_cuMemAllocAsync, hooked_cuMemAllocAsync);
    if (real_cuMemFreeAsync)
        DetourDetach((PVOID*)&real_cuMemFreeAsync, hooked_cuMemFreeAsync);
    if (real_cudaMallocManaged)
        DetourDetach((PVOID*)&real_cudaMallocManaged, hooked_cudaMallocManaged);
    if (real_cuDeviceTotalMem_v2)
        DetourDetach((PVOID*)&real_cuDeviceTotalMem_v2, hooked_cuDeviceTotalMem_v2);
    if (real_cudaMemGetInfo)
        DetourDetach((PVOID*)&real_cudaMemGetInfo, hooked_cudaMemGetInfo);
    if (real_cuMemGetInfo)
        DetourDetach((PVOID*)&real_cuMemGetInfo, hooked_cuMemGetInfo);
    if (real_nvmlDeviceGetMemoryInfo)
        DetourDetach((PVOID*)&real_nvmlDeviceGetMemoryInfo,
                     hooked_nvmlDeviceGetMemoryInfo);
    if (real_nvmlDeviceGetMemoryInfo_v2)
        DetourDetach((PVOID*)&real_nvmlDeviceGetMemoryInfo_v2,
                     hooked_nvmlDeviceGetMemoryInfo_v2);

    DetourTransactionCommit();
    gb_log("hooks removed");
}

#else /* Manual IAT hooking fallback */

/*
 * IAT (Import Address Table) hooking: patch the function pointer in
 * the target module's IAT to point to our hook instead of the real
 * function. This works without Detours but is less robust.
 */
static void *gb_iat_hook(HMODULE module, const char *importModule,
                         const char *funcName, void *hookFunc)
{
    PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)module;
    PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)
        ((BYTE*)module + dosHeader->e_lfanew);
    PIMAGE_IMPORT_DESCRIPTOR imports = (PIMAGE_IMPORT_DESCRIPTOR)
        ((BYTE*)module + ntHeaders->OptionalHeader.DataDirectory[
            IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

    for (; imports->Name; imports++) {
        const char *modName = (const char*)((BYTE*)module + imports->Name);
        if (_stricmp(modName, importModule) != 0)
            continue;

        PIMAGE_THUNK_DATA origThunk = (PIMAGE_THUNK_DATA)
            ((BYTE*)module + imports->OriginalFirstThunk);
        PIMAGE_THUNK_DATA thunk = (PIMAGE_THUNK_DATA)
            ((BYTE*)module + imports->FirstThunk);

        for (; origThunk->u1.AddressOfData; origThunk++, thunk++) {
            if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal))
                continue;

            PIMAGE_IMPORT_BY_NAME importByName = (PIMAGE_IMPORT_BY_NAME)
                ((BYTE*)module + origThunk->u1.AddressOfData);

            if (strcmp(importByName->Name, funcName) != 0)
                continue;

            /* Found it — patch the IAT entry */
            void *original = (void*)thunk->u1.Function;
            DWORD oldProtect;
            VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function),
                           PAGE_READWRITE, &oldProtect);
            thunk->u1.Function = (ULONG_PTR)hookFunc;
            VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function),
                           oldProtect, &oldProtect);
            return original;
        }
    }
    return NULL;
}

static void gb_install_hooks(void)
{
    /*
     * IAT hooking: scan the main executable's import table and patch
     * entries for CUDA functions. Less robust than Detours (only patches
     * the main exe, not DLLs loaded later) but covers the critical paths.
     */
    HMODULE hProcess = GetModuleHandle(NULL);
    if (!hProcess) return;

    /* Runtime API hooks via cudart DLL */
    if (real_cudaMalloc) {
        void *orig = gb_iat_hook(hProcess, "cudart64_12.dll",
                                 "cudaMalloc", hooked_cudaMalloc);
        if (!orig)
            orig = gb_iat_hook(hProcess, "cudart64.dll",
                               "cudaMalloc", hooked_cudaMalloc);
        if (orig) real_cudaMalloc = (pfn_cudaMalloc)orig;
    }
    if (real_cudaFree) {
        void *orig = gb_iat_hook(hProcess, "cudart64_12.dll",
                                 "cudaFree", hooked_cudaFree);
        if (!orig)
            orig = gb_iat_hook(hProcess, "cudart64.dll",
                               "cudaFree", hooked_cudaFree);
        if (orig) real_cudaFree = (pfn_cudaFree)orig;
    }
    if (real_cudaMemGetInfo) {
        void *orig = gb_iat_hook(hProcess, "cudart64_12.dll",
                                 "cudaMemGetInfo", hooked_cudaMemGetInfo);
        if (!orig)
            orig = gb_iat_hook(hProcess, "cudart64.dll",
                               "cudaMemGetInfo", hooked_cudaMemGetInfo);
        if (orig) real_cudaMemGetInfo = (pfn_cudaMemGetInfo)orig;
    }
    /* Driver API hooks via nvcuda.dll */
    if (real_cuMemAlloc_v2) {
        void *orig = gb_iat_hook(hProcess, "nvcuda.dll",
                                 "cuMemAlloc_v2", hooked_cuMemAlloc_v2);
        if (orig) real_cuMemAlloc_v2 = (pfn_cuMemAlloc_v2)orig;
    }
    if (real_cuMemFree_v2) {
        void *orig = gb_iat_hook(hProcess, "nvcuda.dll",
                                 "cuMemFree_v2", hooked_cuMemFree_v2);
        if (orig) real_cuMemFree_v2 = (pfn_cuMemFree_v2)orig;
    }
    if (real_cuDeviceTotalMem_v2) {
        void *orig = gb_iat_hook(hProcess, "nvcuda.dll",
                                 "cuDeviceTotalMem_v2", hooked_cuDeviceTotalMem_v2);
        if (orig) real_cuDeviceTotalMem_v2 = (pfn_cuDeviceTotalMem_v2)orig;
    }
    if (real_cuMemGetInfo) {
        void *orig = gb_iat_hook(hProcess, "nvcuda.dll",
                                 "cuMemGetInfo_v2", hooked_cuMemGetInfo);
        if (orig) real_cuMemGetInfo = (pfn_cuMemGetInfo)orig;
    }

    gb_log("hooks installed via IAT patching (main exe only, use Detours for full support)");
}

static void gb_remove_hooks(void)
{
    /* IAT hooks are automatically removed when the DLL unloads */
    gb_log("IAT hooks released");
}

#endif /* GB_USE_DETOURS */

/* ================================================================== */
/*  Initialization and cleanup                                          */
/* ================================================================== */

static void gb_shim_init(void)
{
    UINT i;

    gb_log("=== GreenBoost v2.3 CUDA Shim (Windows) ===");

    /* Ensure DeviceHandle is INVALID_HANDLE_VALUE before any code path
     * could touch it. Zero-init sets it to 0 which is a valid handle. */
    gb_config.DeviceHandle = INVALID_HANDLE_VALUE;

    /* Initialize hash table locks */
    for (i = 0; i < HT_LOCKS; i++)
        InitializeCriticalSectionAndSpinCount(&ht_locks[i], 4000);

    /* Clear hash table */
    memset(gb_htable, 0, sizeof(gb_htable));

    /* Initialize UVM state (probe deferred to first interception) */
    gb_config.GpuDevice = 0;
    gb_config.UvmAllocatedBytes = 0;

    /* Read configuration */
    gb_read_config();

    /* Resolve real CUDA symbols */
    gb_resolve_symbols();

    /* Validate we have the minimum required symbols */
    if (!real_cuMemHostRegister || !real_cuMemHostGetDevicePointer) {
        gb_log_err("critical CUDA symbols not found — shim disabled");
        gb_config.Initialized = FALSE;
        return;
    }

    /* Start prefetch worker thread */
    InitializeCriticalSectionAndSpinCount(&prefetch_cs, 4000);
    InitializeConditionVariable(&prefetch_cv);
    prefetch_running = TRUE;
    prefetch_thread_handle = CreateThread(NULL, 0, prefetch_worker, NULL, 0, NULL);
    if (prefetch_thread_handle)
        gb_log("prefetch worker started");

    /* Open pressure event for monitoring.
     * The driver creates it at \BaseNamedObjects\GreenBoostPressure
     * which maps to the Global\ prefix in user mode. */
    gb_config.PressureEvent = OpenEventW(SYNCHRONIZE, FALSE,
                                         L"Global\\GreenBoostPressure");
    if (gb_config.PressureEvent)
        gb_log("pressure event opened");

    /* Install hooks */
    gb_install_hooks();

    gb_config.Initialized = TRUE;
    gb_log("shim initialized — intercepting CUDA allocs >= %luMB",
           gb_config.ThresholdMb);
    gb_log("reported VRAM: %zuMB (%luGB phys + %luGB DDR4)",
           gb_config.ReportedTotalVram >> 20,
           gb_config.PhysicalVramGb,
           gb_config.VirtualVramGb);
}

static void gb_shim_cleanup(void)
{
    UINT i;

    gb_log("shutting down GreenBoost shim");

    gb_config.Initialized = FALSE;

    /* Remove hooks */
    gb_remove_hooks();

    /* Stop prefetch worker */
    if (prefetch_thread_handle) {
        prefetch_running = FALSE;
        WakeConditionVariable(&prefetch_cv);
        WaitForSingleObject(prefetch_thread_handle, 5000);
        CloseHandle(prefetch_thread_handle);
        prefetch_thread_handle = NULL;
    }
    DeleteCriticalSection(&prefetch_cs);

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

    /* Close device and event handles */
    gb_close_device();
    if (gb_config.PressureEvent) {
        CloseHandle(gb_config.PressureEvent);
        gb_config.PressureEvent = NULL;
    }

    /* Cleanup hash table locks */
    for (i = 0; i < HT_LOCKS; i++)
        DeleteCriticalSection(&ht_locks[i]);

    /* Unload CUDA libraries */
    /* Note: Don't FreeLibrary on DLL_PROCESS_DETACH — it can cause
     * loader lock issues. The process is terminating anyway. */

    gb_log("shim shutdown complete");
}

/* ================================================================== */
/*  DLL entry point                                                     */
/* ================================================================== */

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    (void)hModule;
    (void)reserved;

    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        gb_shim_init();
        break;

    case DLL_PROCESS_DETACH:
        gb_shim_cleanup();
        break;
    }

    return TRUE;
}

