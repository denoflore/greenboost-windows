/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2024-2026 Ferran Duarri. Dual-licensed: GPL v2 + Commercial.
 *
 * GreenBoost 2.5 experimental Windows CUDA Shim DLL
 *
 * Uses native VRAM first, with an explicit managed-first policy option.
 * Reports real GPU capacity plus a bounded system RAM allowance.
 * The legacy KMDF fallback remains for separately installed driver setups;
 * the portable package does not require or include that driver.
 *
 * Injection methods:
 *   greenboost.exe app.exe [arguments] (Microsoft Detours)
 *
 * Port of the Linux greenboost_cuda_shim.c. Replaces:
 *   - LD_PRELOAD + dlsym -> Detours exports and CUDA entry-point lookup
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
volatile LONG gb_tombstone_count = 0;

/* Real function pointers (resolved at init) */
static pfn_cuMemAlloc_v2        real_cuMemAlloc_v2       = NULL;
static pfn_cuMemFree_v2         real_cuMemFree_v2        = NULL;
static pfn_cuMemAllocManaged    real_cuMemAllocManaged_drv = NULL;
static pfn_cuMemPrefetchAsync     real_cuMemPrefetchAsync    = NULL;
static pfn_cuCtxGetDevice         real_cuCtxGetDevice         = NULL;
static pfn_cuMemGetInfo         real_cuMemGetInfo        = NULL;
static pfn_cuGetProcAddress     real_cuGetProcAddress    = NULL;
static pfn_cuGetProcAddress_v2  real_cuGetProcAddress_v2 = NULL;
static pfn_cuDeviceTotalMem_v2  real_cuDeviceTotalMem_v2 = NULL;
static pfn_cuMemHostRegister    real_cuMemHostRegister   = NULL;
static pfn_cuMemHostUnregister  real_cuMemHostUnregister = NULL;
static pfn_cuMemHostGetDevicePointer real_cuMemHostGetDevicePointer = NULL;

/* CUDA DLL handles */
static HMODULE hCudaDrv = NULL;   /* nvcuda.dll */
static size_t gb_extra_bytes;
static size_t gb_reserve_bytes;
static int gb_managed_first;
static int gb_legacy_driver;
static CUresult (*real_cuDeviceGetAttribute)(int *, int, CUdevice);

static size_t gb_extra_for_device(CUdevice device)
{
    int managed = 0;
    if (!real_cuMemAllocManaged_drv || !real_cuDeviceGetAttribute ||
        real_cuDeviceGetAttribute(&managed, 83, device) != CUDA_SUCCESS || !managed)
        return 0;
    return gb_extra_bytes;
}

#if GB_USE_DETOURS
static PDETOUR_CREATE_PROCESS_ROUTINEA real_CreateProcessA = CreateProcessA;
static PDETOUR_CREATE_PROCESS_ROUTINEW real_CreateProcessW = CreateProcessW;
static char gb_dll_path[MAX_PATH];
static __declspec(thread) int gb_creating_child;

static BOOL WINAPI hooked_CreateProcessA(LPCSTR app, LPSTR command,
    LPSECURITY_ATTRIBUTES process_attr, LPSECURITY_ATTRIBUTES thread_attr,
    BOOL inherit, DWORD flags, LPVOID environment, LPCSTR directory,
    LPSTARTUPINFOA startup, LPPROCESS_INFORMATION process)
{
    BOOL ok;
    DWORD error;
    if (gb_creating_child)
        return real_CreateProcessA(app, command, process_attr, thread_attr,
            inherit, flags, environment, directory, startup, process);
    gb_creating_child = 1;
    ok = DetourCreateProcessWithDllExA(app, command, process_attr, thread_attr,
        inherit, flags, environment, directory, startup, process,
        gb_dll_path, real_CreateProcessA);
    error = GetLastError();
    gb_creating_child = 0;
    if (!ok) gb_log_err("child injection failed: Windows %lu", error);
    SetLastError(error);
    return ok;
}

static BOOL WINAPI hooked_CreateProcessW(LPCWSTR app, LPWSTR command,
    LPSECURITY_ATTRIBUTES process_attr, LPSECURITY_ATTRIBUTES thread_attr,
    BOOL inherit, DWORD flags, LPVOID environment, LPCWSTR directory,
    LPSTARTUPINFOW startup, LPPROCESS_INFORMATION process)
{
    BOOL ok;
    DWORD error;
    if (gb_creating_child)
        return real_CreateProcessW(app, command, process_attr, thread_attr,
            inherit, flags, environment, directory, startup, process);
    gb_creating_child = 1;
    ok = DetourCreateProcessWithDllExW(app, command, process_attr, thread_attr,
        inherit, flags, environment, directory, startup, process,
        gb_dll_path, real_CreateProcessW);
    error = GetLastError();
    gb_creating_child = 0;
    if (!ok) gb_log_err("child injection failed: Windows %lu", error);
    SetLastError(error);
    return ok;
}
#endif

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
            if (e->ptr == HT_TOMBSTONE)
                InterlockedDecrement(&gb_tombstone_count);
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

static void ht_reclaim_tombstones(void);  /* forward declaration */

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
            InterlockedIncrement(&gb_tombstone_count);
            e->size           = 0;
            e->is_managed     = 0;
            e->gb_buf_id      = -1;
            e->mapped_ptr     = NULL;

            LeaveCriticalSection(lk);
            /* Trigger reclamation if tombstones are accumulating */
            if (gb_tombstone_count > HT_TOMBSTONE_THRESHOLD)
                ht_reclaim_tombstones();
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

/*
 * Reclaim tombstone slots by compacting probe chains.
 * Called periodically when tombstone count exceeds threshold.
 * Must NOT be called while any other ht operation is in progress
 * on the same slot range -- we acquire ALL locks sequentially.
 */
static void ht_reclaim_tombstones(void)
{
    uint32_t i;
    LONG reclaimed = 0;

    /* Acquire all locks to ensure exclusive access */
    for (i = 0; i < HT_LOCKS; i++)
        EnterCriticalSection(&ht_locks[i]);

    for (i = 0; i < HT_SIZE; i++) {
        if (gb_htable[i].ptr == HT_TOMBSTONE) {
            gb_htable[i].ptr = HT_EMPTY;
            reclaimed++;
        }
    }

    /*
     * After clearing tombstones to EMPTY, some entries that were
     * inserted past a tombstone may now be unreachable. Re-insert
     * all live entries to fix probe chains.
     */
    for (i = 0; i < HT_SIZE; i++) {
        if (gb_htable[i].ptr != HT_EMPTY && gb_htable[i].ptr != HT_TOMBSTONE) {
            gb_ht_entry_t saved = gb_htable[i];
            gb_htable[i].ptr = HT_EMPTY;

            /* Re-insert at correct position */
            uint32_t h = ht_hash(saved.ptr);
            uint32_t j;
            for (j = 0; j < HT_SIZE; j++) {
                uint32_t idx = (h + j) & HT_MASK;
                if (gb_htable[idx].ptr == HT_EMPTY) {
                    gb_htable[idx] = saved;
                    break;
                }
            }
        }
    }

    InterlockedExchange(&gb_tombstone_count, 0);

    for (i = HT_LOCKS; i > 0; i--)
        LeaveCriticalSection(&ht_locks[i - 1]);

    gb_log("hash table reclaimed %ld tombstones", reclaimed);
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
static int gb_free_buffer(CUdeviceptr ptr, CUresult *result)
{
    size_t size;
    int is_managed;
    void *mapped_ptr;
    int buf_id;

    if (!ht_remove(ptr, &size, &is_managed, &mapped_ptr, &buf_id))
        return 0;  /* Not our allocation */

    *result = CUDA_SUCCESS;

    if (is_managed) {
        /* UVM allocation: free via CUDA directly */
        gb_log("freeing UVM buffer: ptr=%p size=%zuMB",
               (void *)(uintptr_t)ptr, size >> 20);

        *result = real_cuMemFree_v2(ptr);
        if (*result != CUDA_SUCCESS) {
            ht_insert(ptr, size, is_managed, buf_id, mapped_ptr);
            gb_log_err("cuMemFree failed for UVM ptr %p: %d",
                       (void *)(uintptr_t)ptr, *result);
            return 1;
        }

        InterlockedAdd64(&gb_config.UvmAllocatedBytes, -(LONG64)size);
    } else {
        /* Driver allocation: unregister from CUDA, then free via IOCTL */
        gb_log("freeing driver buffer: ptr=%p size=%zuMB buf_id=%d",
               (void *)(uintptr_t)ptr, size >> 20, buf_id);

        if (mapped_ptr && real_cuMemHostUnregister) {
            *result = real_cuMemHostUnregister(mapped_ptr);
            if (*result != CUDA_SUCCESS) {
                ht_insert(ptr, size, is_managed, buf_id, mapped_ptr);
                return 1;
            }
        }

        gb_free_driver_buf(buf_id);
    }

    return 1;
}

/*
 * Allocate via CUDA Unified Virtual Memory.
 * NVIDIA owns placement under the platform's managed-memory capabilities.
 * Returns CUDA_SUCCESS on success, error code on failure.
 */
static CUresult gb_alloc_uvm(void **devPtr, size_t size)
{
    CUdeviceptr dptr = 0;
    CUresult ret;
    CUdevice device = 0;
    int concurrent = 0;

    /* Use the allocating context's device, including after device switches. */
    if (real_cuCtxGetDevice)
        real_cuCtxGetDevice(&device);

    ret = real_cuMemAllocManaged_drv(&dptr, size, CU_MEM_ATTACH_GLOBAL);
    if (ret != CUDA_SUCCESS) {
        gb_log("cuMemAllocManaged failed: %d for %zuMB", ret, size >> 20);
        return ret;
    }

    /* Prefetch to GPU -- bring pages into VRAM proactively.
     * Uses NULL stream (default): subsequent CUDA work on this stream
     * will wait for prefetch to complete. Correct for weight loading. */
    if (real_cuDeviceGetAttribute)
        real_cuDeviceGetAttribute(&concurrent, 89, device);
    if (concurrent && real_cuMemPrefetchAsync) {
        CUresult pfRet = real_cuMemPrefetchAsync(dptr, size,
                                                  device, NULL);
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

    gb_log("UVM alloc: %zuMB -> ptr %p (context device %d)",
           size >> 20, *devPtr, device);

    return CUDA_SUCCESS;
}

/* ================================================================== */
/*  CUDA hook implementations                                           */
/* ================================================================== */



static CUresult CUDAAPI hooked_cuMemAlloc_v2(CUdeviceptr *dptr, size_t bytesize)
{
    CUresult ret;
    CUdevice device;
    void *ptr = NULL;
    if (!gb_config.Initialized || !dptr || !gb_extra_bytes ||
        bytesize < gb_config.ThresholdBytes)
        return real_cuMemAlloc_v2(dptr, bytesize);

    /* Preserve native allocation semantics and bandwidth when it fits.
     * Only capacity failure permits a second memory path. In particular,
     * pending CUDA errors and invalid contexts must reach the application. */
    if (!gb_managed_first) {
        ret = real_cuMemAlloc_v2(dptr, bytesize);
        if (ret != CUDA_ERROR_OUT_OF_MEMORY) {
            if (ret == CUDA_SUCCESS)
                gb_log("native alloc: %zuMB -> ptr %p", bytesize >> 20,
                       (void *)(uintptr_t)*dptr);
            return ret;
        }
        gb_log("native capacity exhausted for %zuMB; checking managed memory",
               bytesize >> 20);
    }

    /* Capability belongs to the current context. A pre-init call or one
     * unsupported card cannot permanently disable another valid context. */
    if (!real_cuCtxGetDevice ||
        real_cuCtxGetDevice(&device) != CUDA_SUCCESS ||
        !gb_extra_for_device(device))
        return real_cuMemAlloc_v2(dptr, bytesize);

    ret = gb_alloc_uvm(&ptr, bytesize);
    if (ret == CUDA_SUCCESS) {
        *dptr = (CUdeviceptr)(uintptr_t)ptr;
        return ret;
    }
    if (ret != CUDA_ERROR_OUT_OF_MEMORY && ret != CUDA_ERROR_NOT_SUPPORTED)
        return ret;
    if (gb_legacy_driver) {
        ret = gb_alloc_from_driver(&ptr, bytesize);
        if (ret == CUDA_SUCCESS) {
            *dptr = (CUdeviceptr)(uintptr_t)ptr;
            return ret;
        }
    }
    return gb_managed_first ? real_cuMemAlloc_v2(dptr, bytesize) : ret;
}

static CUresult CUDAAPI hooked_cuMemFree_v2(CUdeviceptr dptr)
{
    CUresult result;
    if (dptr && gb_free_buffer(dptr, &result))
        return result;
    return real_cuMemFree_v2(dptr);
}



/* ================================================================== */
/*  CUDA capacity queries: physical memory plus the RAM allowance       */
/* ================================================================== */

static CUresult CUDAAPI hooked_cuDeviceTotalMem_v2(size_t *bytes, CUdevice dev)
{
    CUresult ret = real_cuDeviceTotalMem_v2(bytes, dev);
    if (ret == CUDA_SUCCESS && gb_config.Initialized)
        *bytes += gb_extra_for_device(dev);
    return ret;
}

static CUresult CUDAAPI hooked_cuMemGetInfo(size_t *free, size_t *total)
{
    CUresult ret = real_cuMemGetInfo(free, total);
    CUdevice device;
    MEMORYSTATUSEX memory = { sizeof(memory) };
    if (ret == CUDA_SUCCESS && gb_config.Initialized && real_cuCtxGetDevice &&
        real_cuCtxGetDevice(&device) == CUDA_SUCCESS) {
        size_t extra = gb_extra_for_device(device);
        size_t used = (size_t)InterlockedCompareExchange64(&gb_config.UvmAllocatedBytes, 0, 0);
        size_t available = used < extra ? extra - used : 0;
        if (!GlobalMemoryStatusEx(&memory)) available = 0;
        else {
            size_t headroom = memory.ullAvailPhys > gb_reserve_bytes
                ? (size_t)memory.ullAvailPhys - gb_reserve_bytes : 0;
            if (available > headroom) available = headroom;
        }
        *total += extra;
        *free += available;
        if (*free > *total) *free = *total;
    }
    return ret;
}

/* ================================================================== */
/*  Configuration — read from registry                                  */
/* ================================================================== */

/* CUDA runtimes resolve internal entry points through these APIs. Patching
 * nvcuda's exported stubs alone does not intercept those returned pointers.
 * Resolve with NVIDIA first so unsupported symbols, versions and flags keep
 * their documented result. The size_t ABI for these calls starts at 3020.
 */
static CUresult CUDAAPI hooked_cuGetProcAddress(const char *, void **, int, unsigned long long);
static CUresult CUDAAPI hooked_cuGetProcAddress_v2(const char *, void **, int, unsigned long long, int *);

static void gb_redirect_entry(const char *symbol, void **fn, int version)
{
    if (!fn || !*fn || version < 3020)
        return;
    if (strcmp(symbol, "cuGetProcAddress") == 0) {
        /* CUDA bootstraps another resolver through the resolver itself.
         * CUDA 12 added the fifth (symbol status) argument to that ABI. */
        *fn = version >= 12000 ? (void *)hooked_cuGetProcAddress_v2
                               : (void *)hooked_cuGetProcAddress;
    } else if (strcmp(symbol, "cuMemAlloc") == 0)
        *fn = (void *)hooked_cuMemAlloc_v2;
    else if (strcmp(symbol, "cuMemFree") == 0)
        *fn = (void *)hooked_cuMemFree_v2;
    else if (strcmp(symbol, "cuMemGetInfo") == 0)
        *fn = (void *)hooked_cuMemGetInfo;
    else if (strcmp(symbol, "cuDeviceTotalMem") == 0)
        *fn = (void *)hooked_cuDeviceTotalMem_v2;
}

static CUresult CUDAAPI hooked_cuGetProcAddress(
    const char *symbol, void **fn, int version, unsigned long long flags)
{
    CUresult result = real_cuGetProcAddress(symbol, fn, version, flags);
    gb_log("lookup v1: %s version=%d flags=%llu result=%d entry=%p", symbol, version, flags, result, result == CUDA_SUCCESS && fn ? *fn : NULL);
    if (result == CUDA_SUCCESS)
        gb_redirect_entry(symbol, fn, version);
    return result;
}

static CUresult CUDAAPI hooked_cuGetProcAddress_v2(
    const char *symbol, void **fn, int version, unsigned long long flags, int *status)
{
    CUresult result = real_cuGetProcAddress_v2(symbol, fn, version, flags, status);
    gb_log("lookup v2: %s version=%d flags=%llu result=%d entry=%p", symbol, version, flags, result, result == CUDA_SUCCESS && fn ? *fn : NULL);
    if (result == CUDA_SUCCESS && (!status || *status == 0))
        gb_redirect_entry(symbol, fn, version);
    return result;
}

/* Values are scalar byte-accounting limits, not reserved memory. */
static size_t gb_env_mb(const char *name, size_t fallback)
{
    const char *text = getenv(name);
    char *end;
    unsigned long long value;
    if (!text) return fallback;
    value = _strtoui64(text, &end, 10);
    if (!text[0] || text[0] == '-' || *end || value > (SIZE_MAX >> 20)) {
        gb_log_err("invalid %s; disabling this allowance", name);
        return 0;
    }
    return (size_t)value << 20;
}

static void gb_read_config(void)
{
    HKEY key;
    DWORD type, size, value;
    MEMORYSTATUSEX memory = { sizeof(memory) };
    size_t requested = SIZE_MAX, available = 0;
    const char *debug;
    gb_config.ThresholdMb = 256;
    gb_config.DebugMode = 0;
    gb_config.DeviceHandle = INVALID_HANDLE_VALUE;
    gb_reserve_bytes = gb_env_mb("GREENBOOST_RESERVE_MB", (size_t)8192 << 20);

    /* Retain existing threshold/debug and RAM-cap settings, with env last.
     * PhysicalVramGb is obsolete: the driver supplies the actual GPU total. */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Services\\GreenBoost\\Parameters",
            0, KEY_READ, &key) == ERROR_SUCCESS ||
        RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\GreenBoost", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        size = sizeof(value);
        if (RegQueryValueExA(key, "VirtualVramGb", NULL, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS && type == REG_DWORD)
            requested = (size_t)value << 30;
        size = sizeof(value);
        if (RegQueryValueExA(key, "ThresholdMb", NULL, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS && type == REG_DWORD && value)
            gb_config.ThresholdMb = value;
        size = sizeof(value);
        if (RegQueryValueExA(key, "DebugMode", NULL, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS && type == REG_DWORD)
            gb_config.DebugMode = value;
        RegCloseKey(key);
    }
    debug = getenv("GREENBOOST_DEBUG");
    if (debug) gb_config.DebugMode = debug[0] == '1';
    {
        const char *policy = getenv("GREENBOOST_ALLOC_POLICY");
        const char *legacy = getenv("GREENBOOST_LEGACY_DRIVER");
        gb_managed_first = policy && strcmp(policy, "managed") == 0;
        gb_legacy_driver = legacy && strcmp(legacy, "1") == 0;
        if (policy && strcmp(policy, "adaptive") && strcmp(policy, "managed")) {
            gb_log_err("invalid GREENBOOST_ALLOC_POLICY; using adaptive policy");
        }
    }
    gb_config.ThresholdBytes = gb_env_mb("GREENBOOST_THRESHOLD_MB", (size_t)gb_config.ThresholdMb << 20);
    if (!gb_config.ThresholdBytes) gb_config.ThresholdBytes = (size_t)256 << 20;
    gb_config.ThresholdMb = (ULONG)(gb_config.ThresholdBytes >> 20);
    if (GlobalMemoryStatusEx(&memory)) {
        available = memory.ullAvailPhys > gb_reserve_bytes
            ? (size_t)memory.ullAvailPhys - gb_reserve_bytes : 0;
        if (requested == SIZE_MAX) requested = (size_t)memory.ullAvailPhys / 2;
    } else requested = 0;
    requested = gb_env_mb("GREENBOOST_EXTRA_MB", requested);
    gb_extra_bytes = requested < available ? requested : available;
    gb_extra_bytes &= ~(((size_t)1 << 20) - 1);
    gb_log("config: extra=%zuMB reserve=%zuMB threshold=%zuMB (physical GPU capacity queried per device)",
        gb_extra_bytes >> 20, gb_reserve_bytes >> 20, gb_config.ThresholdBytes >> 20);
    gb_log("allocation policy=%s legacy_driver=%d",
           gb_managed_first ? "managed" : "adaptive", gb_legacy_driver);
}

/* ================================================================== */
/*  Symbol resolution — load real CUDA functions                        */
/* ================================================================== */

static void gb_resolve_symbols(void)
{
    /* CUDA runtimes reach this driver boundary after creating a context.
     * Do not load a particular runtime version or modify NVML telemetry. */
    hCudaDrv = LoadLibraryA("nvcuda.dll");
    if (!hCudaDrv) {
        gb_log_err("failed to load nvcuda.dll");
        return;
    }

    /* Resolve driver API functions */
    real_cuDeviceGetAttribute = (CUresult (*)(int *, int, CUdevice))
        GetProcAddress(hCudaDrv, "cuDeviceGetAttribute");
    real_cuMemAlloc_v2 = (pfn_cuMemAlloc_v2)
        GetProcAddress(hCudaDrv, "cuMemAlloc_v2");
    real_cuGetProcAddress = (pfn_cuGetProcAddress)
        GetProcAddress(hCudaDrv, "cuGetProcAddress");
    real_cuGetProcAddress_v2 = (pfn_cuGetProcAddress_v2)
        GetProcAddress(hCudaDrv, "cuGetProcAddress_v2");
    real_cuMemFree_v2 = (pfn_cuMemFree_v2)
        GetProcAddress(hCudaDrv, "cuMemFree_v2");
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

    /* Resolve PrefetchVirtualMemory (Win8+) */
    {
        HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
        if (hKernel32) {
            pPrefetchVirtualMemory = (pfn_PrefetchVirtualMemory)
                GetProcAddress(hKernel32, "PrefetchVirtualMemory");
        }
    }

    gb_log("symbols resolved: cuMemAlloc=%p cuMemHostRegister=%p "
           "cuMemAllocManaged=%p cuMemPrefetchAsync=%p",
           (void*)real_cuMemAlloc_v2,
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

    DetourAttach((PVOID*)&real_CreateProcessA, hooked_CreateProcessA);
    DetourAttach((PVOID*)&real_CreateProcessW, hooked_CreateProcessW);

    if (real_cuGetProcAddress)
        DetourAttach((PVOID*)&real_cuGetProcAddress, hooked_cuGetProcAddress);
    if (real_cuGetProcAddress_v2)
        DetourAttach((PVOID*)&real_cuGetProcAddress_v2, hooked_cuGetProcAddress_v2);

    if (real_cuMemAlloc_v2)
        DetourAttach((PVOID*)&real_cuMemAlloc_v2, hooked_cuMemAlloc_v2);
    if (real_cuMemFree_v2)
        DetourAttach((PVOID*)&real_cuMemFree_v2, hooked_cuMemFree_v2);
    if (real_cuDeviceTotalMem_v2)
        DetourAttach((PVOID*)&real_cuDeviceTotalMem_v2, hooked_cuDeviceTotalMem_v2);
    if (real_cuMemGetInfo)
        DetourAttach((PVOID*)&real_cuMemGetInfo, hooked_cuMemGetInfo);

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

    DetourDetach((PVOID*)&real_CreateProcessA, hooked_CreateProcessA);
    DetourDetach((PVOID*)&real_CreateProcessW, hooked_CreateProcessW);

    if (real_cuGetProcAddress)
        DetourDetach((PVOID*)&real_cuGetProcAddress, hooked_cuGetProcAddress);
    if (real_cuGetProcAddress_v2)
        DetourDetach((PVOID*)&real_cuGetProcAddress_v2, hooked_cuGetProcAddress_v2);

    if (real_cuMemAlloc_v2)
        DetourDetach((PVOID*)&real_cuMemAlloc_v2, hooked_cuMemAlloc_v2);
    if (real_cuMemFree_v2)
        DetourDetach((PVOID*)&real_cuMemFree_v2, hooked_cuMemFree_v2);
    if (real_cuDeviceTotalMem_v2)
        DetourDetach((PVOID*)&real_cuDeviceTotalMem_v2, hooked_cuDeviceTotalMem_v2);
    if (real_cuMemGetInfo)
        DetourDetach((PVOID*)&real_cuMemGetInfo, hooked_cuMemGetInfo);

    DetourTransactionCommit();
    gb_log("hooks removed");
}



#endif /* GB_USE_DETOURS */

/* ================================================================== */
/*  Initialization and cleanup                                          */
/* ================================================================== */

static void gb_shim_init(void)
{
    UINT i;

    gb_log("=== GreenBoost 2.4 CUDA Shim (Windows) ===");

    /* Ensure DeviceHandle is INVALID_HANDLE_VALUE before any code path
     * could touch it. Zero-init sets it to 0 which is a valid handle. */
    gb_config.DeviceHandle = INVALID_HANDLE_VALUE;

    /* Initialize hash table locks */
    for (i = 0; i < HT_LOCKS; i++)
        InitializeCriticalSectionAndSpinCount(&ht_locks[i], 4000);

    /* Clear hash table */
    memset(gb_htable, 0, sizeof(gb_htable));

    /* Managed capability is queried on the allocating context. */
    gb_config.GpuDevice = 0;
    gb_config.UvmAllocatedBytes = 0;

    /* Read configuration */
    gb_read_config();

    /* Resolve real CUDA symbols */
    gb_resolve_symbols();

    /* Validate we have the minimum required symbols */
    if (!real_cuMemAlloc_v2 || !real_cuMemFree_v2 || !real_cuMemGetInfo ||
        !real_cuDeviceTotalMem_v2 || !real_cuCtxGetDevice) {
        gb_log_err("critical CUDA symbols not found — shim disabled");
        gb_config.Initialized = FALSE;
        return;
    }

    /* Start prefetch worker thread */
    InitializeCriticalSectionAndSpinCount(&prefetch_cs, 4000);
    InitializeConditionVariable(&prefetch_cv);
    prefetch_running = gb_legacy_driver;
    if (gb_legacy_driver)
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
    gb_log("additional RAM allowance: %zuMB", gb_extra_bytes >> 20);
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
#if GB_USE_DETOURS
    if (DetourIsHelperProcess()) return TRUE;
#endif

    switch (reason) {
    case DLL_PROCESS_ATTACH:
#if GB_USE_DETOURS
        DetourRestoreAfterWith();
        {
            DWORD length = GetModuleFileNameA(hModule, gb_dll_path, sizeof(gb_dll_path));
            if (!length || length >= sizeof(gb_dll_path)) return FALSE;
        }
#endif
        DisableThreadLibraryCalls(hModule);
        gb_shim_init();
        break;

    case DLL_PROCESS_DETACH:
        /* ExitProcess has already stopped other threads. CUDA and Windows
         * own process teardown; calling into them under loader lock is unsafe. */
        if (!reserved) gb_shim_cleanup();
        break;
    }

    return TRUE;
}

