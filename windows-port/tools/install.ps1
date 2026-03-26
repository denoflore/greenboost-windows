# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2024-2026 Ferran Duarri
# GreenBoost v2.3 — Windows Installer Script
#
# Usage: Run as Administrator
#   .\install.ps1 [-Uninstall] [-SkipDriver] [-SkipShim] [-Force]
#
# Prerequisites:
#   - Windows 10/11 x64
#   - NVIDIA GPU with CUDA 12+ driver
#   - Test signing enabled for development: bcdedit /set testsigning on
#   - Run build.ps1 first to generate artifacts in tools\outputs

[CmdletBinding()]
param(
    [switch]$Uninstall,
    [switch]$SkipDriver,
    [switch]$SkipShim,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$GreenBoostVersion = "2.3.0"
$RegPath = "HKLM:\SYSTEM\CurrentControlSet\Services\GreenBoost\Parameters"
$DriverName = "GreenBoost"
$ShimDll = "greenboost_cuda.dll"
$DriverSys = "greenboost_win.sys"
$DriverInf = "greenboost_win.inf"
$TestExe = "test_ioctl.exe"
$DeviceHardwareId = "Root\GreenBoost"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$outputsDir = Join-Path $scriptDir "outputs"

function Find-DevCon {
    $devconPaths = @(
        (Join-Path $outputsDir "devcon.exe"),
        "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64\devcon.exe",
        "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.22621.0\x64\devcon.exe",
        "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.22000.0\x64\devcon.exe",
        "C:\Program Files (x86)\Windows Kits\10\Tools\x64\devcon.exe"
    )
    foreach ($path in $devconPaths) {
        if (Test-Path $path) {
            return $path
        }
    }
    $kitsRoot = "C:\Program Files (x86)\Windows Kits\10\Tools"
    if (Test-Path $kitsRoot) {
        $found = Get-ChildItem $kitsRoot -Recurse -Filter "devcon.exe" -ErrorAction SilentlyContinue |
                 Where-Object { $_.FullName -match "\\x64\\" } |
                 Sort-Object { $_.Directory.Name } -Descending |
                 Select-Object -First 1
        if ($found) {
            return $found.FullName
        }
    }
    return $null
}

function Write-Status($msg) { Write-Host "[GreenBoost] $msg" -ForegroundColor Green }
function Write-Warn($msg)   { Write-Host "[GreenBoost] WARNING: $msg" -ForegroundColor Yellow }
function Write-Err($msg)    { Write-Host "[GreenBoost] ERROR: $msg" -ForegroundColor Red }

function Test-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-GpuInfo {
    Write-Status "Detecting NVIDIA GPU..."
    try {
        $nvidiaSmi = & "nvidia-smi" "--query-gpu=name,memory.total" "--format=csv,noheader,nounits" 2>$null
        if ($nvidiaSmi) {
            $parts = $nvidiaSmi.Split(",").Trim()
            $gpuName = $parts[0]
            $vramMb = [int]$parts[1]
            $vramGb = [math]::Floor($vramMb / 1024)
            Write-Status "GPU: $gpuName ($vramGb GB VRAM)"
            return @{ Name = $gpuName; VramGb = $vramGb; VramMb = $vramMb }
        }
    } catch { }
    Write-Warn "nvidia-smi not found or failed"
    return @{ Name = "Unknown"; VramGb = 12; VramMb = 12288 }
}

function Get-SystemRamGb {
    $ram = Get-CimInstance Win32_PhysicalMemory | Measure-Object -Property Capacity -Sum
    $totalGb = [math]::Floor($ram.Sum / 1GB)
    Write-Status "System RAM: $totalGb GB"
    return $totalGb
}

function Get-CpuInfo {
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $logicalCpus = (Get-CimInstance Win32_Processor | Measure-Object -Property NumberOfLogicalProcessors -Sum).Sum
    $cores = (Get-CimInstance Win32_Processor | Measure-Object -Property NumberOfCores -Sum).Sum
    Write-Status "CPU: $($cpu.Name) ($cores cores, $logicalCpus threads)"
    return @{ Name = $cpu.Name; Cores = $cores; Threads = $logicalCpus }
}

function Get-NvmeInfo {
    $nvme = Get-PhysicalDisk | Where-Object { $_.MediaType -eq "SSD" -and $_.BusType -eq "NVMe" } | Select-Object -First 1
    if ($nvme) {
        $sizeGb = [math]::Floor($nvme.Size / 1GB)
        Write-Status "NVMe: $($nvme.FriendlyName) ($sizeGb GB)"
        return $sizeGb
    }
    Write-Warn "No NVMe drive detected"
    return 0
}

function Set-GreenBoostConfig {
    param(
        [int]$PhysicalVramGb,
        [int]$SystemRamGb
    )

    $virtualVramGb = [math]::Floor($SystemRamGb * 0.8)
    $safetyReserveGb = [math]::Max(12, [math]::Floor($SystemRamGb * 0.19))
    $nvmeSwapGb = 64
    $nvmePoolGb = 58
    $thresholdMb = 256

    Write-Status "Configuration:"
    Write-Status "  T1 Physical VRAM : $PhysicalVramGb GB"
    Write-Status "  T2 DDR4 Pool Cap : $virtualVramGb GB (80% of $SystemRamGb GB)"
    Write-Status "  Safety Reserve   : $safetyReserveGb GB"
    Write-Status "  T3 NVMe Swap     : $nvmeSwapGb GB (cap $nvmePoolGb GB)"
    Write-Status "  Alloc Threshold  : $thresholdMb MB"
    Write-Status "  Combined Model   : $($PhysicalVramGb + $virtualVramGb + $nvmeSwapGb) GB"

    if (-not (Test-Path $RegPath)) {
        New-Item -Path $RegPath -Force | Out-Null
    }

    Set-ItemProperty -Path $RegPath -Name "PhysicalVramGb"  -Value $PhysicalVramGb  -Type DWord
    Set-ItemProperty -Path $RegPath -Name "VirtualVramGb"   -Value $virtualVramGb   -Type DWord
    Set-ItemProperty -Path $RegPath -Name "SafetyReserveGb" -Value $safetyReserveGb -Type DWord
    Set-ItemProperty -Path $RegPath -Name "NvmeSwapGb"      -Value $nvmeSwapGb      -Type DWord
    Set-ItemProperty -Path $RegPath -Name "NvmePoolGb"      -Value $nvmePoolGb      -Type DWord
    Set-ItemProperty -Path $RegPath -Name "ThresholdMb"     -Value $thresholdMb     -Type DWord
    Set-ItemProperty -Path $RegPath -Name "DebugMode"       -Value 0                -Type DWord

    Write-Status "Registry configuration written to $RegPath"
}

function Install-GreenBoostDriver {
    $sysPath = Join-Path $outputsDir $DriverSys
    $infPath = Join-Path $outputsDir $DriverInf

    Write-Status "Looking for driver artifacts in: $outputsDir"

    if (-not (Test-Path $sysPath)) {
        Write-Err "Driver SYS not found: $sysPath"
        Write-Err "Please run build.ps1 first to generate outputs."
        return $false
    }

    if (-not (Test-Path $infPath)) {
        Write-Err "Driver INF not found: $infPath"
        return $false
    }

    Write-Status "  SYS: $sysPath"
    Write-Status "  INF: $infPath"

    $bcdOutput = & bcdedit /enum "{current}" 2>$null
    if ($bcdOutput -match "testsigning\s+Yes") {
        Write-Status "Test signing is enabled"
    } else {
        Write-Warn "Test signing is NOT enabled. Enable with:"
        Write-Warn "  bcdedit /set testsigning on"
        Write-Warn "  (reboot required)"
        if (-not $Force) {
            return $false
        }
    }

    $svc = Get-Service -Name $DriverName -ErrorAction SilentlyContinue
    $existingDevice = & pnputil /enum-devices /connected 2>$null | Select-String -Pattern "GreenBoost"
    
    if ($existingDevice -and $svc) {
        $sourceFile = Get-Item $sysPath
        $driverStoreFiles = Get-ChildItem "C:\Windows\System32\DriverStore\FileRepository\greenboost*" -Recurse -Filter "*.sys" -ErrorAction SilentlyContinue
        $newestInstalled = $driverStoreFiles | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        
        if ($newestInstalled -and $sourceFile.LastWriteTime -gt $newestInstalled.LastWriteTime) {
            Write-Status "Source driver is newer than installed version, forcing update..."
            Write-Status "  Source: $($sourceFile.LastWriteTime)"
            Write-Status "  Installed: $($newestInstalled.LastWriteTime)"
            Uninstall-GreenBoostDriver
            Start-Sleep -Seconds 1
        } elseif ($svc.Status -eq "Running") {
            Write-Status "Driver already installed and running"
            return $true
        } elseif ($svc) {
            Start-Service -Name $DriverName -ErrorAction SilentlyContinue
            Write-Status "Driver service started"
            return $true
        }
    }

    $devcon = Find-DevCon
    if (-not $devcon) {
        Write-Warn "devcon.exe not found. Falling back to pnputil (device may not be created properly)."
        Write-Warn "For proper installation, install Windows SDK or place devcon.exe in outputs folder."
        Write-Status "Installing driver package with pnputil..."
        try {
            $result = & pnputil /add-driver $infPath /install 2>&1
            Write-Status "pnputil output: $result"
        } catch {
            Write-Err "Driver installation failed: $_"
            return $false
        }
        try {
            Start-Service -Name $DriverName -ErrorAction SilentlyContinue
            Write-Status "Driver service started"
        } catch {
            Write-Warn "Could not start driver service (may need reboot)"
        }
        return $true
    }

    Write-Status "Found devcon at: $devcon"
    Write-Status "Installing driver with devcon (creates device instance)..."

    Push-Location $outputsDir
    try {
        $result = & $devcon install $DriverInf $DeviceHardwareId 2>&1
        Write-Status "devcon output: $result"
        
        if ($LASTEXITCODE -ne 0 -and $result -notmatch "device.*created.*successfully") {
            if ($result -match "already exists") {
                Write-Status "Device already exists, attempting to restart..."
                & $devcon restart $DeviceHardwareId 2>&1 | Out-Null
            } else {
                Write-Warn "devcon install returned non-zero, checking if device exists..."
            }
        }
    } catch {
        Write-Err "Driver installation failed: $_"
        Pop-Location
        return $false
    }
    Pop-Location

    Start-Sleep -Milliseconds 500

    $svc = Get-Service -Name $DriverName -ErrorAction SilentlyContinue
    if ($svc) {
        if ($svc.Status -ne "Running") {
            Start-Service -Name $DriverName -ErrorAction SilentlyContinue
        }
        Write-Status "Driver service status: $($svc.Status)"
    }

    Write-Status "Verifying device accessibility..."
    try {
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class GbDevCheck {
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern IntPtr CreateFile(string lpFileName, uint dwDesiredAccess,
        uint dwShareMode, IntPtr lpSecurityAttributes, uint dwCreationDisposition,
        uint dwFlagsAndAttributes, IntPtr hTemplateFile);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr hObject);
    public static bool TestOpen() {
        IntPtr h = CreateFile("\\\\.\\GreenBoost", 0xC0000000, 0, IntPtr.Zero, 3, 0x80, IntPtr.Zero);
        if (h != (IntPtr)(-1)) { CloseHandle(h); return true; }
        return false;
    }
}
"@ -Language CSharp -ErrorAction SilentlyContinue
        $deviceAccessible = [GbDevCheck]::TestOpen()
        if ($deviceAccessible) {
            Write-Status "Device \\.\GreenBoost is accessible!"
            return $true
        } else {
            Write-Warn "Device \\.\GreenBoost not accessible yet (may need reboot)"
            return $true
        }
    } catch {
        Write-Warn "Could not verify device accessibility"
        return $true
    }
}

function Uninstall-GreenBoostDriver {
    Write-Status "Stopping driver service..."
    Stop-Service -Name $DriverName -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500

    $devcon = Find-DevCon
    
    if ($devcon) {
        Write-Status "Removing device with devcon..."
        Push-Location $outputsDir
        try {
            & $devcon remove $DeviceHardwareId 2>&1 | ForEach-Object { Write-Status "devcon: $_" }
        } catch {
            Write-Warn "devcon remove failed: $_"
        }
        Pop-Location
        Start-Sleep -Milliseconds 500
    } else {
        Write-Warn "devcon not found, using sc to delete service..."
    }

    Write-Status "Deleting service..."
    & sc.exe delete $DriverName 2>&1 | Out-Null

    Write-Status "Finding and removing driver packages..."
    $driverOutput = pnputil /enum-drivers 2>$null
    $oemInfs = @()
    $lines = $driverOutput -split "`r?`n"
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match "greenboost_win.inf") {
            for ($j = [Math]::Max(0, $i - 5); $j -lt $i; $j++) {
                if ($lines[$j] -match "(?:发布名称|Published Name)\s*:\s*(oem\d+\.inf)") {
                    $oemInfs += $Matches[1]
                    break
                }
            }
        }
    }

    if ($oemInfs.Count -eq 0) {
        Write-Status "No driver packages found to remove"
    } else {
        Write-Status "Found $($oemInfs.Count) driver package(s) to remove"
        foreach ($oemInf in $oemInfs) {
            Write-Status "Removing driver package: $oemInf"
            try {
                $result = & pnputil /delete-driver $oemInf /uninstall /force 2>&1
                if ($result -match "deleted successfully|Driver package") {
                    Write-Status "Successfully removed: $oemInf"
                } else {
                    Write-Warn "Result for $oemInf : $result"
                }
            } catch {
                Write-Warn "Failed to remove $oemInf : $_"
            }
        }
    }

    Write-Status "Driver uninstallation complete"
}

function Install-GreenBoostShim {
    $shimDllPath = Join-Path $outputsDir $ShimDll

    Write-Status "Looking for shim artifacts in: $outputsDir"

    if (-not (Test-Path $shimDllPath)) {
        Write-Err "Shim DLL not found: $shimDllPath"
        Write-Err "Please run build.ps1 first to generate outputs."
        return $false
    }

    Write-Status "  DLL: $shimDllPath"

    $lmStudioPath = Join-Path $env:USERPROFILE ".cache\lm-studio"
    if (Test-Path $lmStudioPath) {
        Write-Status "LM Studio detected at $lmStudioPath"

        $launcherPath = Join-Path $lmStudioPath "greenboost_launch.bat"
        $lmStudioExe = Get-ChildItem -Path $lmStudioPath -Filter "LM Studio.exe" -Recurse | Select-Object -First 1
        if ($lmStudioExe) {
            $content = @"
@echo off
REM GreenBoost LM Studio Launcher
REM Injects greenboost_cuda.dll into LM Studio for extended VRAM
set GREENBOOST_DEBUG=0
withdll.exe /d:"$shimDllPath" "$($lmStudioExe.FullName)"
"@
            Set-Content -Path $launcherPath -Value $content
            Write-Status "LM Studio launcher created: $launcherPath"
        }
    }

    $ollamaPath = (Get-Command ollama -ErrorAction SilentlyContinue)
    if ($ollamaPath) {
        Write-Status "Ollama detected at $($ollamaPath.Source)"
        Write-Status "Launch with: withdll.exe /d:$shimDllPath ollama serve"
    }

    Write-Status ""
    Write-Status "Shim DLL location: $shimDllPath"
    Write-Status "Injection methods:"
    Write-Status "  1. withdll.exe /d:$shimDllPath <your-app.exe>"
    Write-Status "  2. Use greenboost_launch.bat for LM Studio"
    Write-Status "  3. Set GREENBOOST_DEBUG=1 for verbose logging"

    return $true
}

function Uninstall-GreenBoostShim {
    Write-Status "Removing shim launchers..."
    $launcherPath = Join-Path $env:USERPROFILE ".cache\lm-studio\greenboost_launch.bat"
    if (Test-Path $launcherPath) {
        Remove-Item $launcherPath -Force
        Write-Status "Removed LM Studio launcher"
    }
}

if (-not (Test-Admin)) {
    Write-Err "This script requires Administrator privileges."
    Write-Err "Right-click PowerShell and select 'Run as Administrator'."
    exit 1
}

Write-Status "=== GreenBoost v$GreenBoostVersion Windows Installer ==="
Write-Status "Outputs directory: $outputsDir"

if (-not (Test-Path $outputsDir)) {
    Write-Err "Outputs directory not found: $outputsDir"
    Write-Err "Please run build.ps1 first to generate outputs."
    exit 1
}

if ($Uninstall) {
    Write-Status "Uninstalling GreenBoost..."
    Uninstall-GreenBoostShim
    if (-not $SkipDriver) { Uninstall-GreenBoostDriver }
    if (Test-Path $RegPath) {
        Remove-Item -Path $RegPath -Recurse -Force
        Write-Status "Registry configuration removed"
    }
    Write-Status "Uninstall complete"
    exit 0
}

$gpu = Get-GpuInfo
$ramGb = Get-SystemRamGb
$cpu = Get-CpuInfo
$nvmeGb = Get-NvmeInfo

Set-GreenBoostConfig -PhysicalVramGb $gpu.VramGb -SystemRamGb $ramGb

if (-not $SkipDriver) {
    $driverOk = Install-GreenBoostDriver
    if (-not $driverOk) {
        Write-Warn "Driver installation incomplete - shim will still work in passthrough mode"
    }
}

if (-not $SkipShim) {
    $shimOk = Install-GreenBoostShim
    if (-not $shimOk) {
        Write-Warn "Shim installation incomplete"
    }
}

Write-Status ""
Write-Status "=== Installation Complete ==="
Write-Status "Combined model capacity: $($gpu.VramGb + [math]::Floor($ramGb * 0.8) + 64) GB"
Write-Status "  T1 GPU VRAM : $($gpu.VramGb) GB"
Write-Status "  T2 DDR4     : $([math]::Floor($ramGb * 0.8)) GB"
Write-Status "  T3 NVMe     : 64 GB"
