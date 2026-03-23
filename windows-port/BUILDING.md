# Building GreenBoost for Windows

## Prerequisites

| Tool | Version | Purpose |
|------|---------|---------|
| Visual Studio 2022 | With C++ desktop workload | Compiler |
| Windows Driver Kit (WDK) | 10/11 | KMDF driver build |
| CMake | 3.20+ | Build system |
| Microsoft Detours | NuGet: `Microsoft.Detours` | CUDA hook injection |
| NVIDIA GPU Driver | 535+ (CUDA 12+) | Runtime |

## Build Steps

### 1. Shim DLL + Test Tool (no WDK required)

```powershell
cd windows-port

# Configure (Release, x64)
cmake -B build -G "Visual Studio 17 2022" -A x64

# Build
cmake --build build --config Release
```

Output:
- `build/shim/Release/greenboost_cuda.dll` — CUDA shim DLL
- `build/tests/Release/test_ioctl.exe` — IOCTL test tool

### 2. KMDF Driver (requires WDK)

```powershell
# Option A: CMake with WDK
cmake -B build -G "Visual Studio 17 2022" -A x64 -DGB_BUILD_DRIVER=ON -DWDK_ROOT="C:\Program Files (x86)\Windows Kits\10"
cmake --build build --config Release

# Option B: Use the WDK project template in Visual Studio
# 1. Create new KMDF project
# 2. Add driver/*.c and driver/*.h to project
# 3. Build as x64 Release
```

Output: `greenboost_win.sys` + `greenboost_win.cat`

### 3. Test-sign the Driver

```powershell
# Enable test signing (reboot required)
bcdedit /set testsigning on

# Create test certificate
makecert -r -pe -ss PrivateCertStore -n "CN=GreenBoostTestCert" GreenBoostTest.cer
certmgr /add GreenBoostTest.cer /s /r localMachine root
certmgr /add GreenBoostTest.cer /s /r localMachine trustedpublisher

# Sign the driver
signtool sign /s PrivateCertStore /n "GreenBoostTestCert" /t http://timestamp.digicert.com /fd sha256 greenboost_win.sys
```

### 4. Install

```powershell
# As Administrator
cd tools
.\install.ps1
```

## Detours Setup

If Detours is not in your system path:

```powershell
# install vcpkg
# git clone https://github.com/Microsoft/vcpkg.git
# cd vcpkg
# bootstrap-vcpkg.bat
# .\vcpkg integrate install
# Install via vcpkg
.\vcpkg install detours:x64-windows

# Point CMake to Detours
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="path/to/detours"
```

If Detours is not available, the shim falls back to IAT patching (limited hook coverage).

## Verification

```powershell
# Run IOCTL tests (driver must be loaded)
build\tests\Release\test_ioctl.exe

# Run diagnostics
tools\diagnose.ps1

# Check driver is loaded
sc query GreenBoost
```
