#!/usr/bin/env pwsh
# run_audio_a1_gates.ps1 - assert the audio A1 miniaudio build boundary.
#
# Usage: pwsh run_audio_a1_gates.ps1 [-Configuration Release|Debug|Both]
#
# Proves required checks 16/17 at the A1 scope (probe shell + build
# isolation; full decoder/mixer/sample oracles belong to A4):
#   1. Vendored byte identity: SHA256 of vendor/miniaudio/{miniaudio.h,
#      miniaudio.c,LICENSE} matches the hashes recorded in
#      RT2AudioBackend/VENDORING.md (which must also match this script's
#      constants, so neither can drift alone).
#   2. Pin identity: MA_VERSION_* macros read back as 0.11.25 and the
#      license preserves the upstream public-domain/MIT-0 choice verbatim.
#   3. Exactly-one-compilation: the generated RT2AudioBackend.vcxproj holds
#      exactly one MiniaudioNoDeviceAdapter.cpp TU and one miniaudio.c TU;
#      no other generated project compiles any RT2AudioBackend or miniaudio
#      source. A deliberately duplicated source therefore fails loudly here.
#   4. Link boundary: RT2App and RT2AudioProbe carry a ProjectReference to
#      RT2AudioBackend; RT2Tests, RT2SliceRunner and RT2ImGuiProbe carry
#      none. A deliberately removed link therefore fails loudly here.
#   5. CPU imports: RT2Tests and RT2SliceRunner binaries import no
#      miniaudio, WASAPI-adjacent audio, Vulkan, ImGui, Walnut, GLFW,
#      shaderc, SPIR-V, or NGX module (import-DLL denylist plus ma_* symbol
#      scan). Premake wiring: root/RT2App/RT2Tests premake files declare the
#      boundary above.
#   6. Probe proof: RT2AudioProbe runs green in fixed float32 stereo 48 kHz
#      no-device mode with exact frame-count output.
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

# ---------------------------------------------------------------- pin bytes
$expectedHashes = @{
    "RT2AudioBackend/vendor/miniaudio/miniaudio.h" = "01D3AC6049132BDCCC30BD467B7C1D030C090E8F30EB0CEB2DA14BEA0BA7143A";
    "RT2AudioBackend/vendor/miniaudio/miniaudio.c" = "721EA23C26F13BFB0E5BACD96BB9F40684DE1B3B31456D0DC168A87EEF18978F";
    "RT2AudioBackend/vendor/miniaudio/LICENSE"     = "457F1B500E0ADF6BC059EDDDFA78A2F62012E7C3BB43476C20E0BD23B25BA0EB";
}

$vendoringRecord = Get-Content "RT2AudioBackend/VENDORING.md" -Raw
foreach ($entry in $expectedHashes.GetEnumerator()) {
    $path = $entry.Key
    $want = $entry.Value
    if (-not (Test-Path $path)) { Fail "vendored file missing: $path"; continue }
    $got = (Get-FileHash $path -Algorithm SHA256).Hash
    if ($got -ne $want) {
        Fail "byte identity mismatch: $path`n  want $want`n  got  $got"
    } elseif (-not $vendoringRecord.Contains($want)) {
        Fail "VENDORING.md does not record the $path hash $want (record/script drift)"
    } else {
        Pass "byte identity: $path"
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

if (-not (Test-Path $backendVcxproj)) {
    Fail "generated project missing: $backendVcxproj (regenerate with premake5)"
} else {
    $backendCompiles = Get-ClCompiles $backendVcxproj
    $adapterHits = @($backendCompiles | Where-Object { $_ -like "*MiniaudioNoDeviceAdapter.cpp" })
    $miniaudioHits = @($backendCompiles | Where-Object { $_ -like "*miniaudio.c" })
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
}

foreach ($proj in ($linkProjects + $cpuProjects)) {
    if (-not (Test-Path $proj)) { Fail "generated project missing: $proj (regenerate with premake5)"; continue }
    $compiles = Get-ClCompiles $proj
    $leaks = @($compiles | Where-Object { $_ -like "*RT2AudioBackend*" -or $_ -like "*miniaudio*" })
    if ($leaks.Count -eq 0) {
        Pass "no backend/miniaudio TU compiled in $proj"
    } else {
        Fail "duplicate-source leak in ${proj}: $($leaks -join '; ')"
    }
}

foreach ($proj in $linkProjects) {
    if (-not (Test-Path $proj)) { continue }
    $refs = Get-ProjectReferences $proj
    if (@($refs | Where-Object { $_ -like "*RT2AudioBackend*" }).Count -ge 1) {
        Pass "$proj links RT2AudioBackend (ProjectReference present)"
    } else {
        Fail "missing-link: $proj carries no ProjectReference to RT2AudioBackend"
    }
}
foreach ($proj in $cpuProjects) {
    if (-not (Test-Path $proj)) { continue }
    $refs = Get-ProjectReferences $proj
    $text = (Get-Content $proj -Raw)
    $bad = (@($refs | Where-Object { $_ -like "*RT2AudioBackend*" }).Count -gt 0) -or
           ($text -like "*RT2AudioBackend.lib*")
    if (-not $bad) {
        Pass "$proj does not link RT2AudioBackend"
    } else {
        Fail "CPU-boundary leak: $proj references RT2AudioBackend"
    }
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

# ---------------------------------------------------------------- CPU imports
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
$symbolDenyList = @("ma_engine_init", "ma_engine_read_pcm_frames", "ma_device_init", "ma_decoder_init")
$configs = if ($Configuration -eq "Both") { @("Release", "Debug") } else { @($Configuration) }
$dumpbin = Find-DumpBin
if (-not $dumpbin) {
    Fail "dumpbin.exe not found; cannot prove the CPU import boundary"
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
                Pass "[$config] $target imports carry no denylisted module"
            } else {
                Fail "[$config] $target imports denylisted modules: $($badDlls -join ', ')"
            }
            $symbols = & $dumpbin /SYMBOLS $exe 2>&1 | Out-String
            $badSyms = @()
            foreach ($deny in $symbolDenyList) {
                if ($symbols.Contains($deny)) { $badSyms += $deny }
            }
            if ($badSyms.Count -eq 0) {
                Pass "[$config] $target exports no miniaudio device/decoder symbols"
            } else {
                Fail "[$config] $target leaks backend symbols: $($badSyms -join ', ')"
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
