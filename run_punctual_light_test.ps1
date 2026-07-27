#!/usr/bin/env pwsh
# run_punctual_light_test.ps1 - GPU regression check for punctual lights.
#
# vertical-slice.rt2scene has one cube, one point light, no emissive
# triangles, no environment map, and showBackground defaults to false in
# headless mode. So every lit pixel comes from the punctual light and from
# nothing else: if punctual lighting regresses, the frame goes black.
#
# Usage: powershell -File run_punctual_light_test.ps1
# Exits 0 if the render is lit, 1 otherwise.

# Deliberately NOT "Stop": RT2App writes progress to stderr, and under Stop
# PowerShell wraps each stderr line as a terminating NativeCommandError, so a
# perfectly healthy render aborts the script. Exit codes are checked instead.
$ErrorActionPreference = "Continue"

$exe   = "bin\Release-windows-x86_64\RT2App\RT2App.exe"
$scene = "RT2App\assets\vertical-slice.rt2scene"
$out   = "artifacts\punctual_light.png"

if (-not (Test-Path $exe))   { Write-Host "ERROR: RT2App not found at $exe"; exit 1 }
if (-not (Test-Path $scene)) { Write-Host "ERROR: scene not found at $scene"; exit 1 }
if (-not (Test-Path "artifacts")) { New-Item -ItemType Directory -Path "artifacts" -Force | Out-Null }

Write-Host "========== Punctual Light GPU Check =========="

& $exe --headless --scene $scene --output $out --frames 16 --width 320 --height 200 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "[PunctualLight] FAIL: headless render exited $LASTEXITCODE"
    exit 1
}
if (-not (Test-Path $out)) {
    Write-Host "[PunctualLight] FAIL: no screenshot produced"
    exit 1
}

Add-Type -AssemblyName System.Drawing
$bmp = [System.Drawing.Bitmap]::FromFile((Resolve-Path $out))
$lit = 0
$maxLum = 0.0
try {
    # Sample a grid rather than every pixel; the lit cube covers the centre.
    for ($y = 0; $y -lt $bmp.Height; $y += 4) {
        for ($x = 0; $x -lt $bmp.Width; $x += 4) {
            $p = $bmp.GetPixel($x, $y)
            $lum = 0.2126 * $p.R + 0.7152 * $p.G + 0.0722 * $p.B
            if ($lum -gt $maxLum) { $maxLum = $lum }
            if ($lum -gt 8) { $lit++ }
        }
    }
}
finally {
    $bmp.Dispose()
}

Write-Host ("[PunctualLight] lit samples: {0}, peak luminance: {1:N1}" -f $lit, $maxLum)

# The cube subtends a small part of a 320x200 frame, so a handful of lit
# samples is the signal. Zero means the light contributed nothing at all.
if ($lit -lt 5) {
    Write-Host "[PunctualLight] FAIL: frame is unlit, punctual lighting is not reaching the surface"
    exit 1
}

Write-Host "[PunctualLight] PASS"
exit 0
