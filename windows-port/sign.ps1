# ============================================================
# GreenBoost driver signing script
# Functions: Certificate detection + auto-find latest signtool + sign driver
# Requires: Run PowerShell as Administrator
# ============================================================

param(
    [string]$CertName = "GreenBoostTestCert",
    [string]$DriverFile = "greenboost_win.sys",
    [string]$Password = "GreenBoost123"  # Development only. Use a real cert for production.
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$cerPath = Join-Path $scriptDir "GreenBoostTest.cer"
$pfxPath = Join-Path $scriptDir "GreenBoostTest.pfx"

function Find-LatestSignTool {
    $sdkBasePath = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"

    if (-not (Test-Path $sdkBasePath)) {
        throw "Windows SDK path not found: $sdkBasePath"
    }

    $versions = Get-ChildItem -Path $sdkBasePath -Directory |
        Where-Object { $_.Name -match "^\d+\.\d+\.\d+\.\d+$" } |
        Sort-Object { [Version]$_.Name } -Descending

    foreach ($version in $versions) {
        $signtoolPath = Join-Path $version.FullName "x64\signtool.exe"
        if (Test-Path $signtoolPath) {
            Write-Host "Found signtool: $signtoolPath" -ForegroundColor Cyan
            return $signtoolPath
        }
    }

    throw "signtool.exe not found in any SDK version"
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
    Write-Host "Certificate not found, initializing..." -ForegroundColor Yellow
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

    Write-Host "Certificate generated, thumbprint: $($cert.Thumbprint)" -ForegroundColor Green

    Export-Certificate -Cert $cert -FilePath $CerPath | Out-Null
    Write-Host "Certificate exported: $CerPath" -ForegroundColor Green

    Export-PfxCertificate -Cert $cert -FilePath $PfxPath -Password $securePassword | Out-Null
    Write-Host "PFX exported: $PfxPath" -ForegroundColor Green

    Import-Certificate -FilePath $CerPath -CertStoreLocation "Cert:\LocalMachine\Root" | Out-Null
    Write-Host "Certificate added to Trusted Root CA" -ForegroundColor Green

    Import-Certificate -FilePath $CerPath -CertStoreLocation "Cert:\LocalMachine\TrustedPublisher" | Out-Null
    Write-Host "Certificate added to Trusted Publishers" -ForegroundColor Green

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
    Write-Host "Signing driver..." -ForegroundColor Yellow
    Write-Host "============================================" -ForegroundColor Yellow

    & $SignToolPath sign /f $PfxPath /p $Password /fd SHA256 /t http://timestamp.digicert.com $DriverFile

    if ($LASTEXITCODE -ne 0) {
        throw "Signing failed, exit code: $LASTEXITCODE"
    }

    Write-Host "Signing complete" -ForegroundColor Green
}

# ============================================================
# Main execution logic
# ============================================================

try {
    Write-Host "============================================" -ForegroundColor Cyan
    Write-Host "GreenBoost Driver Signing Tool" -ForegroundColor Cyan
    Write-Host "============================================" -ForegroundColor Cyan

    Write-Host ""
    Write-Host "[Step 1/4] Finding signtool..." -ForegroundColor White
    $signtoolPath = Find-LatestSignTool

    Write-Host ""
    Write-Host "[Step 2/4] Checking certificate..." -ForegroundColor White
    $certExists = Test-CertificateExists -CertName $CertName

    if (-not $certExists) {
        Write-Host "Certificate not found, initialization required" -ForegroundColor Yellow
        $cert = Initialize-Certificate -CertName $CertName -CerPath $cerPath -PfxPath $pfxPath -Password $Password
    } else {
        Write-Host "Certificate already exists: CN=$CertName" -ForegroundColor Green
    }

    Write-Host ""
    Write-Host "[Step 3/4] Signing driver..." -ForegroundColor White

    $driverDir = Join-Path $scriptDir "build\driver\Release"
    $driverPath = Join-Path $driverDir $DriverFile

    if (-not (Test-Path $driverPath)) {
        throw "Driver file not found: $driverPath"
    }

    Write-Host "Driver path: $driverPath" -ForegroundColor Gray

    Sign-Driver -SignToolPath $signtoolPath -PfxPath $pfxPath -Password $Password -DriverFile $driverPath

    Write-Host ""
    Write-Host "============================================" -ForegroundColor Green
    Write-Host "All steps complete!" -ForegroundColor Green
    Write-Host "============================================" -ForegroundColor Green

} catch {
    Write-Host ""
    Write-Host "============================================" -ForegroundColor Red
    Write-Host "Error: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "============================================" -ForegroundColor Red
    exit 1
}
