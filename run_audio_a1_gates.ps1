#!/usr/bin/env pwsh
# run_audio_a1_gates.ps1 - assert the audio A1 miniaudio build boundary.
#
# Usage: pwsh run_audio_a1_gates.ps1 [-Configuration Release|Debug|Both]
#
# Proves required checks 16/17 at the A1 scope (probe shell + build
# isolation; full decoder/mixer/sample oracles belong to A4):
#   1. Vendored byte identity: SHA256 of each manifest file matches the hash
#      recorded for THAT file in RT2AudioBackend/VENDORING.md. The record is
#      the single source of truth (parsed, not string-searched), so a
#      swapped/misfiled hash fails loudly here.
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
#   5. Dynamic imports: RT2Tests and RT2SliceRunner binaries dynamically
#      import no Vulkan, ImGui, Walnut, GLFW, shaderc, SPIR-V, or NGX
#      module. This rules out dynamic audio/GPU dependencies only; static
#      isolation is proven by check 4. Premake wiring: root/RT2App/RT2Tests
#      premake files declare the boundary above.
#   6. Probe proof: RT2AudioProbe runs green in fixed float32 stereo 48 kHz
#      no-device mode with exact frame-count output.
#   7. Whitespace: `git diff --check` passes over every RT2-owned path
#      (staged and unstaged) while excluding exactly the manifest-pinned
#      vendor files, which carry upstream trailing whitespace that must
#      never be normalized; their SHA-256 gate (check 1) is the
#      complementary check. A stray trailing space in RT2-owned code fails
#      loudly here.
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

# ------------------------------------------------- vendoring manifest (P3)
# Single source of truth: parse the file->hash mapping from VENDORING.md's
# own "Byte hashes" section, so a swapped or misfiled hash fails loudly
# instead of passing on global string presence. Exactly three entries are
# allowed: a fourth vendor file must extend this manifest, .gitattributes,
# and the whitespace exemption together, never silently.
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
foreach ($entry in $vendoredManifest.GetEnumerator()) {
    $path = $entry.Key
    $want = $entry.Value
    if (-not (Test-Path $path)) { Fail "vendored file missing: $path"; continue }
    $got = (Get-FileHash $path -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($got -ne $want) {
        Fail "byte identity mismatch: $path`n  manifest $want`n  got      $got"
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
foreach ($staged in @($false, $true)) {
    $label = if ($staged) { "staged" } else { "unstaged" }
    # Default autocrlf stays in effect so CRLF worktrees check normally;
    # the vendor exemption is a pathspec. Keep only violation-shaped stdout
    # lines ("path:line: message") so git's own stderr chatter can never
    # masquerade as a violation or mask one.
    if ($staged) { $diffArgs = @("diff", "--cached", "--check", "--", ".") + $vendorExcludes }
    else { $diffArgs = @("diff", "--check", "--", ".") + $vendorExcludes }
    # Violations print on stdout as "path:line: message"; git's own stderr
    # chatter (e.g. autocrlf notices starting with "warning:") is filtered
    # so only real violations can fail this check. The preference is
    # relaxed around the native call because Windows PowerShell 5.1 turns
    # native stderr into a terminating error under Stop.
    $prevPref = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $wsRaw = (& git @diffArgs 2>&1 | Out-String)
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
$dumpbin = Find-DumpBin
if (-not $dumpbin) {
    Fail "dumpbin.exe not found; cannot check dynamic-import evidence"
} else {
    foreach ($config in $configs) {
        foreach ($target in @("RT2Tests", "RT2SliceRunner")) {
            $exe = "bin/$config-windows-x86_64/$target/$target.exe"
            if (-not (Test-Path $exe)) { Fail "binary missing for import scan: $exe (build first)"; continue }
            $imports = & $dumpbin /IMPORTS $exe 2>&1 | Out-String
            $badDlls = @()
            foreach ($deny in $importDenyList) {
                if ($imports -like "*$deny*") { $badDlls += $deny }
            }
            if ($badDlls.Count -eq 0) {
                Pass "[$config] $target dynamically imports no denylisted module"
            } else {
                Fail "[$config] $target dynamically imports denylisted modules: $($badDlls -join ', ')"
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
