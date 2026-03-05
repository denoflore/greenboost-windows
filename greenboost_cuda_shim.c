/*
 * GreenBoost v2.0 — CUDA LD_PRELOAD memory shim
 *
 * Intercepts CUDA memory allocations and routes large allocations to
 * GreenBoost DDR4 via DMA-BUF + cudaImportExternalMemory (primary path)
 * or cuMemAllocManaged UVM (fallback).  Exposes combined 12 GB VRAM +
 * 51 GB DDR4 pool to CUDA applications without code changes.
 *
 * USAGE:
 *   LD_PRELOAD=/usr/local/lib/libgreenboost_cuda.so  ./your_cuda_app
 *
 * ENVIRONMENT VARIABLES:
 *   GREENBOOST_USE_DMA_BUF    1 = use DMA-BUF import (default), 0 = UVM only
 *   GREENBOOST_VRAM_HEADROOM_MB  keep ≥ this many MB free in VRAM (default 1024)
 *   GREENBOOST_DEBUG          1 = verbose logging to stderr
 *
 * PREREQUISITES:
 *   greenboost.ko loaded:   sudo insmod greenboost.ko
 *   nvidia_uvm.ko loaded:   sudo modprobe nvidia_uvm
 *
 * Author  : Ferran Duarri
 * License : GPL v2
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <sys/mman.h>
#include "greenboost_ioctl.h"   /* gb_alloc_req, GB_IOCTL_ALLOC — userspace-safe */

/* ------------------------------------------------------------------ */
/*  Minimal CUDA type definitions (no CUDA SDK headers needed)         */
/* ------------------------------------------------------------------ */

typedef unsigned long long CUdeviceptr;
typedef int                CUresult;
typedef int                cudaError_t;
typedef unsigned int       CUmemAttach_flags;
typedef int                CUdevice;
typedef struct CUstream_st *CUstream;
typedef CUstream            cudaStream_t;

#define CUDA_SUCCESS                0
#define CUDA_ERROR_NOT_SUPPORTED    801
#define CUDA_ERROR_OUT_OF_MEMORY    2
#define CU_MEM_ATTACH_GLOBAL        0x1u
#define CU_MEM_ATTACH_HOST          0x2u

/* cudaExternalMemory types (runtime API, no SDK needed) */
typedef void *cudaExternalMemory_t;

typedef enum {
    cudaExternalMemoryHandleTypeOpaqueFd = 1,
} cudaExternalMemoryHandleType;

struct cudaExternalMemoryHandleDesc {
    cudaExternalMemoryHandleType type;
    union {
        int fd;
        struct { void *handle; const char *name; } win32;
    } handle;
    unsigned long long size;
    unsigned int flags;
};

struct cudaExternalMemoryBufferDesc {
    unsigned long long offset;
    unsigned long long size;
    unsigned int flags;
};

/* ------------------------------------------------------------------ */
/*  Open-addressed hash map — replaces alloc_table[65536]              */
/*  131072 slots × 64 bytes = 8 MB, aligned for cache-line access      */
/* ------------------------------------------------------------------ */

static int    gb_debug              = 0;

#define HT_BITS   17u
#define HT_SIZE   (1u << HT_BITS)
#define HT_MASK   (HT_SIZE - 1u)
#define HT_LOCKS  64u

/* ------------------------------------------------------------------ */
/*  Async Prefetching Queue & Worker                                    */
/* ------------------------------------------------------------------ */

#define PREFETCH_QUEUE_SIZE 256
typedef struct {
    void *mapped_ptr;
    size_t size;
} prefetch_req_t;

static prefetch_req_t prefetch_queue[PREFETCH_QUEUE_SIZE];
static int prefetch_head = 0;
static int prefetch_tail = 0;
static pthread_mutex_t prefetch_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t prefetch_cond = PTHREAD_COND_INITIALIZER;
static pthread_t prefetch_thread;

static void* prefetch_worker(void* arg) {
    while (1) {
        prefetch_req_t req;
        pthread_mutex_lock(&prefetch_mutex);
        while (prefetch_head == prefetch_tail) {
            pthread_cond_wait(&prefetch_cond, &prefetch_mutex);
        }
        req = prefetch_queue[prefetch_tail];
        prefetch_tail = (prefetch_tail + 1) % PREFETCH_QUEUE_SIZE;
        pthread_mutex_unlock(&prefetch_mutex);

        /* Hint the kernel to bring the pages into RAM */
        madvise(req.mapped_ptr, req.size, MADV_WILLNEED);
        /* Use fprintf if gb_debug is on, since we can't use gb_log safely here without moving the macro */
        if (gb_debug) fprintf(stderr, "[GreenBoost] prefetch thread: madvise(MADV_WILLNEED) on %p size=%zu\n", req.mapped_ptr, req.size);
    }
    return NULL;
}

static void enqueue_prefetch(void *mapped_ptr, size_t size) {
    if (!mapped_ptr) return;
    pthread_mutex_lock(&prefetch_mutex);
    int next_head = (prefetch_head + 1) % PREFETCH_QUEUE_SIZE;
    if (next_head != prefetch_tail) {
        prefetch_queue[prefetch_head].mapped_ptr = mapped_ptr;
        prefetch_queue[prefetch_head].size = size;
        prefetch_head = next_head;
        pthread_cond_signal(&prefetch_cond);
    }
    pthread_mutex_unlock(&prefetch_mutex);
}

typedef struct {
    CUdeviceptr           ptr;          /* 8 B  — 0 = empty slot         */
    size_t                size;         /* 8 B                            */
    int                   is_managed;   /* 4 B  — 1 = UVM, 0 = device    */
    int                   gb_buf_id;    /* 4 B  — -1 if not DMA-BUF      */
    void                 *mapped_ptr;   /* 8 B  — user-space mmap ptr    */
    int                   fd;           /* 4 B  — DMA-BUF fd             */
    uint8_t               _pad[28];     /* pad to 64 bytes                */
} __attribute__((aligned(64))) gb_ht_entry_t;

static gb_ht_entry_t      gb_htable[HT_SIZE];
static pthread_mutex_t    ht_locks[HT_LOCKS];

static inline uint32_t ht_hash(CUdeviceptr ptr)
{
    /* Fibonacci hash — good distribution for pointer-sized keys */
    return (uint32_t)((ptr * 0x9E3779B97F4A7C15ULL) >> (64 - HT_BITS));
}

static inline pthread_mutex_t *ht_lock(uint32_t h)
{
    return &ht_locks[h & (HT_LOCKS - 1u)];
}

/* Returns 1 on success, 0 if table is full. */
static int ht_insert(CUdeviceptr ptr, size_t size, int is_managed,
                     int gb_buf_id, void *mapped_ptr, int fd)
{
    uint32_t h = ht_hash(ptr);
    uint32_t i;
    for (i = 0; i < HT_SIZE; i++) {
        gb_ht_entry_t *e = &gb_htable[(h + i) & HT_MASK];
        pthread_mutex_t *lk = ht_lock((h + i) & HT_MASK);
        pthread_mutex_lock(lk);
        if (e->ptr == 0) {
            e->ptr        = ptr;
            e->size       = size;
            e->is_managed = is_managed;
            e->gb_buf_id  = gb_buf_id;
            e->mapped_ptr = mapped_ptr;
            e->fd         = fd;
            pthread_mutex_unlock(lk);
            return 1;
        }
        pthread_mutex_unlock(lk);
    }
    return 0; /* table full */
}

/* Returns 1 if found, fills *out_size, *out_managed, *out_mapped_ptr, *out_fd. */
static int ht_remove(CUdeviceptr ptr, size_t *out_size, int *out_managed,
                     void **out_mapped_ptr, int *out_fd)
{
    uint32_t h = ht_hash(ptr);
    uint32_t i;
    for (i = 0; i < HT_SIZE; i++) {
        gb_ht_entry_t *e = &gb_htable[(h + i) & HT_MASK];
        pthread_mutex_t *lk = ht_lock((h + i) & HT_MASK);
        pthread_mutex_lock(lk);
        if (e->ptr == ptr) {
            if (out_size)       *out_size       = e->size;
            if (out_managed)    *out_managed    = e->is_managed;
            if (out_mapped_ptr) *out_mapped_ptr = e->mapped_ptr;
            if (out_fd)         *out_fd         = e->fd;
            memset(e, 0, sizeof(*e));
            pthread_mutex_unlock(lk);
            return 1;
        }
        pthread_mutex_unlock(lk);
        if (e->ptr == 0)
            break; /* empty slot — key not present */
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Function pointer types                                              */
/* ------------------------------------------------------------------ */

typedef CUresult    (*pfn_cuMemAlloc_v2)(CUdeviceptr *, size_t);
typedef CUresult    (*pfn_cuMemFree_v2)(CUdeviceptr);
typedef CUresult    (*pfn_cuMemAllocManaged)(CUdeviceptr *, size_t, CUmemAttach_flags);
typedef CUresult    (*pfn_cuMemGetInfo)(size_t *, size_t *);
typedef CUresult    (*pfn_cuMemAllocAsync)(CUdeviceptr *, size_t, CUstream);
typedef cudaError_t (*pfn_cudaMalloc)(void **, size_t);
typedef cudaError_t (*pfn_cudaFree)(void *);
typedef cudaError_t (*pfn_cudaMallocManaged)(void **, size_t, unsigned int);
typedef cudaError_t (*pfn_cudaMallocAsync)(void **, size_t, cudaStream_t);
typedef cudaError_t (*pfn_cudaImportExternalMemory)(cudaExternalMemory_t *,
                                                    const struct cudaExternalMemoryHandleDesc *);
typedef cudaError_t (*pfn_cudaExternalMemoryGetMappedBuffer)(void **, cudaExternalMemory_t,
                                                             const struct cudaExternalMemoryBufferDesc *);
typedef cudaError_t (*pfn_cudaDestroyExternalMemory)(cudaExternalMemory_t);
typedef cudaError_t (*pfn_cudaMemGetInfo)(size_t *, size_t *);
typedef CUresult    (*pfn_cuDeviceTotalMem_v2)(size_t *, CUdevice);

/* New hooks for mmap DMA-BUF path */
typedef CUresult    (*pfn_cuMemHostRegister)(void *, size_t, unsigned int);
typedef CUresult    (*pfn_cuMemHostGetDevicePointer)(CUdeviceptr *, void *, unsigned int);
#define CU_MEMHOSTREGISTER_PORTABLE     0x01
#define CU_MEMHOSTREGISTER_DEVICEMAP    0x02
#define CU_MEMHOSTREGISTER_IOMEMORY     0x2000000

/* Prefetch API hooks */
typedef CUresult    (*pfn_cuMemPrefetchAsync)(CUdeviceptr, size_t, CUdevice, CUstream);
typedef cudaError_t (*pfn_cudaMemPrefetchAsync)(const void *, size_t, int, cudaStream_t);

/* NVML types (minimal — avoids libnvidia-ml dependency) */
typedef void *nvmlDevice_t;
typedef unsigned int nvmlReturn_t;
#define NVML_SUCCESS 0
typedef struct { unsigned long long total; unsigned long long free; unsigned long long used; } nvmlMemory_t;
typedef struct { unsigned int version; unsigned long long total; unsigned long long reserved;
                 unsigned long long free; unsigned long long used; } nvmlMemory_v2_t;
typedef nvmlReturn_t (*pfn_nvmlDeviceGetMemoryInfo)(nvmlDevice_t, nvmlMemory_t *);
typedef nvmlReturn_t (*pfn_nvmlDeviceGetMemoryInfo_v2)(nvmlDevice_t, nvmlMemory_v2_t *);

/* ------------------------------------------------------------------ */
/*  Global state                                                        */
/* ------------------------------------------------------------------ */

static pfn_cuMemAlloc_v2                   real_cuMemAlloc_v2;
static pfn_cuMemFree_v2                    real_cuMemFree_v2;
static pfn_cuMemAllocManaged               real_cuMemAllocManaged;
static pfn_cuMemGetInfo                    real_cuMemGetInfo;
static pfn_cuMemAllocAsync                 real_cuMemAllocAsync;
static pfn_cudaMalloc                      real_cudaMalloc;
static pfn_cudaFree                        real_cudaFree;
static pfn_cudaMallocManaged               real_cudaMallocManaged;
static pfn_cudaMallocAsync                 real_cudaMallocAsync;
static pfn_cudaImportExternalMemory        real_cudaImportExternalMemory;
static pfn_cudaExternalMemoryGetMappedBuffer real_cudaExternalMemoryGetMappedBuffer;
static pfn_cudaDestroyExternalMemory       real_cudaDestroyExternalMemory;
static pfn_cudaMemGetInfo                  real_cudaMemGetInfo;
static pfn_cuDeviceTotalMem_v2             real_cuDeviceTotalMem_v2;
static pfn_nvmlDeviceGetMemoryInfo         real_nvmlDeviceGetMemoryInfo;
static pfn_nvmlDeviceGetMemoryInfo_v2      real_nvmlDeviceGetMemoryInfo_v2;

static pfn_cuMemHostRegister               real_cuMemHostRegister;
static pfn_cuMemHostGetDevicePointer       real_cuMemHostGetDevicePointer;
static pfn_cuMemPrefetchAsync              real_cuMemPrefetchAsync;
static pfn_cudaMemPrefetchAsync            real_cudaMemPrefetchAsync;

static size_t vram_headroom_bytes   = 1024ULL * 1024 * 1024; /* 1 GB */
static size_t gb_virtual_vram_bytes = 51ULL * 1024 * 1024 * 1024; /* DDR4 pool — reported to CUDA */
/* DMA-BUF mmap+register is the primary path now. */
static int    gb_use_dmabuf         = 1;
static int    initialized           = 0;

/* /dev/greenboost fd — opened lazily on first DMA-BUF allocation */
static int        gb_dev_fd       = -1;
static pthread_mutex_t gb_dev_lock = PTHREAD_MUTEX_INITIALIZER;

/* Moved the macro definition up so it can be used anywhere */
#define gb_log(fmt, ...) \
    do { if (gb_debug) fprintf(stderr, "[GreenBoost] " fmt "\n", ##__VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ */
/*  GreenBoost /dev/greenboost helper                                  */
/* ------------------------------------------------------------------ */

static int gb_open_device(void)
{
    pthread_mutex_lock(&gb_dev_lock);
    if (gb_dev_fd < 0) {
        gb_dev_fd = open("/dev/greenboost", O_RDWR | O_CLOEXEC);
        if (gb_dev_fd < 0)
            fprintf(stderr, "[GreenBoost] Cannot open /dev/greenboost: %m\n");
    }
    pthread_mutex_unlock(&gb_dev_lock);
    return gb_dev_fd;
}

/* ------------------------------------------------------------------ */
/*  DMA-BUF import path: allocate DDR4 via GreenBoost, import as CUDA  */
/* ------------------------------------------------------------------ */

static CUresult gb_import_as_cuda_ptr(CUdeviceptr *dptr, size_t bytesize,
                                       void **mapped_ptr_out, int *fd_out)
{
    struct gb_alloc_req req;
    void *mapped_ptr;
    int fd;
    CUresult ret;

    if (!real_cuMemHostRegister || !real_cuMemHostGetDevicePointer)
        return CUDA_ERROR_NOT_SUPPORTED;

    fd = gb_open_device();
    if (fd < 0)
        return CUDA_ERROR_NOT_SUPPORTED;

    memset(&req, 0, sizeof(req));
    req.size  = bytesize;
    req.flags = GB_ALLOC_WEIGHTS;

    if (ioctl(fd, GB_IOCTL_ALLOC, &req) < 0) {
        fprintf(stderr, "[GreenBoost] GB_IOCTL_ALLOC failed for %zu MB: %m\n",
                bytesize >> 20);
        return CUDA_ERROR_OUT_OF_MEMORY;
    }

    /* Map the DMA-BUF fd into userspace */
    mapped_ptr = mmap(NULL, bytesize, PROT_READ | PROT_WRITE, MAP_SHARED, req.fd, 0);
    if (mapped_ptr == MAP_FAILED) {
        fprintf(stderr, "[GreenBoost] mmap failed for %zu MB: %m\n", bytesize >> 20);
        close(req.fd);
        return CUDA_ERROR_OUT_OF_MEMORY;
    }

    /* Register the userspace pointer with CUDA */
    ret = real_cuMemHostRegister(mapped_ptr, bytesize, CU_MEMHOSTREGISTER_DEVICEMAP);
    if (ret != CUDA_SUCCESS) {
        fprintf(stderr, "[GreenBoost] cuMemHostRegister FAILED ret=%d for %zu MB\n",
                ret, bytesize >> 20);
        munmap(mapped_ptr, bytesize);
        close(req.fd);
        return CUDA_ERROR_OUT_OF_MEMORY;
    }

    /* Get the device pointer for the registered memory */
    ret = real_cuMemHostGetDevicePointer(dptr, mapped_ptr, 0);
    if (ret != CUDA_SUCCESS) {
        fprintf(stderr, "[GreenBoost] cuMemHostGetDevicePointer FAILED ret=%d\n", ret);
        /* CUDA unregister takes no size, just the pointer */
        void (*cuMemHostUnregister)(void*) = dlsym(RTLD_NEXT, "cuMemHostUnregister");
        if (cuMemHostUnregister) cuMemHostUnregister(mapped_ptr);
        munmap(mapped_ptr, bytesize);
        close(req.fd);
        return CUDA_ERROR_OUT_OF_MEMORY;
    }

    *mapped_ptr_out = mapped_ptr;
    *fd_out = req.fd;
    gb_log("DMA-BUF import: %zu MB at cuda_ptr=0x%llx (mapped=%p, fd=%d)",
           bytesize >> 20, (unsigned long long)*dptr, mapped_ptr, req.fd);
    return CUDA_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Constructor — runs before main()                                    */
/* ------------------------------------------------------------------ */

__attribute__((constructor))
static void gb_shim_init(void)
{
    void *libcuda, *libcudart = NULL;
    const char *env;
    uint32_t i;

    env = getenv("GREENBOOST_USE_DMA_BUF");
    if (env) gb_use_dmabuf = (env[0] != '0');

    env = getenv("GREENBOOST_VRAM_HEADROOM_MB");
    if (env) vram_headroom_bytes = (size_t)atoll(env) * 1024ULL * 1024ULL;

    env = getenv("GREENBOOST_VIRTUAL_VRAM_MB");
    if (env) gb_virtual_vram_bytes = (size_t)atoll(env) * 1024ULL * 1024ULL;

    env = getenv("GREENBOOST_DEBUG");
    if (env && env[0] == '1') gb_debug = 1;

    /* Initialize lock arrays */
    for (i = 0; i < HT_LOCKS; i++)
        pthread_mutex_init(&ht_locks[i], NULL);

    /* Start prefetch thread */
    pthread_create(&prefetch_thread, NULL, prefetch_worker, NULL);

    libcuda = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!libcuda) {
        fprintf(stderr, "[GreenBoost] WARNING: cannot open libcuda.so.1: %s\n", dlerror());
        return;
    }

    /* libcudart is Ollama-bundled — try versioned/Ollama-specific paths */
    {
        static const char *cudart_paths[] = {
            "libcudart.so",
            "libcudart.so.13",
            "libcudart.so.12",
            "/usr/local/lib/ollama/cuda_v13/libcudart.so.13.0.96",
            "/usr/local/lib/ollama/mlx_cuda_v13/libcudart.so",
            "/usr/local/lib/ollama/cuda_v12/libcudart.so.12",
            NULL
        };
        const char **p;
        for (p = cudart_paths; *p && !libcudart; p++) {
            libcudart = dlopen(*p, RTLD_NOW | RTLD_GLOBAL);
            if (libcudart)
                fprintf(stderr, "[GreenBoost] libcudart loaded: %s\n", *p);
        }
        if (!libcudart)
            fprintf(stderr, "[GreenBoost] WARNING: libcudart not found — runtime API resolved lazily\n");
    }

    /* Driver API (cu*) — always from libcuda.so.1 */
    real_cuMemAlloc_v2     = (pfn_cuMemAlloc_v2)     dlsym(libcuda, "cuMemAlloc_v2");
    real_cuMemFree_v2      = (pfn_cuMemFree_v2)      dlsym(libcuda, "cuMemFree_v2");
    real_cuMemAllocManaged = (pfn_cuMemAllocManaged)  dlsym(libcuda, "cuMemAllocManaged");
    real_cuMemAllocAsync   = (pfn_cuMemAllocAsync)    dlsym(libcuda, "cuMemAllocAsync");
    real_cuMemGetInfo      = (pfn_cuMemGetInfo)       dlsym(libcuda, "cuMemGetInfo_v2");
    if (!real_cuMemGetInfo)
        real_cuMemGetInfo  = (pfn_cuMemGetInfo)       dlsym(libcuda, "cuMemGetInfo");
    real_cuDeviceTotalMem_v2 = (pfn_cuDeviceTotalMem_v2) dlsym(libcuda, "cuDeviceTotalMem_v2");
    if (!real_cuDeviceTotalMem_v2)
        real_cuDeviceTotalMem_v2 = (pfn_cuDeviceTotalMem_v2) dlsym(libcuda, "cuDeviceTotalMem");
    real_cuMemHostRegister = (pfn_cuMemHostRegister) dlsym(libcuda, "cuMemHostRegister");
    real_cuMemHostGetDevicePointer = (pfn_cuMemHostGetDevicePointer) dlsym(libcuda, "cuMemHostGetDevicePointer");
    real_cuMemPrefetchAsync = (pfn_cuMemPrefetchAsync) dlsym(libcuda, "cuMemPrefetchAsync");

    /* NVML — loaded separately; Ollama uses this for GPU memory discovery */
    {
        void *libnvml = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!libnvml) libnvml = dlopen("libnvidia-ml.so", RTLD_NOW | RTLD_GLOBAL);
        if (libnvml) {
            real_nvmlDeviceGetMemoryInfo    = (pfn_nvmlDeviceGetMemoryInfo)
                dlsym(libnvml, "nvmlDeviceGetMemoryInfo");
            real_nvmlDeviceGetMemoryInfo_v2 = (pfn_nvmlDeviceGetMemoryInfo_v2)
                dlsym(libnvml, "nvmlDeviceGetMemoryInfo_v2");
        }
    }

    /* Runtime API (cuda*) — live in libcudart, not libcuda */
    if (libcudart) {
        real_cudaMalloc        = (pfn_cudaMalloc)        dlsym(libcudart, "cudaMalloc");
        real_cudaFree          = (pfn_cudaFree)           dlsym(libcudart, "cudaFree");
        real_cudaMallocManaged = (pfn_cudaMallocManaged)  dlsym(libcudart, "cudaMallocManaged");
        real_cudaMallocAsync   = (pfn_cudaMallocAsync)    dlsym(libcudart, "cudaMallocAsync");

        real_cudaImportExternalMemory        = (pfn_cudaImportExternalMemory)
            dlsym(libcudart, "cudaImportExternalMemory");
        real_cudaExternalMemoryGetMappedBuffer = (pfn_cudaExternalMemoryGetMappedBuffer)
            dlsym(libcudart, "cudaExternalMemoryGetMappedBuffer");
        real_cudaDestroyExternalMemory       = (pfn_cudaDestroyExternalMemory)
            dlsym(libcudart, "cudaDestroyExternalMemory");
        real_cudaMemGetInfo                  = (pfn_cudaMemGetInfo)
            dlsym(libcudart, "cudaMemGetInfo");
        real_cudaMemPrefetchAsync            = (pfn_cudaMemPrefetchAsync)
            dlsym(libcudart, "cudaMemPrefetchAsync");
    }
    /* Fallback: some CUDA versions export runtime wrappers from libcuda.so.1 */
    if (!real_cudaMalloc)        real_cudaMalloc        = (pfn_cudaMalloc)        dlsym(libcuda, "cudaMalloc");
    if (!real_cudaFree)          real_cudaFree          = (pfn_cudaFree)           dlsym(libcuda, "cudaFree");
    if (!real_cudaMallocManaged) real_cudaMallocManaged = (pfn_cudaMallocManaged)  dlsym(libcuda, "cudaMallocManaged");
    if (!real_cudaMallocAsync)   real_cudaMallocAsync   = (pfn_cudaMallocAsync)    dlsym(libcuda, "cudaMallocAsync");
    if (!real_cudaMemGetInfo)    real_cudaMemGetInfo    = (pfn_cudaMemGetInfo)     dlsym(libcuda, "cudaMemGetInfo");
    if (!real_cudaMemPrefetchAsync) real_cudaMemPrefetchAsync = (pfn_cudaMemPrefetchAsync) dlsym(libcuda, "cudaMemPrefetchAsync");

    if (!real_cuMemAlloc_v2 || !real_cuMemFree_v2) {
        fprintf(stderr, "[GreenBoost] WARNING: failed to resolve core CUDA symbols\n");
        return;
    }

    initialized = 1;

    fprintf(stderr, "[GreenBoost] v2.3 loaded — vram_headroom=%zuMB virtual_vram=%zuMB use_dmabuf=%d debug=%d\n",
            vram_headroom_bytes >> 20, gb_virtual_vram_bytes >> 20, gb_use_dmabuf, gb_debug);
    fprintf(stderr, "[GreenBoost] UVM overflow      : %s (primary DDR4 path)\n",
            real_cuMemAllocManaged ? "available" : "UNAVAILABLE — load nvidia_uvm.ko");
    fprintf(stderr, "[GreenBoost] DMA-BUF import   : %s\n",
            (real_cuMemHostRegister && gb_use_dmabuf) ? "enabled (mmap+register path)"
            : gb_use_dmabuf ? "wanted but cuMemHostRegister missing"
            : "disabled (set GREENBOOST_USE_DMA_BUF=1 to enable)");
    fprintf(stderr, "[GreenBoost] Async alloc hooks : cuMemAllocAsync=%s cudaMallocAsync=%s\n",
            real_cuMemAllocAsync ? "hooked" : "missing",
            real_cudaMallocAsync ? "hooked" : "missing");
    fprintf(stderr, "[GreenBoost] cuMemGetInfo hook : %s (reports +%zu MB virtual VRAM to CUDA)\n",
            real_cuMemGetInfo ? "active" : "missing",
            gb_virtual_vram_bytes >> 20);
    fprintf(stderr, "[GreenBoost] cuDeviceTotalMem  : %s\n",
            real_cuDeviceTotalMem_v2 ? "hooked" : "missing");
    fprintf(stderr, "[GreenBoost] nvmlMemInfo hook  : %s\n",
            real_nvmlDeviceGetMemoryInfo ? "hooked" : "missing (NVML not found)");
    fprintf(stderr, "[GreenBoost] dlsym hook        : active (intercepts dlopen+dlsym GPU API calls)\n");
    fprintf(stderr, "[GreenBoost] Combined VRAM     : 12 GB physical + %zu GB DDR4 via GreenBoost\n",
            gb_virtual_vram_bytes >> 30);
}

/* ------------------------------------------------------------------ */
/*  Destructor                                                          */
/* ------------------------------------------------------------------ */

__attribute__((destructor))
static void gb_shim_fini(void)
{
    if (gb_dev_fd >= 0) {
        close(gb_dev_fd);
        gb_dev_fd = -1;
    }
    gb_log("shim unloaded");
}

/* ------------------------------------------------------------------ */
/*  VRAM-aware overflow decision                                        */
/* ------------------------------------------------------------------ */

static int gb_needs_overflow(size_t bytesize)
{
    size_t free_vram = 0, total_vram = 0;

    if (!real_cuMemGetInfo)
        return 0;

    if (real_cuMemGetInfo(&free_vram, &total_vram) != CUDA_SUCCESS)
        return 0;

    if (bytesize + vram_headroom_bytes > free_vram) {
        gb_log("VRAM: req=%zuMB free=%zuMB headroom=%zuMB → OVERFLOW to DDR4",
               bytesize >> 20, free_vram >> 20, vram_headroom_bytes >> 20);
        return 1;
    }
    gb_log("VRAM: req=%zuMB free=%zuMB → fits", bytesize >> 20, free_vram >> 20);
    return 0;
}

/* Try UVM first (safe, no context corruption), DMA-BUF second (disabled by default) */
static CUresult gb_overflow_alloc(CUdeviceptr *dptr, size_t bytesize)
{
    void *mapped_ptr = NULL;
    int fd = -1;
    CUresult ret;

    /* ---- Path 1: DMA-BUF (primary path now) -------------------------- */
    if (gb_use_dmabuf && real_cuMemHostRegister) {
        /* Allocate anonymous memory using mmap, then ask greenboost.ko to pin it */
        mapped_ptr = mmap(NULL, bytesize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped_ptr == MAP_FAILED) {
            fprintf(stderr, "[GreenBoost] mmap anonymous failed for %zu MB: %m\n", bytesize >> 20);
            return CUDA_ERROR_OUT_OF_MEMORY;
        }

        fd = gb_open_device();
        if (fd >= 0) {
            struct gb_pin_req req;
            memset(&req, 0, sizeof(req));
            req.vaddr = (uint64_t)(uintptr_t)mapped_ptr;
            req.size = bytesize;
            req.flags = GB_ALLOC_WEIGHTS;
            req.fd = -1;

            if (ioctl(fd, GB_IOCTL_PIN_USER_PTR, &req) < 0) {
                fprintf(stderr, "[GreenBoost] GB_IOCTL_PIN_USER_PTR failed for %zu MB: %m\n", bytesize >> 20);
                munmap(mapped_ptr, bytesize);
                mapped_ptr = NULL;
            } else {
                int dmabuf_fd = req.fd;
                /* Register the userspace pointer with CUDA */
                ret = real_cuMemHostRegister(mapped_ptr, bytesize, CU_MEMHOSTREGISTER_DEVICEMAP);
                if (ret != CUDA_SUCCESS) {
                    fprintf(stderr, "[GreenBoost] cuMemHostRegister FAILED ret=%d for %zu MB\n",
                            ret, bytesize >> 20);
                    munmap(mapped_ptr, bytesize);
                    close(dmabuf_fd);
                    return CUDA_ERROR_OUT_OF_MEMORY;
                }

                /* Get the device pointer for the registered memory */
                ret = real_cuMemHostGetDevicePointer(dptr, mapped_ptr, 0);
                if (ret != CUDA_SUCCESS) {
                    fprintf(stderr, "[GreenBoost] cuMemHostGetDevicePointer FAILED ret=%d\n", ret);
                    void (*cuMemHostUnregister)(void*) = dlsym(RTLD_NEXT, "cuMemHostUnregister");
                    if (cuMemHostUnregister) cuMemHostUnregister(mapped_ptr);
                    munmap(mapped_ptr, bytesize);
                    close(dmabuf_fd);
                    return CUDA_ERROR_OUT_OF_MEMORY;
                }

                ht_insert(*dptr, bytesize, 0 /* DMA-BUF */, -1, mapped_ptr, dmabuf_fd);
                gb_log("DMA-BUF import (pinned): %zu MB at cuda_ptr=0x%llx (mapped=%p, fd=%d)",
                       bytesize >> 20, (unsigned long long)*dptr, mapped_ptr, dmabuf_fd);
                return CUDA_SUCCESS;
            }
        }
    }

    /* ---- Path 2: UVM (cuMemAllocManaged) -------------------------------- */
    /* Fallback path. Backed by system RAM; GPU accesses via page migration. */
    if (real_cuMemAllocManaged) {
        ret = real_cuMemAllocManaged(dptr, bytesize, CU_MEM_ATTACH_GLOBAL);
        if (ret == CUDA_SUCCESS) {
            ht_insert(*dptr, bytesize, 1 /* UVM */, -1, NULL, -1);
            gb_log("UVM alloc: %zu MB at 0x%llx", bytesize >> 20,
                   (unsigned long long)*dptr);
            return CUDA_SUCCESS;
        }
        /* Always print UVM failure — visible in journalctl without debug mode */
        fprintf(stderr,
                "[GreenBoost] UVM alloc FAILED ret=%d for %zu MB"
                " — check nvidia_uvm is loaded and CUDA context is valid\n",
                ret, bytesize >> 20);
    } else {
        fprintf(stderr, "[GreenBoost] UVM unavailable (real_cuMemAllocManaged=NULL)"
                " for %zu MB\n", bytesize >> 20);
    }

    return CUDA_ERROR_OUT_OF_MEMORY;
}

/* ------------------------------------------------------------------ */
/*  cuMemAlloc_v2 override                                              */
/* ------------------------------------------------------------------ */

CUresult cuMemAlloc_v2(CUdeviceptr *dptr, size_t bytesize)
{
    CUresult ret;

    if (!initialized || !real_cuMemAlloc_v2)
        return CUDA_ERROR_OUT_OF_MEMORY;

    if (gb_needs_overflow(bytesize)) {
        ret = gb_overflow_alloc(dptr, bytesize);
        if (ret == CUDA_SUCCESS)
            return CUDA_SUCCESS;
        gb_log("overflow alloc failed (ret=%d), falling back to device alloc", ret);
    }

    ret = real_cuMemAlloc_v2(dptr, bytesize);
    if (ret == CUDA_SUCCESS)
        ht_insert(*dptr, bytesize, 0, -1, NULL, -1);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  cuMemFree_v2 override                                               */
/* ------------------------------------------------------------------ */

CUresult cuMemFree_v2(CUdeviceptr dptr)
{
    void *mapped_ptr = NULL;
    int fd = -1;
    size_t sz = 0;
    int managed = 0;

    if (!initialized || !real_cuMemFree_v2)
        return CUDA_SUCCESS;

    if (ht_remove(dptr, &sz, &managed, &mapped_ptr, &fd)) {
        gb_log("cuMemFree_v2 ptr=0x%llx size=%zu MB managed=%d mapped_ptr=%p fd=%d",
               (unsigned long long)dptr, sz >> 20, managed, mapped_ptr, fd);
        if (mapped_ptr) {
            void (*cuMemHostUnregister)(void*) = dlsym(RTLD_NEXT, "cuMemHostUnregister");
            if (cuMemHostUnregister) cuMemHostUnregister(mapped_ptr);
            munmap(mapped_ptr, sz);
        }
        if (fd >= 0) {
            close(fd);
        }
    }

    return real_cuMemFree_v2(dptr);
}

/* ------------------------------------------------------------------ */
/*  cuMemAllocAsync override (CUDA 11.2+ stream-ordered allocator)      */
/* ------------------------------------------------------------------ */

CUresult cuMemAllocAsync(CUdeviceptr *dptr, size_t bytesize, CUstream hStream)
{
    CUresult ret;

    if (!initialized)
        return CUDA_ERROR_OUT_OF_MEMORY;

    /* Fall back to sync cuMemAlloc_v2 if async driver API not available */
    if (!real_cuMemAllocAsync)
        return cuMemAlloc_v2(dptr, bytesize);

    if (gb_needs_overflow(bytesize)) {
        ret = gb_overflow_alloc(dptr, bytesize);
        if (ret == CUDA_SUCCESS)
            return CUDA_SUCCESS;
    }

    ret = real_cuMemAllocAsync(dptr, bytesize, hStream);
    if (ret == CUDA_SUCCESS)
        ht_insert(*dptr, bytesize, 0, -1, NULL, -1);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  cudaMalloc override                                                 */
/* ------------------------------------------------------------------ */

cudaError_t cudaMalloc(void **devPtr, size_t size)
{
    cudaError_t ret;
    CUdeviceptr dptr = 0;

    if (!initialized)
        return (cudaError_t)CUDA_ERROR_OUT_OF_MEMORY;

    /* Lazy resolve: libcudart may have been loaded by caller after our constructor.
     * RTLD_NEXT skips our own symbol and finds the real one in the next library. */
    if (!real_cudaMalloc)
        real_cudaMalloc = (pfn_cudaMalloc)dlsym(RTLD_NEXT, "cudaMalloc");
    if (!real_cudaMalloc)
        return (cudaError_t)CUDA_ERROR_OUT_OF_MEMORY;

    if (gb_needs_overflow(size)) {
        ret = (cudaError_t)gb_overflow_alloc(&dptr, size);
        if (ret == CUDA_SUCCESS) {
            *devPtr = (void *)(uintptr_t)dptr;
            return CUDA_SUCCESS;
        }
        gb_log("cudaMalloc overflow failed (ret=%d), falling back", ret);
    }

    ret = real_cudaMalloc(devPtr, size);
    if (ret == CUDA_SUCCESS)
        ht_insert((CUdeviceptr)(uintptr_t)*devPtr, size, 0, -1, NULL, -1);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  cudaMallocAsync override                                            */
/* ------------------------------------------------------------------ */

cudaError_t cudaMallocAsync(void **devPtr, size_t size, cudaStream_t stream)
{
    cudaError_t ret;
    CUdeviceptr dptr = 0;

    if (!initialized)
        return (cudaError_t)CUDA_ERROR_OUT_OF_MEMORY;

    /* Lazy resolve: RTLD_NEXT skips our own symbol, finds real cudaMallocAsync in libcudart */
    if (!real_cudaMallocAsync)
        real_cudaMallocAsync = (pfn_cudaMallocAsync)dlsym(RTLD_NEXT, "cudaMallocAsync");

    /* Fall back to sync cudaMalloc (stream ordering ignored — safe for model weights) */
    if (!real_cudaMallocAsync)
        return cudaMalloc(devPtr, size);

    if (gb_needs_overflow(size)) {
        ret = (cudaError_t)gb_overflow_alloc(&dptr, size);
        if (ret == CUDA_SUCCESS) {
            *devPtr = (void *)(uintptr_t)dptr;
            return CUDA_SUCCESS;
        }
    }

    ret = real_cudaMallocAsync(devPtr, size, stream);
    if (ret == CUDA_SUCCESS)
        ht_insert((CUdeviceptr)(uintptr_t)*devPtr, size, 0, -1, NULL, -1);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  cudaFree override                                                   */
/* ------------------------------------------------------------------ */

cudaError_t cudaFree(void *devPtr)
{
    void *mapped_ptr = NULL;
    int fd = -1;
    size_t sz = 0;
    int managed = 0;
    CUdeviceptr dptr = (CUdeviceptr)(uintptr_t)devPtr;

    if (!initialized)
        return CUDA_SUCCESS; /* free before init: no-op is safe */

    if (!real_cudaFree)
        real_cudaFree = (pfn_cudaFree)dlsym(RTLD_NEXT, "cudaFree");
    if (!real_cudaFree)
        return CUDA_SUCCESS;

    if (ht_remove(dptr, &sz, &managed, &mapped_ptr, &fd)) {
        gb_log("cudaFree ptr=0x%llx size=%zu MB managed=%d mapped_ptr=%p fd=%d",
               (unsigned long long)dptr, sz >> 20, managed, mapped_ptr, fd);
        if (mapped_ptr) {
            void (*cuMemHostUnregister)(void*) = dlsym(RTLD_NEXT, "cuMemHostUnregister");
            if (cuMemHostUnregister) cuMemHostUnregister(mapped_ptr);
            munmap(mapped_ptr, sz);
        }
        if (fd >= 0) {
            close(fd);
        }
    }

    return real_cudaFree(devPtr);
}

/* ------------------------------------------------------------------ */
/*  Prefetching overrides (cuMemPrefetchAsync, cudaMemPrefetchAsync)    */
/* ------------------------------------------------------------------ */

CUresult cuMemPrefetchAsync(CUdeviceptr dptr, size_t count, CUdevice dstDevice, CUstream hStream)
{
    void *mapped_ptr = NULL;
    size_t sz = 0;
    int managed = 0, fd = -1;

    if (!initialized || !real_cuMemPrefetchAsync)
        return CUDA_SUCCESS;

    /* Check if this is a GreenBoost DMA-BUF allocation */
    if (ht_remove(dptr, &sz, &managed, &mapped_ptr, &fd)) {
        if (mapped_ptr) {
            enqueue_prefetch(mapped_ptr, count < sz ? count : sz);
        }
        /* Re-insert since we only peeked */
        ht_insert(dptr, sz, managed, -1, mapped_ptr, fd);

        /* We handled the prefetch via host thread, skip real CUDA prefetch
           because CUDA doesn't prefetch cuMemHostRegister memory implicitly */
        return CUDA_SUCCESS;
    }

    return real_cuMemPrefetchAsync(dptr, count, dstDevice, hStream);
}

cudaError_t cudaMemPrefetchAsync(const void *devPtr, size_t count, int dstDevice, cudaStream_t stream)
{
    void *mapped_ptr = NULL;
    size_t sz = 0;
    int managed = 0, fd = -1;
    CUdeviceptr dptr = (CUdeviceptr)(uintptr_t)devPtr;

    if (!initialized)
        return CUDA_SUCCESS;

    if (!real_cudaMemPrefetchAsync) {
        real_cudaMemPrefetchAsync = (pfn_cudaMemPrefetchAsync)dlsym(RTLD_NEXT, "cudaMemPrefetchAsync");
    }

    if (ht_remove(dptr, &sz, &managed, &mapped_ptr, &fd)) {
        if (mapped_ptr) {
            enqueue_prefetch(mapped_ptr, count < sz ? count : sz);
        }
        ht_insert(dptr, sz, managed, -1, mapped_ptr, fd);
        return CUDA_SUCCESS;
    }

    if (real_cudaMemPrefetchAsync)
        return real_cudaMemPrefetchAsync(devPtr, count, dstDevice, stream);

    return CUDA_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  cuMemGetInfo_v2 / cuMemGetInfo / cudaMemGetInfo overrides          */
/*                                                                      */
/*  Report virtual VRAM = real VRAM + DDR4 pool so the CUDA runtime    */
/*  and Ollama scheduler offload ALL model layers as GPU tensors.       */
/*  Each resulting cudaMalloc that overflows real VRAM is then          */
/*  redirected to DDR4 via DMA-BUF (gb_needs_overflow uses real VRAM). */
/* ------------------------------------------------------------------ */

CUresult cuMemGetInfo_v2(size_t *free_out, size_t *total_out)
{
    size_t real_free = 0, real_total = 0;
    CUresult ret;

    if (!initialized || !real_cuMemGetInfo)
        return CUDA_ERROR_NOT_SUPPORTED;

    ret = real_cuMemGetInfo(&real_free, &real_total);
    if (ret != CUDA_SUCCESS)
        return ret;

    if (free_out)  *free_out  = real_free  + gb_virtual_vram_bytes;
    if (total_out) *total_out = real_total + gb_virtual_vram_bytes;

    gb_log("cuMemGetInfo_v2: real_free=%zuMB → virtual_free=%zuMB",
           real_free >> 20, (real_free + gb_virtual_vram_bytes) >> 20);
    return CUDA_SUCCESS;
}

CUresult cuMemGetInfo(size_t *free_out, size_t *total_out)
{
    return cuMemGetInfo_v2(free_out, total_out);
}

cudaError_t cudaMemGetInfo(size_t *free_out, size_t *total_out)
{
    size_t real_free = 0, real_total = 0;
    CUresult ret;

    if (!initialized || !real_cuMemGetInfo)
        return (cudaError_t)CUDA_ERROR_NOT_SUPPORTED;

    /* Call real driver function directly — avoids double-inflation if libcudart
     * internally calls cuMemGetInfo_v2 (which we also override). */
    ret = real_cuMemGetInfo(&real_free, &real_total);
    if (ret != CUDA_SUCCESS)
        return (cudaError_t)ret;

    if (free_out)  *free_out  = real_free  + gb_virtual_vram_bytes;
    if (total_out) *total_out = real_total + gb_virtual_vram_bytes;

    gb_log("cudaMemGetInfo: real_free=%zuMB → virtual_free=%zuMB",
           real_free >> 20, (real_free + gb_virtual_vram_bytes) >> 20);
    return CUDA_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  cuDeviceTotalMem overrides                                          */
/*                                                                      */
/*  Ollama's scheduler calls cuDeviceTotalMem at startup to determine   */
/*  how many model layers fit in VRAM.  Inflate by DDR4 pool size so   */
/*  all layers are scheduled as GPU tensors; the allocations that       */
/*  overflow real VRAM are caught by cudaMalloc/cuMemAlloc above.       */
/* ------------------------------------------------------------------ */

CUresult cuDeviceTotalMem_v2(size_t *bytes, CUdevice dev)
{
    CUresult ret;

    if (!initialized || !real_cuDeviceTotalMem_v2)
        return CUDA_ERROR_NOT_SUPPORTED;

    ret = real_cuDeviceTotalMem_v2(bytes, dev);
    if (ret == CUDA_SUCCESS && bytes) {
        gb_log("cuDeviceTotalMem_v2: real=%zuMB → virtual=%zuMB",
               *bytes >> 20, (*bytes + gb_virtual_vram_bytes) >> 20);
        *bytes += gb_virtual_vram_bytes;
    }
    return ret;
}

CUresult cuDeviceTotalMem(size_t *bytes, CUdevice dev)
{
    return cuDeviceTotalMem_v2(bytes, dev);
}

/* ------------------------------------------------------------------ */
/*  NVML overrides — nvmlDeviceGetMemoryInfo[_v2]                      */
/*                                                                      */
/*  Ollama's discover/nvidia.go uses NVML for initial GPU sizing.       */
/*  Inflate total + free so the layer scheduler sees virtual VRAM.      */
/* ------------------------------------------------------------------ */

nvmlReturn_t nvmlDeviceGetMemoryInfo(nvmlDevice_t device, nvmlMemory_t *memory)
{
    nvmlReturn_t ret;

    if (!real_nvmlDeviceGetMemoryInfo)
        return 999; /* NVML_ERROR_FUNCTION_NOT_FOUND */

    ret = real_nvmlDeviceGetMemoryInfo(device, memory);
    if (ret == NVML_SUCCESS && memory) {
        gb_log("nvmlDeviceGetMemoryInfo: real_total=%lluMB → virtual_total=%lluMB",
               memory->total >> 20,
               (memory->total + gb_virtual_vram_bytes) >> 20);
        memory->total += gb_virtual_vram_bytes;
        memory->free  += gb_virtual_vram_bytes;
    }
    return ret;
}

nvmlReturn_t nvmlDeviceGetMemoryInfo_v2(nvmlDevice_t device, nvmlMemory_v2_t *memory)
{
    nvmlReturn_t ret;

    if (!real_nvmlDeviceGetMemoryInfo_v2)
        return 999;

    ret = real_nvmlDeviceGetMemoryInfo_v2(device, memory);
    if (ret == NVML_SUCCESS && memory) {
        gb_log("nvmlDeviceGetMemoryInfo_v2: real_total=%lluMB → virtual_total=%lluMB",
               memory->total >> 20,
               (memory->total + gb_virtual_vram_bytes) >> 20);
        memory->total += gb_virtual_vram_bytes;
        memory->free  += gb_virtual_vram_bytes;
    }
    return ret;
}

/* ------------------------------------------------------------------ */
/*  dlsym hook — intercepts dlopen-based GPU API lookups               */
/*                                                                      */
/*  Ollama accesses NVML and CUDA driver via dlopen+dlsym, which       */
/*  bypasses standard LD_PRELOAD interception.  We override dlsym to   */
/*  return our hooked versions for memory-reporting symbols so that     */
/*  Ollama's GPU discovery sees the virtual (inflated) VRAM size.       */
/*                                                                      */
/*  Bootstrap: __libc_dlsym is used to get the REAL dlsym without      */
/*  triggering a recursive call through our own override.               */
/* ------------------------------------------------------------------ */

typedef void *(*pfn_dlsym_t)(void *, const char *);
typedef void *(*pfn_dlopen_t)(const char *, int);
static pfn_dlsym_t  real_dlsym_fn  = NULL;
static pfn_dlopen_t real_dlopen_fn = NULL;

/* Bootstrap: run before gb_shim_init() to capture real dlsym and dlopen.
 *
 * We override both dlsym and dlopen, so we need their real pointers before
 * our overrides are active.  dlvsym with an explicit version skips our
 * unversioned overrides and finds the glibc implementations.
 *
 * On glibc >= 2.34 (Ubuntu 22.04+) both are at GLIBC_2.34 / GLIBC_2.2.5.
 * Priority 101 ensures this runs before the default-priority gb_shim_init. */
__attribute__((constructor(101)))
static void gb_dlsym_bootstrap(void)
{
    /* dlsym — try newest version first */
    real_dlsym_fn = (pfn_dlsym_t)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
    if (!real_dlsym_fn)
        real_dlsym_fn = (pfn_dlsym_t)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    if (!real_dlsym_fn)
        real_dlsym_fn = (pfn_dlsym_t)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17");
    if (!real_dlsym_fn)
        real_dlsym_fn = (pfn_dlsym_t)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.0");

    /* dlopen — same version chain */
    real_dlopen_fn = (pfn_dlopen_t)dlvsym(RTLD_NEXT, "dlopen", "GLIBC_2.34");
    if (!real_dlopen_fn)
        real_dlopen_fn = (pfn_dlopen_t)dlvsym(RTLD_NEXT, "dlopen", "GLIBC_2.2.5");
    if (!real_dlopen_fn)
        real_dlopen_fn = (pfn_dlopen_t)dlvsym(RTLD_NEXT, "dlopen", "GLIBC_2.17");
    if (!real_dlopen_fn)
        real_dlopen_fn = (pfn_dlopen_t)dlvsym(RTLD_NEXT, "dlopen", "GLIBC_2.0");
}

/* Return our hook for a given symbol name, or NULL if not intercepted. */
static void *gb_get_hook(const char *name)
{
    if (!name) return NULL;

    /* NVML memory reporting — used by Ollama for initial VRAM discovery */
    if (strcmp(name, "nvmlDeviceGetMemoryInfo")    == 0) return (void *)nvmlDeviceGetMemoryInfo;
    if (strcmp(name, "nvmlDeviceGetMemoryInfo_v2") == 0) return (void *)nvmlDeviceGetMemoryInfo_v2;

    /* CUDA device total memory — queried by scheduler at startup */
    if (strcmp(name, "cuDeviceTotalMem_v2")        == 0) return (void *)cuDeviceTotalMem_v2;
    if (strcmp(name, "cuDeviceTotalMem")           == 0) return (void *)cuDeviceTotalMem;

    /* CUDA free/total memory info */
    if (strcmp(name, "cuMemGetInfo_v2")            == 0) return (void *)cuMemGetInfo_v2;
    if (strcmp(name, "cuMemGetInfo")               == 0) return (void *)cuMemGetInfo;
    if (strcmp(name, "cudaMemGetInfo")             == 0) return (void *)cudaMemGetInfo;

    /* CUDA allocation — large allocs redirected to DDR4 pool */
    if (strcmp(name, "cudaMalloc")                 == 0) return (void *)cudaMalloc;
    if (strcmp(name, "cudaMallocAsync")            == 0) return (void *)cudaMallocAsync;
    if (strcmp(name, "cudaFree")                   == 0) return (void *)cudaFree;
    if (strcmp(name, "cuMemAlloc_v2")              == 0) return (void *)cuMemAlloc_v2;
    if (strcmp(name, "cuMemAllocAsync")            == 0) return (void *)cuMemAllocAsync;
    if (strcmp(name, "cuMemFree_v2")               == 0) return (void *)cuMemFree_v2;
    if (strcmp(name, "cuMemPrefetchAsync")         == 0) return (void *)cuMemPrefetchAsync;
    if (strcmp(name, "cudaMemPrefetchAsync")       == 0) return (void *)cudaMemPrefetchAsync;

    return NULL;
}

void *dlsym(void *handle, const char *name)
{
    void *hook;

    /* Only intercept after GreenBoost has fully initialized, and ONLY for
     * library-specific handles (not RTLD_NEXT).  Our own code uses RTLD_NEXT
     * to find real implementations — intercepting those causes infinite
     * recursion (real_cudaMalloc = dlsym(RTLD_NEXT,"cudaMalloc") → our hook
     * → calls itself). */
    if (initialized && handle != RTLD_NEXT) {
        hook = gb_get_hook(name);
        if (hook) {
            gb_log("dlsym hook: '%s' → GreenBoost intercepted", name);
            return hook;
        }
    }

    if (real_dlsym_fn)
        return real_dlsym_fn(handle, name);

    /* Bootstrap failed — return NULL rather than calling the broken
     * dlvsym(handle,name,"GLIBC_2.0") which returns NULL for all CUDA/NVML
     * symbols anyway and would silently break the caller's initialization. */
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  dlopen hook — strips RTLD_DEEPBIND so LD_PRELOAD hooks stay active */
/*                                                                      */
/*  Ollama loads libggml-cuda.so with RTLD_DEEPBIND, which makes CUDA  */
/*  symbol lookups inside that library prefer libcudart.so (bundled),  */
/*  completely bypassing our cudaMalloc/cuMemAlloc LD_PRELOAD hooks.   */
/*  Stripping RTLD_DEEPBIND forces those symbols to resolve from the   */
/*  global namespace where our overrides are registered first.          */
/* ------------------------------------------------------------------ */

void *dlopen(const char *filename, int flags)
{
    /* RTLD_DEEPBIND = 0x008 on Linux — isolates library from global NS */
    if (flags & RTLD_DEEPBIND) {
        flags &= ~RTLD_DEEPBIND;
        fprintf(stderr,
                "[GreenBoost] dlopen: stripped RTLD_DEEPBIND from '%s'"
                " (keeps cudaMalloc/cuMemAlloc hooks active)\n",
                filename ? filename : "(null)");
    }

    if (real_dlopen_fn)
        return real_dlopen_fn(filename, flags);

    /* Fallback: bootstrap hasn't run yet — should never happen since
     * constructor(101) runs before any user code calls dlopen. */
    return NULL;
}
