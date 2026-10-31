# Building the portable Windows package

Use Windows x64, Visual Studio C++ Build Tools and CMake 3.20 or newer. The CUDA SDK and WDK are not required for the launcher, shim or driver-API smoke test.

Run the build script from any working directory. Paths are resolved relative to the script, and existing build files are retained unless you explicitly pass `-Clean`.

## Microsoft Detours source build

This is the release build route. In an **x64 Native Tools Command Prompt**, clone and build Detours 4.0.1:

```bat
git clone --depth 1 --branch v4.0.1 https://github.com/microsoft/Detours.git C:\src\Detours
set DETOURS_TARGET_PROCESSOR=X64
cd /d C:\src\Detours\src
nmake
```

Then, from PowerShell in the GreenBoost repository:

```powershell
.\windows-port\build.ps1 -DetoursRoot C:\src\Detours -StaticRuntime -Package
```

Detours' source Makefile uses the static MSVC runtime. `-StaticRuntime` matches it. The release ZIP therefore needs no separate VC++ runtime for GreenBoost itself. CUDA applications retain their own dependencies.

Outputs go to `windows-port/tools/outputs`:

- `greenboost.exe`
- `greenboost_cuda.dll`
- `test_cuda.exe`
- `test_runtime.exe`, `test_lookup.exe` and `test_compatibility.ps1`
- Usage, validation and licence files
- `GreenBoost-Windows-2.4.0-beta.2-x64.zip` when `-Package` is selected

Only the explicit portable files enter the ZIP. No driver, installer, signing certificate or local configuration is packaged.

## Existing vcpkg installation

If Detours is already installed as `detours:x64-windows`:

```powershell
.\windows-port\build.ps1 -DetoursRoot C:\vcpkg\installed\x64-windows
```

Alternatively set `VCPKG_ROOT`. This dynamic-runtime build uses the corresponding Microsoft VC++ x64 runtime. Do not combine a dynamic-runtime Detours library with `-StaticRuntime`.

## Options

`-Config Release|Debug|RelWithDebInfo` chooses a configuration. Release is used for distribution. `-Generator` overrides CMake's Visual Studio selection, for example `-Generator "Visual Studio 17 2022"`. Normally let CMake discover the installed version.

`-Clean` removes only the port's verified build directory. It is not needed for ordinary incremental builds. Missing Detours fails configuration explicitly.

For direct CMake use:

```powershell
cmake -S windows-port -B windows-port/build -A x64 -DCMAKE_PREFIX_PATH=C:/src/Detours -DGB_STATIC_RUNTIME=ON
cmake --build windows-port/build --config Release
```

## Validation

The build includes `test_cuda`, `test_runtime`, `test_lookup`, `test_child`, `test_capacity` and `test_free_error`. CUDA tests require a real NVIDIA GPU and driver. See [VALIDATION.md](VALIDATION.md) for the tested cases and bounds, and [COMPATIBILITY.md](COMPATIBILITY.md) for repeatable runtime checks. The optional historical `test_uvm` target is available when CMake finds the CUDA SDK.

GitHub's Windows build job checks compilation, packaging, help and the launcher's missing-application error. It does not claim GPU validation on a hosted runner.

## Legacy kernel driver

`GB_BUILD_DRIVER=ON` deliberately stops with a clear message. The old CMake driver target is not a complete WDK driver package. The legacy KMDF source and installation scripts are retained for separate development. This release does not provide or qualify a driver, catalogue or signing procedure, and the portable path does not need them.
