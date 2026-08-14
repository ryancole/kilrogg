# Builds a distributable kilrogg-send.exe, signs it, and zips it into dist\.
#
# Uses a separate build directory (build-dist) configured with a statically
# linked MSVC runtime, so the exe runs on machines without the VC++
# redistributable installed.
#
# Signing uses a self-signed code-signing certificate, created on first run
# and kept in the current user's certificate store. The public half is
# exported into the zip as kilrogg-signing.cer so target machines can choose
# to trust it (instructions in the packaged README).
#
#   .\etc\package-sender.ps1

param(
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$buildDir = Join-Path $root 'build-dist'
if (-not $OutDir) { $OutDir = Join-Path $root 'dist' }

# Looked up by FriendlyName, not Subject: DN values containing commas get
# re-rendered with escaping quotes by the store, which makes Subject string
# comparison silently fail and mint a duplicate cert on every run.
$certFriendlyName = 'kilrogg code signing'
$certSubject = 'CN=kilrogg self-signed (Ryan Cole)'
$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
    Where-Object { $_.FriendlyName -eq $certFriendlyName -and $_.NotAfter -gt (Get-Date) } |
    Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $cert) {
    Write-Host "No signing certificate found, creating one ($certSubject)..."
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $certSubject `
        -FriendlyName $certFriendlyName -CertStoreLocation Cert:\CurrentUser\My `
        -NotAfter (Get-Date).AddYears(5)
}
Write-Host "Signing with $($cert.Subject), thumbprint $($cert.Thumbprint)"

cmake -B $buildDir -S $root -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
if ($LASTEXITCODE -ne 0) { exit 1 }
cmake --build $buildDir --config Release --target kilrogg-send
if ($LASTEXITCODE -ne 0) { exit 1 }

$exe = Join-Path $buildDir 'Release\kilrogg-send.exe'
if (-not (Test-Path $exe)) { Write-Error "expected $exe after build" }

New-Item -ItemType Directory -Force $OutDir | Out-Null
$stage = Join-Path $env:TEMP "kilrogg-package-$PID"
New-Item -ItemType Directory -Force $stage | Out-Null
try {
    Copy-Item $exe $stage
    $stagedExe = Join-Path $stage 'kilrogg-send.exe'

    # Timestamping keeps the signature valid after the cert expires; fall
    # back to an untimestamped signature if the timestamp server is down.
    try {
        $sig = Set-AuthenticodeSignature -FilePath $stagedExe -Certificate $cert `
            -HashAlgorithm SHA256 -TimestampServer 'http://timestamp.digicert.com'
    } catch {
        Write-Warning "timestamp server unreachable, signing without timestamp"
        $sig = Set-AuthenticodeSignature -FilePath $stagedExe -Certificate $cert -HashAlgorithm SHA256
    }
    if (-not $sig.SignerCertificate) {
        Write-Error "signing failed: $($sig.Status) $($sig.StatusMessage)"
    }
    if ($sig.Status -eq 'Valid') {
        Write-Host "Signed (trusted on this machine)"
    } else {
        # Chain-of-trust statuses are expected for a self-signed cert on a
        # machine that hasn't run install-cert.cmd; the signature itself is
        # intact.
        Write-Host "Signed (self-signed: untrusted here until install-cert.cmd runs - this is normal)"
    }

    Export-Certificate -Cert $cert -FilePath (Join-Path $stage 'kilrogg-signing.cer') | Out-Null

    @'
@echo off
rem Installs the kilrogg signing certificate into this machine's trusted
rem stores so Windows trusts kilrogg-send.exe's signature. Only run this on
rem machines you own.

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator access...
    powershell -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
    exit /b
)

cd /d "%~dp0"
certutil -addstore Root kilrogg-signing.cer
certutil -addstore TrustedPublisher kilrogg-signing.cer
echo.
echo Done. kilrogg's signature is now trusted on this machine.
pause
'@ | Set-Content (Join-Path $stage 'install-cert.cmd')

    @"
kilrogg sender
==============

Shares this machine's desktop (primary monitor) over the local network.

Run:            kilrogg-send.exe            (listens on TCP port 47800)
Custom port:    kilrogg-send.exe --port N
Self test:      kilrogg-send.exe --dummy    (streams a test pattern instead)

Allow the program through Windows Firewall when prompted, or the viewer
will not be able to connect. The stream is unencrypted - use on trusted
networks only.

To view, run kilrogg-recv.exe <this-machine's-ip> on another machine.

Signature
---------
kilrogg-send.exe is signed with the self-signed certificate included as
kilrogg-signing.cer. To make Windows trust the signature (reduces
SmartScreen/antivirus friction), double-click install-cert.cmd and accept
the administrator prompt. It runs:

    certutil -addstore Root kilrogg-signing.cer
    certutil -addstore TrustedPublisher kilrogg-signing.cer

Only do this on machines you own - it tells Windows to trust software
signed by this certificate.
"@ | Set-Content (Join-Path $stage 'README.txt')

    $zip = Join-Path $OutDir 'kilrogg-send.zip'
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force

    $size = [math]::Round((Get-Item $zip).Length / 1MB, 2)
    Write-Host "Packaged: $zip ($size MB)"
} finally {
    Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
}
