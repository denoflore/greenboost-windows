/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2024-2026 Ferran Duarri. Dual-licensed: GPL v2 + Commercial.
 * GreenBoost v2.3 — Windows CUDA Shim DLL header
 *
 * Defines CUDA type stubs, function pointer types, hash table, and
 * configuration for the GreenBoost CUDA memory interception shim.
 *
 * Author  : Ferran Duarri
 * License : GPL v2 (open-source) / Commercial — see LICENSE
 */
#ifndef GREENBOOST_CUDA_SHIM_WIN_H
#define GREENBOOST_CUDA_SHIM_WIN_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "greenboost_ioctl_win.h"

/* ------------------------------------------------------------------ */
/*  Minimal CUDA type definitions (no CUDA SDK headers needed)          */
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
#define CU_MEMHOSTREGISTER_PORTABLE   0x01
#define CU_MEMHOSTREGISTER_DEVICEMAP  0x02

/* Minimal NVML types */
typedef int nvmlReturn_t;
typedef void *nvmlDevice_t;
#define NVML_SUCCESS 0

typedef struct {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
} nvmlMemory_t;

typedef struct {
    unsigned int version;
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
    unsigned long long reserved;
} nvmlMemory_v2_t;

/* ------------------------------------------------------------------ */
/*  Function pointer types for CUDA hooks                               */
/* ------------------------------------------------------------------ */

typedef CUresult    (*pfn_cuMemAlloc_v2)(CUdeviceptr *, size_t);
typedef CUresult    (*pfn_cuMemFree_v2)(CUdeviceptr);
typedef CUresult    (*pfn_cuMemAllocManaged)(CUdeviceptr *, size_t, CUmemAttach_flags);
typedef CUresult    (*pfn_cuMemGetInfo)(size_t *, size_t *);
typedef CUresult    (*pfn_cuMemAllocAsync)(CUdeviceptr *, size_t, CUstream);
typedef CUresult    (*pfn_cuMemFreeAsync)(CUdeviceptr, CUstream);
typedef cudaError_t (*pfn_cudaMalloc)(void **, size_t);
typedef cudaError_t (*pfn_cudaFree)(void *);
typedef cudaError_t (*pfn_cudaMallocManaged)(void **, size_t, unsigned int);
typedef cudaError_t (*pfn_cudaMallocAsync)(void **, size_t, cudaStream_t);
typedef cudaError_t (*pfn_cudaMemGetInfo)(size_t *, size_t *);
typedef CUresult    (*pfn_cuDeviceTotalMem_v2)(size_t *, CUdevice);

/* Host registration for mmap+register path */
typedef CUresult    (*pfn_cuMemHostRegister)(void *, size_t, unsigned int);
typedef CUresult    (*pfn_cuMemHostUnregister)(void *);
typedef CUresult    (*pfn_cuMemHostGetDevicePointer)(CUdeviceptr *, void *, unsigned int);

/* NVML hooks */
typedef nvmlReturn_t (*pfn_nvmlDeviceGetMemoryInfo)(nvmlDevice_t, nvmlMemory_t *);
typedef nvmlReturn_t (*pfn_nvmlDeviceGetMemoryInfo_v2)(nvmlDevice_t, nvmlMemory_v2_t *);

/* ------------------------------------------------------------------ */
/*  Hash table — open-addressed with Fibonacci hashing                  */
/*  Fixed: uses tombstone markers instead of zeroing on delete          */
/* ------------------------------------------------------------------ */

#define HT_BITS    17u
#define HT_SIZE    (1u << HT_BITS)       /* 131072 slots */
#define HT_MASK    (HT_SIZE - 1u)
#define HT_LOCKS   64u

/* Sentinel values for open-addressing */
#define HT_EMPTY    ((CUdeviceptr)0)
#define HT_TOMBSTONE ((CUdeviceptr)1)   /* Deleted slot — probe continues */

typedef struct {
    CUdeviceptr         ptr;            /* 0 = empty, 1 = tombstone       */
    size_t              size;
    int                 is_managed;     /* 1 = UVM fallback               */
    int                 gb_buf_id;      /* buffer ID from driver          */
    void               *mapped_ptr;     /* user VA from driver MDL map    */
    uint8_t             _pad[24];       /* pad to 64 bytes                */
} __declspec(align(64)) gb_ht_entry_t;

/* ------------------------------------------------------------------ */
/*  Shim configuration                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    /* From registry HKLM\SOFTWARE\GreenBoost\ */
    ULONG   PhysicalVramGb;
    ULONG   VirtualVramGb;
    ULONG   ThresholdMb;       /* min alloc size to intercept (default 256MB) */
    ULONG   DebugMode;

    /* Derived */
    size_t  ThresholdBytes;    /* ThresholdMb * 1024 * 1024 */
    size_t  ReportedTotalVram; /* (PhysicalVramGb + VirtualVramGb) * 1GB */

    /* State */
    HANDLE  DeviceHandle;      /* \\.\GreenBoost */
    HANDLE  PressureEvent;     /* GreenBoostPressure named event */
    BOOL    Initialized;

    /* UVM probe state — deferred to first interception, not DLL load.
     * At DLL_PROCESS_ATTACH no CUDA context exists yet, so calling
     * cuMemAllocManaged would fail with CUDA_ERROR_NOT_INITIALIZED.
     * The probe runs once on the first intercepted cudaMalloc call,
     * where a CUDA context is guaranteed to exist. */
    BOOL    UvmProbed;         /* TRUE after first-use probe completed */
    BOOL    UvmAvailable;      /* TRUE if cuMemAllocManaged works     */
} gb_shim_config_t;

/* ------------------------------------------------------------------ */
/*  Global declarations                                                 */
/* ------------------------------------------------------------------ */

extern gb_shim_config_t  gb_config;
extern gb_ht_entry_t     gb_htable[HT_SIZE];
extern CRITICAL_SECTION  ht_locks[HT_LOCKS];

/* Hash table functions */
static inline uint32_t ht_hash(CUdeviceptr ptr)
{
    return (uint32_t)((ptr * 0x9E3779B97F4A7C15ULL) >> (64 - HT_BITS));
}

/* Logging */
#define gb_log(fmt, ...) \
    do { if (gb_config.DebugMode) \
        fprintf(stderr, "[GreenBoost] " fmt "\n", ##__VA_ARGS__); \
    } while (0)

#define gb_log_err(fmt, ...) \
    fprintf(stderr, "[GreenBoost] ERROR: " fmt "\n", ##__VA_ARGS__)

#endif /* GREENBOOST_CUDA_SHIM_WIN_H */
