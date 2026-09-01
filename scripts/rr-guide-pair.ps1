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

# Process 1: no report mode, writes the actual canonical GPU readback checksum.
& $Executable @common "--rr-guide-pair" $Manifest
$baselineExit = $LASTEXITCODE
if ($baselineExit -ne 0) { throw "RR pair baseline process failed with exit $baselineExit" }

# Process 2: report mode, reads the manifest and rejects a changed canonical image.
& $Executable @common "--rr-guide-pair" $Manifest "--rr-guide-report" $Report
$reportExit = $LASTEXITCODE
if ($reportExit -ne 0) { throw "RR pair report process failed with exit $reportExit" }

$reportDoc = Get-Content -Raw -LiteralPath $Report | ConvertFrom-Json
if ($reportDoc.valid -ne $true -or $reportDoc.canonical_pair_match -ne $true) {
    throw "RR pair acceptance failed: report valid=$($reportDoc.valid), pair=$($reportDoc.canonical_pair_match)"
}
$manifestText = Get-Content -Raw -LiteralPath $Manifest
$manifestHash = (($manifestText -split "`n") | Where-Object { $_ -like "fnv1a64=*" } | Select-Object -First 1).Substring(8)
$evidenceDoc = [ordered]@{
    schema = "rt2.rr-guide-pair-evidence.v1"
    baseline_command = "$Executable $($common -join ' ') --rr-guide-pair $Manifest"
    report_command = "$Executable $($common -join ' ') --rr-guide-pair $Manifest --rr-guide-report $Report"
    baseline_exit_code = $baselineExit
    report_exit_code = $reportExit
    baseline_fnv1a64 = $manifestHash
    report_fnv1a64 = $reportDoc.canonical_readback_checksum_fnv1a64
    canonical_pair_match = $reportDoc.canonical_pair_match
    report_valid = $reportDoc.valid
}
$json = $evidenceDoc | ConvertTo-Json -Depth 4
$evidenceParent = Split-Path -Parent $Evidence
if ($evidenceParent) { New-Item -ItemType Directory -Force -Path $evidenceParent | Out-Null }
[IO.File]::WriteAllText([IO.Path]::GetFullPath($Evidence), $json + "`n")
if (!(Test-Path -LiteralPath $Evidence)) { throw "RR pair evidence was not written: $Evidence" }
Write-Host "RR pair accepted: $Evidence"
