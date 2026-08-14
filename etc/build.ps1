# Builds kilrogg (configuring CMake first if needed), same as the VS Code
# build task.
#
#   .\etc\build.ps1           # Release
#   .\etc\build.ps1 -Debug    # Debug

param(
    [switch]$Debug
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$buildDir = Join-Path $root 'build'
$config = if ($Debug) { 'Debug' } else { 'Release' }

if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
    cmake -B $buildDir -S $root
    if ($LASTEXITCODE -ne 0) { exit 1 }
}

cmake --build $buildDir --config $config
if ($LASTEXITCODE -ne 0) { exit 1 }

Write-Host "Built $config -> $(Join-Path $buildDir "$config")"
