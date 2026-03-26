# Changelog

All notable changes to the GreenBoost Windows port.

## [Unreleased]

### Planned
- Phase 2: Explicit VRAM staging with MADVISE-driven promotion/eviction (if UVM heuristics prove insufficient)
- Phase 3: cuMemCreate/cuMemMap virtual memory API for sub-allocation granularity
- Driver Verifier hardening pass
- Multi-GPU device selection support

---

## 2026-03-26

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
