#!/usr/bin/env pwsh
# run_slice_test.ps1 — run the RT2SliceRunner on the vertical-slice fixture
# and verify the exit code + expected final transform.
#
# Usage: pwsh run_slice_test.ps1
# Exits 0 if the slice passes, 1 if it fails.

$ErrorActionPreference = "Stop"

$exe = "bin\Release-windows-x86_64\RT2SliceRunner\RT2SliceRunner.exe"
$fixture = "RT2App\assets\vertical-slice.rt2scene"
$report = "artifacts\slice_report.json"

if (-not (Test-Path $exe)) {
    Write-Host "ERROR: RT2SliceRunner not found at $exe" -ForegroundColor Red
    Write-Host "Build RT2SliceRunner first: premake5 vs2022 && msbuild RT2SliceRunner /p:Configuration=Release /p:Platform=x64"
    exit 1
}

if (-not (Test-Path $fixture)) {
    Write-Host "ERROR: Fixture not found at $fixture" -ForegroundColor Red
    Write-Host "Run RT2Tests to generate it first."
    exit 1
}

# Ensure artifacts directory exists
$artifactsDir = "artifacts"
if (-not (Test-Path $artifactsDir)) {
    New-Item -ItemType Directory -Path $artifactsDir -Force | Out-Null
}

Write-Host "========== Vertical Slice Test ==========" -ForegroundColor Cyan
Write-Host "Fixture: $fixture"
Write-Host "Steps:   60"

& $exe --scene $fixture --steps 60 --out $report
$exitCode = $LASTEXITCODE

if ($exitCode -ne 0) {
    Write-Host "[Slice] FAIL: exit code $exitCode" -ForegroundColor Red
    exit 1
}

# Verify the report contains the expected final cube transform (x ≈ 1.0).
$reportContent = Get-Content $report -Raw
if ($reportContent -notmatch '"authoringIntact": true') {
    Write-Host "[Slice] FAIL: authoring scene was not intact" -ForegroundColor Red
    exit 1
}

# Check the cube's final translation x is approximately 1.0
if ($reportContent -match '"name":\s*"Cube"[\s\S]*?"translation":\s*\[([0-9.eE+-]+)') {
    $cubeX = [double]$Matches[1]
    if ([Math]::Abs($cubeX - 1.0) -gt 0.01) {
        Write-Host "[Slice] FAIL: cube x=$cubeX, expected ~1.0" -ForegroundColor Red
        exit 1
    }
    Write-Host "[Slice] Cube final x=$cubeX (expected ~1.0)"
} else {
    Write-Host "[Slice] FAIL: could not find cube transform in report" -ForegroundColor Red
    exit 1
}

Write-Host "[Slice] PASS" -ForegroundColor Green
exit 0