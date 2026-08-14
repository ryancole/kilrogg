# Deletes everything regenerable: build trees, packaging output, and IDE
# caches. Source, git history, and hand-written config are never touched.
#
#   .\etc\clean.ps1           # delete
#   .\etc\clean.ps1 -DryRun   # show what would be deleted
#
# After cleaning, the next build reconfigures from scratch (cmake -B build,
# which the test/package scripts run automatically when needed).

param(
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

# All regenerable: CMake build trees (build-dist is the packaging one),
# packaged zips, and the Visual Studio IDE cache.
$targets = @('build', 'build-dist', 'dist', 'out', '.vs')

$totalMB = 0
foreach ($name in $targets) {
    $path = Join-Path $root $name
    if (-not (Test-Path $path)) { continue }
    $sum = (Get-ChildItem $path -Recurse -Force -ErrorAction SilentlyContinue |
        Measure-Object Length -Sum).Sum
    if (-not $sum) { $sum = 0 }
    $mb = [math]::Round($sum / 1MB, 1)
    $totalMB += $mb
    if ($DryRun) {
        Write-Host "would delete $name\ ($mb MB)"
    } else {
        try {
            Remove-Item -Recurse -Force $path -ErrorAction Stop
            Write-Host "deleted $name\ ($mb MB)"
        } catch {
            # Typically a running exe or a terminal/debugger parked inside
            # the directory. Delete what we can, flag the rest.
            Write-Warning "$name\ is partly in use, some files were left behind (close whatever is using it and re-run)"
            $totalMB -= $mb
        }
    }
}

if ($totalMB -eq 0) {
    Write-Host 'nothing to clean'
} else {
    Write-Host "$(if ($DryRun) { 'would free' } else { 'freed' }) $totalMB MB"
}
