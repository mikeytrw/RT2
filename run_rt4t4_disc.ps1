#!/usr/bin/env pwsh
# run_rt4t4_disc.ps1 — RT4-T4 bounded visibility discriminators.
#
# Control (pre-fix staged exe) vs candidate (tree exe) on:
#   - rt4t4-override.rt2scene  (override path: opaque + BLEND + MASK cubes)
#   - rt4t4-mixed-sun.rt2scene (per-triangle path: imported whole-model OBJ
#     with opaque + BLEND tris, meshRef -1, plus directional sun + floor)
# Regression (must be identical): script-scenario synth + optional sponza.
#
# The dirty any-hit INVERTED-keep mutant is explicitly excluded: this script
# aborts if the tree anyhit.rahit differs from HEAD semantics (r >= alpha).
#
# Usage: pwsh run_rt4t4_disc.ps1 [-ControlDir <dir>] [-OutRoot <dir>] [-Sponza <scene>]
# Exits 0 only if every capture succeeds AND rt4t4_disc_assert.py passes.

param(
    [string]$ControlDir = "C:\Users\mikey\AppData\Local\Temp\opencode\rt4t4r-control",
    [string]$OutRoot = "C:\Users\mikey\AppData\Local\Temp\opencode\rt4t4-sun",
    [string]$Sponza = ""
)

$ErrorActionPreference = "Stop"

$repo    = $PSScriptRoot
$candDir = "$repo\bin\Release-windows-x86_64\RT2App"
$candExe = "$candDir\RT2App.exe"
$ctrlExe = "$ControlDir\RT2App.exe"
$overFix = "$repo\RT2App\assets\rt4t4-override.rt2scene"
$sunFix  = "$repo\RT2App\assets\rt4t4-mixed-sun.rt2scene"
$synth   = "$repo\RT2App\assets\script-scenario.rt2scene"

foreach ($p in @($candExe, $ctrlExe, $overFix, $sunFix, $synth)) {
    if (-not (Test-Path $p)) { Write-Host "ERROR: missing $p"; exit 1 }
}

# Mutant guard: the INVERTED-keep diagnostic must not be in the tree.
$anyhitSrc = Get-Content "$repo\RT2App\shaders\anyhit.rahit" -Raw
if ($anyhitSrc -match "INVERTED|if\s*\(\s*r\s*<\s*alpha\s*\)") {
    Write-Host "ERROR: tree anyhit.rahit carries the INVERTED-keep mutant; reverted semantics required."
    exit 1
}
if ($anyhitSrc -notmatch "if\s*\(\s*r\s*>=\s*alpha\s*\)") {
    Write-Host "ERROR: tree anyhit.rahit lacks the intended 'r >= alpha' keep rule."
    exit 1
}

function Hash-File($p) { return (Get-FileHash $p -Algorithm SHA256).Hash }

New-Item -ItemType Directory -Path $OutRoot -Force | Out-Null
"control exe: $(Hash-File $ctrlExe)" | Out-File "$OutRoot\exe-hashes.txt" -Encoding ascii
"candidate exe: $(Hash-File $candExe)" | Out-File "$OutRoot\exe-hashes.txt" -Append -Encoding ascii
"control anyhit.spv: $(Hash-File "$ControlDir\anyhit.spv")" | Out-File "$OutRoot\exe-hashes.txt" -Append -Encoding ascii
"candidate anyhit.spv: $(Hash-File "$candDir\anyhit.spv")" | Out-File "$OutRoot\exe-hashes.txt" -Append -Encoding ascii
"tree anyhit.spv: $(Hash-File "$repo\RT2App\shaders\anyhit.spv")" | Out-File "$OutRoot\exe-hashes.txt" -Append -Encoding ascii
Get-Content "$OutRoot\exe-hashes.txt"

function Run-One($name, $exe, $exeDir, $scene, $w, $h, $frames, $spp, $extra) {
    $folder = "$OutRoot\$name"
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    $cmd = @($exe, "--headless", "--scene", $scene,
        "--output-display", "$folder\frame.png",
        "--frames", "$frames", "--width", "$w", "--height", "$h",
        "--spp", "$spp", "--bounces", "8", "--seed", "0",
        "--denoiser-mode", "off") + $extra
    $prevErr = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    Push-Location $exeDir
    & $cmd[0] $cmd[1..($cmd.Count-1)] > "$folder\run.log" 2> "$folder\run.err"
    $code = $LASTEXITCODE
    Copy-Item "$exeDir\rt2_log.txt" "$folder\rt2_log.txt" -Force -ErrorAction SilentlyContinue
    Pop-Location
    $ErrorActionPreference = $prevErr
    if ($code -ne 0) { Write-Host "FAIL $name exit=$code"; exit 1 }
    $hash = Hash-File "$folder\frame.png"
    Write-Host "DONE $name $hash"
    $ngx = Select-String -Path "$folder\run.log" -Pattern "NGX|Effective|active-RR|RR " -SimpleMatch | Select-Object -First 5
    if ($ngx) { $ngx | ForEach-Object { Write-Host "  [$name] $($_.Line.Trim())" } }
}

Run-One "over-control" $ctrlExe $ControlDir $overFix 960 540 12 1 @("--no-accumulate")
Run-One "over-candidate" $candExe $candDir $overFix 960 540 12 1 @("--no-accumulate")
Run-One "sun-control" $ctrlExe $ControlDir $sunFix 960 540 12 8 @()
Run-One "sun-candidate" $candExe $candDir $sunFix 960 540 12 8 @()
Run-One "synth-control" $ctrlExe $ControlDir $synth 960 540 12 1 @("--no-accumulate")
Run-One "synth-candidate" $candExe $candDir $synth 960 540 12 1 @("--no-accumulate")
if ($Sponza -ne "" -and (Test-Path $Sponza)) {
    $yard = @("--camera-pos", "0", "1.6", "0", "--camera-forward", "1", "0", "0", "--no-accumulate")
    Run-One "yard-control" $ctrlExe $ControlDir $Sponza 1470 609 12 1 $yard
    Run-One "yard-candidate" $candExe $candDir $Sponza 1470 609 12 1 $yard
} else {
    Write-Host "Sponza skipped (no -Sponza scene)."
}

Write-Host "--- captures done, running assertions ---"
& python "$repo\rt4t4_disc_assert.py" $OutRoot
$assertCode = $LASTEXITCODE
if ($assertCode -ne 0) { Write-Host "ASSERTS FAIL exit=$assertCode"; exit 1 }
Write-Host "RT4-T4 DISCRIMINATORS PASS"
