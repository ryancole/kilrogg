# Builds a distributable kilrogg-send.exe and zips it into dist\.
#
# Uses a separate build directory (build-dist) configured with a statically
# linked MSVC runtime, so the exe runs on machines without the VC++
# redistributable installed.
#
#   .\etc\package-sender.ps1

param(
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$buildDir = Join-Path $root 'build-dist'
if (-not $OutDir) { $OutDir = Join-Path $root 'dist' }

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
"@ | Set-Content (Join-Path $stage 'README.txt')

    $zip = Join-Path $OutDir 'kilrogg-send.zip'
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force

    $size = [math]::Round((Get-Item $zip).Length / 1MB, 2)
    Write-Host "Packaged: $zip ($size MB)"
} finally {
    Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
}
