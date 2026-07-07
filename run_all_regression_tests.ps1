#!/usr/bin/env pwsh
# run_all_regression_tests.ps1 — run all regression tests across scenes and modes
# Usage: pwsh run_all_regression_tests.ps1
#
# Runs 3 modes (RT-primary, Raster-first, Raster-first+NRD) x 2 scenes (sofa, ABG)
# Exits 0 if all pass, 1 if any fail.

$Env = "C:\Users\mikey\Downloads\kloofendal_48d_partly_cloudy_puresky_4k.exr"
$Scenes = @(
    @{ path = "C:\Users\mikey\Downloads\sofa_and_lamp.glb"; name = "sofa" },
    @{ path = "C:\Users\mikey\Downloads\ABeautifulGame.glb"; name = "abg" }
)

$Modes = @(
    @{ flag = "-RTPrimary";   name = "rt_primary";   spp = 5; frames = 10 },
    @{ flag = "-RasterFirst"; name = "raster_first"; spp = 5; frames = 10 },
    @{ flag = "-NRD";         name = "raster_nrd";   spp = 1; frames = 10 }
)

$passed = 0
$failed = 0
$results = @()

foreach ($scene in $Scenes) {
    foreach ($mode in $Modes) {
        $label = "$($scene.name)_$($mode.name)"
        Write-Host "`n========== Regression: $label ==========" -ForegroundColor Cyan

        $args = @(
            "-Label", $label,
            "-Scene", $scene.path,
            "-Env", $Env,
            "-Spp", $mode.spp,
            "-Frames", $mode.frames,
            "-Bounces", 4,
            $mode.flag
        )

        & powershell -NoProfile -ExecutionPolicy Bypass -File "run_regression_test.ps1" @args
        $exitCode = $LASTEXITCODE

        if ($exitCode -eq 0) {
            $passed++
            $results += @{ label = $label; status = "PASS" }
            Write-Host "[$label] PASS" -ForegroundColor Green
        } else {
            $failed++
            $results += @{ label = $label; status = "FAIL" }
            Write-Host "[$label] FAIL" -ForegroundColor Red
        }
    }
}

Write-Host "`n========== Summary ==========" -ForegroundColor Cyan
foreach ($r in $results) {
    $color = if ($r.status -eq "PASS") { "Green" } else { "Red" }
    Write-Host "  $($r.label): $($r.status)" -ForegroundColor $color
}
Write-Host "`n$passed passed, $failed failed"

if ($failed -gt 0) { exit 1 } else { exit 0 }