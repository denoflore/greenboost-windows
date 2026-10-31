# SPDX-License-Identifier: GPL-2.0-only
# Run each real runtime in a fresh process against the packaged launcher.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string[]]$RuntimePath,
    [ValidateRange(0,1024)][int]$Device = 0,
    [string]$PackageDirectory
)
$ErrorActionPreference = 'Stop'
if (!$PackageDirectory) { $PackageDirectory = $PSScriptRoot }
$launcher = Join-Path $PackageDirectory 'greenboost.exe'
$probe = Join-Path $PackageDirectory 'test_runtime.exe'
foreach ($file in @($launcher,$probe)) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing packaged executable: $file" }
}
$saved = @{}
foreach ($name in @('GREENBOOST_EXTRA_MB','GREENBOOST_THRESHOLD_MB','GREENBOOST_RESERVE_MB','GREENBOOST_DEBUG','GREENBOOST_ALLOC_POLICY')) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    $env:GREENBOOST_THRESHOLD_MB = '256'
    $env:GREENBOOST_RESERVE_MB = '8192'
    $env:GREENBOOST_DEBUG = '0'
    $cases = @(
        @{Name='native'; Inject=$false; MiB=512; Managed=0; Extra=1024; Policy='adaptive'},
        @{Name='adaptive'; Inject=$true; MiB=512; Managed=0; Extra=1024; Policy='adaptive'},
        @{Name='managed'; Inject=$true; MiB=512; Managed=1; Extra=1024; Policy='managed'},
        @{Name='below-threshold'; Inject=$true; MiB=8; Managed=0; Extra=1024; Policy='managed'},
        @{Name='extension-disabled'; Inject=$true; MiB=512; Managed=0; Extra=0; Policy='managed'}
    )
    foreach ($runtime in $RuntimePath) {
        $resolved = (Resolve-Path -LiteralPath $runtime).Path
        Write-Host "Runtime: $resolved"
        Write-Host "DLL version: $((Get-Item -LiteralPath $resolved).VersionInfo.FileVersion)"
        foreach ($case in $cases) {
            $env:GREENBOOST_EXTRA_MB = [string]$case.Extra
            $env:GREENBOOST_ALLOC_POLICY = $case.Policy
            Write-Host "Case: $($case.Name)"
            if ($case.Inject) { & $launcher $probe $resolved $Device $case.MiB $case.Managed }
            else { & $probe $resolved $Device $case.MiB $case.Managed }
            if ($LASTEXITCODE -ne 0) { throw "$($case.Name) failed for $resolved (exit $LASTEXITCODE)" }
        }
    }
    Write-Host 'PASS: every requested runtime completed all five CUDA cases.'
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
