# SPDX-License-Identifier: GPL-2.0-only
# Download the tested upstream kernel backend without modifying installed applications.
[CmdletBinding()]
param(
    [ValidateSet('12.4','13.4')][string]$Cuda = '13.4',
    [string]$Destination
)
$ErrorActionPreference = 'Stop'
$release = 'b12019'
if (!$Destination) { $Destination = Join-Path $PSScriptRoot "backends\llama-$release-cuda$Cuda" }
if (![IO.Path]::IsPathRooted($Destination)) { $Destination = Join-Path $PWD.Path $Destination }
$target = [IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $target) { throw "Destination already exists: $target. Choose a new folder to preserve the existing installation." }
$parent = Split-Path -Parent $target
New-Item -ItemType Directory -Path $parent -Force | Out-Null
$stage = Join-Path $parent ('.greenboost-download-' + [guid]::NewGuid().ToString('N'))
$bin = Join-Path $stage 'bin'
New-Item -ItemType Directory -Path $bin -Force | Out-Null
$savedProgress = $ProgressPreference
try {
    $ProgressPreference = 'SilentlyContinue'
    foreach ($asset in @("llama-$release-bin-win-cuda-$Cuda-x64.zip", "cudart-llama-bin-win-cuda-$Cuda-x64.zip")) {
        $url = "https://github.com/ggml-org/llama.cpp/releases/download/$release/$asset"
        $archive = Join-Path $stage $asset
        Write-Host "Downloading $url"
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $archive -TimeoutSec 600
        Expand-Archive -LiteralPath $archive -DestinationPath $bin -Force
        Remove-Item -LiteralPath $archive
    }
    $major = $Cuda.Split('.')[0]
    foreach ($name in @('llama-bench.exe','llama-completion.exe','llama-server.exe','ggml-cuda.dll',"cudart64_$major.dll","cublas64_$major.dll","cublasLt64_$major.dll")) {
        if (!(Test-Path -LiteralPath (Join-Path $bin $name) -PathType Leaf)) { throw "Upstream archive is missing $name" }
    }
    # Both paths are owned by this invocation. Never merge into an existing install.
    if ((Split-Path -Parent ([IO.Path]::GetFullPath($stage))) -ne $parent -or
        (Test-Path -LiteralPath $target)) { throw 'Installation destination changed during download.' }
    Move-Item -LiteralPath $bin -Destination $target
    Remove-Item -LiteralPath $stage
    Write-Host "Installed llama.cpp $release, CUDA ${Cuda}: $target"
    Write-Host 'No system driver, CUDA Toolkit, PATH or existing application was changed.'
    $target
} catch {
    Write-Warning "Installation did not complete. Any partial download remains at $stage."
    throw
} finally {
    $ProgressPreference = $savedProgress
}
