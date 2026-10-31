# SPDX-License-Identifier: GPL-2.0-only
# Single CUDA device profile for the pinned llama.cpp backend.
[CmdletBinding(DefaultParameterSetName='Run')]
param(
    [Parameter(Mandatory=$true,ParameterSetName='Run')][string]$Model,
    [Parameter(Mandatory=$true,ParameterSetName='List')][switch]$ListDevices,
    [ValidateSet('Completion','Benchmark')][string]$Mode = 'Completion',
    [string]$LlamaDirectory,
    [string]$GreenBoost,
    [ValidateSet('12.4','13.4')][string]$Cuda = '13.4',
    [ValidateSet('adaptive','managed')][string]$AllocationPolicy = 'adaptive',
    [ValidateSet('f16','q8_0','q4_0')][string]$CacheType = 'f16',
    [ValidateRange(1,32768)][int]$Batch = 2048,
    [ValidateRange(1,32768)][int]$MicroBatch = 512,
    [switch]$VerboseBackend,
    [ValidateRange(0,31)][int]$Gpu = 0,
    [ValidateRange(0,1048576)][int]$ExtraMiB = 4096,
    [ValidateRange(1,1048576)][int]$Context = 4096,
    [ValidateRange(1,32768)][int]$PromptTokens = 512,
    [ValidateRange(1,32768)][int]$Tokens = 128,
    [ValidateRange(1,100)][int]$Repetitions = 3,
    [string]$Prompt = 'Explain how GPU memory differs from system RAM in one paragraph.'
)
$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 -File has no PSScriptRoot during default binding.
if (!$LlamaDirectory) { $LlamaDirectory = Join-Path $PSScriptRoot "backends\llama-b12019-cuda$Cuda" }
if (!$GreenBoost) { $GreenBoost = Join-Path $PSScriptRoot 'greenboost.exe' }
$llama = (Resolve-Path -LiteralPath $LlamaDirectory).Path
if (!(Test-Path -LiteralPath (Join-Path $llama 'ggml-cuda.dll') -PathType Leaf)) { throw 'The selected installation has no CUDA backend.' }
if ($MicroBatch -gt $Batch) { throw 'MicroBatch must not exceed Batch.' }
$names = @('CUDA_VISIBLE_DEVICES','GREENBOOST_EXTRA_MB','GREENBOOST_ALLOC_POLICY','GGML_CUDA_GRAPH_OPT','GGML_CUDA_DISABLE_FUSION')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
try {
    # List the full CUDA ordinal space, then expose only the selected GPU for a run.
    [Environment]::SetEnvironmentVariable('CUDA_VISIBLE_DEVICES',$null,'Process')
    if ($ListDevices) {
        & (Join-Path $llama 'llama-bench.exe') --list-devices
        if ($LASTEXITCODE) { throw "CUDA device discovery failed ($LASTEXITCODE)." }
        return
    }
    $launcher = (Resolve-Path -LiteralPath $GreenBoost).Path
    $modelPath = (Resolve-Path -LiteralPath $Model).Path
    if (!(Test-Path -LiteralPath (Join-Path (Split-Path -Parent $launcher) 'greenboost_cuda.dll') -PathType Leaf)) { throw 'Keep greenboost_cuda.dll beside greenboost.exe.' }
    $env:CUDA_VISIBLE_DEVICES = "$Gpu"
    $env:GREENBOOST_EXTRA_MB = "$ExtraMiB"
    $env:GREENBOOST_ALLOC_POLICY = $AllocationPolicy
    # Keep upstream fusion and graph chunking enabled. Concurrent QKV streams are
    # a separate experimental setting; this profile uses the measured default.
    $env:GGML_CUDA_GRAPH_OPT = '0'
    $env:GGML_CUDA_DISABLE_FUSION = '0'
    $arguments = @('-m',$modelPath,'-ngl','999','-dev','CUDA0','-sm','none','-mg','0','-ot','.*=CUDA0','-fa','on','-t','1')
    $arguments += @('-ctk',$CacheType,'-ctv',$CacheType,'-b',"$Batch",'-ub',"$MicroBatch")
    if ($VerboseBackend -and $Mode -ne 'Benchmark') { $arguments += '-v' }
    if ($Mode -eq 'Benchmark') {
        $application = Join-Path $llama 'llama-bench.exe'
        $arguments += @('--no-host','1','-p',"$PromptTokens",'-n',"$Tokens",'-r',"$Repetitions",'-v')
    } else {
        $application = Join-Path $llama 'llama-completion.exe'
        # Greedy CUDA sampling is deliberate: grammar, reasoning budgets and
        # unsupported sampler chains can move sampling back to the CPU upstream.
        $arguments += @('--no-host','-fit','off','-c',"$Context",'-n',"$Tokens",'-p',$Prompt,
            '--backend-sampling','--samplers','temperature','--temp','0','--no-conversation')
    }
    Write-Host "GreenBoost: CUDA ordinal $Gpu -> CUDA0, extra allowance $ExtraMiB MiB, $Mode"
    Write-Host "Allocation: $AllocationPolicy; KV cache: $CacheType; batch: $Batch/$MicroBatch"
    & $launcher $application @arguments
    if ($LASTEXITCODE) { throw "llama.cpp failed ($LASTEXITCODE). No alternate backend was launched." }
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') }
}
