# ============================================================
# GreenBoost Windows Port - Build Script
# Functions: Clean, Configure, Build, Collect outputs
# Requires: Visual Studio 2022, CMake 3.20+
# Note: Driver .sys must be built separately via VS KMDF project
#       (CMake cannot produce a correct KMDF driver binary)
# ============================================================

param(
    [string]$Config = "Release",
    [string]$Arch = "x64",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $scriptDir "build"
$outputsDir = Join-Path $scriptDir "tools\outputs"

function Write-Step {
    param([string]$Message)
    Write-Host ""
    Write-Host "============================================" -ForegroundColor Cyan
    Write-Host " $Message" -ForegroundColor Cyan
    Write-Host "============================================" -ForegroundColor Cyan
}

function Test-Administrator {
    $currentUser = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($currentUser)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

try {
    Write-Host ""
    Write-Host "   ____                     ____                  _   " -ForegroundColor Green
    Write-Host "  / ___|_ __ ___  ___ _ __ | __ )  ___   ___  __| |_ " -ForegroundColor Green
    Write-Host " | |  _| '__/ _ \/ _ \ '_ \|  _ \ / _ \ / _ \/ __| __|" -ForegroundColor Green
    Write-Host " | |_| | | |  __/  __/ | | | |_) | (_) | (_) \__ \ |_ " -ForegroundColor Green
    Write-Host "  \____|_|  \___|\___|_| |_|____/ \___/ \___/|___/\__|" -ForegroundColor Green
    Write-Host "  v2.3 Windows Build                                  " -ForegroundColor Green
    Write-Host ""

    if (-not (Test-Administrator)) {
        Write-Host "[WARNING] Not running as Administrator." -ForegroundColor Yellow
    }

    Write-Step "[1/4] Clean Build Directory"
    if (Test-Path $buildDir) {
        Write-Host "Removing existing build directory..." -ForegroundColor Yellow
        Remove-Item -Path $buildDir -Recurse -Force
        Write-Host "Build directory cleaned." -ForegroundColor Green
    } else {
        Write-Host "No existing build directory found." -ForegroundColor Gray
    }

    Write-Step "[2/4] Configure CMake"
    $configureArgs = @(
        "-B", "build",
        "-G", "Visual Studio 17 2022",
        "-A", $Arch,
        "-DCMAKE_BUILD_TYPE=$Config"
    )
    # Add vcpkg toolchain if available (for Detours)
    if ($env:VCPKG_ROOT -and (Test-Path "$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake")) {
        $configureArgs += "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
    }
    # Note: GB_BUILD_DRIVER is OFF by default. The KMDF driver must be
    # built via a Visual Studio KMDF project to avoid BSOD-inducing
    # binaries. CMake is used for the shim DLL and test tools only.
    Write-Host "Running: cmake $configureArgs" -ForegroundColor Gray
    & cmake $configureArgs
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configuration failed with exit code: $LASTEXITCODE"
    }
    Write-Host "CMake configuration completed." -ForegroundColor Green

    Write-Step "[3/4] Build Project"
    $buildArgs = @(
        "--build", "build",
        "--config", $Config
    )
    Write-Host "Running: cmake $buildArgs" -ForegroundColor Gray
    & cmake $buildArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code: $LASTEXITCODE"
    }
    Write-Host "Build completed." -ForegroundColor Green

    $shimPath = Join-Path $buildDir "shim\$Config\greenboost_cuda.dll"
    $testPath = Join-Path $buildDir "tests\$Config\test_ioctl.exe"

    Write-Host ""
    Write-Host "Build Artifacts:" -ForegroundColor White
    if (Test-Path $shimPath) {
        Write-Host "  [DLL]  $shimPath" -ForegroundColor Green
    }
    if (Test-Path $testPath) {
        Write-Host "  [EXE]  $testPath" -ForegroundColor Green
    }

    Write-Step "[4/4] Collect Outputs"
    Write-Host "Collecting build artifacts to tools\outputs..." -ForegroundColor Gray

    if (-not (Test-Path $outputsDir)) {
        New-Item -Path $outputsDir -ItemType Directory -Force | Out-Null
    }

    $collected = @()

    if (Test-Path $shimPath) {
        Copy-Item -Path $shimPath -Destination $outputsDir -Force
        $collected += "greenboost_cuda.dll"
        Write-Host "  Copied: greenboost_cuda.dll" -ForegroundColor Green
    }

    if (Test-Path $testPath) {
        Copy-Item -Path $testPath -Destination $outputsDir -Force
        $collected += "test_ioctl.exe"
        Write-Host "  Copied: test_ioctl.exe" -ForegroundColor Green
    }

    # Copy INF to outputs (for install.ps1)
    $infSource = Join-Path $scriptDir "driver\greenboost_win.inf"
    if (Test-Path $infSource) {
        Copy-Item -Path $infSource -Destination $outputsDir -Force
        $collected += "greenboost_win.inf"
        Write-Host "  Copied: greenboost_win.inf" -ForegroundColor Green
    }

    Write-Host ""
    Write-Host "============================================" -ForegroundColor Green
    Write-Host " BUILD COMPLETED SUCCESSFULLY!" -ForegroundColor Green
    Write-Host "============================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "Outputs collected to: $outputsDir" -ForegroundColor White
    foreach ($item in $collected) {
        Write-Host "  - $item" -ForegroundColor Gray
    }
    Write-Host ""
    Write-Host "NOTE: The KMDF driver (greenboost_win.sys) must be built" -ForegroundColor Yellow
    Write-Host "      separately via Visual Studio KMDF project and signed" -ForegroundColor Yellow
    Write-Host "      before install.ps1 can deploy it." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "To install, run: .\tools\install.ps1" -ForegroundColor Cyan
    Write-Host ""

} catch {
    Write-Host ""
    Write-Host "============================================" -ForegroundColor Red
    Write-Host " BUILD FAILED: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "============================================" -ForegroundColor Red
    Write-Host ""
    exit 1
}
