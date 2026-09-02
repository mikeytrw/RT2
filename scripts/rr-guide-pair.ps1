param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$Scene,
    [Parameter(Mandatory=$true)][string]$Report,
    [Parameter(Mandatory=$true)][string]$Manifest,
    [string]$Evidence = "",
    [int]$Width = 256,
    [int]$Height = 256,
    [int]$Frames = 2,
    [uint32]$Seed = 1,
    [string]$Scenario = "",
    [double]$CameraSweepAmplitude = 0,
    [int]$CameraSweepWarmup = 0,
    [int]$CameraSweepPeriod = 32,
    [ValidateSet("lateral", "forward", "yaw")][string]$CameraSweepMode = "lateral"
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($Evidence)) { $Evidence = "$Report.pair.json" }
$common = @("--headless", "--raster-first", "--scene", $Scene,
    "--width", "$Width", "--height", "$Height", "--frames", "$Frames",
    "--seed", "$Seed")
if ($CameraSweepAmplitude -ne 0) {
    $common += @("--camera-sweep", "$CameraSweepAmplitude", "$CameraSweepWarmup", "$CameraSweepPeriod",
        "--camera-sweep-mode", $CameraSweepMode)
}

$evidenceFull = [IO.Path]::GetFullPath($Evidence)
$evidenceParent = Split-Path -Parent $evidenceFull
if ([string]::IsNullOrWhiteSpace($evidenceParent)) { $evidenceParent = (Get-Location).Path }
## Keep the command line deterministic for byte-reproducible reports while
## isolating incidental PNGs from the repository root.
$transientRoot = Join-Path $evidenceParent ".rr-guide-pair-output"
if (Test-Path -LiteralPath $transientRoot) { Remove-Item -LiteralPath $transientRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $transientRoot | Out-Null
if ($Scenario) { $common += @("--rr-guide-scenario", $Scenario) }
$baselineOutput = Join-Path $transientRoot "baseline.png"
$reportOutput = Join-Path $transientRoot "report.png"
$baselineArgs = @($common + @("--output", $baselineOutput))
$reportArgs = @($common + @("--output", $reportOutput))

try {

# Process 1: no report mode, writes the actual canonical GPU readback checksum.
& $Executable @baselineArgs "--rr-guide-pair" $Manifest
$baselineExit = $LASTEXITCODE
if ($baselineExit -ne 0) { throw "RR pair baseline process failed with exit $baselineExit" }
if (!(Test-Path -LiteralPath $baselineOutput)) { throw "RR pair baseline output was not written: $baselineOutput" }

# Process 2: report mode, reads the manifest and rejects a changed canonical image.
& $Executable @reportArgs "--rr-guide-pair" $Manifest "--rr-guide-report" $Report
$reportExit = $LASTEXITCODE
if ($reportExit -ne 0) { throw "RR pair report process failed with exit $reportExit" }
if (!(Test-Path -LiteralPath $reportOutput)) { throw "RR pair report output was not written: $reportOutput" }

$reportDoc = Get-Content -Raw -LiteralPath $Report | ConvertFrom-Json
if ($reportDoc.valid -ne $true -or $reportDoc.canonical_pair_match -ne $true) {
    throw "RR pair acceptance failed: report valid=$($reportDoc.valid), pair=$($reportDoc.canonical_pair_match)"
}
$manifestText = Get-Content -Raw -LiteralPath $Manifest
$manifestHash = (($manifestText -split "`n") | Where-Object { $_ -like "fnv1a64=*" } | Select-Object -First 1).Substring(8)
$evidenceDoc = [ordered]@{
    schema = "rt2.rr-guide-pair-evidence.v1"
    baseline_command = "$Executable $($baselineArgs -join ' ') --rr-guide-pair $Manifest"
    report_command = "$Executable $($reportArgs -join ' ') --rr-guide-pair $Manifest --rr-guide-report $Report"
    baseline_exit_code = $baselineExit
    report_exit_code = $reportExit
    baseline_fnv1a64 = $manifestHash
    report_fnv1a64 = $reportDoc.canonical_readback_checksum_fnv1a64
    canonical_pair_match = $reportDoc.canonical_pair_match
    report_valid = $reportDoc.valid
}
$json = $evidenceDoc | ConvertTo-Json -Depth 4
if ($evidenceParent) { New-Item -ItemType Directory -Force -Path $evidenceParent | Out-Null }
[IO.File]::WriteAllText($evidenceFull, $json + "`n")
if (!(Test-Path -LiteralPath $evidenceFull)) { throw "RR pair evidence was not written: $Evidence" }
Write-Host "RR pair accepted: $Evidence"
}
finally {
    if (Test-Path -LiteralPath $transientRoot) {
        Remove-Item -LiteralPath $transientRoot -Recurse -Force
        if (Test-Path -LiteralPath $transientRoot) { throw "RR pair transient cleanup failed: $transientRoot" }
    }
}
