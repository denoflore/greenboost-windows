# SPDX-License-Identifier: GPL-2.0-only
# Builds a separate, opt-in backend. The stock portable backend is untouched.
[CmdletBinding()]
param(
    [string]$Directory,
    [string]$CudaRoot=$env:CUDA_PATH,
    [string]$Architectures='75;86;89;120',
    [ValidateRange(1,32)][int]$Jobs=8,
    [string]$SourceDirectory
)
$ErrorActionPreference='Stop'
if(!$Directory) {$Directory=Join-Path $PSScriptRoot 'backends\greenboost-moe-b12019'}
$Directory=[IO.Path]::GetFullPath($Directory)
if(!$CudaRoot -or !(Test-Path -LiteralPath (Join-Path $CudaRoot 'bin\nvcc.exe'))) {throw 'Set -CudaRoot to an installed CUDA Toolkit.'}
if($Architectures -notmatch '^\d+(;\d+)*$') {throw 'Architectures must be CUDA SM numbers separated by semicolons.'}
$cmake=(Get-Command cmake.exe -ErrorAction Stop).Source
$ninja=(Get-Command ninja.exe -ErrorAction Stop).Source
$vswhere=Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs) {throw 'Visual Studio C++ Build Tools were not found.'}
$vcvars=Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
New-Item -ItemType Directory -Force -Path $Directory|Out-Null
$source=Join-Path $Directory 'llama.cpp-b12019'
$marker=Join-Path $Directory 'source-prepared.txt'
if(!(Test-Path -LiteralPath $marker)) {
    if(Test-Path -LiteralPath $source) {throw "Unprepared source already exists: $source. Use a fresh -Directory."}
    if($SourceDirectory) {
        Copy-Item -LiteralPath (Resolve-Path -LiteralPath $SourceDirectory).Path -Destination $source -Recurse
    } else {
        $archive=Join-Path $Directory 'llama-b12019.zip'
        if(Test-Path -LiteralPath $archive) {throw "Archive already exists: $archive. Use a fresh -Directory."}
        Invoke-WebRequest -Uri 'https://codeload.github.com/ggml-org/llama.cpp/zip/refs/tags/b12019' -OutFile $archive
        Expand-Archive -LiteralPath $archive -DestinationPath $Directory
    }
    & (Join-Path $repo 'windows-port\integrations\llama\apply.ps1') -Source $source
    'llama.cpp b12019 with GreenBoost expert residency; rebuild after changing GreenBoost source.'|Set-Content -LiteralPath $marker
}
$build=Join-Path $Directory 'build'
# cmd is used only for the established Visual Studio compiler environment.
$configure=@(
    '"'+$cmake+'"','-S','"'+$source+'"','-B','"'+$build+'"','-G','Ninja',
    '"-DCMAKE_MAKE_PROGRAM='+$ninja+'"','-DCMAKE_BUILD_TYPE=Release',
    '"-DCMAKE_CUDA_COMPILER='+ (Join-Path $CudaRoot 'bin\nvcc.exe')+'"',
    '"-DCUDAToolkit_ROOT='+$CudaRoot+'"',
    '"-DCUDA_CUDART='+ (Join-Path $CudaRoot 'lib\x64\cudart.lib')+'"',
    '"-DCMAKE_CUDA_ARCHITECTURES='+$Architectures+'"',
    '-DGGML_CUDA=ON','-DGGML_CUDA_GRAPHS=OFF','-DGGML_NATIVE=OFF',
    '-DLLAMA_CURL=OFF','-DLLAMA_BUILD_TESTS=OFF','-DLLAMA_BUILD_SERVER=OFF',
    '-DLLAMA_BUILD_TOOLS=ON','-DBUILD_SHARED_LIBS=ON','-DGGML_CUDA_FA_QUANTS=f16-f16'
) -join ' '
$commands=@('@echo off',('call "'+$vcvars+'" >nul'), 'if errorlevel 1 exit /b %errorlevel%',
    $configure,'if errorlevel 1 exit /b %errorlevel%',
    ('"'+$cmake+'" --build "'+$build+'" --target llama-completion llama-bench --parallel '+$Jobs))
$commandFile=Join-Path $Directory 'build.cmd'
$commands|Set-Content -LiteralPath $commandFile
& $env:ComSpec /d /c $commandFile
if($LASTEXITCODE) {throw "CUDA backend build failed ($LASTEXITCODE)."}
$bin=Join-Path $build 'bin'
foreach($pattern in @('cudart64_*.dll','cublas64_*.dll','cublasLt64_*.dll')) {
    $dlls=@()
    foreach($relative in @('bin\x64','bin')) {
        $runtimeDirectory=Join-Path $CudaRoot $relative
        if(Test-Path -LiteralPath $runtimeDirectory) {
            $dlls=@(Get-ChildItem -LiteralPath $runtimeDirectory -Filter $pattern -File)
            if($dlls){break}
        }
    }
    if(!$dlls) {throw "CUDA runtime dependency missing: $pattern"}
    foreach($dll in $dlls){Copy-Item -LiteralPath $dll.FullName -Destination $bin}
}
Write-Host "Backend: $bin"
Write-Host 'Use run_moe.ps1 with this directory, a quantised MoE GGUF, and explicit VRAM/RAM budgets.'
