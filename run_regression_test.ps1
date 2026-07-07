#!/usr/bin/env pwsh
# run_regression_test.ps1 — render headless screenshots and compare to baseline
# Usage:
#   pwsh run_regression_test.ps1 [-Label name] [-Scene path] [-Env path] [-Baseline path]
#                               [-Frames n] [-Spp n] [-Bounces n] [-NRD] [-RasterFirst]
#                               [-NoNRD] [-RTPrimary]
#
# Mode selection (pick one):
#   -RTPrimary    : RT-primary path (default)
#   -RasterFirst  : Raster-first without NRD
#   -NRD          : Raster-first with NRD denoiser
#
# Baseline auto-selection by mode + scene name:
#   baseline_rt_primary_<scene>.png
#   baseline_raster_first_<scene>.png
#   baseline_raster_nrd_<scene>.png

param(
    [string]$Label = "current",
    [string]$Scene = "C:\Users\mikey\Downloads\sofa_and_lamp.glb",
    [string]$Env = "C:\Users\mikey\Downloads\kloofendal_48d_partly_cloudy_puresky_4k.exr",
    [string]$Baseline = "",
    [int]$Frames = 10,
    [int]$Spp = 5,
    [int]$Bounces = 4,
    [switch]$RTPrimary,
    [switch]$RasterFirst,
    [switch]$NRD
)

$ErrorActionPreference = "Continue"
$Exe = "bin\Release-windows-x86_64\RT2App\RT2App.exe"
$Output = "regression_$Label.png"

if (-not (Test-Path $Exe)) {
    Write-Error "RT2App.exe not found at $Exe. Build first."
    exit 1
}

# Determine mode and render args
$modeName = "rt_primary"
$renderArgs = @("--headless", "--scene", $Scene, "--env", $Env, "--output", $Output,
                 "--frames", $Frames, "--spp", $Spp, "--bounces", $Bounces)

if ($NRD) {
    $modeName = "raster_nrd"
    $renderArgs += @("--raster-first", "--nrd")
} elseif ($RasterFirst) {
    $modeName = "raster_first"
    $renderArgs += "--raster-first"
} elseif ($RTPrimary) {
    $modeName = "rt_primary"
}

# Derive scene short name for baseline filename
$sceneName = [System.IO.Path]::GetFileNameWithoutExtension($Scene)
$sceneName = $sceneName.ToLower() -replace '[^a-z0-9]', '_'

# Auto-select baseline if not provided
if ($Baseline -eq "") {
    $Baseline = "baseline_${modeName}_${sceneName}.png"
}

Write-Host "Regression: mode=$modeName scene=$sceneName frames=$Frames spp=$Spp bounces=$Bounces"
Write-Host "Rendering..."
$output_lines = & $Exe @renderArgs 2>&1
$output_lines | Select-String "Headless|saved"

if (-not (Test-Path $Output)) {
    Write-Error "Screenshot was not generated: $Output"
    exit 1
}

if (-not (Test-Path $Baseline)) {
    Write-Host "Regression: No baseline found at $Baseline -- saving current as baseline"
    Copy-Item $Output $Baseline
    Write-Host "Regression PASS (baseline created)"
    exit 0
}

# Compare using System.Drawing
Add-Type -AssemblyName System.Drawing
$a = [System.Drawing.Image]::FromFile((Resolve-Path $Baseline))
$b = [System.Drawing.Image]::FromFile((Resolve-Path $Output))

if ($a.Width -ne $b.Width -or $a.Height -ne $b.Height) {
    Write-Error "Dimension mismatch: baseline=$($a.Width)x$($a.Height) vs current=$($b.Width)x$($b.Height)"
    $a.Dispose(); $b.Dispose()
    exit 1
}

$bmpA = New-Object System.Drawing.Bitmap($a)
$bmpB = New-Object System.Drawing.Bitmap($b)

$diffPixels = 0
$sumDiff = 0
$maxDiff = 0
$total = $a.Width * $a.Height

for ($y = 0; $y -lt $a.Height; $y++) {
    for ($x = 0; $x -lt $a.Width; $x++) {
        $pa = $bmpA.GetPixel($x, $y)
        $pb = $bmpB.GetPixel($x, $y)
        $d = [Math]::Abs([int]$pa.R - [int]$pb.R) + [Math]::Abs([int]$pa.G - [int]$pb.G) + [Math]::Abs([int]$pa.B - [int]$pb.B)
        if ($d -gt 0) {
            $diffPixels++
            $sumDiff += $d
            if ($d -gt $maxDiff) { $maxDiff = $d }
        }
    }
}

$bmpA.Dispose(); $bmpB.Dispose(); $a.Dispose(); $b.Dispose()

if ($diffPixels -gt 0) {
    $avgDiff = [math]::Round($sumDiff / $diffPixels, 2)
} else {
    $avgDiff = 0
}
$diffPct = [math]::Round(($diffPixels / $total) * 100, 1)

# Thresholds: stochastic noise should give avg diff < 5 and max diff < 50
# A real regression will show avg diff > 20 or max diff > 200
$pass = ($avgDiff -lt 10 -and $maxDiff -lt 100)

Write-Host ""
Write-Host "Regression Label:    $Label"
Write-Host "Regression Mode:      $modeName"
Write-Host "Regression Baseline:  $Baseline"
Write-Host "Regression Diff:     $diffPixels / $total pixels ($diffPct pct)"
Write-Host "Regression Avg diff: $avgDiff per differing pixel"
Write-Host "Regression Max diff: $maxDiff"

if ($pass) {
    Write-Host "Regression PASS (within noise threshold)"
    exit 0
} else {
    Write-Host "Regression FAIL (possible regression - check visually)"
    exit 1
}