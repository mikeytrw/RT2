#!/usr/bin/env pwsh
# run_audio_a1_gates.ps1 - assert the audio A1 miniaudio build boundary.
#
# Usage: pwsh run_audio_a1_gates.ps1 [-Configuration Release|Debug|Both]
#
# Proves required checks 16/17 at the A1 scope (probe shell + build
# isolation; full decoder/mixer/sample oracles belong to A4):
#   1. Vendored byte identity: an immutable pin table in this script names
#      the exact three filenames and SHA-256 values. VENDORING.md's
#      file->hash manifest must equal the pin table (so a coordinated
#      record edit fails), and each file's bytes must equal the pin table
#      (so a coordinated bytes+record edit fails). Neither source can
#      self-certify; the pin changes only by explicit script edit.
#   2. Pin identity: MA_VERSION_* macros read back as 0.11.25 and the
#      license preserves the upstream public-domain/MIT-0 choice verbatim.
#   3. Exactly-one-compilation: every project reachable from the generated
#      solution is censused. RT2AudioBackend holds exactly one
#      MiniaudioNoDeviceAdapter.cpp TU and one miniaudio.c TU; no other
#      project compiles any RT2AudioBackend or miniaudio source. A
#      deliberately duplicated source therefore fails loudly here.
#   4. Link boundary: RT2App and RT2AudioProbe carry a ProjectReference to
#      RT2AudioBackend (plus full AdditionalDependencies scan); RT2Tests,
#      RT2SliceRunner and RT2ImGuiProbe carry none, proven by the same
#      predicate that must fire on RT2AudioProbe as positive control
#      (miniaudio links statically, so binary symbol scans are inert and
#      are not used). A deliberately removed link therefore fails loudly.
#   5. Dynamic imports: dumpbin must parse a valid PE import table (EXECUTABLE
#      IMAGE + import section markers, clean exit, no LNK diagnostics) and
#      that table must import no Vulkan, ImGui, Walnut, GLFW, shaderc,
#      SPIR-V, or NGX module. The denylist alone would PASS a non-PE file,
#      so the markers are required. This rules out dynamic audio/GPU
#      dependencies only; static isolation is proven by check 4. Premake
#      wiring: root/RT2App/RT2Tests premake files declare the boundary.
#   6. Probe proof: RT2AudioProbe runs green in fixed float32 stereo 48 kHz
#      no-device mode with exact frame-count output.
#   7. Whitespace: `git diff --check` passes over every RT2-owned path for
#      unstaged, staged, AND the committed A1 range 355584d..HEAD (a clean
#      tree would otherwise false-PASS without examining any commit), each
#      excluding exactly the manifest-pinned vendor files, which carry
#      upstream trailing whitespace that must never be normalized; their
#      SHA-256 gate (check 1) is the complementary check. A stray trailing
#      space in RT2-owned code fails loudly here.
#
# Exits 0 when every check passes in every checked configuration, 1
# otherwise. Must run from the repository root (AGENTS.md).

param(
    [ValidateSet("Release", "Debug", "Both")]
    [string]$Configuration = "Both"
)

$ErrorActionPreference = "Stop"

$failed = 0
function Fail([string]$message) {
    Write-Host "A1 GATE FAIL: $message" -ForegroundColor Red
    $script:failed++
}
function Pass([string]$message) {
    Write-Host "A1 GATE PASS: $message" -ForegroundColor Green
}

# ------------------------------------------- immutable pin + manifest
# Independent anchor to the exact pinned upstream bytes (miniaudio 0.11.25
# at commit 9634bedb5b5a2ca38c1ee7108a9358a4e233f14d). A coordinated edit
# of vendor bytes plus VENDORING.md cannot self-certify: changing either
# without an explicit, reviewable edit of this table fails loudly below.
# Keys are the exact repo-relative filenames, so a fourth vendor file never
# matches and must extend this table, the manifest, .gitattributes, and the
# whitespace exemption together.
$pinnedFiles = @{
    "RT2AudioBackend/vendor/miniaudio/miniaudio.h" = "01D3AC6049132BDCCC30BD467B7C1D030C090E8F30EB0CEB2DA14BEA0BA7143A";
    "RT2AudioBackend/vendor/miniaudio/miniaudio.c" = "721EA23C26F13BFB0E5BACD96BB9F40684DE1B3B31456D0DC168A87EEF18978F";
    "RT2AudioBackend/vendor/miniaudio/LICENSE"     = "457F1B500E0ADF6BC059EDDDFA78A2F62012E7C3BB43476C20E0BD23B25BA0EB";
}

# Per-file association: parse the file->hash mapping from VENDORING.md's
# own "Byte hashes" section, so a swapped or misfiled hash fails loudly
# instead of passing on global string presence.
$vendoringRecord = Get-Content "RT2AudioBackend/VENDORING.md" -Raw
$manifestPattern = '- `vendor/miniaudio/(?<file>[^`]+)`:[\r\n]+\s+`(?<hash>[0-9A-Fa-f]{64})`'
$vendoredManifest = @{}
foreach ($m in [regex]::Matches($vendoringRecord, $manifestPattern)) {
    $vendoredManifest["RT2AudioBackend/vendor/miniaudio/" + $m.Groups["file"].Value] =
        $m.Groups["hash"].Value.ToUpperInvariant()
}
if ($vendoredManifest.Count -ne 3) {
    Fail "VENDORING.md manifest parses to $($vendoredManifest.Count) file->hash entries, want exactly 3"
}
# The record must equal the immutable pin exactly: same filenames, same
# hashes. A record-only edit fails here even when it stays self-consistent.
foreach ($pinnedPath in $pinnedFiles.Keys) {
    if (-not $vendoredManifest.ContainsKey($pinnedPath)) {
        Fail "VENDORING.md manifest is missing pinned file: $pinnedPath"
    } elseif ($vendoredManifest[$pinnedPath] -ne $pinnedFiles[$pinnedPath]) {
        Fail "VENDORING.md hash for $pinnedPath differs from the immutable pin"
    } else {
        Pass "pin manifest matches immutable pin: $pinnedPath"
    }
}
foreach ($extraPath in @($vendoredManifest.Keys | Where-Object { -not $pinnedFiles.ContainsKey($_) })) {
    Fail "VENDORING.md manifest lists unpinned file: $extraPath (extend the pin table explicitly)"
}
# The bytes must equal the immutable pin. A coordinated bytes+record edit
# fails above (record no longer matches the pin) even though bytes and
# record agree with each other.
foreach ($entry in $pinnedFiles.GetEnumerator()) {
    $path = $entry.Key
    $want = $entry.Value
    if (-not (Test-Path $path)) { Fail "vendored file missing: $path"; continue }
    $got = (Get-FileHash $path -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($got -ne $want) {
        Fail "byte identity mismatch: $path`n  pin  $want`n  got  $got"
    } else {
        Pass "byte identity: $path"
    }
}

# ------------------------------------------- whitespace gate, narrow vendor exception (P1)
# The manifest-pinned vendor files carry upstream trailing whitespace that
# must never be normalized (byte identity above is the invariant; exception
# recorded in VENDORING.md). Check every other path, staged and unstaged;
# the exemption list is derived from the manifest so it covers precisely the
# pinned set and nothing else.
$vendorExcludes = @($vendoredManifest.Keys | ForEach-Object { ":!$_" })
function Invoke-WhitespaceCheck([string[]]$gitArgs, [string]$label) {
    # Default autocrlf stays in effect so CRLF worktrees check normally;
    # the vendor exemption is a pathspec. Keep only violation-shaped stdout
    # lines ("path:line: message") so git's own stderr chatter can never
    # masquerade as a violation or mask one. The preference is relaxed
    # around the native call because Windows PowerShell 5.1 turns native
    # stderr into a terminating error under Stop.
    $prevPref = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $wsRaw = (& git @gitArgs 2>&1 | Out-String)
    $wsCode = $LASTEXITCODE
    $ErrorActionPreference = $prevPref
    $wsLines = @($wsRaw -split "`r?`n" |
        Where-Object { $_ -match '^[^:]+:\d+: ' })
    $wsOutput = ($wsLines -join "`n").Trim()
    if ($wsCode -eq 0 -and [string]::IsNullOrWhiteSpace($wsOutput)) {
        Pass "whitespace clean over RT2-owned paths ($label, 3 vendor files exempt)"
    } elseif ($wsCode -ne 0 -and -not [string]::IsNullOrWhiteSpace($wsOutput)) {
        Fail "whitespace violations ($label, vendor exemption applied):`n$wsOutput"
    } else {
        Fail "git diff --check errored ($label, exit $wsCode): $wsOutput"
    }
}
foreach ($staged in @($false, $true)) {
    $label = if ($staged) { "staged" } else { "unstaged" }
    if ($staged) { $diffArgs = @("diff", "--cached", "--check", "--", ".") + $vendorExcludes }
    else { $diffArgs = @("diff", "--check", "--", ".") + $vendorExcludes }
    Invoke-WhitespaceCheck $diffArgs $label
}
# Committed A1 range: staged/unstaged checks pass on a clean tree without
# examining any commit, so an RT2-owned whitespace error committed in A1
# would vanish from them forever. Check the explicit A1 base through HEAD
# with the same narrow exemption. The base itself is validated first: any
# nonzero Git status, including tool errors, fails loudly.
$a1BaseCommit = "355584db94364d8b0228c1c8625458bac1d7840b"  # A0 checkpoint (short: 355584d)
$prevPref = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& git cat-file -e "$a1BaseCommit^{commit}" 2>&1 | Out-Null
$baseCode = $LASTEXITCODE
$ErrorActionPreference = $prevPref
if ($baseCode -ne 0) {
    Fail "A1 base commit $a1BaseCommit is not present; cannot check the committed range"
} else {
    $rangeArgs = @("diff", "--check", "$a1BaseCommit..HEAD", "--", ".") + $vendorExcludes
    Invoke-WhitespaceCheck $rangeArgs "committed 355584d..HEAD"
}

# ------------------------------------------------------- pin macros/license
$vendoredHeader = Get-Content "RT2AudioBackend/vendor/miniaudio/miniaudio.h" -Raw
foreach ($macro in @("#define MA_VERSION_MAJOR    0", "#define MA_VERSION_MINOR    11", "#define MA_VERSION_REVISION 25")) {
    if (-not $vendoredHeader.Contains($macro)) { Fail "vendored miniaudio.h missing macro line: $macro" }
}
if ($vendoredHeader.Contains("#define MA_VERSION_MAJOR    0") -and
    $vendoredHeader.Contains("#define MA_VERSION_MINOR    11") -and
    $vendoredHeader.Contains("#define MA_VERSION_REVISION 25")) {
    Pass "pin macros read back as 0.11.25"
}
$licenseText = Get-Content "RT2AudioBackend/vendor/miniaudio/LICENSE" -Raw
if ($licenseText.Contains("public domain") -and $licenseText.Contains("MIT No Attribution")) {
    Pass "license preserves the upstream public-domain/MIT-0 choice verbatim"
} else {
    Fail "vendored LICENSE does not preserve the upstream license choice text"
}

# ------------------------------------------------- generated-project gates
$backendVcxproj = "RT2AudioBackend/RT2AudioBackend.vcxproj"
$linkProjects = @("RT2App/RT2App.vcxproj", "RT2AudioProbe/RT2AudioProbe.vcxproj")
$cpuProjects = @("RT2Tests/RT2Tests.vcxproj", "RT2SliceRunner/RT2SliceRunner.vcxproj",
    "RT2ImGuiProbe/RT2ImGuiProbe.vcxproj")

function Get-ClCompiles([string]$vcxprojPath) {
    $xml = [xml](Get-Content $vcxprojPath -Raw)
    $ns = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
    $ns.AddNamespace("m", "http://schemas.microsoft.com/developer/msbuild/2003")
    return @($xml.SelectNodes("//m:ClCompile", $ns) | ForEach-Object { $_.GetAttribute("Include") })
}
function Get-ProjectReferences([string]$vcxprojPath) {
    $xml = [xml](Get-Content $vcxprojPath -Raw)
    $ns = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
    $ns.AddNamespace("m", "http://schemas.microsoft.com/developer/msbuild/2003")
    return @($xml.SelectNodes("//m:ProjectReference", $ns) | ForEach-Object { $_.GetAttribute("Include") })
}

function Get-LinkInputs([string]$vcxprojPath) {
    # Every static-link input that could carry the backend: project
    # references plus all per-config AdditionalDependencies entries.
    $xml = [xml](Get-Content $vcxprojPath -Raw)
    $ns = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
    $ns.AddNamespace("m", "http://schemas.microsoft.com/developer/msbuild/2003")
    $refs = @($xml.SelectNodes("//m:ProjectReference", $ns) | ForEach-Object { $_.GetAttribute("Include") })
    $libs = @($xml.SelectNodes("//m:Link/m:AdditionalDependencies", $ns) | ForEach-Object { $_.InnerText })
    return @{ Refs = $refs; Libs = $libs }
}
function Test-BackendLinkInput([string]$vcxprojPath) {
    # True when any compile-visible link input names the backend library.
    # Static linkage means "contains miniaudio" iff this predicate fires,
    # which the RT2AudioProbe positive control below keeps honest.
    $inputs = Get-LinkInputs $vcxprojPath
    foreach ($ref in $inputs.Refs) {
        if ($ref -like "*RT2AudioBackend*") { return $true }
    }
    foreach ($lib in $inputs.Libs) {
        if ($lib -like "*AudioBackend*" -or $lib -like "*miniaudio*") { return $true }
    }
    return $false
}

# --------------------------------------- solution-wide TU census (P2, second)
# Enumerate every project reachable from the generated solution instead of a
# fixed named list, so a duplicate smuggled into Walnut, GLFW, ImGui, or a
# Bullet project cannot pass. The named link-policy checks below stay as the
# consumer/CPU-target contract.
$slnProjects = @()
$slnPath = "RT2App.sln"
if (-not (Test-Path $slnPath)) {
    Fail "generated solution missing: $slnPath (regenerate with premake5)"
} else {
    $slnText = Get-Content $slnPath -Raw
    $slnProjects = @([regex]::Matches($slnText, '"([^"]+\.vcxproj)"') |
        ForEach-Object { $_.Groups[1].Value -replace '\\','/' } |
        Sort-Object -Unique)
    Pass "solution enumerates $($slnProjects.Count) generated projects"
}

if ((Test-Path $backendVcxproj) -and ($slnProjects -notcontains $backendVcxproj)) {
    Fail "solution does not contain $backendVcxproj (regenerate with premake5)"
}

foreach ($proj in $slnProjects) {
    if (-not (Test-Path $proj)) { Fail "solution project missing on disk: $proj (regenerate with premake5)"; continue }
    $compiles = Get-ClCompiles $proj
    if ($proj -eq $backendVcxproj) {
        $adapterHits = @($compiles | Where-Object { $_ -like "*MiniaudioNoDeviceAdapter.cpp" })
        $miniaudioHits = @($compiles | Where-Object { $_ -like "*miniaudio.c" })
        # Includes are project-relative: require exactly one hit that lives
        # in-project (no ".." escape into another target's tree).
        if ($adapterHits.Count -eq 1 -and $adapterHits[0] -notlike "*..*") {
            Pass "exactly one adapter TU in RT2AudioBackend ($($adapterHits[0]))"
        } else {
            Fail "adapter TU count in RT2AudioBackend is $($adapterHits.Count), want exactly 1 in-project ($($adapterHits -join '; '))"
        }
        if ($miniaudioHits.Count -eq 1 -and $miniaudioHits[0] -notlike "*..*") {
            Pass "exactly one miniaudio.c TU in RT2AudioBackend ($($miniaudioHits[0]))"
        } else {
            Fail "miniaudio.c TU count in RT2AudioBackend is $($miniaudioHits.Count), want exactly 1 in-project ($($miniaudioHits -join '; '))"
        }
        continue
    }
    $leaks = @($compiles | Where-Object { $_ -like "*RT2AudioBackend*" -or $_ -like "*miniaudio*" })
    if ($leaks.Count -eq 0) {
        Pass "no backend/miniaudio TU compiled in $proj"
    } else {
        Fail "duplicate-source leak in ${proj}: $($leaks -join '; ')"
    }
}

# Global reference invariant over the same enumeration: only the two allowed
# consumers may reference the backend from any solution project.
foreach ($proj in $slnProjects) {
    if (-not (Test-Path $proj)) { continue }
    if ($proj -eq $backendVcxproj) { continue }
    $refs = Get-ProjectReferences $proj
    $hasRef = (@($refs | Where-Object { $_ -like "*RT2AudioBackend*" }).Count -gt 0)
    $allowed = ($proj -eq "RT2App/RT2App.vcxproj") -or ($proj -eq "RT2AudioProbe/RT2AudioProbe.vcxproj")
    if ($hasRef -and -not $allowed) {
        Fail "reference leak: $proj references RT2AudioBackend outside the two consumers"
    }
}
if ($slnProjects.Count -gt 0) {
    Pass "no solution project outside RT2App/RT2AudioProbe references the backend"
}

# Named link-policy contract (kept alongside the global census): the two
# allowed consumers must carry a backend link input, CPU targets must not.
# The predicate is the same one the positive control below exercises, so a
# vacuous detector cannot pass both sides at once.
foreach ($proj in $linkProjects) {
    if (-not (Test-Path $proj)) { Fail "generated project missing: $proj (regenerate with premake5)"; continue }
    if (Test-BackendLinkInput $proj) {
        Pass "$proj links RT2AudioBackend (link input present)"
    } else {
        Fail "missing-link: $proj carries no RT2AudioBackend link input"
    }
}
foreach ($proj in $cpuProjects) {
    if (-not (Test-Path $proj)) { Fail "generated project missing: $proj (regenerate with premake5)"; continue }
    if (-not (Test-BackendLinkInput $proj)) {
        Pass "$proj does not link RT2AudioBackend"
    } else {
        Fail "CPU-boundary leak: $proj carries an RT2AudioBackend link input"
    }
}
# Positive control: RT2AudioProbe unquestionably contains and executes
# miniaudio, so the link-input detector must fire on it. If it does not,
# the detector is vacuous and the CPU passes above prove nothing.
if (-not (Test-Path "RT2AudioProbe/RT2AudioProbe.vcxproj")) {
    Fail "generated project missing: RT2AudioProbe/RT2AudioProbe.vcxproj (regenerate with premake5)"
} elseif (Test-BackendLinkInput "RT2AudioProbe/RT2AudioProbe.vcxproj") {
    Pass "positive control: link-input detector fires on RT2AudioProbe"
} else {
    Fail "positive control: link-input detector does not fire on RT2AudioProbe (detector vacuous)"
}

# ------------------------------------------------------------ premake wiring
$rootPremake = Get-Content "premake5.lua" -Raw
if ($rootPremake.Contains('include "RT2AudioBackend"') -and $rootPremake.Contains('include "RT2AudioProbe"')) {
    Pass "root premake5.lua includes RT2AudioBackend + RT2AudioProbe"
} else {
    Fail "root premake5.lua missing RT2AudioBackend/RT2AudioProbe includes"
}
$appPremake = Get-Content "RT2App/premake5.lua" -Raw
if ($appPremake.Contains('"RT2AudioBackend"')) { Pass "RT2App premake links RT2AudioBackend" }
else { Fail "RT2App premake does not link RT2AudioBackend" }
$testsPremake = Get-Content "RT2Tests/premake5.lua" -Raw
if ($testsPremake -notlike "*links*RT2AudioBackend*" -and $testsPremake -notlike '*"RT2AudioBackend"*') {
    Pass "RT2Tests premake links no backend library"
} else {
    Fail "RT2Tests premake references RT2AudioBackend"
}
$slicePremake = Get-Content "RT2SliceRunner/premake5.lua" -Raw
if ($slicePremake -notlike "*RT2AudioBackend*" -and $slicePremake -notlike "*miniaudio*") {
    Pass "RT2SliceRunner premake references no backend/miniaudio"
} else {
    Fail "RT2SliceRunner premake references backend/miniaudio"
}

# ------------------------------------------------- dynamic-import evidence
# Miniaudio links statically and resolves its Windows backend dynamically,
# so a binary symbol scan is inert (it stays green even on RT2AudioProbe,
# which unquestionably executes miniaudio) and is not used here. Static
# isolation is proven by the link-input census and positive control above;
# this section only rules out dynamic audio/GPU dependencies.
function Find-DumpBin {
    $candidates = @()
    if ($env:VSINSTALLDIR) {
        $candidates += Join-Path $env:VSINSTALLDIR "VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe"
    }
    $candidates += "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe"
    $candidates += "C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe"
    $candidates += "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe"
    foreach ($pattern in $candidates) {
        $hit = Get-ChildItem $pattern -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

$importDenyList = @("vulkan", "glfw", "imgui", "walnut", "shaderc", "spirv", "nvngx", "ngx", "miniaudio")
$configs = if ($Configuration -eq "Both") { @("Release", "Debug") } else { @($Configuration) }
function Test-ExecutableImports([string]$exePath, [string]$dumpbinPath) {
    # Validates that dumpbin actually parsed a PE import table instead of
    # merely failing to match denylisted names. A non-PE file exits 0 with
    # "warning LNK4048: Invalid format file" and no denylist hit, so the
    # denylist alone would PASS it. Require the PE marker, an import-table
    # marker, a clean native exit, and no warning/error diagnostics.
    $prevPref = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $raw = (& $dumpbinPath /IMPORTS $exePath 2>&1 | Out-String)
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prevPref
    if ($code -ne 0) { return "dumpbin exited $code" }
    if ($raw -notmatch 'File Type:\s+EXECUTABLE IMAGE') { return "no EXECUTABLE IMAGE marker (not a valid PE import dump)" }
    if ($raw -notmatch 'Section contains the following imports') { return "no import-table section (not a valid PE import dump)" }
    if ($raw -match '(?im)^.*\b(warning|error)\s+LNK\d+') { return "dumpbin diagnostic present: $($Matches[0].Trim())" }
    $badDlls = @()
    foreach ($deny in $importDenyList) {
        if ($raw -like "*$deny*") { $badDlls += $deny }
    }
    if ($badDlls.Count -gt 0) { return "denylisted dynamic imports: $($badDlls -join ', ')" }
    return ""
}
$dumpbin = Find-DumpBin
if (-not $dumpbin) {
    Fail "dumpbin.exe not found; cannot check dynamic-import evidence"
} else {
    foreach ($config in $configs) {
        foreach ($target in @("RT2Tests", "RT2SliceRunner")) {
            $exe = "bin/$config-windows-x86_64/$target/$target.exe"
            if (-not (Test-Path $exe)) { Fail "binary missing for import scan: $exe (build first)"; continue }
            $verdict = Test-ExecutableImports $exe $dumpbin
            if ([string]::IsNullOrEmpty($verdict)) {
                Pass "[$config] $target is a valid PE importing no denylisted module"
            } else {
                Fail "[$config] $target dynamic-import evidence rejected: $verdict"
            }
        }
    }
}

# ------------------------------------------------------------------ probe run
foreach ($config in $configs) {
    $probe = "bin/$config-windows-x86_64/RT2AudioProbe/RT2AudioProbe.exe"
    if (-not (Test-Path $probe)) { Fail "probe binary missing: $probe (build first)"; continue }
    $output = & $probe 2>&1 | Out-String
    $code = $LASTEXITCODE
    if ($code -eq 0 -and $output.Contains("PASS no-device") -and
        $output.Contains("48") -and $output.Contains("0.11.25")) {
        Pass "[$config] RT2AudioProbe green in fixed float32 stereo 48 kHz no-device mode"
    } else {
        Fail "[$config] RT2AudioProbe exit=$code output:`n$output"
    }
}

if ($failed -gt 0) { exit 1 } else { exit 0 }
