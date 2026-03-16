/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2024-2026 Ferran Duarri. Dual-licensed: GPL v2 + Commercial.
 * GreenBoost v2.3 — Windows IOCTL definitions (kernel + userspace)
 *
 * Windows port of greenboost_ioctl.h. Uses CTL_CODE macro for IOCTLs.
 * Compiles in both kernel (KMDF) and userspace (Win32) contexts.
 *
 * Author  : Ferran Duarri
 * License : GPL v2 (open-source) / Commercial — see LICENSE
 */
#ifndef GREENBOOST_IOCTL_WIN_H
#define GREENBOOST_IOCTL_WIN_H

#ifdef _KERNEL_MODE
#  include <ntddk.h>
#else
#  include <windows.h>
#  include <winioctl.h>
#endif

/* ------------------------------------------------------------------ */
/*  Portable integer types                                              */
/* ------------------------------------------------------------------ */

typedef UINT64  gb_u64;
typedef UINT32  gb_u32;
typedef INT32   gb_s32;

/* ------------------------------------------------------------------ */
/*  IOCTL codes                                                         */
/* ------------------------------------------------------------------ */

#define GB_IOCTL_TYPE  0x8000  /* Device type for custom driver */

#define GB_IOCTL_ALLOC        CTL_CODE(GB_IOCTL_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GB_IOCTL_GET_INFO     CTL_CODE(GB_IOCTL_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GB_IOCTL_RESET        CTL_CODE(GB_IOCTL_TYPE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GB_IOCTL_MADVISE      CTL_CODE(GB_IOCTL_TYPE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GB_IOCTL_EVICT        CTL_CODE(GB_IOCTL_TYPE, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GB_IOCTL_FREE         CTL_CODE(GB_IOCTL_TYPE, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GB_IOCTL_POLL_FD      CTL_CODE(GB_IOCTL_TYPE, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GB_IOCTL_PIN_USER_PTR CTL_CODE(GB_IOCTL_TYPE, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* ------------------------------------------------------------------ */
/*  Allocation flags — stored in gb_alloc_req_win.flags                 */
/* ------------------------------------------------------------------ */

#define GB_ALLOC_WEIGHTS     (1u << 0)  /* model weight tensor              */
#define GB_ALLOC_KV_CACHE    (1u << 1)  /* KV cache page                    */
#define GB_ALLOC_ACTIVATIONS (1u << 2)  /* ephemeral activation buffer      */
#define GB_ALLOC_FROZEN      (1u << 3)  /* never evict from T2              */
#define GB_ALLOC_NO_HUGEPAGE (1u << 4)  /* force 4K (for T3-spillable)      */

/* ------------------------------------------------------------------ */
/*  IOCTL request/response structures                                   */
/* ------------------------------------------------------------------ */

/*
 * GB_IOCTL_ALLOC -- Allocate a pinned DDR4 buffer and map it into the
 * calling process. Returns a userspace VA the shim passes directly to
 * cuMemHostRegister. The driver maps the physical pages via
 * MmMapLockedPagesSpecifyCache into UserMode -- this is the Windows
 * equivalent of Linux mmap(dma_buf_fd).
 *
 * The shim must call GB_IOCTL_FREE with buf_id to release the buffer.
 * Linux relies on close(fd) triggering DMA-BUF release; Windows has
 * no equivalent automatic cleanup, so the free is explicit.
 */
#pragma pack(push, 8)
struct gb_alloc_req_win {
    gb_u64  size;       /* bytes to allocate               (in)  */
    gb_u64  user_va;    /* userspace VA of mapped buffer    (out) */
    gb_s32  buf_id;     /* buffer ID returned               (out) */
    gb_u32  flags;      /* GB_ALLOC_* flags                 (in)  */
};
#pragma pack(pop)

/* GB_IOCTL_FREE -- Release a buffer allocated by GB_IOCTL_ALLOC */
struct gb_free_req_win {
    gb_s32  buf_id;     /* buffer ID to free                (in)  */
    gb_u32  _pad;
};

/* GB_IOCTL_GET_INFO — Pool statistics (three-tier memory hierarchy) */
#pragma pack(push, 8)
struct gb_info_win {
    /* Tier 1 — GPU VRAM (physical, managed by NVIDIA driver) */
    gb_u64 vram_physical_mb;

    /* Tier 2 -- DDR4 RAM pool (pinned pages, MDL-mapped to userspace) */
    gb_u64 total_ram_mb;
    gb_u64 free_ram_mb;
    gb_u64 allocated_mb;
    gb_u64 max_pool_mb;
    gb_u64 safety_reserve_mb;
    gb_u64 available_mb;
    gb_u32 active_buffers;
    gb_u32 oom_active;

    /* Tier 3 — NVMe / pagefile overflow */
    gb_u64 nvme_swap_total_mb;
    gb_u64 nvme_swap_used_mb;
    gb_u64 nvme_swap_free_mb;
    gb_u64 nvme_t3_allocated_mb;
    gb_u32 swap_pressure;
    gb_u32 _pad;

    /* Combined view */
    gb_u64 total_combined_mb;
};
#pragma pack(pop)

/* GB_IOCTL_MADVISE — Advise on buffer eviction priority */
struct gb_madvise_req_win {
    gb_s32 buf_id;
    gb_u32 advise;
};
#define GB_MADVISE_COLD   0  /* demote in LRU (evict sooner)    */
#define GB_MADVISE_HOT    1  /* promote to LRU head             */
#define GB_MADVISE_FREEZE 2  /* pin — never evict while frozen  */

/* GB_IOCTL_EVICT — Push a T2 buffer to T3 immediately */
struct gb_evict_req_win {
    gb_s32 buf_id;
    gb_u32 _pad;
};

/*
 * GB_IOCTL_POLL_FD — Register for pressure notifications.
 * On Windows, the driver creates a named event
 * \\BaseNamedObjects\\GreenBoostPressure that userspace opens by name.
 * This IOCTL accepts an optional client event handle for per-process
 * signaling. Pass NULL to use the global named event.
 */
struct gb_poll_req_win {
    HANDLE  event_handle;  /* client event handle (in, optional) */
};

/* GB_IOCTL_PIN_USER_PTR -- Pin existing user-space buffer */
#pragma pack(push, 8)
struct gb_pin_req_win {
    gb_u64  vaddr;      /* user-space virtual address       (in)  */
    gb_u64  size;       /* bytes to pin                     (in)  */
    gb_u64  mapped_va;  /* driver-mapped VA returned        (out) */
    gb_s32  buf_id;     /* buffer ID returned               (out) */
    gb_u32  flags;      /* GB_ALLOC_* flags                 (in)  */
};
#pragma pack(pop)

/* ------------------------------------------------------------------ */
/*  Swap pressure thresholds                                            */
/* ------------------------------------------------------------------ */

#define GB_SWAP_PRESSURE_OK       0
#define GB_SWAP_PRESSURE_WARN     1  /* >75% swap used */
#define GB_SWAP_PRESSURE_CRITICAL 2  /* >90% swap used */

/* ------------------------------------------------------------------ */
/*  Memory tier identifiers                                             */
/* ------------------------------------------------------------------ */

#define GB_TIER2_DDR4  2
#define GB_TIER3_NVME  3

#endif /* GREENBOOST_IOCTL_WIN_H */
