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

$certSubject = 'CN=kilrogg (Ryan Cole, self-signed)'
$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
    Where-Object { $_.Subject -eq $certSubject -and $_.NotAfter -gt (Get-Date) } |
    Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $cert) {
    Write-Host "No signing certificate found, creating one ($certSubject)..."
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $certSubject `
        -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(5)
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
    # Status is NotTrusted/UnknownError on machines that haven't installed the
    # cert - expected for self-signed. Signed is what matters here.
    Write-Host "Signed: $($sig.Status) ($($sig.StatusMessage))"

    Export-Certificate -Cert $cert -FilePath (Join-Path $stage 'kilrogg-signing.cer') | Out-Null

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
SmartScreen/antivirus friction), open an ADMIN command prompt in this
folder and run:

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
