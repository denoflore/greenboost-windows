# GreenBoost Windows Port — CC Build Instructions

## EXECUTION PATTERN

**Follow this iterative pattern:**
1. Read a section of this file
2. Build what that section specifies
3. Test/validate what you built
4. Read the file AGAIN to find where you are
5. Continue to next section
6. Repeat until complete

**DO NOT try to build everything at once.**

---

## PROJECT OVERVIEW

Port the open-source GreenBoost GPU memory extension (Linux) to Windows. GreenBoost transparently extends NVIDIA GPU VRAM with system RAM and NVMe storage for running larger LLMs locally. The Linux version uses a kernel module (greenboost.ko) to allocate pinned DDR4 pages exported as DMA-BUF, plus a CUDA shim (LD_PRELOAD) to intercept memory allocations.

The Windows port replaces Linux-specific subsystems with Windows equivalents while preserving identical CUDA-facing behavior. End state: a Windows KMDF driver (greenboost.sys) + proxy DLL (greenboost_cuda.dll) that lets LM Studio / Ollama / llama.cpp see and use system RAM as extended VRAM.

**Target:** Windows 10/11 x64, NVIDIA GPUs with CUDA 12+, test-signed driver initially.

---

## REQUIRED READING

Before writing ANY code, read and internalize these source files from the cloned repo:

1. **`reference/greenboost_ioctl.h`** — IOCTL interface contract (shared kernel/user). This is the ABI. Preserve it exactly.
2. **`reference/architecture.md`** — Full architecture doc with 3-tier memory hierarchy
3. **`reference/greenboost.c`** — Linux kernel module (1268 lines). Understand: gb_buf struct, DMA-BUF ops, IOCTL handlers, watchdog kthread, sysfs attrs
4. **`reference/greenboost_cuda_shim.c`** — CUDA shim (1143 lines). Understand: dlsym interception, cudaMalloc hook, hash table, DMA-BUF import path, NVML spoofing

---

## CRITICAL REQUIREMENTS

**ARCHITECTURE MAPPING (Linux -> Windows):**

| Linux Concept | Windows Equivalent | Notes |
|---|---|---|
| Kernel module (.ko) | KMDF driver (.sys) | Use WDF, not legacy WDM |
| `/dev/greenboost` char device | `\\.\GreenBoost` device via WDF | IoCreateDevice + symbolic link |
| `ioctl()` | `DeviceIoControl()` | Same IOCTL codes, redefine with CTL_CODE macro |
| DMA-BUF fd export | NT handle to shared memory section | `ZwCreateSection` + `ObOpenObjectByPointer` |
| `alloc_pages(GFP_KERNEL \| __GFP_COMP, 9)` | `MmAllocateContiguousMemorySpecifyCache()` for 2MB blocks | Or `MmAllocatePagesForMdlEx` for flexibility |
| `sg_table` / `dma_map_sgtable` | MDL (Memory Descriptor List) + `MmGetSystemAddressForMdlSafe` | Windows DMA uses MDLs |
| `LD_PRELOAD` shim injection | Proxy DLL pattern for `nvcuda.dll` | Rename real -> `nvcuda_real.dll`, our DLL becomes `nvcuda.dll` |
| `dlsym` interception | IAT hooking via Microsoft Detours (MIT license) | Or manual IAT patching |
| `cudaExternalMemoryHandleTypeOpaqueFd` | `cudaExternalMemoryHandleTypeOpaqueWin32` | Windows CUDA uses NT handles, not fds |
| `mmap(MAP_SHARED, dma_buf_fd)` | `MapViewOfFile` on section handle | Or `cuMemHostRegister` on VirtualAlloc'd memory |
| `eventfd` for pressure signals | Named Event object (`CreateEvent`) | Shim opens by name, driver signals via `KeSetEvent` |
| `sysfs` attributes | WMI provider or DeviceIoControl query | Start with IOCTL-based stats, WMI later |
| `kthread` watchdog | `PsCreateSystemThread` + `KeDelayExecutionThread` | System worker thread |
| `pthread_mutex_t` | `CRITICAL_SECTION` (userspace) / `FAST_MUTEX` (kernel) | Direct mapping |
| `nr_free_pages` / swap info | `GlobalMemoryStatusEx` (user) / `MmQuerySystemMemoryInformation` (kernel) | Different API, same data |

**CUDA EXTERNAL MEMORY — THE KEY DIFFERENCE:**

The Linux shim actually has TWO import paths:
1. **Primary:** `cudaImportExternalMemory(cudaExternalMemoryHandleTypeOpaqueFd)` + `cudaExternalMemoryGetMappedBuffer` — imports DMA-BUF fd directly as a CUDA external memory object
2. **Fallback (mmap path):** kernel exports DMA-BUF fd -> shim `mmap(fd)` -> `cuMemHostRegister(DEVICEMAP)` -> `cuMemHostGetDevicePointer`

The Linux shim code uses path #2 (mmap + cuMemHostRegister). On Windows:
- Path #1 would use `cudaExternalMemoryHandleTypeOpaqueWin32` with an NT handle — but this requires CUDA to understand the memory backing, which is complex with section objects
- **Path #2 (recommended for Windows):** driver allocates pages -> creates shared section -> returns NT handle -> shim `MapViewOfFile(handle)` -> `cuMemHostRegister(DEVICEMAP)` -> `cuMemHostGetDevicePointer`

The `cuMemHostRegister` path works identically on both platforms. This is the critical insight that makes the port tractable. The only difference is how the memory gets mapped into the shim's address space (mmap vs MapViewOfFile).

**DRIVER SIGNING:**
- Use test signing during development: `bcdedit /set testsigning on`
- For distribution: obtain EV code signing certificate + submit to Microsoft for attestation signing
- Document both paths

**SECURITY:** Driver must validate all IOCTL input sizes and buffer pointers. Use `METHOD_BUFFERED` for all IOCTLs. Never trust userspace sizes without bounds checking.

**TESTING:** Each phase must compile and load. Driver must not BSOD under any input. Shim must gracefully fall through to real CUDA on any failure.

**DO NOT:**
- Do NOT modify any NVIDIA driver files
- Do NOT require CUDA SDK to build the shim (define minimal types inline, as original does)
- Do NOT use GPL-only Windows APIs (this port is independently developed, clean-room from architecture doc)
- Do NOT hardcode hardware values; detect at runtime like the Linux version

**ADDITIONAL WINDOWS-SPECIFIC NOTES:**

1. **Async prefetch worker:** The Linux shim has a `prefetch_worker` thread that calls `madvise(MADV_WILLNEED)`. On Windows, port this to `PrefetchVirtualMemory` (available Windows 8+). The circular queue + condition variable logic ports directly (`pthread_cond_t` → `CONDITION_VARIABLE`, `pthread_mutex_t` → `CRITICAL_SECTION`).

2. **Hash table delete bug:** The Linux `ht_remove` clears slots with `memset(e, 0, sizeof(*e))` which breaks open-addressing probe chains (a later probe past the cleared slot will stop early). The Windows port should use a tombstone marker instead of zeroing, or implement Robin Hood deletion. This is a pre-existing Linux bug worth fixing in the port.

3. **ExLlamaV3 Python integration:** The Linux ExLlamaV3 cache uses `mmap.mmap(dma_buf_fd, ...)` for zero-copy DMA-BUF → numpy → PyTorch. On Windows, this path would use `mmap.mmap(-1, size, tagname='GreenBoostBuf_N')` on a named file mapping, or the shim could expose a Python-callable C extension. This is a stretch goal — Phase 5 focuses on Ollama/LM Studio first.

4. **IOCTL error cleanup in GB_IOCTL_ALLOC:** The Linux code's error path for `dma_buf_export` failure only frees 4K pages (`__free_page`), not hugepages. The Windows port must handle both contiguous and MDL-based allocations in error cleanup.

5. **Target architecture:** x64 only. The driver and shim are 64-bit. No 32-bit support needed (CUDA 12+ dropped 32-bit).

---

## Phase 1: Project Scaffold + Shared Header

**Goal:** Create the project structure with the Windows IOCTL header and build system.

**Files to create:**

```
greenboost-win/
  CMakeLists.txt                    — Top-level CMake (driver + shim)
  README.md                         — Project overview
  LICENSE                            — GPL v2 (matching upstream)
  reference/                         — Copy of Linux source for reference
    greenboost_ioctl.h
    greenboost.c
    greenboost_cuda_shim.c
    architecture.md
  driver/
    CMakeLists.txt                   — WDK/KMDF build config
    greenboost_ioctl_win.h           — Windows IOCTL definitions (CTL_CODE)
    greenboost_win.h                 — Driver internal header
    greenboost_win.c                 — Main driver source (stub)
    greenboost_win.inf               — Driver install INF
  shim/
    CMakeLists.txt                   — DLL build config
    greenboost_cuda_shim_win.h       — Shim header
    greenboost_cuda_shim_win.c       — Shim source (stub)
    greenboost_cuda.def              — DLL export definitions
  tools/
    install.ps1                      — PowerShell installer
    diagnose.ps1                     — Diagnostic script
    test_pool.py                     — Pool stress test (port of gb_pool_stress.py)
  tests/
    test_ioctl.c                     — IOCTL smoke tests
    test_shim.c                      — Shim unit tests
```

**greenboost_ioctl_win.h** must define:
```c
#define GB_IOCTL_TYPE 0x8000  // Device type for custom driver

// Windows IOCTL codes using CTL_CODE macro
// Preserve the same logical operations as Linux
#define GB_IOCTL_ALLOC       CTL_CODE(GB_IOCTL_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GB_IOCTL_GET_INFO    CTL_CODE(GB_IOCTL_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GB_IOCTL_RESET       CTL_CODE(GB_IOCTL_TYPE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GB_IOCTL_MADVISE     CTL_CODE(GB_IOCTL_TYPE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GB_IOCTL_EVICT       CTL_CODE(GB_IOCTL_TYPE, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GB_IOCTL_POLL_FD     CTL_CODE(GB_IOCTL_TYPE, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GB_IOCTL_PIN_USER_PTR CTL_CODE(GB_IOCTL_TYPE, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)
```

Reuse the struct definitions from `greenboost_ioctl.h` with Windows-compatible types:
- `gb_u64` -> `UINT64`
- `gb_u32` -> `UINT32`
- `gb_s32` -> `INT32`
- `gb_alloc_req.fd` becomes `gb_alloc_req.handle` (HANDLE, not int fd)
- `gb_alloc_req` must also return `buf_id` (INT32) — Linux derives buf_id from fd via IDR lookup, but Windows needs it explicit for MADVISE/EVICT operations
- `gb_pin_req.fd` -> `gb_pin_req.handle` (HANDLE) + add `buf_id` (INT32)
- `gb_poll_req.efd` is replaced entirely — Windows uses a named event (`\\BaseNamedObjects\\GreenBoostPressure`), so this IOCTL either becomes a no-op (shim opens event by name) or changes to accept a client event handle
- `gb_madvise_req` and `gb_evict_req` are unchanged (use buf_id, not fd)

**Validation:**
- [ ] Project compiles (cmake generates solution / makefile)
- [ ] IOCTL header compiles in both kernel and userspace contexts
- [ ] Reference files are present and readable
- [ ] README documents the architecture mapping

**After completing Phase 1, read this document again to find Phase 2.**

---

## Phase 2: KMDF Driver — Device Creation + Memory Pool

**Goal:** Create a loadable KMDF driver that registers `\\.\GreenBoost`, implements GB_IOCTL_ALLOC and GB_IOCTL_GET_INFO, and can allocate pinned system memory.

**Core driver structure (greenboost_win.c):**

```c
// Driver entry point
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    // Create WDFDRIVER
    // Create WDFDEVICE with device interface
    // Create symbolic link \\.\GreenBoost
    // Initialize memory pool state
}

// IOCTL dispatch
VOID GbEvtIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request,
                           size_t OutputBufferLength, size_t InputBufferLength,
                           ULONG IoControlCode)
{
    switch (IoControlCode) {
        case GB_IOCTL_ALLOC:    GbHandleAlloc(Request);    break;
        case GB_IOCTL_GET_INFO: GbHandleGetInfo(Request);  break;
        case GB_IOCTL_RESET:    GbHandleReset(Request);    break;
        // ... etc
    }
}
```

**Memory allocation (the critical function):**

The Linux version uses `alloc_pages(GFP_KERNEL | __GFP_COMP, 9)` for 2MB compound pages.

Windows equivalent:
```c
// Option A: Contiguous physical memory (best for DMA, like Linux compound pages)
PHYSICAL_ADDRESS low  = {0};
PHYSICAL_ADDRESS high = {.QuadPart = 0xFFFFFFFFFFFFFFFF};
PHYSICAL_ADDRESS boundary = {.QuadPart = 0x200000};  // 2MB alignment
PVOID va = MmAllocateContiguousMemorySpecifyCache(
    2 * 1024 * 1024,  // 2MB
    low, high, boundary,
    MmCached  // or MmWriteCombined for GPU access
);

// Option B: MDL-based (more flexible, works with non-contiguous)
PMDL mdl = MmAllocatePagesForMdlEx(
    low, high, low,
    totalBytes,
    MmCached,
    MM_ALLOCATE_FULLY_REQUIRED | MM_ALLOCATE_REQUIRE_CONTIGUOUS_CHUNKS
);
```

**Sharing memory with userspace:**

The Linux version exports a DMA-BUF fd. On Windows:
```c
// Create a section object backed by the physical pages
HANDLE sectionHandle;
OBJECT_ATTRIBUTES oa;
LARGE_INTEGER maxSize = {.QuadPart = allocSize};

// Lock pages in MDL, create section, return handle to userspace
// The userspace shim will MapViewOfFile on this handle
// Then call cuMemHostRegister on the mapped pointer
```

**Per-buffer tracking:**

Port the `gb_buf` struct. Replace Linux IDR with a simple ID-indexed array or Windows RTL_GENERIC_TABLE:
```c
typedef struct _GB_BUF_WIN {
    PVOID           KernelVa;       // Kernel virtual address
    PMDL            Mdl;            // Memory descriptor list
    PHYSICAL_ADDRESS PhysAddr;      // Physical address (for contiguous)
    SIZE_T          Size;
    LONG            Id;             // Unique buffer ID
    LONG            Tier;           // GB_TIER2_DDR4 or GB_TIER3_NVME
    HANDLE          SectionHandle;  // Shared section for userspace
    LIST_ENTRY      LruNode;        // LRU list link
    LARGE_INTEGER   AllocTime;
    LARGE_INTEGER   LastAccessTime;
    ULONG           AllocFlags;     // GB_ALLOC_* flags
    BOOLEAN         Frozen;
} GB_BUF_WIN, *PGB_BUF_WIN;
```

**Global state:**
```c
typedef struct _GB_DEVICE_WIN {
    WDFDEVICE       WdfDevice;
    FAST_MUTEX      Lock;
    LONG            NextBufId;
    LONG            ActiveBufs;
    LONG64          PoolAllocated;   // Bytes pinned (T2)
    LONG64          NvmeAllocated;   // Bytes in T3
    LONG            SwapPressure;
    LIST_ENTRY      LruList;
    KSPIN_LOCK      LruLock;
    KEVENT          PressureEvent;   // Named event for userspace signaling
    PKTHREAD        WatchdogThread;
    BOOLEAN         WatchdogRunning;
    // Configuration (from registry or auto-detect)
    ULONG           PhysicalVramGb;
    ULONG           VirtualVramGb;
    ULONG           SafetyReserveGb;
} GB_DEVICE_WIN, *PGB_DEVICE_WIN;
```

**Memory info query:** Replace Linux `/proc/meminfo` reads with:
```c
MEMORYSTATUSEX memStatus;
memStatus.dwLength = sizeof(memStatus);
// In kernel: use MmQuerySystemMemoryInformation or ZwQuerySystemInformation
```

**Validation:**
- [ ] Driver compiles with WDK
- [ ] Driver loads with `devcon install greenboost_win.inf` (test-signed)
- [ ] `\\.\GreenBoost` device appears in Device Manager
- [ ] GB_IOCTL_GET_INFO returns valid pool statistics
- [ ] GB_IOCTL_ALLOC allocates 2MB of pinned memory, returns handle
- [ ] Allocated memory survives for duration of handle lifetime
- [ ] Memory is freed when handle is closed
- [ ] No BSOD under repeated alloc/free cycles

**After completing Phase 2, read this document again to find Phase 3.**

---

## Phase 3: KMDF Driver — Watchdog + Pressure Events

**Goal:** Add the watchdog thread that monitors memory pressure and signals userspace.

**Watchdog thread (port of Linux kthread):**

```c
VOID GbWatchdogThread(PVOID Context)
{
    PGB_DEVICE_WIN dev = (PGB_DEVICE_WIN)Context;
    LARGE_INTEGER interval;
    interval.QuadPart = -10000000LL;  // 1 second in 100ns units

    while (dev->WatchdogRunning) {
        KeDelayExecutionThread(KernelMode, FALSE, &interval);

        // Query system memory
        ULONG64 totalPhys, availPhys;
        GbQueryMemoryStatus(&totalPhys, &availPhys);

        ULONG64 safetyBytes = (ULONG64)dev->SafetyReserveGb * 1024 * 1024 * 1024;

        if (availPhys < safetyBytes) {
            // Trip OOM guard
            InterlockedExchange(&dev->OomActive, 1);

            // Signal pressure event to userspace
            KeSetEvent(&dev->PressureEvent, IO_NO_INCREMENT, FALSE);
        } else {
            InterlockedExchange(&dev->OomActive, 0);
        }

        // Update swap pressure (check pagefile usage)
        GbUpdateSwapPressure(dev);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}
```

**CPU affinity (port P-core pinning):**

```c
// Set thread affinity to P-cores only
// On Windows, use KeSetSystemAffinityThreadEx with appropriate mask
// Auto-detect P/E core topology via CPUID or registry
GROUP_AFFINITY affinity = {0};
affinity.Mask = pCoreMask;  // Computed from topology detection
KeSetSystemGroupAffinityThread(&affinity, NULL);
```

**Named event for userspace:**

```c
// Create named event accessible from userspace
UNICODE_STRING eventName;
RtlInitUnicodeString(&eventName, L"\\BaseNamedObjects\\GreenBoostPressure");
OBJECT_ATTRIBUTES oa;
InitializeObjectAttributes(&oa, &eventName, OBJ_KERNEL_HANDLE, NULL, NULL);
ZwCreateEvent(&dev->PressureEventHandle, EVENT_ALL_ACCESS, &oa, SynchronizationEvent, FALSE);
```

**Validation:**
- [ ] Watchdog thread starts on driver load
- [ ] Watchdog detects low memory conditions
- [ ] Pressure event is signalable and observable from userspace
- [ ] Thread terminates cleanly on driver unload
- [ ] CPU affinity is set correctly (verify with Process Explorer)

**After completing Phase 3, read this document again to find Phase 4.**

---

## Phase 4: CUDA Shim DLL — Proxy Pattern + cudaMalloc Hook

**Goal:** Create the proxy DLL that intercepts CUDA memory allocation calls and routes large allocations to the GreenBoost driver.

**Proxy DLL strategy:**

Instead of LD_PRELOAD (Linux), use the proxy DLL pattern:
1. Rename the real `nvcuda.dll` to `nvcuda_real.dll` (or load it by full path)
2. Our `nvcuda.dll` (or `greenboost_cuda.dll` loaded via AppInit_DLLs or similar) exports the same symbols
3. Forward all calls to the real DLL, intercepting only memory allocation functions

**Better approach — Detours-based injection:**

Microsoft Detours (MIT license) is cleaner than proxy DLL:
```c
#include <detours.h>

// Real function pointers
static pfn_cudaMalloc real_cudaMalloc = NULL;
static pfn_cudaFree   real_cudaFree   = NULL;

// Our hook
cudaError_t CUDAAPI hooked_cudaMalloc(void **devPtr, size_t size)
{
    if (size < GB_THRESHOLD_BYTES) {
        return real_cudaMalloc(devPtr, size);
    }
    // Route to GreenBoost driver
    return gb_alloc_from_driver(devPtr, size);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        // Load real CUDA DLL
        HMODULE hCuda = LoadLibraryA("nvcuda.dll");  // or full path
        real_cudaMalloc = (pfn_cudaMalloc)GetProcAddress(hCuda, "cudaMalloc");

        // Install hooks
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)real_cudaMalloc, hooked_cudaMalloc);
        DetourTransactionCommit();
    }
    return TRUE;
}
```

**GreenBoost driver communication from userspace:**

```c
static HANDLE gb_open_device(void)
{
    EnterCriticalSection(&gb_dev_lock);
    if (gb_dev_handle == INVALID_HANDLE_VALUE) {
        gb_dev_handle = CreateFileW(
            L"\\\\.\\GreenBoost",
            GENERIC_READ | GENERIC_WRITE,
            0, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL
        );
    }
    LeaveCriticalSection(&gb_dev_lock);
    return gb_dev_handle;
}

static CUresult gb_alloc_from_driver(void **devPtr, size_t size)
{
    HANDLE hDev = gb_open_device();
    if (hDev == INVALID_HANDLE_VALUE)
        return real_cudaMalloc(devPtr, size);  // graceful fallthrough

    // Send IOCTL to driver
    struct gb_alloc_req_win req = {0};
    req.size = size;
    req.flags = GB_ALLOC_WEIGHTS;
    DWORD bytesReturned;
    BOOL ok = DeviceIoControl(hDev, GB_IOCTL_ALLOC,
        &req, sizeof(req), &req, sizeof(req), &bytesReturned, NULL);

    if (!ok) {
        // Fall through to real CUDA
        return real_cudaMalloc(devPtr, size);
    }

    // Map the shared section into our process
    PVOID mappedPtr = MapViewOfFile(req.handle,
        FILE_MAP_ALL_ACCESS, 0, 0, size);

    // Register with CUDA
    CUresult ret = real_cuMemHostRegister(mappedPtr, size,
        CU_MEMHOSTREGISTER_DEVICEMAP);
    if (ret != CUDA_SUCCESS) {
        UnmapViewOfFile(mappedPtr);
        CloseHandle(req.handle);
        return real_cudaMalloc(devPtr, size);
    }

    // Get device pointer
    CUdeviceptr dptr;
    ret = real_cuMemHostGetDevicePointer(&dptr, mappedPtr, 0);
    if (ret != CUDA_SUCCESS) {
        // cleanup...
        return real_cudaMalloc(devPtr, size);
    }

    *devPtr = (void*)dptr;
    // Track in hash table for cleanup
    // Note: Windows version stores HANDLE (req.handle) instead of int fd
    // The gb_ht_entry_t struct needs fd field changed to HANDLE type
    ht_insert(dptr, size, 0, req.bufId, mappedPtr, req.handle);
    return CUDA_SUCCESS;
}
```

**Hash table:** Port directly from Linux. Replace `pthread_mutex_t` with `CRITICAL_SECTION`. The Fibonacci hash and open-addressing logic is platform-independent.

**VRAM spoofing (cuDeviceTotalMem hook):**

Same logic as Linux, hook `cuDeviceTotalMem_v2` and `nvmlDeviceGetMemoryInfo` to report physical_vram + virtual_vram.

**DLL injection method:** Document multiple options:
1. **AppInit_DLLs registry key** (simplest for development)
2. **SetWindowsHookEx** (process-specific)
3. **Detours `withdll.exe`** (cleanest for per-app injection)
4. **CreateRemoteThread** (most control)
5. **LM Studio custom launch script** (application-specific, easiest)

Recommend option 5 for LM Studio: create a batch file that sets environment or uses Detours to inject before LM Studio loads CUDA.

**Validation:**
- [ ] Shim DLL compiles
- [ ] Shim successfully hooks cudaMalloc when injected into a test CUDA program
- [ ] Small allocations (<256MB) pass through to real CUDA unchanged
- [ ] Large allocations route to GreenBoost driver
- [ ] cuDeviceTotalMem reports extended VRAM
- [ ] cudaFree correctly releases GreenBoost-allocated memory
- [ ] Graceful fallthrough to real CUDA on any GreenBoost failure
- [ ] Hash table correctly tracks all allocations
- [ ] No memory leaks on repeated alloc/free cycles

**After completing Phase 4, read this document again to find Phase 5.**

---

## Phase 5: Integration + LM Studio / Ollama Support

**Goal:** End-to-end integration with LM Studio and Ollama on Windows.

**LM Studio integration:**

LM Studio on Windows uses llama.cpp under the hood. It loads CUDA via the standard Windows DLL search path.

Injection approach for LM Studio:
```powershell
# install.ps1 — Installer script

# 1. Detect LM Studio install path
$lmStudioPath = "$env:USERPROFILE\.cache\lm-studio"

# 2. Copy greenboost_cuda.dll to injection point
# 3. Create launcher that uses Detours withdll.exe
#    withdll.exe /d:greenboost_cuda.dll "C:\path\to\LM Studio.exe"

# 4. Or: set AppInit_DLLs for targeted process (less clean)
```

**Ollama integration (Windows):**

Ollama on Windows also uses CUDA via nvcuda.dll. Same injection approach.

**Configuration (registry-based, replaces Linux module params):**

```
HKLM\SOFTWARE\GreenBoost\
    PhysicalVramGb  REG_DWORD  (auto-detected from nvidia-smi)
    VirtualVramGb   REG_DWORD  (computed: 80% of system RAM)
    SafetyReserveGb REG_DWORD  (default: 12)
    ThresholdMb     REG_DWORD  (default: 256, allocation routing threshold)
    DebugMode       REG_DWORD  (0 or 1)
```

**Hardware auto-detection:**
```c
// Detect VRAM from NVML or nvidia-smi
// Detect system RAM from GlobalMemoryStatusEx
// Detect NVMe from GetDiskFreeSpaceEx on system drive
// Detect CPU topology from GetLogicalProcessorInformationEx
// Write detected values to registry
```

**Diagnostic tool (diagnose.ps1):**
```powershell
# Check driver loaded
sc query greenboost

# Check device accessible
# Try CreateFile on \\.\GreenBoost

# Query pool info via IOCTL
# Check if shim is loaded in LM Studio process
# Verify cuDeviceTotalMem reports extended VRAM
```

**Validation:**
- [ ] Full install script works on clean Windows 10/11
- [ ] LM Studio sees extended VRAM
- [ ] LM Studio loads model larger than physical VRAM
- [ ] Model inference actually works (tokens generated)
- [ ] Ollama sees extended VRAM
- [ ] diagnose.ps1 reports correct system state
- [ ] Clean uninstall removes all components
- [ ] System stable under sustained inference workload

**After completing Phase 5, read this document again to find Phase 6.**

---

## Phase 6: Optimization + Hardening

**Goal:** Performance optimization, error handling hardening, and documentation.

**Performance optimizations:**
1. **Large page support** — Use `MmAllocatePagesForMdlEx` with `MM_ALLOCATE_REQUIRE_CONTIGUOUS_CHUNKS` for 2MB pages, matching Linux compound page behavior
2. **Write-combined memory** — Set `PAGE_WRITECOMBINE` on mapped regions for better GPU access patterns
3. **Prefetch thread** — Port the async prefetch worker. Replace `madvise(MADV_WILLNEED)` with `PrefetchVirtualMemory` (Windows 8+)
4. **NUMA awareness** — Use `MmAllocatePagesForMdlEx` with NUMA node preference matching the GPU's PCIe topology

**Error handling hardening:**
1. All IOCTLs must validate input buffer size before dereferencing
2. All memory allocations must handle failure gracefully
3. Driver unload must wait for all outstanding buffers to be released
4. Shim must never crash the host process -- any failure falls through to real CUDA
5. Add structured exception handling (SEH) around all hook entry points

**Documentation:**
1. README.md with full build instructions
2. ARCHITECTURE.md documenting the Windows-specific design decisions
3. INSTALL.md with step-by-step for end users
4. TROUBLESHOOTING.md for common issues

**Validation:**
- [ ] No memory leaks after 1000 alloc/free cycles (verified with Driver Verifier)
- [ ] Driver passes Driver Verifier with all checks enabled
- [ ] No BSOD under any tested input combination
- [ ] Performance within 20% of Linux GreenBoost on equivalent hardware
- [ ] Documentation is complete and accurate

---

## SUCCESS CRITERIA

- [ ] KMDF driver loads on Windows 10/11 (test-signed)
- [ ] Device `\\.\GreenBoost` is accessible from userspace
- [ ] CUDA shim successfully hooks cudaMalloc/cudaFree
- [ ] cuDeviceTotalMem reports physical VRAM + DDR4 pool
- [ ] Large CUDA allocations transparently use system RAM
- [ ] LM Studio can load models larger than physical VRAM
- [ ] Inference generates correct tokens at usable speed
- [ ] Watchdog monitors memory pressure and prevents OOM
- [ ] Clean install/uninstall via PowerShell
- [ ] All tests pass
- [ ] No BSOD, no memory leaks, graceful degradation on failure

---

## DEPENDENCIES

**Build tools:**
- Visual Studio 2022 with C++ workload
- Windows Driver Kit (WDK) 10/11
- CMake 3.20+
- Microsoft Detours library (MIT license, NuGet: `Microsoft.Detours`)

**Runtime:**
- NVIDIA GPU driver 535+ (CUDA 12+)
- Windows 10 1903+ or Windows 11
- Test signing enabled during development

**Test infrastructure:**
- Any NVIDIA GPU with 8+ GB VRAM
- 32+ GB system RAM
- LM Studio or Ollama installed

---

## REFERENCE: KEY FUNCTIONS TO PORT

These are the most important functions from the Linux source. Each needs a Windows equivalent:

### From greenboost.c (kernel):
| Linux Function | Purpose | Windows Equivalent |
|---|---|---|
| `gb_alloc_tier2()` | Allocate pinned 2MB pages | `MmAllocateContiguousMemorySpecifyCache` |
| `gb_alloc_tier3()` | Allocate swappable 4K pages | Standard paged pool + pagefile |
| `gb_release()` | Free buffer + pages | `MmFreeContiguousMemory` + cleanup |
| `gb_map_dma_buf()` | Create sg_table for DMA | Create MDL, `MmGetPhysicalAddress` |
| `gb_mmap()` | Map pages to userspace | `ZwCreateSection` + return handle |
| `greenboost_ioctl()` | IOCTL dispatcher | WDF EvtIoDeviceControl callback |
| `gb_watchdog_fn()` | Memory pressure monitor | `PsCreateSystemThread` worker |
| `gb_sysfs_*` | Stats exposure | GB_IOCTL_GET_INFO response |

### From greenboost_cuda_shim.c (userspace):
| Linux Function | Purpose | Windows Equivalent |
|---|---|---|
| `gb_shim_init()` | Constructor, resolve symbols | `DllMain(DLL_PROCESS_ATTACH)` |
| `dlsym` interception | Hook symbol resolution | Detours `DetourAttach` |
| `gb_import_as_cuda_ptr()` | DMA-BUF -> CUDA pointer | Section handle -> MapViewOfFile -> cuMemHostRegister |
| `cudaMalloc` hook | Route large allocs | Same logic, different injection |
| `ht_insert/ht_remove` | Track allocations | Direct port (platform-independent) |
| `cuDeviceTotalMem_v2` hook | Report extended VRAM | Same hook via Detours |
| `gb_open_device()` | Open /dev/greenboost | `CreateFileW(L"\\\\.\\GreenBoost")` |
| `gb_pin_user_buf()` | Pin user pages (FOLL_LONGTERM) | `MmProbeAndLockPages` on MDL from user VA |
