#!/usr/bin/env pwsh
# run_recovery_test.ps1 — run the Phase 1B recovery regression scenario.
#
# Usage: pwsh run_recovery_test.ps1
# Exits 0 if the recovery scenario passes, 1 if it fails.

$ErrorActionPreference = "Stop"

$exe = "bin\Release-windows-x86_64\RT2SliceRunner\RT2SliceRunner.exe"
$report = "artifacts\recovery_report.json"

if (-not (Test-Path $exe)) {
    Write-Host "ERROR: RT2SliceRunner not found at $exe" -ForegroundColor Red
    exit 1
}

# Ensure artifacts directory exists
$artifactsDir = "artifacts"
if (-not (Test-Path $artifactsDir)) {
    New-Item -ItemType Directory -Path $artifactsDir -Force | Out-Null
}

Write-Host "========== Phase 1B Recovery Scenario ==========" -ForegroundColor Cyan

& $exe --recovery-scenario --out $report
$exitCode = $LASTEXITCODE

if ($exitCode -ne 0) {
    Write-Host "[Recovery] FAIL: exit code $exitCode" -ForegroundColor Red
    $reportContent = Get-Content $report -Raw -ErrorAction SilentlyContinue
    if ($reportContent) { Write-Host $reportContent }
    exit 1
}

# Verify the report says pass.
$reportContent = Get-Content $report -Raw -ErrorAction SilentlyContinue
if ($reportContent -notmatch '"recoveryScenario":\s*"pass"') {
    Write-Host "[Recovery] FAIL: report does not confirm pass" -ForegroundColor Red
    Write-Host $reportContent
    exit 1
}
if ($reportContent -notmatch '"assetBacked":\s*true') {
    Write-Host "[Recovery] FAIL: scenario did not exercise asset-backed recovery" -ForegroundColor Red
    Write-Host $reportContent
    exit 1
}

Write-Host "[Recovery] PASS" -ForegroundColor Green
exit 0
