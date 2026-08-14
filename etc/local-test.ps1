# Loopback test: builds kilrogg, starts the sender and a receiver connected to
# it on 127.0.0.1, then waits. Close the receiver window (or press Esc in it)
# to end the test; the sender is stopped automatically.
#
#   .\etc\local-test.ps1            # dummy source (bouncing square)
#   .\etc\local-test.ps1 -Desktop   # capture the real desktop (hall of mirrors!)
#   .\etc\local-test.ps1 -SkipBuild # reuse existing binaries

param(
    [switch]$Desktop,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$buildDir = Join-Path $root 'build'
$exeDir = Join-Path $buildDir 'Release'

if (-not $SkipBuild) {
    if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
        cmake -B $buildDir -S $root
        if ($LASTEXITCODE -ne 0) { exit 1 }
    }
    cmake --build $buildDir --config Release
    if ($LASTEXITCODE -ne 0) { exit 1 }
}

$senderExe = Join-Path $exeDir 'kilrogg-send.exe'
$receiverExe = Join-Path $exeDir 'kilrogg-recv.exe'
if (-not (Test-Path $senderExe)) {
    Write-Error "sender not found at $senderExe - build first (drop -SkipBuild)"
}

if ($Desktop) {
    Write-Host 'Starting sender (desktop capture)...'
    $send = Start-Process -FilePath $senderExe -PassThru
} else {
    Write-Host 'Starting sender (dummy source)...'
    $send = Start-Process -FilePath $senderExe -ArgumentList '--dummy' -PassThru
}

Start-Sleep -Milliseconds 500
Write-Host 'Starting receiver on 127.0.0.1...'
$recv = Start-Process -FilePath $receiverExe -ArgumentList '127.0.0.1' -PassThru

Write-Host 'Close the receiver window (or press Esc in it) to end the test.'
try {
    Wait-Process -Id $recv.Id
} finally {
    if (-not $send.HasExited) { Stop-Process -Id $send.Id -Force }
    Write-Host 'Test ended, sender stopped.'
}
