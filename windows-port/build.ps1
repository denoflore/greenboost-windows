# SPDX-License-Identifier: GPL-2.0-only
# Build the portable shim and launcher without elevation or driver installation.
[CmdletBinding()]
param(
    [ValidateSet('Release','Debug','RelWithDebInfo')][string]$Config = 'Release',
    [ValidateSet('x64')][string]$Arch = 'x64',
    [string]$Generator,
    [string]$DetoursRoot,
    [switch]$StaticRuntime,
    [switch]$Package,
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'
$source = $PSScriptRoot
$build = Join-Path $source 'build'
$output = Join-Path $source 'tools\outputs'
try {
    $cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if (!$cmake) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($vs) {
                $bundled = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
                if (Test-Path -LiteralPath $bundled) { $cmake = Get-Item -LiteralPath $bundled }
            }
        }
    }
    if (!$cmake) { throw 'CMake was not found. Install CMake or the Visual Studio C++ CMake component.' }
    $cmakePath = if ($cmake.Source) { $cmake.Source } else { $cmake.FullName }
    if ($Clean -and (Test-Path -LiteralPath $build)) {
        $resolved = (Resolve-Path -LiteralPath $build).Path
        if ($resolved -ne [IO.Path]::GetFullPath((Join-Path $source 'build')) -or
            ((Get-Item -LiteralPath $build).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing to clean an unexpected build path: $resolved"
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
    $configure = @('-S',$source,'-B',$build,'-A',$Arch)
    $configure += "-DGB_STATIC_RUNTIME=$($StaticRuntime.IsPresent.ToString().ToUpperInvariant())"
    if ($Generator) { $configure += @('-G',$Generator) }
    if (!$DetoursRoot -and $env:VCPKG_ROOT) { $DetoursRoot = Join-Path $env:VCPKG_ROOT 'installed\x64-windows' }
    if ($DetoursRoot) {
        if (!(Test-Path -LiteralPath $DetoursRoot)) { throw "Detours prefix does not exist: $DetoursRoot" }
        $configure += "-DCMAKE_PREFIX_PATH=$([IO.Path]::GetFullPath($DetoursRoot))"
    }
    & $cmakePath @configure
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed ($LASTEXITCODE)." }
    & $cmakePath --build $build --config $Config
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed ($LASTEXITCODE)." }
    New-Item -ItemType Directory -Path $output -Force | Out-Null
    foreach ($relative in @("shim\$Config\greenboost_cuda.dll","$Config\greenboost.exe","tests\$Config\test_cuda.exe")) {
        $file = Join-Path $build $relative
        if (!(Test-Path -LiteralPath $file)) { throw "Required output is missing: $file" }
        Copy-Item -LiteralPath $file -Destination $output -Force
    }
    Copy-Item -LiteralPath (Join-Path $source '..\LICENSE') -Destination (Join-Path $output 'LICENSE') -Force
    Copy-Item -LiteralPath (Join-Path $source 'README.md'),(Join-Path $source 'VALIDATION.md'),(Join-Path $source 'licenses\Detours.txt') -Destination $output -Force
    if ($Package) {
        $files = @('greenboost.exe','greenboost_cuda.dll','test_cuda.exe','LICENSE','README.md','VALIDATION.md','Detours.txt') |
            ForEach-Object { Join-Path $output $_ }
        $archive = Join-Path $output 'GreenBoost-Windows-2.4.0-beta.1-x64.zip'
        Compress-Archive -LiteralPath $files -DestinationPath $archive -Force
        Write-Host "Package: $archive"
    }
    Write-Host "Portable build complete: $output"
    Write-Host 'Run: greenboost.exe your-cuda-application.exe [arguments]'
    Write-Host 'No kernel driver is included or installed by this build.'
} catch {
    Write-Error $_
    exit 1
}
