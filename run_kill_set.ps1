#!/usr/bin/env pwsh
# run_kill_set.ps1 — S6-E executable kill-set gate (on-demand / nightly).
#
# For each guard in kill_set/manifest.tsv it:
#   1. asserts the production file is pristine in the working tree;
#   2. asserts, for every stepN.before.txt/stepN.after.txt pair, that the
#      production file contains the exact before text EXACTLY ONCE — a
#      mismatch, or a stale line that would patch different code, ABORTS
#      LOUDLY before any edit or build;
#   3. applies the fault (before -> after) through the SAME content-anchored
#      routine used by the corruption proof, asserts the file actually changed;
#   4. builds the CPU-only RT2Tests (the fault lives in production sources the
#      test project compiles directly) and runs the named doctest filter,
#      asserting it is RED (exit != 0) with at least the pinned number of
#      failed assertions (manifest red_failed_asserts_min) AND, where the
#      manifest pins one, a RED signature: a regex that must match the doctest
#      ERROR lines, so the guard is pinned to WHICH assertion fails and not
#      merely how many. A count floor equal to the default of 1 admits any RED
#      whatsoever (a FATAL REQUIRE reports exactly one failed assertion), so
#      guards whose lethal fault kills a REQUIRE pin the signature instead;
#      capturing EXACT case/assertion counts;
#   5. restores the production file byte-for-byte and asserts GREEN on the same
#      filter after a rebuild;
#   6. records, per guard, the commit sha actually under test (git HEAD at run
#      time) as the lethality-established commit.
#
# Usage (run from the repository root, Windows PowerShell):
#   powershell -File .\run_kill_set.ps1                  # all guards
#   powershell -File .\run_kill_set.ps1 -Guard route-seam
#   powershell -File .\run_kill_set.ps1 -ProveContentAlignment -Guard route-seam
# The parameter block is [CmdletBinding()], so an unknown or misspelled switch
# is a hard binding error. That matters: without it PowerShell silently ignores
# the unknown name and falls through to the DEFAULT path, which is the full
# seven-guard run that patches production files — a typo would turn a read-only
# proof into a long destructive run.
# Exit 0 when every guard discriminates; 1 on any failure. Generated
# artifacts (report.tsv, build logs) are written under kill_set/ and are
# .gitignore`d; the tree is left clean of generated output.

[CmdletBinding()]
param(
    [string]$Guard = "",
    [string]$Config = "Release",
    # Prove the content-anchoring guard: run ONE guard's real fault through the
    # same Apply-Content routine but against a deliberately corrupted before
    # text; the routine must abort loudly and leave the file untouched.
    [switch]$ProveContentAlignment
)

$ErrorActionPreference = "Stop"

# MSBuild's in-proc node spawns SetEnvironmentVariable with the inherited
# environment; a sandbox-injected oversized variable (e.g. OPENCODE_CONFIG_CONTENT)
# exceeds the 32KB limit and crashes MSBuild at node shutdown. Strip it here so
# the kill-set builds are reproducible outside the interactive shell too.
Remove-Item Env:OPENCODE_CONFIG_CONTENT -ErrorAction SilentlyContinue
$env:MSBUILDDISABLENODEREUSE = "1"

$root = (Resolve-Path ".").Path
$manifestPath = Join-Path $root "kill_set\manifest.tsv"
$guardRoot = Join-Path $root "kill_set\guards"
$outDir = Join-Path $root "kill_set\output"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$reportPath = Join-Path $outDir "report.tsv"
$msbuild = Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msbuild)) { Write-Host "MSBuild not found at $msbuild" -ForegroundColor Red; exit 1 }

$tests = Join-Path $root ("bin\{0}-windows-x86_64\RT2Tests\RT2Tests.exe" -f $Config)
if (-not (Test-Path $tests)) { Write-Host "Tests binary not found at $tests; run the solution build first" -ForegroundColor Red; exit 1 }

function Resolve-Anchor {
    # Payloads are stored with LF endings. Convert before/after to the target
    # file's own newline convention so CRLF and LF sources behave identically
    # and the patched region keeps the surrounding file's endings untouched.
    param([string[]]$content, [string]$nl)
    return $content -replace "`r`n", "`n" -replace "`n", $nl
}

# The single content-anchoring routine. Applies every (before, after) pair to
# $content, aborting loudly on anything unexpected. Returns the new content.
function Invoke-ContentRound {
    param(
        [string]$content,
        [string]$before,
        [string]$after,
        [string]$nl,
        [string]$guardName,
        [string]$stepName
    )
    $b = Resolve-Anchor -content @($before) -nl $nl
    $a = Resolve-Anchor -content @($after) -nl $nl
    if ($a -eq $b) { throw "step $stepName after == before (no-op fault)" }
    $count = ([regex]::Matches($content, [regex]::Escape($b))).Count
    if ($count -ne 1) {
        throw "guard ${guardName} step ${stepName}: before anchor matched $count times (expected exactly 1)."
    }
    return $content.Replace($b, $a)
}

function Invoke-Build {
    param([string]$logSuffix)
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $log = Join-Path $outDir "build-$logSuffix.log"
    & $msbuild (Join-Path $root "RT2App.sln") "-p:Configuration=$Config" "-p:Platform=x64" -m:1 -t:RT2Tests -v:minimal 2>&1 | Out-File -FilePath $log -Encoding utf8
    $code = $LASTEXITCODE
    $ErrorActionPreference = $saved
    if ($code -ne 0) {
        Write-Host "BUILD FAILED (exit $code; see $log)" -ForegroundColor Red
        Get-Content $log | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
        return $false
    }
    return $true
}

function Run-Filter {
    param([string]$filter)
    $out = @(& $tests "--test-case=$filter" 2>&1)
    $exit = $LASTEXITCODE
    $casesLine = $out | Where-Object { $_ -match '^\[doctest\] test cases:' } | Select-Object -First 1
    $assertLine = $out | Where-Object { $_ -match '^\[doctest\] assertions:' } | Select-Object -First 1
    if (-not $casesLine) { $casesLine = $out | Where-Object { $_ -match 'test cases:' } | Select-Object -First 1 }
    $failedAsserts = 0
    if ($assertLine -match '(\d+)\s+failed') { [int]$failedAsserts = $Matches[1] }
    # The doctest ERROR lines name the exact assertion that died. This is the
    # signature a guard is pinned to when its failed-assertion count cannot
    # discriminate (see red_expected_signature in kill_set/manifest.tsv).
    $errorLines = @($out | Where-Object { $_ -match 'ERROR:' })
    return [pscustomobject]@{
        ExitCode = [int]$exit
        CasesLine = $casesLine
        AssertLine = $assertLine
        FailedAsserts = $failedAsserts
        ErrorLines = $errorLines
        Signature = ($errorLines -join "`n")
        Output = ($out -join "`n")
    }
}

$guards = Import-Csv -Delimiter "`t" -Path $manifestPath
if ($Guard) { $guards = $guards | Where-Object { $_.guard -eq $Guard } }

$report = New-Object System.Collections.Generic.List[string]
$report.Add("guard`tcommit`tred_cases`tred_asserts_failed`tred_signature_pin`tgreen_cases`tgreen_asserts_failed`tresult")

foreach ($g in $guards) {
    $guardName = $g.guard
    $prodFile = Join-Path $root $g.production_file
    $nl = if ([System.IO.File]::ReadAllText($prodFile).Contains("`r`n")) { "`r`n" } else { "`n" }
    Write-Host ""
    Write-Host "==== $guardName ====" -ForegroundColor Cyan
    Write-Host "anchor: $($g.anchor)"

    $dirty = git -C $root status --porcelain -- $g.production_file
    if ($dirty) {
        Write-Host "ABORT: production file '$($g.production_file)' is dirty; kill-set requires a pristine tree." -ForegroundColor Red
        exit 1
    }

    $guardDir = Join-Path $guardRoot $guardName
    if (-not (Test-Path $guardDir)) { Write-Host "ABORT: no guard dir $guardDir" -ForegroundColor Red; exit 1 }
    $steps = Get-ChildItem $guardDir -Filter "step*.before.txt" | Sort-Object Name
    if (-not $steps) { Write-Host "ABORT: no step files for $guardName" -ForegroundColor Red; exit 1 }

    $original = [System.IO.File]::ReadAllText($prodFile)

    # ---- ProveAnchorAlignment: run the REAL content routine with a corrupted
    # anchor. The corruption is a byte mutation inside the stored before text
    # (not a suffix append), so the routine must fail its at-most-once check
    # and abort before any patch or build.
    if ($ProveContentAlignment) {
        $step0 = $steps[0]
        $afterPath = Join-Path $guardDir ($step0.Name -replace '\.before\.txt$', '.after.txt')
        if (-not (Test-Path $afterPath)) { Write-Host "ABORT: no after for $($step0.Name)" -ForegroundColor Red; exit 1 }
        $goodText = [System.IO.File]::ReadAllText($step0.FullName)
        $corrupt = $goodText
        if ($corrupt.Length -gt 4) {
            $pos = 2
            if ($corrupt[$pos] -eq "`t") { $pos = 1 }
            $corrupt = $corrupt.Remove($pos, 1).Insert($pos, "Z")
        } else { $corrupt = $corrupt + "X" }
        $after = [System.IO.File]::ReadAllText($afterPath)
        $aborted = $false
        try {
            $outContent = Invoke-ContentRound -content $original -before $corrupt -after $after -nl $nl -guardName $guardName -stepName "corrupted-probe"
            Write-Host "FAIL: corrupted anchor was patched (no abort)" -ForegroundColor Red
            exit 1
        } catch {
            $aborted = $true
            Write-Host ("Anchor corruption detected: {0}" -f $_.Exception.Message) -ForegroundColor Yellow
        }
        if ($aborted) {
            $check = git -C $root status --porcelain -- $g.production_file
            if ($check) { Write-Host "FAIL: production file mutated during corrupt probe" -ForegroundColor Red; exit 1 }
            Write-Host "ProveContentAlignment: OK - real routine aborted loudly, file untouched." -ForegroundColor Green
            exit 0
        }
        exit 1
    }

    # ---- real path: apply every step through the SAME content routine
    $newContent = $original
    foreach ($step in $steps) {
        $before = [System.IO.File]::ReadAllText($step.FullName)
        $afterPath = Join-Path $guardDir ($step.Name -replace '\.before\.txt$', '.after.txt')
        if (-not (Test-Path $afterPath)) { Write-Host "ABORT: no after for $($step.Name)" -ForegroundColor Red; exit 1 }
        $after = [System.IO.File]::ReadAllText($afterPath)
        $newContent = Invoke-ContentRound -content $newContent -before $before -after $after -nl $nl -guardName $guardName -stepName $step.Name
    }
    if ($newContent -eq $original) {
        Write-Host "ABORT: guard $guardName produced no file change" -ForegroundColor Red
        exit 1
    }
    [System.IO.File]::WriteAllText($prodFile, $newContent)
    Write-Host "fault applied (content-anchored, $($steps.Count) step(s))."

    # ---- RED
    if (-not (Invoke-Build "fault-$guardName")) {
        [System.IO.File]::WriteAllText($prodFile, $original)
        exit 1
    }
    $red = Run-Filter $g.expected_red_filter
    Write-Host "RED run: exit=$($red.ExitCode)"
    Write-Host "  $($red.CasesLine)"
    Write-Host "  $($red.AssertLine)"
    foreach ($e in $red.ErrorLines) { Write-Host "  signature: $e" -ForegroundColor DarkYellow }
    $minExpected = 1
    if ($g.red_failed_asserts_min) { $minExpected = [int]$g.red_failed_asserts_min }
    if ($red.ExitCode -eq 0) {
        Write-Host "FAIL: filter expected RED but suite stayed GREEN under guard $guardName" -ForegroundColor Red
        [System.IO.File]::WriteAllText($prodFile, $original)
        exit 1
    }
    if ($red.FailedAsserts -lt $minExpected) {
        Write-Host "FAIL: RED but only $($red.FailedAsserts) failed assertions; manifest pins >= $minExpected (wrong reason)" -ForegroundColor Red
        [System.IO.File]::WriteAllText($prodFile, $original)
        exit 1
    }
    # Signature pin. Every guard must state a value: a regex, or the literal '-'
    # meaning "count floor is the discriminator here". A missing column is a
    # manifest error, not a silent pass — that is exactly the failure mode this
    # check exists to remove.
    $sigPin = $g.red_expected_signature
    if ($null -eq $sigPin -or $sigPin -eq "") {
        Write-Host "FAIL: manifest guard $guardName has no red_expected_signature value (use '-' to state it is unpinned)" -ForegroundColor Red
        [System.IO.File]::WriteAllText($prodFile, $original)
        exit 1
    }
    if ($sigPin -eq "-") {
        if ($minExpected -le 1) {
            Write-Host "FAIL: guard $guardName pins neither a signature nor a discriminating count floor (red_failed_asserts_min=$minExpected is the default and admits any RED)" -ForegroundColor Red
            [System.IO.File]::WriteAllText($prodFile, $original)
            exit 1
        }
        Write-Host "  signature: not pinned; discrimination rests on the count floor >= $minExpected"
    }
    elseif ($red.Signature -notmatch $sigPin) {
        Write-Host "FAIL: RED for the wrong reason; no doctest ERROR line matched the pinned signature /$sigPin/" -ForegroundColor Red
        [System.IO.File]::WriteAllText($prodFile, $original)
        exit 1
    }

    # ---- restore + GREEN
    [System.IO.File]::WriteAllText($prodFile, $original)
    $restored = [System.IO.File]::ReadAllText($prodFile)
    if ($restored -ne $original) { Write-Host "FAIL: restore was not byte-for-byte" -ForegroundColor Red; exit 1 }
    $diff = git -C $root status --porcelain -- $g.production_file
    if ($diff) { Write-Host "FAIL: production file still differs from HEAD after restore" -ForegroundColor Red; exit 1 }
    if (-not (Invoke-Build "green-$guardName")) { exit 1 }
    $green = Run-Filter $g.expected_red_filter
    Write-Host "GREEN run: exit=$($green.ExitCode)"
    Write-Host "  $($green.CasesLine)"
    Write-Host "  $($green.AssertLine)"
    if ($green.ExitCode -ne 0) { Write-Host "FAIL: filter expected GREEN after restore but suite stayed RED" -ForegroundColor Red; exit 1 }

    $commit = git -C $root rev-parse HEAD
    $report.Add(("{0}`t{1}`t{2}`t{3}`t{4}`t{5}`t{6}`tPASS" -f $guardName, $commit,
        $red.CasesLine, $red.FailedAsserts, $sigPin, $green.CasesLine, $green.FailedAsserts))
    Write-Host ("guard {0}: PASS (lethal at {1})" -f $guardName, $commit) -ForegroundColor Green
}

[System.IO.File]::WriteAllText($reportPath, ($report -join "`n"))
Write-Host ""
Write-Host "Kill-set complete. Report: $reportPath" -ForegroundColor Green
exit 0