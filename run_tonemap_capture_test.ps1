#!/usr/bin/env pwsh
# run_tonemap_capture_test.ps1 — bounded GPU/headless differential checks
# for camera-owned filmic tone mapping (batch 3).
#
# Renders a small deterministic scene headlessly under three camera looks
# (AgX/0 old-file migration, Reinhard/0, ACES/+2), plus diagnostic-view and
# native-path variants, then compares PNG display output against the PFM
# scene-linear source through scripts/compare_tonemap_captures.py:
#   - GPU/CPU parity (<=1 code value) per operator,
#   - raw PFM isolation across operators,
#   - operator/EV effect on the display output,
#   - diagnostic-view bypass of the camera look,
#   - unwritable-output nonzero exit.
#
# Usage: powershell -File run_tonemap_capture_test.ps1
# Exits 0 when every check passes, 1 otherwise.

# Deliberately NOT "Stop": RT2App writes progress to stderr, and under Stop
# PowerShell wraps each stderr line as a terminating NativeCommandError.
$ErrorActionPreference = "Continue"

$exe    = "bin\Release-windows-x86_64\RT2App\RT2App.exe"
$source = "RT2App\assets\vertical-slice.rt2scene"
$work   = "artifacts\tonemap"
$frames = 16
$width  = 320
$height = 200

if (-not (Test-Path $exe))    { Write-Host "ERROR: RT2App not found at $exe"; exit 1 }
if (-not (Test-Path $source)) { Write-Host "ERROR: scene not found at $source"; exit 1 }
New-Item -ItemType Directory -Path $work -Force | Out-Null

function Render-Variant($scene, $png, $pfm, $extraArgs) {
    $args = @("--headless", "--scene", $scene, "--frames", $frames,
              "--width", $width, "--height", $height) + $extraArgs
    if ($png) { $args += @("--output", $png) }
    if ($pfm) { $args += @("--output-hdr", $pfm) }
    & $exe @args > "$work/last.log" 2>&1
    return $LASTEXITCODE
}

function Inject-Look($name, $toneMap, $ev) {
    $scene = Join-Path $work "$name.rt2scene"
    Copy-Item $source $scene -Force
    $text = Get-Content $scene -Raw | ConvertFrom-Json
    # PowerShell 5.1 has no -AsHashtable; add via PSCustomObject members.
    $text.camera | Add-Member -NotePropertyName "toneMap" -NotePropertyValue $toneMap -Force
    $text.camera | Add-Member -NotePropertyName "exposureEV" -NotePropertyValue $ev -Force
    $text | ConvertTo-Json -Depth 32 | Set-Content $scene
    return $scene
}

Write-Host "========== Tonemap Capture Checks =========="

# Old-file shape (no presentation keys) migrates to AgX/0 live.
Copy-Item $source (Join-Path $work "agx.rt2scene") -Force
$reinhardScene = Inject-Look "reinhard" "reinhard" 0.0
$acesScene     = Inject-Look "aces" "aces" 2.0

$code = Render-Variant (Join-Path $work "agx.rt2scene") (Join-Path $work "agx.png") (Join-Path $work "agx.pfm") @()
if ($code -ne 0) { Write-Host "[Tonemap] FAIL: agx render exited $code"; exit 1 }
$code = Render-Variant $reinhardScene (Join-Path $work "reinhard.png") (Join-Path $work "reinhard.pfm") @()
if ($code -ne 0) { Write-Host "[Tonemap] FAIL: reinhard render exited $code"; exit 1 }
$code = Render-Variant $acesScene (Join-Path $work "aces.png") (Join-Path $work "aces.pfm") @()
if ($code -ne 0) { Write-Host "[Tonemap] FAIL: aces render exited $code"; exit 1 }

# Diagnostic views bypass the camera look (same mode, both looks).
$code = Render-Variant (Join-Path $work "agx.rt2scene") (Join-Path $work "debug_agx.png") $null @("--gbuffer-debug", "0")
if ($code -ne 0) { Write-Host "[Tonemap] FAIL: debug agx render exited $code"; exit 1 }
$code = Render-Variant $acesScene (Join-Path $work "debug_aces.png") $null @("--gbuffer-debug", "0")
if ($code -ne 0) { Write-Host "[Tonemap] FAIL: debug aces render exited $code"; exit 1 }

# Native path source selection (RGBA32F) alongside the default RR path.
# Proof is content parity on the native pair (checked by the comparison),
# not a log line: rt2_log.txt is truncated by every process start.
$code = Render-Variant (Join-Path $work "agx.rt2scene") (Join-Path $work "native.png") (Join-Path $work "native.pfm") @("--denoiser-mode", "off")
if ($code -ne 0) { Write-Host "[Tonemap] FAIL: native render exited $code"; exit 1 }
if (-not (Test-Path (Join-Path $work "native.png"))) { Write-Host "[Tonemap] FAIL: no native screenshot"; exit 1 }
if (-not (Test-Path (Join-Path $work "native.pfm"))) { Write-Host "[Tonemap] FAIL: no native HDR source"; exit 1 }

# Unwritable output must fail loudly (nonzero exit).
$code = Render-Variant (Join-Path $work "agx.rt2scene") $work $null @("--frames", "1")
if ($code -eq 0) { Write-Host "[Tonemap] FAIL: unwritable output exited 0"; exit 1 }
Write-Host "[Tonemap] write failure exits nonzero (code $code)"

# CLI overrides overlay the scene look for the invocation only: an AgX
# scene with explicit flags must match the equivalent authored scene
# byte-for-byte, and invalid values must exit nonzero before rendering.
$code = Render-Variant (Join-Path $work "agx.rt2scene") (Join-Path $work "cli_reinhard.png") $null @("--tone-map", "reinhard", "--exposure-ev", "0")
if ($code -ne 0) { Write-Host "[Tonemap] FAIL: CLI override render exited $code"; exit 1 }
$code = Render-Variant (Join-Path $work "agx.rt2scene") (Join-Path $work "cli_aces.png") $null @("--tone-map", "aces", "--exposure-ev", "2")
if ($code -ne 0) { Write-Host "[Tonemap] FAIL: CLI aces render exited $code"; exit 1 }
if ((Get-FileHash (Join-Path $work "cli_reinhard.png")).Hash -ne (Get-FileHash (Join-Path $work "reinhard.png")).Hash) {
    Write-Host "[Tonemap] FAIL: CLI override PNG differs from the authored-look PNG"; exit 1 }
if ((Get-FileHash (Join-Path $work "cli_aces.png")).Hash -ne (Get-FileHash (Join-Path $work "aces.png")).Hash) {
    Write-Host "[Tonemap] FAIL: CLI aces PNG differs from the authored-look PNG"; exit 1 }
Write-Host "[Tonemap] CLI overrides match authored looks"

foreach ($bad in @(@("--tone-map", "hdr"), @("--exposure-ev", "99"), @("--tone-map"), @("--exposure-ev", "abc"))) {
    $code = Render-Variant (Join-Path $work "agx.rt2scene") (Join-Path $work "bad.png") $null $bad
    if ($code -eq 0) { Write-Host "[Tonemap] FAIL: invalid CLI ($bad) exited 0"; exit 1 }
}
Write-Host "[Tonemap] invalid CLI values exit nonzero"

& $exe --help > "$work/help.txt" 2>&1
$help = Get-Content "$work/help.txt" -Raw
if ($help -notmatch "--tone-map" -or $help -notmatch "--exposure-ev") {
    Write-Host "[Tonemap] FAIL: --help omits the presentation flags"; exit 1 }

python scripts/compare_tonemap_captures.py $work
if ($LASTEXITCODE -ne 0) { Write-Host "[Tonemap] FAIL: differential comparison failed"; exit 1 }

Write-Host "[Tonemap] ALL CHECKS PASS"
exit 0
