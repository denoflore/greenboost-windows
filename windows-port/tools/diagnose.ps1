# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2024-2026 Ferran Duarri
# GreenBoost v2.3 — Windows Diagnostic Script
#
# Usage: .\diagnose.ps1 [-Verbose]
#
# Checks driver status, device accessibility, GPU detection,
# memory pool state, and shim injection status.

[CmdletBinding()]
param()

$ErrorActionPreference = "SilentlyContinue"
$RegPath = "HKLM:\SOFTWARE\GreenBoost"
$issues = @()

function Write-Check($msg, $ok) {
    if ($ok) {
        Write-Host "  [PASS] $msg" -ForegroundColor Green
    } else {
        Write-Host "  [FAIL] $msg" -ForegroundColor Red
        $script:issues += $msg
    }
}

function Write-Info($msg) {
    Write-Host "  [INFO] $msg" -ForegroundColor Cyan
}

Write-Host ""
Write-Host "=== GreenBoost v2.3 Diagnostics ===" -ForegroundColor White
Write-Host ""

# ----------------------------------------------------------------
#  1. System prerequisites
# ----------------------------------------------------------------

Write-Host "1. System Prerequisites" -ForegroundColor Yellow

# OS version
$os = Get-CimInstance Win32_OperatingSystem
Write-Info "OS: $($os.Caption) ($($os.BuildNumber))"
$osOk = [int]$os.BuildNumber -ge 18362  # Win10 1903+
Write-Check "Windows 10 1903+ or Windows 11" $osOk

# Architecture
Write-Check "64-bit OS" ([Environment]::Is64BitOperatingSystem)

# Test signing
$bcd = & bcdedit /enum "{current}" 2>$null | Out-String
$testSigning = $bcd -match "testsigning\s+Yes"
Write-Check "Test signing enabled" $testSigning
if (-not $testSigning) {
    Write-Info "Enable with: bcdedit /set testsigning on (reboot required)"
}

Write-Host ""

# ----------------------------------------------------------------
#  2. NVIDIA GPU
# ----------------------------------------------------------------

Write-Host "2. NVIDIA GPU" -ForegroundColor Yellow

$nvidiaSmi = $null
try {
    $nvidiaSmi = & nvidia-smi --query-gpu=name,memory.total,driver_version --format=csv,noheader,nounits 2>$null
} catch { }

if ($nvidiaSmi) {
    $parts = $nvidiaSmi.Split(",").Trim()
    Write-Info "GPU: $($parts[0])"
    Write-Info "VRAM: $($parts[1]) MB"
    Write-Info "Driver: $($parts[2])"
    Write-Check "NVIDIA GPU detected" $true

    $driverMajor = [int]($parts[2].Split(".")[0])
    Write-Check "Driver version >= 535 (CUDA 12+)" ($driverMajor -ge 535)
} else {
    Write-Check "NVIDIA GPU detected" $false
    Write-Info "nvidia-smi not found or failed"
}

Write-Host ""

# ----------------------------------------------------------------
#  3. System Memory
# ----------------------------------------------------------------

Write-Host "3. System Memory" -ForegroundColor Yellow

$mem = Get-CimInstance Win32_OperatingSystem
$totalRamGb = [math]::Round($mem.TotalVisibleMemorySize / 1MB, 1)
$freeRamGb = [math]::Round($mem.FreePhysicalMemory / 1MB, 1)
Write-Info "Total RAM: $totalRamGb GB"
Write-Info "Free RAM: $freeRamGb GB"
Write-Check "32+ GB system RAM" ($totalRamGb -ge 32)

# Pagefile
$pagefile = Get-CimInstance Win32_PageFileSetting
if ($pagefile) {
    Write-Info "Pagefile max: $([math]::Round($pagefile.MaximumSize / 1024, 1)) GB"
} else {
    Write-Info "Pagefile: system managed"
}

Write-Host ""

# ----------------------------------------------------------------
#  4. GreenBoost Registry Configuration
# ----------------------------------------------------------------

Write-Host "4. Registry Configuration" -ForegroundColor Yellow

if (Test-Path $RegPath) {
    Write-Check "Registry key exists ($RegPath)" $true
    $regValues = Get-ItemProperty -Path $RegPath
    Write-Info "PhysicalVramGb  : $($regValues.PhysicalVramGb)"
    Write-Info "VirtualVramGb   : $($regValues.VirtualVramGb)"
    Write-Info "SafetyReserveGb : $($regValues.SafetyReserveGb)"
    Write-Info "NvmeSwapGb      : $($regValues.NvmeSwapGb)"
    Write-Info "ThresholdMb     : $($regValues.ThresholdMb)"
    Write-Info "DebugMode       : $($regValues.DebugMode)"
} else {
    Write-Check "Registry key exists ($RegPath)" $false
    Write-Info "Run install.ps1 to create configuration"
}

Write-Host ""

# ----------------------------------------------------------------
#  5. Driver Status
# ----------------------------------------------------------------

Write-Host "5. Driver Status" -ForegroundColor Yellow

$svc = Get-Service -Name "GreenBoost" 2>$null
if ($svc) {
    Write-Check "Driver service registered" $true
    Write-Info "Service status: $($svc.Status)"
    Write-Check "Driver service running" ($svc.Status -eq "Running")
} else {
    Write-Check "Driver service registered" $false
}

# Check device
$deviceExists = $false
try {
    # Use .NET to attempt to open the device
    $code = @"
using System;
using System.Runtime.InteropServices;

public class GreenBoostDevice {
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern IntPtr CreateFile(string lpFileName, uint dwDesiredAccess,
        uint dwShareMode, IntPtr lpSecurityAttributes, uint dwCreationDisposition,
        uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr hObject);

    public static bool TestOpen() {
        IntPtr h = CreateFile("\\\\.\\GreenBoost",
            0xC0000000, 0, IntPtr.Zero, 3, 0x80, IntPtr.Zero);
        if (h != (IntPtr)(-1)) {
            CloseHandle(h);
            return true;
        }
        return false;
    }
}
"@
    Add-Type -TypeDefinition $code -Language CSharp -ErrorAction SilentlyContinue
    $deviceExists = [GreenBoostDevice]::TestOpen()
} catch { }

Write-Check "Device \\.\GreenBoost accessible" $deviceExists

Write-Host ""

# ----------------------------------------------------------------
#  6. CUDA Libraries
# ----------------------------------------------------------------

Write-Host "6. CUDA Libraries" -ForegroundColor Yellow

$cudaPaths = @(
    "$env:SystemRoot\System32\nvcuda.dll",
    "$env:ProgramFiles\NVIDIA GPU Computing Toolkit\CUDA\v12.0\bin\cudart64_12.dll"
)

$nvcudaExists = Test-Path "$env:SystemRoot\System32\nvcuda.dll"
Write-Check "nvcuda.dll present" $nvcudaExists
if ($nvcudaExists) {
    $ver = (Get-Item "$env:SystemRoot\System32\nvcuda.dll").VersionInfo.FileVersion
    Write-Info "nvcuda.dll version: $ver"
}

$nvmlExists = Test-Path "$env:SystemRoot\System32\nvml.dll"
Write-Check "nvml.dll present" $nvmlExists

Write-Host ""

# ----------------------------------------------------------------
#  7. LM Studio / Ollama
# ----------------------------------------------------------------

Write-Host "7. Application Detection" -ForegroundColor Yellow

$lmStudioPath = Join-Path $env:USERPROFILE ".cache\lm-studio"
if (Test-Path $lmStudioPath) {
    Write-Check "LM Studio detected" $true
    $launcherPath = Join-Path $lmStudioPath "greenboost_launch.bat"
    Write-Check "GreenBoost launcher present" (Test-Path $launcherPath)
} else {
    Write-Info "LM Studio not found at $lmStudioPath"
}

$ollamaCmd = Get-Command ollama -ErrorAction SilentlyContinue
Write-Check "Ollama installed" ($null -ne $ollamaCmd)
if ($ollamaCmd) {
    Write-Info "Ollama path: $($ollamaCmd.Source)"
}

Write-Host ""

# ----------------------------------------------------------------
#  Summary
# ----------------------------------------------------------------

Write-Host "=== Summary ===" -ForegroundColor White
if ($issues.Count -eq 0) {
    Write-Host "  All checks passed!" -ForegroundColor Green
} else {
    Write-Host "  $($issues.Count) issue(s) found:" -ForegroundColor Red
    foreach ($issue in $issues) {
        Write-Host "    - $issue" -ForegroundColor Red
    }
}
Write-Host ""
