# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2024-2026 Ferran Duarri
# GreenBoost v2.3 - Configuration Tool
#
# Usage: Run as Administrator
#   .\config.ps1                    # Auto-detect and configure
#   .\config.ps1 -PhysicalVramGb 32 # Manual override
#   .\config.ps1 -Show              # Show current config only

[CmdletBinding()]
param(
    [switch]$Show,
    [int]$PhysicalVramGb,
    [int]$VirtualVramGb,
    [int]$SafetyReserveGb,
    [int]$NvmeSwapGb = 64,
    [int]$NvmePoolGb = 58,
    [int]$ThresholdMb = 256,
    [switch]$Restart
)

$ErrorActionPreference = "Stop"
$RegPath = "HKLM:\SYSTEM\CurrentControlSet\Services\GreenBoost\Parameters"
$ShimRegPath = "HKLM:\SOFTWARE\GreenBoost"
$DriverName = "GreenBoost"

function Write-Status($msg) { Write-Host "[GreenBoost] $msg" -ForegroundColor Green }
function Write-Warn($msg)   { Write-Host "[GreenBoost] WARNING: $msg" -ForegroundColor Yellow }
function Write-Err($msg)    { Write-Host "[GreenBoost] ERROR: $msg" -ForegroundColor Red }

function Test-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-GpuVramGb {
    try {
        $nvidiaSmi = & "nvidia-smi" "--query-gpu=memory.total" "--format=csv,noheader,nounits" 2>$null
        if ($nvidiaSmi) {
            return [math]::Floor([int]$nvidiaSmi / 1024)
        }
    } catch { }
    return 12
}

function Get-SystemRamGb {
    $ram = Get-CimInstance Win32_PhysicalMemory | Measure-Object -Property Capacity -Sum
    return [math]::Floor($ram.Sum / 1GB)
}

function Get-CurrentConfig {
    if (-not (Test-Path $RegPath)) {
        return $null
    }
    $reg = Get-ItemProperty -Path $RegPath
    return @{
        PhysicalVramGb  = $reg.PhysicalVramGb
        VirtualVramGb   = $reg.VirtualVramGb
        SafetyReserveGb = $reg.SafetyReserveGb
        NvmeSwapGb      = $reg.NvmeSwapGb
        NvmePoolGb      = $reg.NvmePoolGb
        ThresholdMb     = $reg.ThresholdMb
        DebugMode       = $reg.DebugMode
    }
}

function Show-Config {
    $config = Get-CurrentConfig
    if (-not $config) {
        Write-Err "No configuration found. Run install.ps1 first."
        return
    }
    
    Write-Host ""
    Write-Host "=== GreenBoost Configuration ===" -ForegroundColor Cyan
    Write-Host "  PhysicalVramGb  : $($config.PhysicalVramGb) GB (T1 GPU VRAM)"
    Write-Host "  VirtualVramGb   : $($config.VirtualVramGb) GB (T2 DDR4 Pool)"
    Write-Host "  SafetyReserveGb : $($config.SafetyReserveGb) GB"
    Write-Host "  NvmeSwapGb      : $($config.NvmeSwapGb) GB (T3 NVMe)"
    Write-Host "  NvmePoolGb      : $($config.NvmePoolGb) GB"
    Write-Host "  ThresholdMb     : $($config.ThresholdMb) MB"
    Write-Host "  DebugMode       : $($config.DebugMode)"
    
    $total = $config.PhysicalVramGb + $config.VirtualVramGb + $config.NvmeSwapGb
    Write-Host ""
    Write-Host "  Combined Capacity: $total GB" -ForegroundColor Green
    Write-Host ""
}

function Set-Config {
    param(
        [int]$PhysVram,
        [int]$SysRam
    )
    
    if (-not (Test-Path $RegPath)) {
        New-Item -Path $RegPath -Force | Out-Null
    }
    
    if ($PhysicalVramGb -eq 0) {
        $PhysicalVramGb = $PhysVram
    }
    if ($VirtualVramGb -eq 0) {
        $VirtualVramGb = [math]::Floor($SysRam * 0.8)
    }
    if ($SafetyReserveGb -eq 0) {
        $SafetyReserveGb = [math]::Max(12, [math]::Floor($SysRam * 0.19))
    }
    
    Set-ItemProperty -Path $RegPath -Name "PhysicalVramGb"  -Value $PhysicalVramGb  -Type DWord
    Set-ItemProperty -Path $RegPath -Name "VirtualVramGb"   -Value $VirtualVramGb   -Type DWord
    Set-ItemProperty -Path $RegPath -Name "SafetyReserveGb" -Value $SafetyReserveGb -Type DWord
    Set-ItemProperty -Path $RegPath -Name "NvmeSwapGb"      -Value $NvmeSwapGb      -Type DWord
    Set-ItemProperty -Path $RegPath -Name "NvmePoolGb"      -Value $NvmePoolGb      -Type DWord
    Set-ItemProperty -Path $RegPath -Name "ThresholdMb"     -Value $ThresholdMb     -Type DWord
    
    # Mirror shim-relevant values to SOFTWARE\GreenBoost for backward compat
    if (-not (Test-Path $ShimRegPath)) {
        New-Item -Path $ShimRegPath -Force | Out-Null
    }
    Set-ItemProperty -Path $ShimRegPath -Name "PhysicalVramGb"  -Value $PhysicalVramGb  -Type DWord
    Set-ItemProperty -Path $ShimRegPath -Name "VirtualVramGb"   -Value $VirtualVramGb   -Type DWord
    Set-ItemProperty -Path $ShimRegPath -Name "ThresholdMb"     -Value $ThresholdMb     -Type DWord
    Set-ItemProperty -Path $ShimRegPath -Name "DebugMode"       -Value 0                -Type DWord
    
    Write-Status "Configuration updated:"
    Write-Status "  T1 Physical VRAM : $PhysicalVramGb GB"
    Write-Status "  T2 DDR4 Pool     : $VirtualVramGb GB"
    Write-Status "  Safety Reserve   : $SafetyReserveGb GB"
    Write-Status "  T3 NVMe Swap     : $NvmeSwapGb GB"
    Write-Status "  Combined         : $($PhysicalVramGb + $VirtualVramGb + $NvmeSwapGb) GB"
}

function Restart-Driver {
    $svc = Get-Service -Name $DriverName -ErrorAction SilentlyContinue
    if (-not $svc) {
        Write-Warn "Driver service not found"
        return $false
    }
    
    Write-Status "Attempting to stop driver service..."
    try {
        Stop-Service -Name $DriverName -Force -ErrorAction Stop
        Start-Sleep -Milliseconds 1000
    } catch {
        Write-Warn "Cannot stop driver (may have open handles or is in use)"
        Write-Warn "Configuration will take effect after system reboot."
        Write-Status "You can also try: sc stop GreenBoost; sc start GreenBoost"
        return $false
    }
    
    Write-Status "Starting driver service..."
    try {
        Start-Service -Name $DriverName -ErrorAction Stop
        Start-Sleep -Milliseconds 500
    } catch {
        Write-Err "Failed to start driver: $_"
        return $false
    }
    
    $svc = Get-Service -Name $DriverName
    Write-Status "Driver service status: $($svc.Status)"
    return $true
}

if (-not (Test-Admin)) {
    Write-Err "This script requires Administrator privileges."
    exit 1
}

if ($Show) {
    Show-Config
    exit 0
}

Write-Status "Detecting hardware..."
$gpuVram = Get-GpuVramGb
$sysRam = Get-SystemRamGb

Write-Status "GPU VRAM: $gpuVram GB"
Write-Status "System RAM: $sysRam GB"

Set-Config -PhysVram $gpuVram -SysRam $sysRam

if ($Restart) {
    Restart-Driver
} else {
    Write-Host ""
    Write-Warn "Driver not restarted. Use -Restart to apply changes immediately."
    Write-Warn "Or run: Restart-Service GreenBoost"
}

Show-Config

