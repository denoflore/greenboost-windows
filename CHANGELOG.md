# Changelog

All notable changes to the GreenBoost Windows port.

## [Unreleased]

### Planned
- Phase 2: Explicit VRAM staging with MADVISE-driven promotion/eviction (if UVM heuristics prove insufficient)
- Phase 3: cuMemCreate/cuMemMap virtual memory API for sub-allocation granularity
- Driver Verifier hardening pass
- Multi-GPU device selection support

---

## 2026-03-26 (Audit)

### Fixed -- Full Codebase Audit (20 issues, all resolved)

**Critical (4):**
- **Registry path mismatch**: Shim read config from `SOFTWARE\GreenBoost` while driver and install scripts wrote to `Services\GreenBoost\Parameters`. Shim always ran on hardcoded defaults. Fixed: shim now reads Services path first, falls back to SOFTWARE. Install scripts write to both.
- **Pressure event namespace mismatch**: Shim opened `GreenBoostPressure` (session-local) instead of `Global\GreenBoostPressure` (kernel namespace). The watchdog's pressure signaling was completely disconnected from the shim. Fixed with `Global\` prefix.
- **No process crash cleanup**: Driver had no `EvtFileCleanup` callback. Crashed processes permanently leaked pinned physical memory until reboot. Fixed: added `GB_FILE_CONTEXT` per-file-object bitmap tracking buffer ownership, freed on handle close/crash.
- **Fake eviction corrupting accounting**: `GbHandleEvict` moved pool bytes from T2 to T3 counters without actually freeing RAM. Safety reserve math reported false headroom, enabling OOM. Fixed: eviction no longer manipulates pool accounting (memory stays in RAM; tier relabel is for shim bookkeeping only).

**High (4):**
- **IAT hooking fallback only hooked `cudaMalloc`**: Without Detours, `cudaFree`, `cuMemAlloc_v2`, `cuMemFree_v2`, memory info spoofing were all unhooked. Every allocation leaked. Fixed: expanded IAT hooks to cover 7 critical functions across both cudart and nvcuda DLLs.
- **`GbHandleReset` was a no-op**: Logged "RESET requested" and returned success without freeing anything. Fixed: walks `BufTable` and frees all active buffers.
- **`cudaMallocManaged` (runtime API) not intercepted**: Function pointer was resolved but never passed to `DetourAttach`. PyTorch managed memory path bypassed GreenBoost. Fixed: added `hooked_cudaMallocManaged` implementation + Detour attach/detach.
- **Build pipeline broken**: `build.ps1` had driver build disabled and 117 lines of commented-out signing/catalog code. `install.ps1` expected artifacts that `build.ps1` never collected. Fixed: cleaned dead code, fixed step numbering, added INF collection to outputs.

**Medium (5):**
- **SDDL too permissive**: Device was accessible to Everyone (WD). Changed to Interactive Users (IU).
- **VRAM spoofing underflow**: `cudaMemGetInfo` and `cuMemGetInfo` could wrap to ~2^64 if configured VRAM < actual. Added underflow guards.
- **INF `$KMDFVERSION$` placeholder**: Replaced with concrete `1.33` (WDK 10.0.22621+).
- **`DeviceHandle` initialized to 0** (valid handle on Windows): Added explicit `INVALID_HANDLE_VALUE` assignment at top of `gb_shim_init`.
- **Hash table tombstone accumulation**: Added `ht_reclaim_tombstones()` with 25% threshold trigger, full probe chain rebuild under exclusive lock.

**Low (7):**
- Removed 18 committed Linux build artifacts from repo root (`.o`, `.ko`, `.so`, `.cmd` files).
- Removed accidental pip install log files (`=2.6.0`, `=2.7.4.post1`).
- Translated `sign.ps1` from Chinese to English.
- Fixed `build.ps1` ASCII art (was gibberish, now says GreenBoost).
- Removed Ollama references from `diagnose.ps1`.
- Wired `test_uvm.c` into CMake with conditional `find_package(CUDAToolkit)`.
- Fixed `build.ps1` step numbering ([1/5]...[6/6] became [1/4]...[4/4]).

### Added
- **UVM allocation path** (PR #8, PR #6): The shim now allocates via `cuMemAllocManaged` with `cuMemPrefetchAsync` to GPU as the primary path. NVIDIA's UVM driver handles transparent page migration between VRAM and RAM. Weights that fit in physical VRAM are accessed at full HBM bandwidth instead of being PCIe-bound. Fallback to driver+cuMemHostRegister when UVM is unavailable.
- Deferred UVM capability probe: runs on first intercepted `cudaMalloc`, not at DLL load (where no CUDA context exists).
- `gb_alloc_uvm()`: UVM allocation with inline GPU prefetch.
- `is_managed`-aware free path in `gb_free_buffer()` and cleanup loop.
- Extended prefetch worker: CUDA device prefetch for UVM allocations, Win32 `PrefetchVirtualMemory` for driver-path allocations.
- `test_uvm.c`: 6-test validation suite for UVM allocation path.
- `pfn_cuMemPrefetchAsync`, `pfn_cuCtxGetDevice` typedefs and symbol resolution.
- `GpuDevice`, `UvmAllocatedBytes` tracking in shim config.
- Repo header image.

### Fixed
- **Build fixes for Win11 + VS2022 + WDK** (PR #4, contributed by @sjmind2):
  - Registry reading: `WdfRegistryOpenKey` replaced with `ZwOpenKey`/`ZwQueryValueKey` for early driver init reliability.
  - `MmAvailablePages` replaced with `ZwQuerySystemInformation(SystemPerformanceInformation)` -- the exported kernel variable is not available in all WDK versions.
  - `ntddk.h` replaced with `ntifs.h` (superset header, resolves missing type definitions).
  - `ExFreeIoMdl` replaced with `IoFreeMdl` (correct public WDK API).
  - Pressure event: dropped `OBJ_KERNEL_HANDLE` flag (event must be visible to user-mode shim), added `OBJ_OPENIF` for driver reload resilience.
  - Test 7: `OpenEventW` now uses `Global\\` prefix to reach `\BaseNamedObjects\` from user mode.
  - Shim CMakeLists: proper vcpkg-compatible `find_path`/`find_library` for Detours with separate release/debug paths.
  - `CUDAAPI __stdcall` define for Windows CUDA driver API calling convention.
  - INF: parameterized KMDF version (`$KMDFVERSION$`), minimum OS version targeting.
  - Build automation: `build.ps1`, `sign.ps1`, `config.ps1`, `list-device.ps1` scripts.
  - Expanded `BUILDING.md` with detailed VS2022 + WDK + vcpkg instructions.

### Changed
- All four allocation hooks (`hooked_cudaMalloc`, `hooked_cudaMallocAsync`, `hooked_cuMemAlloc_v2`, `hooked_cuMemAllocAsync`) now follow UVM-first, driver-fallback pattern.
- Prefetch worker extended with `is_managed` and `to_device` fields for routing between CUDA and Win32 prefetch paths.
- `enqueue_prefetch` signature extended to 4 arguments (ptr, size, is_managed, to_device).

---

## 2026-03-16

### Added
- Complete Windows port: KMDF driver, CUDA shim DLL, build system, tests, documentation (~4,500 lines across 17 files).
- Three-tier memory hierarchy: T1 (GPU VRAM), T2 (DDR4 pinned pages), T3 (NVMe/pagefile overflow).
- Detours-based CUDA hook injection with IAT patching fallback.
- VRAM spoofing for `cudaMemGetInfo`, `cuMemGetInfo`, `cuDeviceTotalMem`, and NVML.
- Hash table with tombstone deletion (fixes upstream Linux bug with zeroed slots breaking probe chains).
- `GB_IOCTL_FREE` for explicit buffer lifecycle management (no Windows equivalent of Linux `close(fd)` DMA-BUF release).
- Memory pressure monitoring via named kernel event (`\BaseNamedObjects\GreenBoostPressure`).
- LRU eviction with MADVISE hints (HOT/COLD/FREEZE).
- `PIN_USER_PTR` IOCTL for pinning existing user-space buffers.
- Async prefetch worker thread.
- PowerShell installer and diagnostics scripts.

### Fixed
- Critical memory sharing bug: replaced `ZwCreateSection` (creates anonymous pagefile-backed memory) with `MmMapLockedPagesSpecifyCache(UserMode)` (maps actual pinned physical pages into calling process).

