#!/usr/bin/env pwsh
# run_script_test.ps1 — run the Phase 6C headless script scenario.
#
# Usage: pwsh run_script_test.ps1
# Exits 0 if the script scenario passes, 1 if it fails.

$ErrorActionPreference = "Stop"

$exe = "bin\Release-windows-x86_64\RT2SliceRunner\RT2SliceRunner.exe"
$scenario = "RT2App\assets\script-scenario.json"
$report = "artifacts\script_scenario_report.json"

if (-not (Test-Path $exe)) {
    Write-Host "ERROR: RT2SliceRunner not found at $exe" -ForegroundColor Red
    exit 1
}

if (-not (Test-Path $scenario)) {
    Write-Host "ERROR: scenario file not found at $scenario" -ForegroundColor Red
    exit 1
}

# Ensure artifacts directory exists
$artifactsDir = "artifacts"
if (-not (Test-Path $artifactsDir)) {
    New-Item -ItemType Directory -Path $artifactsDir -Force | Out-Null
}

Write-Host "========== Phase 6C Headless Script Scenario ==========" -ForegroundColor Cyan

# Mirrors ScenarioExit in RT2App/src/ScriptScenarioCompare.h, which is the
# contract. Add a code there first, then here.
$exitMeaning = @{
    0 = "pass"
    1 = "scenario JSON missing, unreadable, or malformed"
    2 = "scene path unresolvable or scene load failed"
    3 = "Play failed"
    4 = "--out path could not be opened"
    5 = "expectation failed (transform mismatch, missing entity, or spawn violation)"
    6 = "script error (quarantined, or no instance survived)"
}

$savedErrorActionPreference = $ErrorActionPreference
# PowerShell 7 promotes redirected native stderr to ErrorRecord objects. Keep
# those records in the captured stream so advisory Asset diagnostics can be
# classified by severity below instead of terminating at process invocation.
$ErrorActionPreference = "Continue"
$scenarioOutput = @(& $exe --script-scenario $scenario --out $report 2>&1)
$exitCode = $LASTEXITCODE
$ErrorActionPreference = $savedErrorActionPreference
$scenarioOutput | ForEach-Object { Write-Host $_ }

if ($exitCode -ne 0) {
    $meaning = $exitMeaning[$exitCode]
    if (-not $meaning) { $meaning = "unrecognised - is ScenarioExit ahead of this script?" }
    Write-Host "[ScriptScenario] FAIL: exit code $exitCode ($meaning)" -ForegroundColor Red
    $reportContent = Get-Content $report -Raw -ErrorAction SilentlyContinue
    if ($reportContent) { Write-Host $reportContent }
    exit 1
}

# Verify the report has no mismatches, no spawn violation, no script error.
$reportContent = Get-Content $report -Raw -ErrorAction SilentlyContinue
if ($reportContent -notmatch '"mismatchCount":\s*0') {
    Write-Host "[ScriptScenario] FAIL: report has transform mismatches" -ForegroundColor Red
    Write-Host $reportContent
    exit 1
}
if ($reportContent -notmatch '"spawnViolation":\s*false') {
    Write-Host "[ScriptScenario] FAIL: spawn violation detected" -ForegroundColor Red
    Write-Host $reportContent
    exit 1
}
if ($reportContent -notmatch '"scriptError":\s*false') {
    Write-Host "[ScriptScenario] FAIL: script error (quarantined instances)" -ForegroundColor Red
    Write-Host $reportContent
    exit 1
}
foreach ($line in $scenarioOutput) {
    if ($line -notmatch '\[ScriptScenario\] Asset diagnostic:') {
        continue
    }
    if ($line -notmatch 'severity=(\d+)') {
        Write-Host "[ScriptScenario] FAIL: asset diagnostic has no parseable severity" -ForegroundColor Red
        Write-Host $line
        exit 1
    }
    # AssetDiagnostic::Missing is explicitly value 2. Stale (0) and
    # NonPortable (1) are rendered advisories, not scripting failures.
    if ([int]$Matches[1] -ge 2) {
        Write-Host "[ScriptScenario] FAIL: tracked scenario asset emitted a terminal asset diagnostic" -ForegroundColor Red
        Write-Host $line
        exit 1
    }
}

Write-Host "[ScriptScenario] PASS" -ForegroundColor Green
exit 0
