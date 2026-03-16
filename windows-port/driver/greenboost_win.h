/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2024-2026 Ferran Duarri. Dual-licensed: GPL v2 + Commercial.
 * GreenBoost v2.3 — Windows KMDF driver internal header
 *
 * Defines per-buffer state (GB_BUF_WIN), global device state (GB_DEVICE_WIN),
 * and internal function prototypes.
 *
 * Author  : Ferran Duarri
 * License : GPL v2 (open-source) / Commercial — see LICENSE
 */
#ifndef GREENBOOST_WIN_H
#define GREENBOOST_WIN_H

#include <ntddk.h>
#include <wdf.h>
#include <wdmsec.h>
#include "greenboost_ioctl_win.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                           */
/* ------------------------------------------------------------------ */

#define GB_DRIVER_NAME      L"GreenBoost"
#define GB_DEVICE_NAME      L"\\Device\\GreenBoost"
#define GB_SYMLINK_NAME     L"\\DosDevices\\GreenBoost"
#define GB_PRESSURE_EVENT   L"\\BaseNamedObjects\\GreenBoostPressure"

/* 2 MiB contiguous block size (matching Linux hugepage path) */
#define GB_BLOCK_SIZE       (2u * 1024u * 1024u)
#define GB_BLOCK_PAGES      (GB_BLOCK_SIZE / PAGE_SIZE)

/* Maximum tracked buffers */
#define GB_MAX_BUFS         4096

/* Default configuration values */
#define GB_DEFAULT_PHYSICAL_VRAM_GB   12   /* RTX 5070 */
#define GB_DEFAULT_VIRTUAL_VRAM_GB    51   /* 80% of 64 GB DDR4 */
#define GB_DEFAULT_SAFETY_RESERVE_GB  12
#define GB_DEFAULT_NVME_SWAP_GB       64
#define GB_DEFAULT_NVME_POOL_GB       58
#define GB_DEFAULT_THRESHOLD_MB       256
#define GB_DEFAULT_DEBUG_MODE         0

/* ------------------------------------------------------------------ */
/*  Debug logging                                                       */
/* ------------------------------------------------------------------ */

#define GB_TAG  'bGrB'  /* Pool tag: BrGb */

#define gb_dbg(fmt, ...) \
    do { if (GbGlobalDevice.DebugMode) \
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, \
                   "GreenBoost: " fmt "\n", ##__VA_ARGS__); \
    } while (0)

#define gb_info(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, \
               "GreenBoost: " fmt "\n", ##__VA_ARGS__)

#define gb_err(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, \
               "GreenBoost: ERROR: " fmt "\n", ##__VA_ARGS__)

#define gb_warn(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL, \
               "GreenBoost: WARN: " fmt "\n", ##__VA_ARGS__)

/* ------------------------------------------------------------------ */
/*  Per-buffer object (port of struct gb_buf)                           */
/* ------------------------------------------------------------------ */

typedef struct _GB_BUF_WIN {
    /* Memory backing */
    PVOID               KernelVa;       /* kernel virtual address         */
    PMDL                Mdl;            /* MDL for non-contiguous allocs  */
    PHYSICAL_ADDRESS    PhysAddr;       /* physical addr (contiguous)     */
    SIZE_T              Size;           /* allocation size in bytes       */
    BOOLEAN             Contiguous;     /* TRUE = contiguous 2MB block    */

    /* Identity */
    LONG                Id;             /* unique buffer ID (1-based)     */
    LONG                Tier;           /* GB_TIER2_DDR4 or GB_TIER3_NVME */
    ULONG               AllocFlags;     /* GB_ALLOC_* flags               */

    /* Sharing -- direct MDL mapping to userspace */
    PVOID               UserVa;         /* VA in user process address space */
    PEPROCESS           UserProcess;    /* process that owns the mapping   */

    /* LRU tracking */
    LIST_ENTRY          LruNode;        /* link in GB_DEVICE_WIN.LruList  */
    LARGE_INTEGER       AllocTime;      /* KeQuerySystemTime at alloc     */
    LARGE_INTEGER       LastAccessTime; /* updated on MADVISE_HOT         */
    BOOLEAN             Frozen;         /* 1 = never evict from T2        */

    /* Lifecycle */
    BOOLEAN             Active;         /* TRUE while buffer is live      */
} GB_BUF_WIN, *PGB_BUF_WIN;

/* ------------------------------------------------------------------ */
/*  Global device state (port of struct gb_device)                      */
/* ------------------------------------------------------------------ */

typedef struct _GB_DEVICE_WIN {
    WDFDEVICE           WdfDevice;
    WDFQUEUE            IoQueue;

    /* Buffer tracking — simple array + interlocked counter */
    FAST_MUTEX          BufLock;            /* protects BufTable         */
    PGB_BUF_WIN         BufTable[GB_MAX_BUFS]; /* ID-indexed (1-based)  */
    LONG volatile       NextBufId;          /* interlocked counter       */
    LONG volatile       ActiveBufs;         /* live buffer count         */

    /* Tier 2 — DDR4 pool */
    LONG64 volatile     PoolAllocated;      /* bytes currently pinned    */
    LONG volatile       OomActive;          /* 1 when safety guard trips */

    /* Tier 3 — NVMe / pagefile overflow */
    LONG64 volatile     NvmeAllocated;      /* bytes in T3 allocations   */
    LONG volatile       SwapPressure;       /* 0=ok 1=warn 2=critical    */

    /* LRU list */
    LIST_ENTRY          LruList;
    KSPIN_LOCK          LruLock;

    /* Pressure notification */
    PKEVENT             PressureEvent;      /* named kernel event        */
    HANDLE              PressureEventHandle;

    /* Watchdog thread */
    PKTHREAD            WatchdogThread;
    BOOLEAN volatile    WatchdogRunning;

    /* Configuration (from registry or auto-detect) */
    ULONG               PhysicalVramGb;
    ULONG               VirtualVramGb;
    ULONG               SafetyReserveGb;
    ULONG               NvmeSwapGb;
    ULONG               NvmePoolGb;
    ULONG               ThresholdMb;
    ULONG               DebugMode;
} GB_DEVICE_WIN, *PGB_DEVICE_WIN;

/* Global device instance */
extern GB_DEVICE_WIN GbGlobalDevice;

/* ------------------------------------------------------------------ */
/*  Function prototypes                                                 */
/* ------------------------------------------------------------------ */

/* Driver entry / device creation */
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD GbEvtDeviceAdd;
EVT_WDF_DEVICE_CONTEXT_CLEANUP GbEvtDeviceCleanup;

/* IOCTL dispatch */
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL GbEvtIoDeviceControl;

/* IOCTL handlers */
NTSTATUS GbHandleAlloc(_In_ WDFREQUEST Request);
NTSTATUS GbHandleFree(_In_ WDFREQUEST Request);
NTSTATUS GbHandleGetInfo(_In_ WDFREQUEST Request);
NTSTATUS GbHandleReset(_In_ WDFREQUEST Request);
NTSTATUS GbHandleMadvise(_In_ WDFREQUEST Request);
NTSTATUS GbHandleEvict(_In_ WDFREQUEST Request);
NTSTATUS GbHandlePollFd(_In_ WDFREQUEST Request);
NTSTATUS GbHandlePinUserPtr(_In_ WDFREQUEST Request);

/* Memory allocator */
PGB_BUF_WIN GbAllocTier2(_In_ SIZE_T Size, _In_ ULONG Flags);
PGB_BUF_WIN GbAllocTier3(_In_ SIZE_T Size, _In_ ULONG Flags);
VOID GbFreeBuf(_In_ PGB_BUF_WIN Buf);

/* Buffer management */
LONG GbRegisterBuf(_In_ PGB_BUF_WIN Buf);
PGB_BUF_WIN GbLookupBuf(_In_ LONG Id);
VOID GbUnregisterBuf(_In_ LONG Id);

/* User mapping -- MDL-based (replaces broken section approach) */
NTSTATUS GbMapToUser(_Inout_ PGB_BUF_WIN Buf);
VOID GbUnmapFromUser(_Inout_ PGB_BUF_WIN Buf);

/* Watchdog */
NTSTATUS GbStartWatchdog(VOID);
VOID GbStopWatchdog(VOID);
VOID GbWatchdogThread(_In_ PVOID Context);

/* Configuration */
NTSTATUS GbReadConfig(VOID);

/* Memory query */
VOID GbQueryMemoryStatus(_Out_ PULONG64 TotalBytes, _Out_ PULONG64 AvailBytes);

#endif /* GREENBOOST_WIN_H */
