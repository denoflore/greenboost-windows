# SPDX-License-Identifier: GPL-2.0-only
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Model,
    [Parameter(Mandatory)][string]$LlamaDirectory,
    [ValidateRange(0,31)][int]$Gpu=0,
    [ValidateRange(2,1048576)][int]$ExpertVramMiB=1024,
    [ValidateRange(0,1048576)][int]$ExpertRamMiB=4096,
    [ValidateRange(128,1048576)][int]$Context=2048,
    [ValidateRange(1,32768)][int]$Tokens=128,
    [ValidateRange(1,512)][int]$MicroBatch=8,
    [string]$Prompt='Explain how GPU memory differs from system RAM.',
    [switch]$VerboseBackend
)
$ErrorActionPreference='Stop'
$backend=(Resolve-Path -LiteralPath $LlamaDirectory).Path
$exe=Join-Path $backend 'llama-completion.exe'
if(!(Test-Path -LiteralPath $exe)) {throw 'Build the opt-in backend with build_moe.ps1 first.'}
$modelPath=(Resolve-Path -LiteralPath $Model).Path
$names=@('CUDA_VISIBLE_DEVICES','CUDA_DEVICE_ORDER','GREENBOOST_EXPERT_VRAM_MB','GREENBOOST_EXPERT_RAM_MB','GGML_CUDA_DISABLE_FUSION','GGML_CUDA_GRAPH_OPT')
$saved=@{}
foreach($name in $names){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
try {
    $env:CUDA_VISIBLE_DEVICES="$Gpu"
    $env:CUDA_DEVICE_ORDER=$null
    $env:GREENBOOST_EXPERT_VRAM_MB="$ExpertVramMiB"
    $env:GREENBOOST_EXPERT_RAM_MB="$ExpertRamMiB"
    $env:GGML_CUDA_DISABLE_FUSION='1'
    $env:GGML_CUDA_GRAPH_OPT='0'
    $argsRun=@('-m',$modelPath,'-ngl','999','-dev','CUDA0','-sm','none','-mg','0',
        '-ot','.*=CUDA0','-fa','on','-t','1','-b',"$MicroBatch",'-ub',"$MicroBatch",
        '--no-host','-fit','off','-c',"$Context",'-n',"$Tokens",'-p',$Prompt,
        '--backend-sampling','--samplers','temperature','--temp','0','--no-conversation')
    if($VerboseBackend){$argsRun+='-v'}
    & $exe @argsRun
    if($LASTEXITCODE){throw "Paged CUDA inference failed ($LASTEXITCODE)."}
} finally {
    foreach($name in $names){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}
}
