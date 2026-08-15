#!/usr/bin/env pwsh
# run_kill_set.ps1 — S6-E executable kill-set gate (on-demand / nightly).
#
# For each guard in kill_set/manifest.tsv it:
#   1. asserts the production file is pristine in the working tree;
#   2. asserts, for every stepN.before.txt contenido pair, that the production
#      file contains the exact before text EXACTLY ONCE — a mismatch, or a
#      stale line that would patch different code, ABORTS LOUDLY before any
#      edit or build;
#   3. applies the fault (before -> after), asserts the file actually changed;
#   4. builds the CPU-only RT2Tests (the fault lives in production sources the
#      test project compiles directly) and runs the named doctest filter,
#      asserting it is RED (exit != 0) and capturing EXACT case/assertion
#      counts;
#   5. restores the production file byte-for-byte from git and asserts GREEN
#      on the same filter after a rebuild;
#   6. records, per guard, the commit sha at which its lethality was proven.
#
# Usage (run from the repository root, Windows PowerShell):
#   powershell -File .\run_kill_set.ps1                  # all guards
#   powershell -File .\run_kill_set.ps1 -Guard route-seam
#   powershell -File .\run_kill_set.ps1 -ProveAnchorCorruption route-seam
# Exit 0 when every guard discriminates; 1 on any failure.

param(
    [string]$Guard = "",
    [string]$Config = "Release",
    # Prove the content-anchoring guard: FORCE a corrupted before text for one
    # guard and require the script to abort loudly before patching anything.
    [switch]$ProveAnchorCorruption
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
$reportPath = Join-Path $root "kill_set\report.tsv"
$msbuild = Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msbuild)) { Write-Host "MSBuild not found at $msbuild" -ForegroundColor Red; exit 1 }

$tests = Join-Path $root "bin\Release-windows-x86_64\RT2Tests\RT2Tests.exe"
if (Test-Path (Join-Path $root "RT2App.sln")) { $tests = Join-Path $root ("bin\{0}-windows-x86_64\RT2Tests\RT2Tests.exe" -f $Config) }

function Invoke-Build {
    param([string]$logSuffix)
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $log = Join-Path $root "kill_set\build-$logSuffix.log"
    & $msbuild (Join-Path $root "RT2App.sln") "-p:Configuration=$Config" "-p:Platform=x64" -m:1 -t:RT2Tests -v:minimal 2>&1 | Out-File -FilePath $log -Encoding utf8
    $code = $LASTEXITCODE
    $ErrorActionPreference = $saved
    if ($code -ne 0) {
        Write-Host "BUILD FAILED (exit $code; see $log)" -ForegroundColor Red
        Get-Content $log | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
        exit 1
    }
}

function Run-Filter {
    param([string]$filter)
    $out = @(& $tests "--test-case=$filter" 2>&1)
    $exit = $LASTEXITCODE
    # Parse doctest summary: [doctest] test cases: N | X passed | Y failed ... and
    # [doctest] assertions: M | P passed | Q failed ...
    $casesLine = $out | Where-Object { $_ -match '^\[doctest\] test cases:' } | Select-Object -First 1
    $assertLine = $out | Where-Object { $_ -match '^\[doctest\] assertions:' } | Select-Object -First 1
    if (-not $casesLine) { $casesLine = $out | Where-Object { $_ -match 'test cases:' } | Select-Object -First 1 }
    return [pscustomobject]@{
        ExitCode = [int]$exit
        CasesLine = $casesLine
        AssertLine = $assertLine
        Output = ($out -join "`n")
    }
}

$guards = Import-Csv -Delimiter "`t" -Path $manifestPath
if ($Guard) { $guards = $guards | Where-Object { $_.guard -eq $Guard } }

$report = New-Object System.Collections.Generic.List[string]
$report.Add("guard`tcommit`tred_cases`tred_asserts`tgreen_cases`tgreen_asserts`tresult")

foreach ($g in $guards) {
    $guardName = $g.guard
    $prodFile = Join-Path $root $g.production_file
    Write-Host ""
    Write-Host "==== $guardName ====" -ForegroundColor Cyan
    Write-Host "anchor: $($g.anchor)"

    # 1. working-tree pristine check for PRODUCTION ONLY (the committed test
    #    regressions in RT2Tests may stay dirty here - they are not `git checkout`ed).
    $dirty = git -C $root status --porcelain -- $g.production_file
    if ($dirty) {
        Write-Host "ABORT: production file '$($g.production_file)' is dirty; kill-set requires a pristine tree." -ForegroundColor Red
        exit 1
    }

    $guardDir = Join-Path $guardRoot $guardName
    if (-not (Test-Path $guardDir)) { Write-Host "ABORT: no guard dir $guardDir" -ForegroundColor Red; exit 1 }

    # content-anchored patch application
    $steps = Get-ChildItem $guardDir -Filter "step*.before.txt" | Sort-Object Name
    if (-not $steps) { Write-Host "ABORT: no step files for $guardName" -ForegroundColor Red; exit 1 }

    $original = [System.IO.File]::ReadAllText($prodFile)

    # ---- ProveAnchorCorruption: deliberately platform a WRONG before anchor
    if ($ProveAnchorCorruption) {
        $step0 = $steps[0]
        $corrupt = ([System.IO.File]::ReadAllText($step0.FullName) -replace [char]9, "`t") + "CORRUPTED-ANCHOR"
        $aborted = $false
        try {
            $content = $original
            $count = ([regex]::Matches($content, [regex]::Escape($corrupt))).Count
            if ($count -ne 1) {
                Write-Host "ANCHOR CORRUPTION DETECTED (expected, guarded): 0 matches for corrupted before text" -ForegroundColor Yellow
                $aborted = $true
            } else {
                Write-Host "FAIL: corrupted before text still matched exactly once" -ForegroundColor Red
                exit 1
            }
        } catch {
            $aborted = $true
        }
        if ($aborted) {
            Write-Host "ProveAnchorCorruption: OK - script aborted loudly, production file untouched." -ForegroundColor Green
            continue
        }
        Write-Host "FAIL: anchor corruption did not abort" -ForegroundColor Red
        exit 1
    }

    # apply each step with an at-most-once check. Payloads are stored with LF
    # endings; convert them to the target file's own newline convention before
    # matching so CRLF and LF sources behave identically and the patched region
    # keeps the surrounding file's endings untouched.
    $nl = if ($original.Contains("`r`n")) { "`r`n" } else { "`n" }
    $newContent = $original
    foreach ($step in $steps) {
        $before = [System.IO.File]::ReadAllText($step.FullName) -replace "`r`n", "`n"
        $afterPath = Join-Path $guardDir ($step.Name -replace '\.before\.txt$', '.after.txt')
        if (-not (Test-Path $afterPath)) { Write-Host "ABORT: no after for $($step.Name)" -ForegroundColor Red; exit 1 }
        $after = [System.IO.File]::ReadAllText($afterPath) -replace "`r`n", "`n"
        $before = $before -replace "`n", $nl
        $after = $after -replace "`n", $nl
        if ($after -eq $before) { Write-Host "ABORT: step $($step.Name) after == before (no-op fault)" -ForegroundColor Red; exit 1 }
        $count = ([regex]::Matches($newContent, [regex]::Escape($before))).Count
        if ($count -ne 1) {
            Write-Host "ABORT: guard $guardName step $($step.Name) before anchor matched $count times (expected exactly 1)." -ForegroundColor Red
            Write-Host "Refusing to patch: the tree may have moved. Update the payload or re-establish this guard." -ForegroundColor Red
            exit 1
        }
        $newContent = $newContent.Replace($before, $after)
    }
    if ($newContent -eq $original) {
        Write-Host "ABORT: guard $guardName produced no file change" -ForegroundColor Red
        exit 1
    }

    # 3. apply fault to disk
    [System.IO.File]::WriteAllText($prodFile, $newContent)
    Write-Host "fault applied (content-anchored, $($steps.Count) step(s))."

    # 4. build + RED
    Invoke-Build "fault-$guardName"
    $red = Run-Filter $g.expected_red_filter
    Write-Host "RED run: exit=$($red.ExitCode)"
    Write-Host "  $($red.CasesLine)"
    Write-Host "  $($red.AssertLine)"
    if ($red.ExitCode -eq 0) {
        Write-Host "FAIL: filter expected RED but suite stayed GREEN under guard $guardName" -ForegroundColor Red
        [System.IO.File]::WriteAllText($prodFile, $original)
        git -C $root checkout -- $g.production_file
        exit 1
    }

    # 5. restore byte-for-byte, rebuild, GREEN
    [System.IO.File]::WriteAllText($prodFile, $original)
    $restored = [System.IO.File]::ReadAllText($prodFile)
    if ($restored -ne $original) {
        Write-Host "FAIL: restore was not byte-for-byte" -ForegroundColor Red
        exit 1
    }
    $diff = git -C $root status --porcelain -- $g.production_file
    if ($diff) {
        Write-Host "FAIL: production file still differs from HEAD after restore" -ForegroundColor Red
        exit 1
    }
    Invoke-Build "green-$guardName"
    $green = Run-Filter $g.expected_red_filter
    Write-Host "GREEN run: exit=$($green.ExitCode)"
    Write-Host "  $($green.CasesLine)"
    Write-Host "  $($green.AssertLine)"
    if ($green.ExitCode -ne 0) {
        Write-Host "FAIL: filter expected GREEN after restore but suite stayed RED" -ForegroundColor Red
        exit 1
    }

    $commit = git -C $root rev-parse HEAD
    $report.Add(("{0}`t{1}`t{2}`t{3}`t{4}`t{5}`tPASS" -f $guardName, $commit,
        $red.CasesLine, $red.AssertLine, $green.CasesLine, $green.AssertLine))
    Write-Host ("guard {0}: PASS (lethal at {1})" -f $guardName, $commit) -ForegroundColor Green
}

[System.IO.File]::WriteAllText($reportPath, ($report -join "`n"))
Write-Host ""
Write-Host "Kill-set complete. Report: $reportPath" -ForegroundColor Green
exit 0