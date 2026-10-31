# Windows portable troubleshooting

**The application still reports physical VRAM.** Start a fresh process through `greenboost.exe`, with `GREENBOOST_DEBUG=1`. An existing background server is unaffected. Check for `UVM alloc` messages and run the supplied GPU test. A memory display alone does not prove interception. NVML and nvidia-smi intentionally retain physical totals.

**No large allocations are intercepted.** The default threshold is 256 MiB per allocation. Memory pools, CUDA virtual-memory APIs and explicit managed allocations are outside the extension. Setting `GREENBOOST_EXTRA_MB=0`, or having no RAM left after the reserve, disables extension.

**An allocation fails despite a larger displayed total.** The allowance is a planning limit, not reserved memory. Reduce the model/context size or allowance, close competing workloads and check available RAM. Larger Windows managed allocations depend on the NVIDIA driver and GPU. NVMe/pagefile capacity is not added to the allowance.

**The launcher reports a Windows error.** Keep its DLL beside the EXE, check the application path and use an x64 application with x64 children. Protected processes and mixed-architecture child trees are not qualified. The portable package does not require elevation.

**A CUDA library is missing.** Install/extract the dependencies provided by your CUDA application. GreenBoost does not bundle NVIDIA runtime libraries. The driver-level smoke test needs only the installed NVIDIA driver.

**A newer CUDA application fails, or an older one stopped working.** Run `test_compatibility.ps1` against that application's runtime DLL and your intended CUDA device. Compare the native and injected cases. A native failure also needs attention in the application's driver/runtime setup. Use [COMPATIBILITY.md](COMPATIBILITY.md) to distinguish the tested runtime combinations from newer driver requirements and older GPU architecture limits. Keep NVIDIA libraries with their matching application build.

**The build cannot find Detours or Visual Studio.** Follow [BUILDING.md](BUILDING.md), pass the Detours prefix, and use a CMake version supporting your installed Visual Studio. The build command works from the repository root or another folder.

**Old install/diagnose scripts complain about greenboost.sys.** Those scripts target the legacy kernel-driver setup. They are not the installation or acceptance test for this portable beta.

When reporting a failure, include Windows/NVIDIA driver versions, GPU and RAM, the application/build, exact launch command without secrets, the smoke-test result, and relevant `GREENBOOST_DEBUG=1` output.
