/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2024-2026 Ferran Duarri. Dual-licensed: GPL v2 + Commercial.
 *
 * GreenBoost v2.3 — Windows KMDF Driver
 *
 * 3-Tier GPU Memory Pool: VRAM + DDR4 + NVMe pagefile
 *   Tier 1 — GPU VRAM (managed by NVIDIA driver)
 *   Tier 2 — DDR4 pool: pinned contiguous 2MB blocks, shared via NT sections
 *   Tier 3 — NVMe/pagefile: swappable 4K pages for model overflow
 *
 * Device: \\.\GreenBoost
 * IOCTLs: ALLOC, GET_INFO, RESET, MADVISE, EVICT, POLL_FD, PIN_USER_PTR
 *
 * Port of the Linux greenboost.ko kernel module. Replaces:
 *   - Linux char device → KMDF device with symbolic link
 *   - DMA-BUF export → NT section objects (MapViewOfFile in userspace)
 *   - alloc_pages(GFP_KERNEL|__GFP_COMP, 9) → MmAllocateContiguousMemorySpecifyCache
 *   - kthread → PsCreateSystemThread
 *   - eventfd → Named kernel event
 *   - IDR → Array-based buffer table
 *   - sysfs → GB_IOCTL_GET_INFO response
 *
 * Author  : Ferran Duarri
 * License : GPL v2 (open-source) / Commercial — see LICENSE
 */

#include "greenboost_win.h"

/* ================================================================== */
/*  Global state                                                        */
/* ================================================================== */

GB_DEVICE_WIN GbGlobalDevice = { 0 };

/* ================================================================== */
/*  Configuration — read from registry                                  */
/* ================================================================== */

NTSTATUS GbReadConfig(VOID)
{
    NTSTATUS status;
    WDFKEY hKey = NULL;
    DECLARE_CONST_UNICODE_STRING(regPath, L"\\Registry\\Machine\\SOFTWARE\\GreenBoost");
    DECLARE_CONST_UNICODE_STRING(valPhysVram, L"PhysicalVramGb");
    DECLARE_CONST_UNICODE_STRING(valVirtVram, L"VirtualVramGb");
    DECLARE_CONST_UNICODE_STRING(valSafety, L"SafetyReserveGb");
    DECLARE_CONST_UNICODE_STRING(valNvmeSwap, L"NvmeSwapGb");
    DECLARE_CONST_UNICODE_STRING(valNvmePool, L"NvmePoolGb");
    DECLARE_CONST_UNICODE_STRING(valThreshold, L"ThresholdMb");
    DECLARE_CONST_UNICODE_STRING(valDebug, L"DebugMode");
    ULONG value;

    /* Set defaults first */
    GbGlobalDevice.PhysicalVramGb  = GB_DEFAULT_PHYSICAL_VRAM_GB;
    GbGlobalDevice.VirtualVramGb   = GB_DEFAULT_VIRTUAL_VRAM_GB;
    GbGlobalDevice.SafetyReserveGb = GB_DEFAULT_SAFETY_RESERVE_GB;
    GbGlobalDevice.NvmeSwapGb      = GB_DEFAULT_NVME_SWAP_GB;
    GbGlobalDevice.NvmePoolGb      = GB_DEFAULT_NVME_POOL_GB;
    GbGlobalDevice.ThresholdMb     = GB_DEFAULT_THRESHOLD_MB;
    GbGlobalDevice.DebugMode       = GB_DEFAULT_DEBUG_MODE;

    /* Try to open registry key — failure is OK, we use defaults */
    status = WdfRegistryOpenKey(NULL, &regPath, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &hKey);
    if (!NT_SUCCESS(status)) {
        gb_info("Registry key not found, using defaults");
        return STATUS_SUCCESS;
    }

    if (NT_SUCCESS(WdfRegistryQueryULong(hKey, &valPhysVram, &value)))
        GbGlobalDevice.PhysicalVramGb = value;
    if (NT_SUCCESS(WdfRegistryQueryULong(hKey, &valVirtVram, &value)))
        GbGlobalDevice.VirtualVramGb = value;
    if (NT_SUCCESS(WdfRegistryQueryULong(hKey, &valSafety, &value)))
        GbGlobalDevice.SafetyReserveGb = value;
    if (NT_SUCCESS(WdfRegistryQueryULong(hKey, &valNvmeSwap, &value)))
        GbGlobalDevice.NvmeSwapGb = value;
    if (NT_SUCCESS(WdfRegistryQueryULong(hKey, &valNvmePool, &value)))
        GbGlobalDevice.NvmePoolGb = value;
    if (NT_SUCCESS(WdfRegistryQueryULong(hKey, &valThreshold, &value)))
        GbGlobalDevice.ThresholdMb = value;
    if (NT_SUCCESS(WdfRegistryQueryULong(hKey, &valDebug, &value)))
        GbGlobalDevice.DebugMode = value;

    WdfRegistryClose(hKey);

    gb_info("Config loaded: T1=%luGB T2=%luGB reserve=%luGB T3=%luGB/%luGB thresh=%luMB debug=%lu",
            GbGlobalDevice.PhysicalVramGb,
            GbGlobalDevice.VirtualVramGb,
            GbGlobalDevice.SafetyReserveGb,
            GbGlobalDevice.NvmeSwapGb,
            GbGlobalDevice.NvmePoolGb,
            GbGlobalDevice.ThresholdMb,
            GbGlobalDevice.DebugMode);

    return STATUS_SUCCESS;
}

/* ================================================================== */
/*  Memory query helpers                                                */
/* ================================================================== */

VOID GbQueryMemoryStatus(_Out_ PULONG64 TotalBytes, _Out_ PULONG64 AvailBytes)
{
    /*
     * In kernel mode, query system memory via ZwQuerySystemInformation.
     * SystemBasicInformation gives total physical pages.
     * SystemPerformanceInformation gives available pages.
     */
    SYSTEM_BASIC_INFORMATION basicInfo = { 0 };
    NTSTATUS status;
    ULONG retLen;

    *TotalBytes = 0;
    *AvailBytes = 0;

    status = ZwQuerySystemInformation(SystemBasicInformation,
                                      &basicInfo, sizeof(basicInfo), &retLen);
    if (NT_SUCCESS(status)) {
        *TotalBytes = (ULONG64)basicInfo.NumberOfPhysicalPages * basicInfo.PageSize;

        /*
         * For available memory, use MmAvailablePages if accessible,
         * or fall back to a conservative estimate.
         * MmAvailablePages is an exported kernel variable on most
         * Windows versions.
         */
        ULONG64 availPages = MmAvailablePages;
        *AvailBytes = availPages * basicInfo.PageSize;
    }
}

/* ================================================================== */
/*  Buffer management — array-based (replaces Linux IDR)                */
/* ================================================================== */

/*
 * Register a buffer in the table. Returns the assigned ID (1-based)
 * or 0 on failure (table full).
 */
LONG GbRegisterBuf(_In_ PGB_BUF_WIN Buf)
{
    LONG id;

    ExAcquireFastMutex(&GbGlobalDevice.BufLock);

    id = InterlockedIncrement(&GbGlobalDevice.NextBufId);
    if (id <= 0 || id >= GB_MAX_BUFS) {
        /* Wrap around — find a free slot */
        for (id = 1; id < GB_MAX_BUFS; id++) {
            if (GbGlobalDevice.BufTable[id] == NULL)
                break;
        }
        if (id >= GB_MAX_BUFS) {
            ExReleaseFastMutex(&GbGlobalDevice.BufLock);
            gb_err("buffer table full (%d slots)", GB_MAX_BUFS);
            return 0;
        }
        InterlockedExchange(&GbGlobalDevice.NextBufId, id);
    }

    Buf->Id = id;
    Buf->Active = TRUE;
    GbGlobalDevice.BufTable[id] = Buf;

    ExReleaseFastMutex(&GbGlobalDevice.BufLock);

    InterlockedIncrement(&GbGlobalDevice.ActiveBufs);
    return id;
}

PGB_BUF_WIN GbLookupBuf(_In_ LONG Id)
{
    PGB_BUF_WIN buf = NULL;

    if (Id <= 0 || Id >= GB_MAX_BUFS)
        return NULL;

    ExAcquireFastMutex(&GbGlobalDevice.BufLock);
    buf = GbGlobalDevice.BufTable[Id];
    if (buf && !buf->Active)
        buf = NULL;
    ExReleaseFastMutex(&GbGlobalDevice.BufLock);

    return buf;
}

VOID GbUnregisterBuf(_In_ LONG Id)
{
    if (Id <= 0 || Id >= GB_MAX_BUFS)
        return;

    ExAcquireFastMutex(&GbGlobalDevice.BufLock);
    GbGlobalDevice.BufTable[Id] = NULL;
    ExReleaseFastMutex(&GbGlobalDevice.BufLock);

    InterlockedDecrement(&GbGlobalDevice.ActiveBufs);
}

/* ================================================================== */
/*  Section sharing — create NT section for userspace mapping           */
/* ================================================================== */

NTSTATUS GbCreateSection(_Inout_ PGB_BUF_WIN Buf)
{
    NTSTATUS status;
    OBJECT_ATTRIBUTES oa;
    LARGE_INTEGER maxSize;
    HANDLE sectionHandle = NULL;
    PVOID sectionObject = NULL;

    if (!Buf->KernelVa || Buf->Size == 0)
        return STATUS_INVALID_PARAMETER;

    /*
     * Create a section object backed by the physical pages.
     *
     * For contiguous memory, we create an MDL describing the physical
     * region, then create a section from it. For non-contiguous (MDL-based)
     * allocations, the MDL already exists.
     */
    if (Buf->Contiguous && !Buf->Mdl) {
        /* Build MDL for contiguous allocation */
        Buf->Mdl = IoAllocateMdl(Buf->KernelVa, (ULONG)Buf->Size, FALSE, FALSE, NULL);
        if (!Buf->Mdl)
            return STATUS_INSUFFICIENT_RESOURCES;
        MmBuildMdlForNonPagedPool(Buf->Mdl);
    }

    /*
     * Create a section object. We use ZwCreateSection with
     * SEC_COMMIT to back the section with physical memory.
     * The section allows userspace to MapViewOfFile the buffer.
     */
    maxSize.QuadPart = (LONGLONG)Buf->Size;
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);

    status = ZwCreateSection(&sectionHandle,
                             SECTION_ALL_ACCESS,
                             &oa,
                             &maxSize,
                             PAGE_READWRITE,
                             SEC_COMMIT,
                             NULL);
    if (!NT_SUCCESS(status)) {
        gb_err("ZwCreateSection failed: 0x%08X", status);
        return status;
    }

    /* Get the section object pointer for kernel reference */
    status = ObReferenceObjectByHandle(sectionHandle,
                                       SECTION_ALL_ACCESS,
                                       NULL,
                                       KernelMode,
                                       &sectionObject,
                                       NULL);
    if (!NT_SUCCESS(status)) {
        ZwClose(sectionHandle);
        gb_err("ObReferenceObjectByHandle failed: 0x%08X", status);
        return status;
    }

    Buf->SectionHandle = sectionHandle;
    Buf->SectionObject = sectionObject;

    gb_dbg("section created for buf id=%ld size=%lluMB",
           Buf->Id, (ULONG64)Buf->Size >> 20);

    return STATUS_SUCCESS;
}

VOID GbDestroySection(_Inout_ PGB_BUF_WIN Buf)
{
    if (Buf->SectionObject) {
        ObDereferenceObject(Buf->SectionObject);
        Buf->SectionObject = NULL;
    }
    if (Buf->SectionHandle) {
        ZwClose(Buf->SectionHandle);
        Buf->SectionHandle = NULL;
    }
}

/* ================================================================== */
/*  Tier 2 allocator — pinned contiguous 2MB blocks (DDR4)              */
/* ================================================================== */

PGB_BUF_WIN GbAllocTier2(_In_ SIZE_T Size, _In_ ULONG Flags)
{
    PGB_BUF_WIN buf;
    PHYSICAL_ADDRESS low = { 0 };
    PHYSICAL_ADDRESS high;
    PHYSICAL_ADDRESS boundary;
    PVOID va;
    ULONG64 t2Max, t2Used, freeBytes, reserveBytes;
    ULONG64 totalBytes, availBytes;

    high.QuadPart = (LONGLONG)-1;   /* max physical address */
    boundary.QuadPart = GB_BLOCK_SIZE;  /* 2MB alignment */

    /* Check T2 pool capacity */
    t2Max = (ULONG64)GbGlobalDevice.VirtualVramGb * (1ULL << 30);
    t2Used = (ULONG64)InterlockedCompareExchange64(
        &GbGlobalDevice.PoolAllocated, 0, 0);
    /* Read without modify — InterlockedCompareExchange64 with equal
     * comparand/exchange acts as an atomic read on x64 */

    if (t2Used + Size > t2Max) {
        gb_dbg("T2 DDR4 cap reached: used=%lluMB + req=%lluMB > cap=%luGB",
               t2Used >> 20, (ULONG64)Size >> 20, GbGlobalDevice.VirtualVramGb);
        return NULL;  /* Caller should try T3 */
    }

    /* Check free memory against safety reserve */
    GbQueryMemoryStatus(&totalBytes, &availBytes);
    freeBytes = availBytes;
    reserveBytes = (ULONG64)GbGlobalDevice.SafetyReserveGb * (1ULL << 30);

    if (freeBytes < reserveBytes + Size) {
        InterlockedExchange(&GbGlobalDevice.OomActive, 1);
        gb_warn("OOM guard: free=%lluMB < reserve=%luGB + req=%lluMB",
                freeBytes >> 20, GbGlobalDevice.SafetyReserveGb,
                (ULONG64)Size >> 20);
        return NULL;
    }

    /* Allocate buffer descriptor */
    buf = (PGB_BUF_WIN)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(GB_BUF_WIN), GB_TAG);
    if (!buf)
        return NULL;

    RtlZeroMemory(buf, sizeof(GB_BUF_WIN));

    /* Round up to 2MB block boundary */
    Size = ALIGN_UP_BY(Size, GB_BLOCK_SIZE);

    /*
     * Allocate contiguous physical memory — this is the Windows equivalent
     * of alloc_pages(GFP_KERNEL|__GFP_COMP|__GFP_ZERO, 9) for 2MB pages.
     *
     * MmAllocateContiguousMemorySpecifyCache returns a kernel VA for a
     * physically contiguous block. MmCached gives best CPU access perf;
     * the GPU will access via cuMemHostRegister which handles cache
     * coherency via PCIe snooping.
     */
    if (!(Flags & GB_ALLOC_NO_HUGEPAGE)) {
        va = MmAllocateContiguousMemorySpecifyCache(
            Size, low, high, boundary, MmCached);
    } else {
        va = NULL;  /* Fall through to non-contiguous */
    }

    if (va) {
        /* Contiguous allocation succeeded */
        RtlZeroMemory(va, Size);
        buf->KernelVa = va;
        buf->PhysAddr = MmGetPhysicalAddress(va);
        buf->Size = Size;
        buf->Contiguous = TRUE;
        buf->Mdl = NULL;  /* Created lazily in GbCreateSection */

        gb_dbg("T2 contiguous alloc: %lluMB at phys=0x%llX",
               (ULONG64)Size >> 20, buf->PhysAddr.QuadPart);
    } else {
        /*
         * Contiguous allocation failed — fall back to MDL-based
         * non-contiguous allocation. This is the Windows equivalent
         * of per-page alloc_page(GFP_KERNEL|__GFP_ZERO).
         */
        PMDL mdl;
        PVOID sysAddr;

        mdl = MmAllocatePagesForMdlEx(low, high, low, Size,
                                       MmCached,
                                       MM_ALLOCATE_FULLY_REQUIRED);
        if (!mdl) {
            ExFreePoolWithTag(buf, GB_TAG);
            gb_warn("T2 MDL alloc failed for %lluMB", (ULONG64)Size >> 20);
            return NULL;
        }

        sysAddr = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
        if (!sysAddr) {
            MmFreePagesFromMdl(mdl);
            ExFreeIoMdl(mdl);
            ExFreePoolWithTag(buf, GB_TAG);
            gb_err("MmGetSystemAddressForMdlSafe failed");
            return NULL;
        }

        RtlZeroMemory(sysAddr, Size);
        buf->KernelVa = sysAddr;
        buf->Mdl = mdl;
        buf->Size = Size;
        buf->Contiguous = FALSE;

        gb_dbg("T2 MDL alloc: %lluMB (non-contiguous)", (ULONG64)Size >> 20);
    }

    buf->Tier = GB_TIER2_DDR4;
    buf->AllocFlags = Flags;
    buf->Frozen = (Flags & GB_ALLOC_FROZEN) ? TRUE : FALSE;
    KeQuerySystemTime(&buf->AllocTime);
    buf->LastAccessTime = buf->AllocTime;
    InitializeListHead(&buf->LruNode);

    /* Update pool accounting */
    InterlockedAdd64(&GbGlobalDevice.PoolAllocated, (LONG64)Size);

    /* Add to LRU list */
    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&GbGlobalDevice.LruLock, &oldIrql);
        InsertTailList(&GbGlobalDevice.LruList, &buf->LruNode);
        KeReleaseSpinLock(&GbGlobalDevice.LruLock, oldIrql);
    }

    return buf;
}

/* ================================================================== */
/*  Tier 3 allocator — swappable 4K pages (NVMe/pagefile overflow)      */
/* ================================================================== */

PGB_BUF_WIN GbAllocTier3(_In_ SIZE_T Size, _In_ ULONG Flags)
{
    PGB_BUF_WIN buf;
    ULONG64 t3Max, t3Used;
    PVOID va;

    /* Check T3 pool capacity */
    t3Max = (ULONG64)GbGlobalDevice.NvmePoolGb * (1ULL << 30);
    t3Used = (ULONG64)InterlockedCompareExchange64(
        &GbGlobalDevice.NvmeAllocated, 0, 0);

    if (t3Used + Size > t3Max) {
        gb_warn("T3 NVMe cap reached: used=%lluMB + req=%lluMB > cap=%luGB",
                t3Used >> 20, (ULONG64)Size >> 20, GbGlobalDevice.NvmePoolGb);
        return NULL;
    }

    buf = (PGB_BUF_WIN)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(GB_BUF_WIN), GB_TAG);
    if (!buf)
        return NULL;

    RtlZeroMemory(buf, sizeof(GB_BUF_WIN));

    /* Round up to page boundary */
    Size = ALIGN_UP_BY(Size, PAGE_SIZE);

    /*
     * T3 allocation: use paged pool. These pages are swappable to
     * the pagefile (backed by NVMe). This is the Windows equivalent
     * of alloc_page(GFP_HIGHUSER|__GFP_ZERO) which allows the Linux
     * kernel to swap these pages to NVMe under memory pressure.
     *
     * Note: ExAllocatePool2 with POOL_FLAG_PAGED returns pageable memory.
     * Under memory pressure, Windows will page these out to the pagefile
     * which sits on NVMe — achieving the same T3 spill behavior as Linux.
     */
    va = ExAllocatePool2(POOL_FLAG_PAGED, Size, GB_TAG);
    if (!va) {
        ExFreePoolWithTag(buf, GB_TAG);
        gb_warn("T3 paged alloc failed for %lluMB", (ULONG64)Size >> 20);
        return NULL;
    }

    RtlZeroMemory(va, Size);

    buf->KernelVa = va;
    buf->Size = Size;
    buf->Contiguous = FALSE;
    buf->Mdl = NULL;
    buf->Tier = GB_TIER3_NVME;
    buf->AllocFlags = Flags | GB_ALLOC_NO_HUGEPAGE;
    buf->Frozen = FALSE;
    KeQuerySystemTime(&buf->AllocTime);
    buf->LastAccessTime = buf->AllocTime;
    InitializeListHead(&buf->LruNode);

    /* Build MDL for the paged allocation so it can be shared */
    buf->Mdl = IoAllocateMdl(va, (ULONG)Size, FALSE, FALSE, NULL);
    if (buf->Mdl) {
        __try {
            MmProbeAndLockPages(buf->Mdl, KernelMode, IoWriteAccess);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            IoFreeMdl(buf->Mdl);
            buf->Mdl = NULL;
            gb_warn("MmProbeAndLockPages failed for T3 buffer");
        }
    }

    /* Update T3 accounting */
    InterlockedAdd64(&GbGlobalDevice.NvmeAllocated, (LONG64)Size);

    gb_dbg("T3 paged alloc: %lluMB (NVMe-spillable)", (ULONG64)Size >> 20);

    return buf;
}

/* ================================================================== */
/*  Buffer free                                                         */
/* ================================================================== */

VOID GbFreeBuf(_In_ PGB_BUF_WIN Buf)
{
    KIRQL oldIrql;

    if (!Buf)
        return;

    gb_dbg("freeing buf id=%ld size=%lluMB tier=%s",
           Buf->Id, (ULONG64)Buf->Size >> 20,
           Buf->Tier == GB_TIER2_DDR4 ? "T2-DDR4" : "T3-NVMe");

    Buf->Active = FALSE;

    /* Remove from LRU list */
    KeAcquireSpinLock(&GbGlobalDevice.LruLock, &oldIrql);
    if (!IsListEmpty(&Buf->LruNode))
        RemoveEntryList(&Buf->LruNode);
    InitializeListHead(&Buf->LruNode);
    KeReleaseSpinLock(&GbGlobalDevice.LruLock, oldIrql);

    /* Unregister from buffer table */
    if (Buf->Id > 0)
        GbUnregisterBuf(Buf->Id);

    /* Destroy section */
    GbDestroySection(Buf);

    /* Update accounting */
    if (Buf->Tier == GB_TIER3_NVME) {
        InterlockedAdd64(&GbGlobalDevice.NvmeAllocated, -(LONG64)Buf->Size);
    } else {
        InterlockedAdd64(&GbGlobalDevice.PoolAllocated, -(LONG64)Buf->Size);
    }

    /* Free memory */
    if (Buf->Contiguous) {
        /* Contiguous path: free MDL if we created one, then free memory */
        if (Buf->Mdl) {
            /* MDL was built for non-paged pool, no unlock needed */
            IoFreeMdl(Buf->Mdl);
            Buf->Mdl = NULL;
        }
        if (Buf->KernelVa) {
            MmFreeContiguousMemory(Buf->KernelVa);
            Buf->KernelVa = NULL;
        }
    } else {
        /* Non-contiguous / paged path */
        if (Buf->Tier == GB_TIER3_NVME) {
            /* T3: paged pool — unlock MDL pages first, then free */
            if (Buf->Mdl) {
                MmUnlockPages(Buf->Mdl);
                IoFreeMdl(Buf->Mdl);
                Buf->Mdl = NULL;
            }
            if (Buf->KernelVa) {
                ExFreePoolWithTag(Buf->KernelVa, GB_TAG);
                Buf->KernelVa = NULL;
            }
        } else {
            /* T2: MDL-allocated non-paged pages */
            if (Buf->Mdl) {
                if (Buf->KernelVa) {
                    MmUnmapLockedPages(Buf->KernelVa, Buf->Mdl);
                    Buf->KernelVa = NULL;
                }
                MmFreePagesFromMdl(Buf->Mdl);
                ExFreePool(Buf->Mdl);
                Buf->Mdl = NULL;
            }
        }
    }

    ExFreePoolWithTag(Buf, GB_TAG);
}

/* ================================================================== */
/*  Watchdog thread — monitors RAM + pagefile pressure                  */
/* ================================================================== */

VOID GbWatchdogThread(_In_ PVOID Context)
{
    LARGE_INTEGER interval;
    ULONG64 totalBytes, availBytes;
    ULONG64 reserveBytes;
    LONG newPressure, oldPressure;

    UNREFERENCED_PARAMETER(Context);

    /* 1 second interval in 100ns units (negative = relative) */
    interval.QuadPart = -10000000LL;

    gb_info("watchdog started (1s interval, T2 RAM + T3 pagefile)");

    while (GbGlobalDevice.WatchdogRunning) {
        KeDelayExecutionThread(KernelMode, FALSE, &interval);

        if (!GbGlobalDevice.WatchdogRunning)
            break;

        /* --- Tier 2: RAM safety reserve check --- */
        GbQueryMemoryStatus(&totalBytes, &availBytes);
        reserveBytes = (ULONG64)GbGlobalDevice.SafetyReserveGb * (1ULL << 30);

        if (availBytes < reserveBytes) {
            if (!InterlockedCompareExchange(&GbGlobalDevice.OomActive, 1, 0)) {
                gb_warn("T2 OOM guard TRIPPED — free=%lluMB < reserve=%luGB",
                        availBytes >> 20, GbGlobalDevice.SafetyReserveGb);
            }
        } else {
            if (InterlockedCompareExchange(&GbGlobalDevice.OomActive, 0, 1)) {
                gb_info("T2 OOM guard cleared — free=%lluMB", availBytes >> 20);
            }
        }

        /* --- Tier 3: Pagefile/swap pressure check --- */
        {
            SYSTEM_PERFORMANCE_INFORMATION perfInfo = { 0 };
            NTSTATUS status;
            ULONG retLen;

            status = ZwQuerySystemInformation(SystemPerformanceInformation,
                                              &perfInfo, sizeof(perfInfo), &retLen);
            if (NT_SUCCESS(status) && perfInfo.CommitLimit > 0) {
                ULONG64 commitUsedPct = ((ULONG64)perfInfo.CommittedPages * 100ULL)
                                        / (ULONG64)perfInfo.CommitLimit;

                if (commitUsedPct >= 90)
                    newPressure = GB_SWAP_PRESSURE_CRITICAL;
                else if (commitUsedPct >= 75)
                    newPressure = GB_SWAP_PRESSURE_WARN;
                else
                    newPressure = GB_SWAP_PRESSURE_OK;

                oldPressure = InterlockedExchange(&GbGlobalDevice.SwapPressure,
                                                  newPressure);

                if (newPressure != oldPressure) {
                    /* Signal pressure event to userspace */
                    if (GbGlobalDevice.PressureEvent)
                        KeSetEvent(GbGlobalDevice.PressureEvent,
                                   IO_NO_INCREMENT, FALSE);

                    if (newPressure == GB_SWAP_PRESSURE_CRITICAL)
                        gb_warn("T3 pagefile CRITICAL — %llu%% commit used",
                                commitUsedPct);
                    else if (newPressure == GB_SWAP_PRESSURE_WARN)
                        gb_warn("T3 pagefile warn — %llu%% commit used",
                                commitUsedPct);
                    else
                        gb_info("T3 pagefile pressure cleared");
                }
            }
        }
    }

    gb_info("watchdog stopped");
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS GbStartWatchdog(VOID)
{
    NTSTATUS status;
    HANDLE threadHandle;
    OBJECT_ATTRIBUTES oa;

    GbGlobalDevice.WatchdogRunning = TRUE;

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);

    status = PsCreateSystemThread(&threadHandle,
                                  THREAD_ALL_ACCESS,
                                  &oa,
                                  NULL,
                                  NULL,
                                  GbWatchdogThread,
                                  NULL);
    if (!NT_SUCCESS(status)) {
        GbGlobalDevice.WatchdogRunning = FALSE;
        gb_err("PsCreateSystemThread failed: 0x%08X", status);
        return status;
    }

    /* Get thread object for later join */
    status = ObReferenceObjectByHandle(threadHandle,
                                       THREAD_ALL_ACCESS,
                                       *PsThreadType,
                                       KernelMode,
                                       (PVOID*)&GbGlobalDevice.WatchdogThread,
                                       NULL);
    ZwClose(threadHandle);

    if (!NT_SUCCESS(status)) {
        GbGlobalDevice.WatchdogRunning = FALSE;
        gb_err("ObReferenceObjectByHandle for watchdog failed: 0x%08X", status);
        return status;
    }

    /*
     * Set CPU affinity — pin watchdog away from P-cores (E-cores only)
     * to keep golden P-cores free for inference. Auto-detect topology
     * by checking processor groups.
     *
     * TODO: Read P-core/E-core topology from CPUID or registry.
     * For now, let the OS schedule freely.
     */

    gb_info("watchdog thread started");
    return STATUS_SUCCESS;
}

VOID GbStopWatchdog(VOID)
{
    if (!GbGlobalDevice.WatchdogThread)
        return;

    gb_info("stopping watchdog...");
    GbGlobalDevice.WatchdogRunning = FALSE;

    /* Wait for thread to exit (up to 10 seconds) */
    {
        LARGE_INTEGER timeout;
        timeout.QuadPart = -100000000LL;  /* 10 seconds */
        KeWaitForSingleObject(GbGlobalDevice.WatchdogThread,
                              Executive, KernelMode, FALSE, &timeout);
    }

    ObDereferenceObject(GbGlobalDevice.WatchdogThread);
    GbGlobalDevice.WatchdogThread = NULL;
    gb_info("watchdog stopped");
}

/* ================================================================== */
/*  Named pressure event — for userspace signaling                      */
/* ================================================================== */

static NTSTATUS GbCreatePressureEvent(VOID)
{
    NTSTATUS status;
    UNICODE_STRING eventName;
    OBJECT_ATTRIBUTES oa;
    HANDLE eventHandle;

    RtlInitUnicodeString(&eventName, GB_PRESSURE_EVENT);
    InitializeObjectAttributes(&oa, &eventName,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL, NULL);

    status = ZwCreateEvent(&eventHandle, EVENT_ALL_ACCESS, &oa,
                           SynchronizationEvent, FALSE);
    if (!NT_SUCCESS(status)) {
        gb_err("ZwCreateEvent failed: 0x%08X", status);
        return status;
    }

    status = ObReferenceObjectByHandle(eventHandle, EVENT_ALL_ACCESS,
                                       *ExEventObjectType, KernelMode,
                                       (PVOID*)&GbGlobalDevice.PressureEvent,
                                       NULL);
    if (!NT_SUCCESS(status)) {
        ZwClose(eventHandle);
        gb_err("ObReferenceObjectByHandle for event failed: 0x%08X", status);
        return status;
    }

    GbGlobalDevice.PressureEventHandle = eventHandle;
    gb_info("pressure event created: %wZ", &eventName);
    return STATUS_SUCCESS;
}

static VOID GbDestroyPressureEvent(VOID)
{
    if (GbGlobalDevice.PressureEvent) {
        ObDereferenceObject(GbGlobalDevice.PressureEvent);
        GbGlobalDevice.PressureEvent = NULL;
    }
    if (GbGlobalDevice.PressureEventHandle) {
        ZwClose(GbGlobalDevice.PressureEventHandle);
        GbGlobalDevice.PressureEventHandle = NULL;
    }
}

/* ================================================================== */
/*  IOCTL handlers                                                      */
/* ================================================================== */

NTSTATUS GbHandleAlloc(_In_ WDFREQUEST Request)
{
    NTSTATUS status;
    struct gb_alloc_req_win *req;
    size_t bufLen;
    PGB_BUF_WIN buf;
    LONG id;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*req),
                                           (PVOID*)&req, &bufLen);
    if (!NT_SUCCESS(status))
        return status;

    if (bufLen < sizeof(*req) || req->size == 0)
        return STATUS_INVALID_PARAMETER;

    /* Cap allocation size */
    if (req->size > (ULONG64)GbGlobalDevice.VirtualVramGb * (1ULL << 30) +
                    (ULONG64)GbGlobalDevice.NvmePoolGb * (1ULL << 30))
        return STATUS_INVALID_PARAMETER;

    /* Try T2 first, then fall back to T3 */
    buf = GbAllocTier2((SIZE_T)req->size, req->flags);
    if (!buf) {
        buf = GbAllocTier3((SIZE_T)req->size, req->flags);
        if (!buf)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Register buffer */
    id = GbRegisterBuf(buf);
    if (id == 0) {
        GbFreeBuf(buf);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Create section for userspace sharing */
    status = GbCreateSection(buf);
    if (!NT_SUCCESS(status)) {
        GbFreeBuf(buf);
        return status;
    }

    /* Return results to caller via output buffer */
    {
        struct gb_alloc_req_win *out;
        size_t outLen;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out),
                                                (PVOID*)&out, &outLen);
        if (!NT_SUCCESS(status)) {
            GbFreeBuf(buf);
            return status;
        }

        out->size = buf->Size;
        out->handle = buf->SectionHandle;
        out->buf_id = buf->Id;
        out->flags = req->flags;

        WdfRequestSetInformation(Request, sizeof(*out));
    }

    gb_info("allocated %lluMB buffer (id=%ld %s)",
            (ULONG64)buf->Size >> 20, buf->Id,
            buf->Tier == GB_TIER2_DDR4 ? "T2-DDR4" : "T3-NVMe");

    return STATUS_SUCCESS;
}

NTSTATUS GbHandleGetInfo(_In_ WDFREQUEST Request)
{
    NTSTATUS status;
    struct gb_info_win *info;
    size_t bufLen;
    ULONG64 totalBytes, availBytes;
    ULONG64 allocBytes, t3AllocBytes;

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*info),
                                            (PVOID*)&info, &bufLen);
    if (!NT_SUCCESS(status))
        return status;

    RtlZeroMemory(info, sizeof(*info));

    GbQueryMemoryStatus(&totalBytes, &availBytes);
    allocBytes = (ULONG64)InterlockedCompareExchange64(
        &GbGlobalDevice.PoolAllocated, 0, 0);
    t3AllocBytes = (ULONG64)InterlockedCompareExchange64(
        &GbGlobalDevice.NvmeAllocated, 0, 0);

    /* Tier 1 */
    info->vram_physical_mb = (gb_u64)GbGlobalDevice.PhysicalVramGb * 1024ULL;

    /* Tier 2 */
    info->total_ram_mb = totalBytes >> 20;
    info->free_ram_mb = availBytes >> 20;
    info->allocated_mb = allocBytes >> 20;
    info->max_pool_mb = (gb_u64)GbGlobalDevice.VirtualVramGb * 1024ULL;
    info->safety_reserve_mb = (gb_u64)GbGlobalDevice.SafetyReserveGb * 1024ULL;
    {
        ULONG64 reserveBytes = (ULONG64)GbGlobalDevice.SafetyReserveGb * (1ULL << 30);
        info->available_mb = (availBytes > reserveBytes + allocBytes)
            ? (availBytes - reserveBytes - allocBytes) >> 20 : 0;
    }
    info->active_buffers = (gb_u32)GbGlobalDevice.ActiveBufs;
    info->oom_active = (gb_u32)GbGlobalDevice.OomActive;

    /* Tier 3 */
    info->nvme_swap_total_mb = (gb_u64)GbGlobalDevice.NvmeSwapGb * 1024ULL;
    info->nvme_t3_allocated_mb = t3AllocBytes >> 20;
    info->swap_pressure = (gb_u32)GbGlobalDevice.SwapPressure;
    /* Pagefile used/free — filled from watchdog's perf info if available */
    info->nvme_swap_used_mb = 0;
    info->nvme_swap_free_mb = info->nvme_swap_total_mb;

    /* Combined */
    info->total_combined_mb = info->vram_physical_mb
                            + info->max_pool_mb
                            + info->nvme_swap_total_mb;

    WdfRequestSetInformation(Request, sizeof(*info));
    return STATUS_SUCCESS;
}

NTSTATUS GbHandleReset(_In_ WDFREQUEST Request)
{
    UNREFERENCED_PARAMETER(Request);
    gb_info("RESET requested — close all handles to release buffers");
    return STATUS_SUCCESS;
}

NTSTATUS GbHandleMadvise(_In_ WDFREQUEST Request)
{
    NTSTATUS status;
    struct gb_madvise_req_win *req;
    size_t bufLen;
    PGB_BUF_WIN buf;
    KIRQL oldIrql;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*req),
                                           (PVOID*)&req, &bufLen);
    if (!NT_SUCCESS(status))
        return status;

    buf = GbLookupBuf(req->buf_id);
    if (!buf)
        return STATUS_NOT_FOUND;

    KeAcquireSpinLock(&GbGlobalDevice.LruLock, &oldIrql);

    switch (req->advise) {
    case GB_MADVISE_HOT:
        KeQuerySystemTime(&buf->LastAccessTime);
        /* Move to head of LRU (most recently used) */
        RemoveEntryList(&buf->LruNode);
        InsertHeadList(&GbGlobalDevice.LruList, &buf->LruNode);
        break;

    case GB_MADVISE_COLD:
        /* Move to tail of LRU (least recently used, evict first) */
        RemoveEntryList(&buf->LruNode);
        InsertTailList(&GbGlobalDevice.LruList, &buf->LruNode);
        break;

    case GB_MADVISE_FREEZE:
        buf->Frozen = TRUE;
        break;

    default:
        KeReleaseSpinLock(&GbGlobalDevice.LruLock, oldIrql);
        return STATUS_INVALID_PARAMETER;
    }

    KeReleaseSpinLock(&GbGlobalDevice.LruLock, oldIrql);

    gb_dbg("madvise buf id=%ld advise=%u", req->buf_id, req->advise);
    return STATUS_SUCCESS;
}

NTSTATUS GbHandleEvict(_In_ WDFREQUEST Request)
{
    NTSTATUS status;
    struct gb_evict_req_win *req;
    size_t bufLen;
    PGB_BUF_WIN buf;
    KIRQL oldIrql;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*req),
                                           (PVOID*)&req, &bufLen);
    if (!NT_SUCCESS(status))
        return status;

    buf = GbLookupBuf(req->buf_id);
    if (!buf)
        return STATUS_NOT_FOUND;

    /* Move T2 accounting to T3 (conceptual eviction) */
    if (buf->Tier == GB_TIER2_DDR4) {
        InterlockedAdd64(&GbGlobalDevice.PoolAllocated, -(LONG64)buf->Size);
        InterlockedAdd64(&GbGlobalDevice.NvmeAllocated, (LONG64)buf->Size);
        buf->Tier = GB_TIER3_NVME;

        /* Remove from LRU — evicted buffers are not candidates */
        KeAcquireSpinLock(&GbGlobalDevice.LruLock, &oldIrql);
        RemoveEntryList(&buf->LruNode);
        InitializeListHead(&buf->LruNode);
        KeReleaseSpinLock(&GbGlobalDevice.LruLock, oldIrql);

        gb_dbg("evict buf id=%ld: T2->T3 (%lluMB)",
               buf->Id, (ULONG64)buf->Size >> 20);
    }

    return STATUS_SUCCESS;
}

NTSTATUS GbHandlePollFd(_In_ WDFREQUEST Request)
{
    /*
     * On Windows, pressure signaling uses a global named event
     * (\\BaseNamedObjects\\GreenBoostPressure). Userspace opens it
     * by name with OpenEvent(). This IOCTL is kept for API parity
     * but the primary mechanism is the named event.
     *
     * If the caller provides a per-process event handle, we could
     * duplicate and signal it too. For now, return success.
     */
    UNREFERENCED_PARAMETER(Request);
    gb_dbg("POLL_FD: using named event %ls", GB_PRESSURE_EVENT);
    return STATUS_SUCCESS;
}

NTSTATUS GbHandlePinUserPtr(_In_ WDFREQUEST Request)
{
    NTSTATUS status;
    struct gb_pin_req_win *req;
    size_t bufLen;
    PGB_BUF_WIN buf;
    PMDL mdl;
    PVOID sysAddr;
    LONG id;
    ULONG64 t2Max, t2Used;
    SIZE_T alignedSize;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*req),
                                           (PVOID*)&req, &bufLen);
    if (!NT_SUCCESS(status))
        return status;

    if (!req->vaddr || !req->size)
        return STATUS_INVALID_PARAMETER;

    /* Check T2 capacity */
    t2Max = (ULONG64)GbGlobalDevice.VirtualVramGb * (1ULL << 30);
    t2Used = (ULONG64)InterlockedCompareExchange64(
        &GbGlobalDevice.PoolAllocated, 0, 0);

    alignedSize = ALIGN_UP_BY((SIZE_T)req->size, PAGE_SIZE);

    if (t2Used + alignedSize > t2Max)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Allocate buffer descriptor */
    buf = (PGB_BUF_WIN)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(GB_BUF_WIN), GB_TAG);
    if (!buf)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(buf, sizeof(GB_BUF_WIN));

    /*
     * Pin user pages — Windows equivalent of pin_user_pages(FOLL_LONGTERM).
     * Create an MDL for the user VA and lock the pages in memory.
     */
    mdl = IoAllocateMdl((PVOID)(ULONG_PTR)req->vaddr, (ULONG)alignedSize,
                        FALSE, FALSE, NULL);
    if (!mdl) {
        ExFreePoolWithTag(buf, GB_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    __try {
        MmProbeAndLockPages(mdl, UserMode, IoWriteAccess);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        IoFreeMdl(mdl);
        ExFreePoolWithTag(buf, GB_TAG);
        gb_err("MmProbeAndLockPages failed for user VA 0x%llX", req->vaddr);
        return STATUS_ACCESS_VIOLATION;
    }

    sysAddr = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
    if (!sysAddr) {
        MmUnlockPages(mdl);
        IoFreeMdl(mdl);
        ExFreePoolWithTag(buf, GB_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    buf->KernelVa = sysAddr;
    buf->Mdl = mdl;
    buf->Size = alignedSize;
    buf->Contiguous = FALSE;
    buf->Tier = GB_TIER2_DDR4;
    buf->AllocFlags = req->flags;
    buf->Frozen = (req->flags & GB_ALLOC_FROZEN) ? TRUE : FALSE;
    KeQuerySystemTime(&buf->AllocTime);
    buf->LastAccessTime = buf->AllocTime;
    InitializeListHead(&buf->LruNode);

    /* Update T2 accounting */
    InterlockedAdd64(&GbGlobalDevice.PoolAllocated, (LONG64)alignedSize);
    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&GbGlobalDevice.LruLock, &oldIrql);
        InsertTailList(&GbGlobalDevice.LruList, &buf->LruNode);
        KeReleaseSpinLock(&GbGlobalDevice.LruLock, oldIrql);
    }

    /* Register buffer */
    id = GbRegisterBuf(buf);
    if (id == 0) {
        GbFreeBuf(buf);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Create section for sharing */
    status = GbCreateSection(buf);
    if (!NT_SUCCESS(status)) {
        GbFreeBuf(buf);
        return status;
    }

    /* Return results */
    {
        struct gb_pin_req_win *out;
        size_t outLen;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out),
                                                (PVOID*)&out, &outLen);
        if (!NT_SUCCESS(status)) {
            GbFreeBuf(buf);
            return status;
        }

        out->vaddr = req->vaddr;
        out->size = buf->Size;
        out->handle = buf->SectionHandle;
        out->buf_id = buf->Id;
        out->flags = req->flags;

        WdfRequestSetInformation(Request, sizeof(*out));
    }

    gb_info("pinned %lluMB user buffer (id=%ld)",
            (ULONG64)buf->Size >> 20, buf->Id);

    return STATUS_SUCCESS;
}

/* ================================================================== */
/*  IOCTL dispatch                                                      */
/* ================================================================== */

VOID GbEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case GB_IOCTL_ALLOC:
        status = GbHandleAlloc(Request);
        break;
    case GB_IOCTL_GET_INFO:
        status = GbHandleGetInfo(Request);
        break;
    case GB_IOCTL_RESET:
        status = GbHandleReset(Request);
        break;
    case GB_IOCTL_MADVISE:
        status = GbHandleMadvise(Request);
        break;
    case GB_IOCTL_EVICT:
        status = GbHandleEvict(Request);
        break;
    case GB_IOCTL_POLL_FD:
        status = GbHandlePollFd(Request);
        break;
    case GB_IOCTL_PIN_USER_PTR:
        status = GbHandlePinUserPtr(Request);
        break;
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestComplete(Request, status);
}

/* ================================================================== */
/*  WDF device creation                                                 */
/* ================================================================== */

NTSTATUS GbEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE queue;
    DECLARE_CONST_UNICODE_STRING(deviceName, L"\\Device\\GreenBoost");
    DECLARE_CONST_UNICODE_STRING(symlinkName, L"\\DosDevices\\GreenBoost");

    UNREFERENCED_PARAMETER(Driver);

    /* Set device name */
    status = WdfDeviceInitAssignName(DeviceInit, &deviceName);
    if (!NT_SUCCESS(status)) {
        gb_err("WdfDeviceInitAssignName failed: 0x%08X", status);
        return status;
    }

    /* Set device type and characteristics */
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    /* Allow non-admin access */
    {
        DECLARE_CONST_UNICODE_STRING(sddl,
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;WD)");
        status = WdfDeviceInitAssignSDDLString(DeviceInit, &sddl);
        if (!NT_SUCCESS(status)) {
            gb_err("WdfDeviceInitAssignSDDLString failed: 0x%08X", status);
            return status;
        }
    }

    /* Create the device */
    {
        WDF_OBJECT_ATTRIBUTES deviceAttrs;
        WDF_OBJECT_ATTRIBUTES_INIT(&deviceAttrs);
        deviceAttrs.EvtCleanupCallback = GbEvtDeviceCleanup;

        status = WdfDeviceCreate(&DeviceInit, &deviceAttrs, &device);
        if (!NT_SUCCESS(status)) {
            gb_err("WdfDeviceCreate failed: 0x%08X", status);
            return status;
        }
    }

    GbGlobalDevice.WdfDevice = device;

    /* Create symbolic link \\.\GreenBoost */
    status = WdfDeviceCreateSymbolicLink(device, &symlinkName);
    if (!NT_SUCCESS(status)) {
        gb_err("WdfDeviceCreateSymbolicLink failed: 0x%08X", status);
        return status;
    }

    /* Create default I/O queue for IOCTL dispatch */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig,
                                            WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = GbEvtIoDeviceControl;

    status = WdfIoQueueCreate(device, &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        gb_err("WdfIoQueueCreate failed: 0x%08X", status);
        return status;
    }

    GbGlobalDevice.IoQueue = queue;

    /* Create pressure event */
    status = GbCreatePressureEvent();
    if (!NT_SUCCESS(status)) {
        gb_warn("pressure event creation failed (non-fatal): 0x%08X", status);
        /* Continue — pressure signaling is optional */
    }

    /* Start watchdog */
    status = GbStartWatchdog();
    if (!NT_SUCCESS(status)) {
        gb_warn("watchdog start failed (non-fatal): 0x%08X", status);
    }

    gb_info("device ready: \\\\.\\GreenBoost");
    return STATUS_SUCCESS;
}

/* ================================================================== */
/*  Device cleanup — called on driver unload                            */
/* ================================================================== */

VOID GbEvtDeviceCleanup(_In_ WDFOBJECT Device)
{
    LONG i;

    UNREFERENCED_PARAMETER(Device);

    gb_info("unloading GreenBoost v2.3");

    /* Stop watchdog */
    GbStopWatchdog();

    /* Free all outstanding buffers */
    ExAcquireFastMutex(&GbGlobalDevice.BufLock);
    for (i = 1; i < GB_MAX_BUFS; i++) {
        PGB_BUF_WIN buf = GbGlobalDevice.BufTable[i];
        if (buf && buf->Active) {
            GbGlobalDevice.BufTable[i] = NULL;
            ExReleaseFastMutex(&GbGlobalDevice.BufLock);
            GbFreeBuf(buf);
            ExAcquireFastMutex(&GbGlobalDevice.BufLock);
        }
    }
    ExReleaseFastMutex(&GbGlobalDevice.BufLock);

    /* Destroy pressure event */
    GbDestroyPressureEvent();

    gb_info("unloaded cleanly");
}

/* ================================================================== */
/*  DriverEntry                                                         */
/* ================================================================== */

NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    WDF_DRIVER_CONFIG driverConfig;

    gb_info("=====================================================");
    gb_info("GreenBoost v2.3 — 3-Tier GPU Memory Pool (Windows)");
    gb_info("Author  : Ferran Duarri");
    gb_info("=====================================================");

    /* Initialize global state */
    RtlZeroMemory(&GbGlobalDevice, sizeof(GbGlobalDevice));
    ExInitializeFastMutex(&GbGlobalDevice.BufLock);
    InitializeListHead(&GbGlobalDevice.LruList);
    KeInitializeSpinLock(&GbGlobalDevice.LruLock);

    /* Read configuration from registry */
    GbReadConfig();

    gb_info("T1 VRAM : %lu GB", GbGlobalDevice.PhysicalVramGb);
    gb_info("T2 DDR4 : pool cap %lu GB (reserve %lu GB)",
            GbGlobalDevice.VirtualVramGb, GbGlobalDevice.SafetyReserveGb);
    gb_info("T3 NVMe : %lu GB (cap %lu GB)",
            GbGlobalDevice.NvmeSwapGb, GbGlobalDevice.NvmePoolGb);
    gb_info("Combined: %lu GB total model capacity",
            GbGlobalDevice.PhysicalVramGb +
            GbGlobalDevice.VirtualVramGb +
            GbGlobalDevice.NvmeSwapGb);

    /* Create WDF driver */
    WDF_DRIVER_CONFIG_INIT(&driverConfig, GbEvtDeviceAdd);

    status = WdfDriverCreate(DriverObject, RegistryPath,
                             WDF_NO_OBJECT_ATTRIBUTES,
                             &driverConfig, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        gb_err("WdfDriverCreate failed: 0x%08X", status);
        return status;
    }

    return STATUS_SUCCESS;
}
