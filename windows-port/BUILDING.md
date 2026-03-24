# Building GreenBoost for Windows

This guide covers the complete build process for GreenBoost Windows port, including all dependencies, driver build using Visual Studio 2022 KMDF template, and output management.

## Table of Contents

- [Prerequisites](#prerequisites)
- [Dependency Installation](#dependency-installation)
  - [Visual Studio 2022](#visual-studio-2022)
  - [Windows Driver Kit (WDK)](#windows-driver-kit-wdk)
  - [CMake](#cmake)
  - [vcpkg and Detours](#vcpkg-and-detours)
- [Building the CUDA Shim DLL](#building-the-cuda-shim-dll)
- [Building the KMDF Driver](#building-the-kmdf-driver)
  - [Creating the Driver Project](#creating-the-driver-project)
  - [Adding Source Files](#adding-source-files)
  - [Building the Driver](#building-the-driver)
- [Merging Outputs](#merging-outputs)
- [Driver Signing](#driver-signing)
- [Installation](#installation)
- [Scripts Reference](#scripts-reference)
- [Troubleshooting](#troubleshooting)

---

## Prerequisites

| Tool | Version | Purpose | Download |
|------|---------|---------|----------|
| Visual Studio 2022 | 17.8+ with C++ workload | Compiler & IDE | [Download](https://visualstudio.microsoft.com/downloads/) |
| Windows Driver Kit (WDK) | 10.0.26100.0+ | KMDF driver build | [Download](https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk) |
| CMake | 3.20+ | Build system for shim | [Download](https://cmake.org/download/) |
| Git | Latest | Clone repositories | [Download](https://git-scm.com/downloads) |
| NVIDIA GPU Driver | 535+ (CUDA 12+) | Runtime | [Download](https://www.nvidia.com/Download/index.aspx) |

---

## Dependency Installation

### Visual Studio 2022

1. Download from: https://visualstudio.microsoft.com/downloads/
   - Select **Visual Studio Community 2022** (free) or Professional/Enterprise

2. Run the installer and select these workloads:
   - **Desktop development with C++** (required)
   - **Windows driver development** (optional, if installing WDK separately)

3. In the **Individual components** tab, ensure these are selected:
   - MSVC v143 - VS 2022 C++ x64/x86 build tools
   - Windows 10/11 SDK (latest available, e.g., 10.0.26100.0)
   - C++ CMake tools for Windows

4. Complete the installation and restart if prompted.

### Windows Driver Kit (WDK)

1. Download from: https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk
   - Choose the WDK version that matches your Windows SDK version
   - For Windows 11 24H2: WDK 10.0.26100.0

2. Run the WDK installer - it will detect your Visual Studio installation

3. After installation, verify:
   ```
   dir "C:\Program Files (x86)\Windows Kits\10\Include\wdf\kmdf"
   ```
   Should show KMDF version folders (e.g., `1.33`)

### CMake

1. Download from: https://cmake.org/download/
   - Choose `cmake-3.x.x-windows-x86_64.msi`

2. During installation, select **Add CMake to the system PATH for all users**

3. Verify installation:
   ```powershell
   cmake --version
   ```

### vcpkg and Detours

The CUDA shim uses Microsoft Detours for API hooking. Install via vcpkg:

#### Step 1: Clone vcpkg

```powershell
# Choose a directory for vcpkg (e.g., C:\dev\vcpkg)
cd C:\dev
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
```

#### Step 2: Bootstrap vcpkg

```powershell
.\bootstrap-vcpkg.bat
```

#### Step 3: Integrate with Visual Studio

```powershell
.\vcpkg integrate install
```

This outputs a toolchain path like:
```
-DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake
```

#### Step 4: Install Detours

```powershell
.\vcpkg install detours:x64-windows
```

#### Step 5: Set Environment Variable

Set the `VCPKG_ROOT` environment variable permanently:

```powershell
# For current session
$env:VCPKG_ROOT = "C:\dev\vcpkg"

# Permanently (system-wide, requires admin)
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "C:\dev\vcpkg", "Machine")

# Permanently (user-only)
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "C:\dev\vcpkg", "User")
```

After setting, restart your terminal/PowerShell to apply.

---

## Building the CUDA Shim DLL

The shim DLL intercepts CUDA memory allocation calls and redirects large allocations to system RAM.

### Quick Build (build.ps1)

```powershell
cd windows-port

# Build shim and test tool (requires VCPKG_ROOT to be set)
.\build.ps1

# Debug build
.\build.ps1 -Config Debug

# Skip signing step
.\build.ps1 -NoSign
```

### Manual Build (CMake)

```powershell
cd windows-port

# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

# Build
cmake --build build --config Release
```

### Build Outputs

After building, outputs are in `windows-port\build\`:

| File | Location |
|------|----------|
| `greenboost_cuda.dll` | `build\shim\Release\` |
| `test_ioctl.exe` | `build\tests\Release\` |

---

## Building the KMDF Driver

> **Important**: CMake cannot build the driver correctly (may cause BSOD). Use Visual Studio 2022 with the KMDF Driver template for proper driver builds.

### Creating the Driver Project

#### Step 1: Open Visual Studio 2022

1. Launch Visual Studio 2022
2. Select **Create a new project**

#### Step 2: Select KMDF Driver Template

1. Search for **"Kernel Mode Driver, KMDF"** in the project templates
2. Select **Kernel Mode Driver, KMDF (KMDF)** template
3. Click **Next**

   > If you don't see this template, ensure WDK is installed correctly.

#### Step 3: Configure Project

- **Project name**: `GreenBoostDriver` (or any name you prefer)
- **Location**: Choose a directory (can be inside `windows-port\driver\vs-project\`)
- Click **Create**

#### Step 4: Project Settings

In the **Windows Driver Project** wizard:
- **Driver Type**: Kernel Mode Driver (KMDF)
- **KMDF Version**: Use the latest available (e.g., 1.33)
- **Target OS Version**: Windows 10 or later

Click **OK** to create the project.

### Adding Source Files

#### Step 1: Remove Template Files

Delete the default `Driver.c` or similar template files from the project.

#### Step 2: Add GreenBoost Source Files

Copy these files from `windows-port\driver\` to your VS project folder:

| Source File | Description |
|-------------|-------------|
| `greenboost_win.c` | Main driver implementation |
| `greenboost_win.h` | Driver internal header |
| `greenboost_ioctl_win.h` | IOCTL definitions (shared with shim) |
| `greenboost_win.inf` | Driver installation file |

In Visual Studio:
1. Right-click the project in Solution Explorer
2. Select **Add** > **Existing Item...**
3. Navigate to `windows-port\driver\` and select the `.c` files
4. For header files, you can add them to **Header Files** filter or just include the directory

#### Step 3: Configure Include Paths

1. Right-click project > **Properties**
2. Go to **C/C++** > **General** > **Additional Include Directories**
3. Add the path to `windows-port\driver\` directory
4. Apply changes

#### Step 4: Configure Preprocessor Definitions

In **Project Properties** > **C/C++** > **Preprocessor** > **Preprocessor Definitions**, ensure:
```
_WIN64
_AMD64_
NTDDI_VERSION=0x0A000006
_WIN32_WINNT=0x0A00
```

### Building the Driver

#### Step 1: Select Configuration

1. In the toolbar, select **Release** and **x64**
2. Or use **Debug** for development/debugging

#### Step 2: Build

- Press **Ctrl+Shift+B** or
- Select **Build** > **Build Solution**

#### Step 3: Locate Output

The built driver files are in your project's output directory:

```
<YourProjectFolder>\x64\Release\
├── GreenBoostDriver.sys    # The kernel driver
├── GreenBoostDriver.pdb    # Debug symbols
└── GreenBoostDriver.inf    # Installation file (if configured)
```

### Driver Project Structure Summary

```
windows-port/driver/
├── greenboost_win.c           # Main driver source
├── greenboost_win.h           # Internal header
├── greenboost_ioctl_win.h     # IOCTL definitions
├── greenboost_win.inf         # Driver INF file
├── CMakeLists.txt             # CMake config (for reference only)
└── vs-project/                # Your VS2022 project (create this)
    ├── GreenBoostDriver.vcxproj
    ├── GreenBoostDriver.sys   # Built output
    └── ...
```

---

## Merging Outputs

After building both the shim and driver, collect all outputs to `tools\outputs\` for installation.

### Automatic Collection (build.ps1)

The `build.ps1` script automatically collects shim outputs. For the driver, you need to copy manually:

```powershell
# Create outputs directory if not exists
New-Item -ItemType Directory -Force -Path "windows-port\tools\outputs"

# Copy driver files from VS project output
Copy-Item "path\to\your\GreenBoostDriver\x64\Release\GreenBoostDriver.sys" `
          "windows-port\tools\outputs\greenboost_win.sys"

Copy-Item "windows-port\driver\greenboost_win.inf" `
          "windows-port\tools\outputs\greenboost_win.inf"
```

### Expected outputs Directory Structure

```
windows-port/tools/outputs/
├── greenboost_cuda.dll    # CUDA shim DLL (from CMake build)
├── greenboost_win.sys     # KMDF driver (from VS build)
├── greenboost_win.inf     # Driver installation file
└── test_ioctl.exe         # IOCTL test tool (from CMake build)
```

### Manual Collection Script

Create `windows-port\collect-outputs.ps1`:

```powershell
param(
    [string]$DriverPath = ".\driver\vs-project\x64\Release\GreenBoostDriver.sys"
)

$outputsDir = ".\tools\outputs"
New-Item -ItemType Directory -Force -Path $outputsDir | Out-Null

# Copy shim (from CMake build)
$shimPath = ".\build\shim\Release\greenboost_cuda.dll"
if (Test-Path $shimPath) {
    Copy-Item $shimPath $outputsDir -Force
    Write-Host "Copied: greenboost_cuda.dll" -ForegroundColor Green
}

# Copy test tool
$testPath = ".\build\tests\Release\test_ioctl.exe"
if (Test-Path $testPath) {
    Copy-Item $testPath $outputsDir -Force
    Write-Host "Copied: test_ioctl.exe" -ForegroundColor Green
}

# Copy driver (from VS build)
if (Test-Path $DriverPath) {
    Copy-Item $DriverPath "$outputsDir\greenboost_win.sys" -Force
    Write-Host "Copied: greenboost_win.sys" -ForegroundColor Green
} else {
    Write-Host "Driver not found at: $DriverPath" -ForegroundColor Yellow
    Write-Host "Build the driver in Visual Studio first, then specify -DriverPath" -ForegroundColor Yellow
}

# Copy INF
Copy-Item ".\driver\greenboost_win.inf" $outputsDir -Force
Write-Host "Copied: greenboost_win.inf" -ForegroundColor Green

Write-Host "`nOutputs collected to: $outputsDir" -ForegroundColor Cyan
```

---

## Driver Signing

Test-signed drivers require enabling test signing mode in Windows.

### Enable Test Signing

> **Warning**: This requires disabling Secure Boot in BIOS/UEFI first.

```powershell
# Check current status
bcdedit | findstr testsigning

# Enable test signing (requires admin)
bcdedit /set testsigning on

# Reboot required
```

### Sign the Driver

Use the provided signing script:

```powershell
cd windows-port
.\sign.ps1
```

Or manually with signtool:

```powershell
# Find signtool
$signtool = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe" |
            Sort-Object { $_.Directory.Parent.Name } -Descending |
            Select-Object -First 1

# Create test certificate (first time only)
$cert = New-SelfSignedCertificate -Type Custom -Subject "CN=GreenBoostTestCert" `
        -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 2048 `
        -CertStoreLocation "Cert:\LocalMachine\My"

# Export and trust
Export-Certificate -Cert $cert -FilePath "GreenBoostTest.cer"
Import-Certificate -FilePath "GreenBoostTest.cer" -CertStoreLocation "Cert:\LocalMachine\Root"
Import-Certificate -FilePath "GreenBoostTest.cer" -CertStoreLocation "Cert:\LocalMachine\TrustedPublisher"

# Sign driver
& $signtool.FullName sign /s My /n GreenBoostTestCert /fd sha256 ".\tools\outputs\greenboost_win.sys"
```

---

## Installation

### Using install.ps1

```powershell
# As Administrator
cd windows-port\tools
.\install.ps1

# Options
.\install.ps1 -SkipDriver     # Install shim only
.\install.ps1 -SkipShim       # Install driver only
.\install.ps1 -Force          # Force install without checks
.\install.ps1 -Uninstall      # Remove GreenBoost
```

### Manual Installation

#### Driver Installation

```powershell
# Using pnputil (recommended)
pnputil /add-driver ".\tools\outputs\greenboost_win.inf" /install

# Or using devcon (from WDK)
devcon install ".\tools\outputs\greenboost_win.inf" Root\GreenBoost
```

#### Shim Installation

Copy `greenboost_cuda.dll` to the target application directory:

```powershell
# For LM Studio
Copy-Item ".\tools\outputs\greenboost_cuda.dll" "C:\Users\<user>\.lm-studio\"

# For Ollama
Copy-Item ".\tools\outputs\greenboost_cuda.dll" "C:\Program Files\Ollama\lib\ollama\"
```

---

## Scripts Reference

### build.ps1

Main build script for the shim DLL and test tool.

```powershell
.\build.ps1 [-Config <Release|Debug>] [-Arch <x64>] [-NoSign] [-Clean]
```

| Parameter | Default | Description |
|-----------|---------|-------------|
| `-Config` | Release | Build configuration |
| `-Arch` | x64 | Target architecture |
| `-NoSign` | false | Skip driver signing |
| `-Clean` | false | Clean build directory first |

### sign.ps1

Driver signing utility.

```powershell
.\sign.ps1 [-CertName <name>] [-DriverFile <path>] [-Password <pass>]
```

| Parameter | Default | Description |
|-----------|---------|-------------|
| `-CertName` | GreenBoostTestCert | Certificate subject name |
| `-DriverFile` | greenboost_win.sys | Driver file to sign |
| `-Password` | GreenBoost123 | PFX password |

### tools/install.ps1

Full installation script with hardware detection.

```powershell
.\install.ps1 [-Uninstall] [-SkipDriver] [-SkipShim] [-Force]
```

Features:
- Auto-detects NVIDIA GPU and VRAM
- Auto-detects system RAM and CPU topology
- Configures registry settings
- Installs driver and shim

### tools/diagnose.ps1

Diagnostic utility to check system status.

```powershell
.\diagnose.ps1
```

Checks:
- Windows version compatibility
- Test signing status
- NVIDIA GPU and driver
- System memory
- Driver service status
- Device accessibility

### tools/config.ps1

Configuration management tool.

```powershell
.\config.ps1                    # Auto-configure
.\config.ps1 -Show              # Show current config
.\config.ps1 -PhysicalVramGb 32 # Manual override
```

### tools/list-device.ps1

List GreenBoost device entries in Device Manager.

```powershell
.\list-device.ps1
```

---

## Troubleshooting

### Build Errors

| Error | Solution |
|-------|----------|
| `DETOURS_LIBRARY_RELEASE-NOTFOUND` | Install Detours via vcpkg, ensure `VCPKG_ROOT` is set |
| `CMAKE_TOOLCHAIN_FILE not found` | Set `VCPKG_ROOT` environment variable correctly |
| `wdm.lib not found` | Install WDK from Visual Studio Installer |
| `BufferOverflowK.lib not found` | WDK version mismatch, reinstall WDK matching SDK |
| `KMDF template not found` | Install WDK, restart Visual Studio |

### Driver Build Errors

| Error | Solution |
|-------|----------|
| `MSB8040: Spectre-mitigated libraries` | Disable Spectre mitigation in project properties: C/C++ > Spectre Mitigation = Disabled |
| `WDF01000.sys not found` | Ensure KMDF redistributable is installed with WDK |
| BSOD on driver load | Ensure driver is built with VS2022 KMDF template, not CMake |

### Installation Errors

| Error | Solution |
|-------|----------|
| `Test signing is NOT enabled` | Disable Secure Boot in BIOS, then `bcdedit /set testsigning on` |
| `The value is protected by Secure Boot` | Disable Secure Boot in BIOS/UEFI settings |
| `Driver installation failed` | Run `signtool verify /pa driver.sys` to check signature |
| `Access denied` | Run PowerShell as Administrator |

### Runtime Issues

| Issue | Solution |
|-------|----------|
| Shim not loading | Ensure DLL is in the same directory as the target executable |
| `cudaMalloc` not intercepted | Check if Detours is properly linked; try rebuild |
| Driver not starting | Check Event Viewer > Windows Logs > System for errors |
| Memory not allocated | Run `diagnose.ps1` to verify driver is loaded correctly |

---

## Quick Reference Commands

```powershell
# Full build and install workflow

# 1. Set environment
$env:VCPKG_ROOT = "C:\dev\vcpkg"

# 2. Build shim
cd windows-port
.\build.ps1

# 3. Build driver in Visual Studio 2022 (manual step)
#    - Open VS2022
#    - Create KMDF Driver project
#    - Add driver source files
#    - Build in Release x64

# 4. Collect outputs
Copy-Item ".\driver\vs-project\x64\Release\GreenBoostDriver.sys" ".\tools\outputs\greenboost_win.sys"

# 5. Enable test signing
bcdedit /set testsigning on
# Reboot

# 6. Install
cd tools
.\install.ps1

# 7. Verify
.\diagnose.ps1
```

---

## Additional Resources

- [Windows Driver Kit Documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/)
- [KMDF Driver Development Guide](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/)
- [Microsoft Detours Library](https://github.com/microsoft/Detours)
- [vcpkg Documentation](https://vcpkg.io/en/getting-started.html)
