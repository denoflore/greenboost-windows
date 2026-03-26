# ============================================================
# GreenBoost 驱动签名脚本（优化版）
# 功能：证书存在检测 + 自动查找最新 signtool + 签名驱动
# 要求：以管理员身份运行 PowerShell
# ============================================================

param(
    [string]$CertName = "GreenBoostTestCert",
    [string]$DriverFile = "greenboost_win.sys",
    [string]$Password = "GreenBoost123"
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$cerPath = Join-Path $scriptDir "GreenBoostTest.cer"
$pfxPath = Join-Path $scriptDir "GreenBoostTest.pfx"

function Find-LatestSignTool {
    $sdkBasePath = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
    
    if (-not (Test-Path $sdkBasePath)) {
        throw "未找到 Windows SDK 路径：$sdkBasePath"
    }
    
    $versions = Get-ChildItem -Path $sdkBasePath -Directory | 
        Where-Object { $_.Name -match "^\d+\.\d+\.\d+\.\d+$" } |
        Sort-Object { [Version]$_.Name } -Descending
    
    foreach ($version in $versions) {
        $signtoolPath = Join-Path $version.FullName "x64\signtool.exe"
        if (Test-Path $signtoolPath) {
            Write-Host "找到 signtool: $signtoolPath" -ForegroundColor Cyan
            return $signtoolPath
        }
    }
    
    throw "未在任何 SDK 版本中找到 signtool.exe"
}

function Test-CertificateExists {
    param([string]$CertName)
    
    $cert = Get-ChildItem Cert:\LocalMachine\My | 
        Where-Object { $_.Subject -eq "CN=$CertName" }
    
    return $null -ne $cert
}

function Initialize-Certificate {
    param(
        [string]$CertName,
        [string]$CerPath,
        [string]$PfxPath,
        [string]$Password
    )
    
    Write-Host "============================================" -ForegroundColor Yellow
    Write-Host "证书不存在，开始初始化..." -ForegroundColor Yellow
    Write-Host "============================================" -ForegroundColor Yellow
    
    $securePassword = ConvertTo-SecureString -String $Password -Force -AsPlainText
    
    $cert = New-SelfSignedCertificate `
        -Type Custom `
        -Subject "CN=$CertName" `
        -KeyUsage DigitalSignature `
        -KeyAlgorithm RSA `
        -KeyLength 2048 `
        -CertStoreLocation "Cert:\LocalMachine\My" `
        -FriendlyName $CertName
    
    Write-Host "证书已生成，指纹：$($cert.Thumbprint)" -ForegroundColor Green
    
    Export-Certificate -Cert $cert -FilePath $CerPath | Out-Null
    Write-Host "证书已导出：$CerPath" -ForegroundColor Green
    
    Export-PfxCertificate -Cert $cert -FilePath $PfxPath -Password $securePassword | Out-Null
    Write-Host "PFX 已导出：$PfxPath" -ForegroundColor Green
    
    Import-Certificate -FilePath $CerPath -CertStoreLocation "Cert:\LocalMachine\Root" | Out-Null
    Write-Host "证书已添加到受信任根证书颁发机构" -ForegroundColor Green
    
    Import-Certificate -FilePath $CerPath -CertStoreLocation "Cert:\LocalMachine\TrustedPublisher" | Out-Null
    Write-Host "证书已添加到受信任发布者" -ForegroundColor Green
    
    return $cert
}

function Sign-Driver {
    param(
        [string]$SignToolPath,
        [string]$PfxPath,
        [string]$Password,
        [string]$DriverFile
    )
    
    Write-Host "============================================" -ForegroundColor Yellow
    Write-Host "开始签名驱动..." -ForegroundColor Yellow
    Write-Host "============================================" -ForegroundColor Yellow
    
    & $SignToolPath sign /f $PfxPath /p $Password /fd SHA256 /t http://timestamp.digicert.com $DriverFile
    
    if ($LASTEXITCODE -ne 0) {
        throw "签名失败，退出码：$LASTEXITCODE"
    }
    
    Write-Host "签名完成" -ForegroundColor Green
}

# ============================================================
# 主执行逻辑
# ============================================================

try {
    Write-Host "============================================" -ForegroundColor Cyan
    Write-Host "GreenBoost 驱动签名工具" -ForegroundColor Cyan
    Write-Host "============================================" -ForegroundColor Cyan
    
    Write-Host ""
    Write-Host "[步骤 1/4] 查找 signtool..." -ForegroundColor White
    $signtoolPath = Find-LatestSignTool
    
    Write-Host ""
    Write-Host "[步骤 2/4] 检测证书..." -ForegroundColor White
    $certExists = Test-CertificateExists -CertName $CertName
    
    if (-not $certExists) {
        Write-Host "证书不存在，需要初始化" -ForegroundColor Yellow
        $cert = Initialize-Certificate -CertName $CertName -CerPath $cerPath -PfxPath $pfxPath -Password $Password
    } else {
        Write-Host "证书已存在：CN=$CertName" -ForegroundColor Green
    }
    
    Write-Host ""
    Write-Host "[步骤 3/4] 签名驱动..." -ForegroundColor White
    
    $driverDir = Join-Path $scriptDir "build\driver\Release"
    $driverPath = Join-Path $driverDir $DriverFile
    
    if (-not (Test-Path $driverPath)) {
        throw "驱动文件不存在：$driverPath"
    }
    
    Write-Host "驱动路径：$driverPath" -ForegroundColor Gray
    
    Sign-Driver -SignToolPath $signtoolPath -PfxPath $pfxPath -Password $Password -DriverFile $driverPath
    
    Write-Host ""
    Write-Host "============================================" -ForegroundColor Green
    Write-Host "所有步骤完成！" -ForegroundColor Green
    Write-Host "============================================" -ForegroundColor Green
    
} catch {
    Write-Host ""
    Write-Host "============================================" -ForegroundColor Red
    Write-Host "错误：$($_.Exception.Message)" -ForegroundColor Red
    Write-Host "============================================" -ForegroundColor Red
    exit 1
}