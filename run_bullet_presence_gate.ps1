#!/usr/bin/env pwsh
# run_bullet_presence_gate.ps1 - assert the named T1/T2/T3 Bullet cases exist
# in the built RT2Tests executables.
#
# Usage: pwsh run_bullet_presence_gate.ps1 [-Configuration Release|Debug|Both]
#
# A zero exit from RT2Tests proves nothing about T1/T2/T3 when the tracked
# Visual Studio projects silently omit their translation units (a 1218-case
# false green passed while exercising none of the new tests). This gate
# enumerates --list-test-cases and requires every named T1/T2/T3 case, so a
# build that drops them fails loudly instead of passing silently.
#
# Exits 0 when every named case is present in every checked binary, 1
# otherwise. Must run from the repository root (AGENTS.md).

param(
    [ValidateSet("Release", "Debug", "Both")]
    [string]$Configuration = "Both"
)

$ErrorActionPreference = "Stop"

$requiredCases = @(
    # T1: pinned Bullet core vendoring + build-isolation smoke tests.
    "T1 RED_NoVulkanInPhysicsIncludes: physics test unit stays CPU-only",
    "T1 pin identity: vendored Bullet reads back as 3.25 @ 2c204c49 with zlib bytes",
    "T1 smoke: fast CCD sphere stops at a thin wall while discrete tunnels",
    "T1 smoke: motorized hinge flipper reaches its limit without pivot drift",
    "T1 smoke: driven slider plunger launches its ball",
    "T1 smoke: static triangle-mesh ramp deflects a falling ball",
    "T1 smoke: ghost trigger overlaps without blocking",
    # T2: physics persistence foundation.
    "T2 GREEN_V8Roundtrip: all four physics components survive save and load exactly",
    "T2 GREEN_V3V7Migration: scenes without physics load as no-body with core data intact",
    "T2 GREEN_PhysicsCodecCoverage: all persisted components survive every manual codec",
    "T2 GREEN_ConstraintUuidRemapInternal: duplicate rebases intra-copy otherBody refs",
    "T2 GREEN_ExternalOtherBodyPreserved: duplicate keeps external refs and world anchors",
    "T2 paste preserves physics and rebases internal otherBody refs",
    "T2 prefab instantiate preserves physics and remaps template-internal refs",
    "T2 RED_BadConstraintUuidRefused: dangling, self, and missing-owner refs fail loudly",
    "T2 physics prefab wires are non-overridable and carry no propagation adapter",
    "T2 scene load rejects a prefab override naming a physics wire",
    "T2 malformed v8 physics blocks fail loudly with entity identity",
    "T2 nested asset fields and float overflow fail loudly with wire path",
    "T2 both persisted collision refs are visited unconditionally",
    "T2 save rejects a physics ref with a path but no asset kind",
    # T3: physics world lifecycle (candidate-commit Play, fixed-step, teardown).
    "T3 GREEN_EmptyPlayHasZeroHandles: empty Play commits an empty world and steps silently",
    "T3 GREEN_StepIsOneTick: paused Step advances exactly one kFixedDt tick",
    "T3 GREEN_StopZeroHandles: repeated Play/Stop cycles leave zero handles",
    "T3 GREEN_StopRestoresAuthoring: Play/Stop cycles leave authoring bytes identical",
    "T3 GREEN_NonIdentityTransformRefreshed: runtime worldMatrix matches non-identity authoring",
    "T3 GREEN_ProviderPresentLetsPlayProceed: collision refs with a provider Play clean",
    "T3 RED_MotionPlusBodyRefused: MotionComponent plus physics body refuses Play",
    "T3 RED_ParentedBodyRefused: parented physics body refuses Play",
    "T3 RED_DynamicTriMeshRefused: dynamic triangle mesh refuses Play",
    "T3 RED_BadLayerMaskRefused: non-single layer or bad mask refuses Play",
    "T3 RED_TriggerLayerMismatchRefused: trigger shapes and Trigger layer must agree",
    "T3 GREEN_LayerMaskPolicyAccepted: settled single-layer policy Plays clean",
    "T3 RED_MissingEntityIdRefused: physics components without authored IDs refuse Play",
    "T3 RED_BadScaleRefused: non-uniform or non-positive scale refuses Play",
    "T3 RED_MissingCollisionProviderRefusesPlay: refs without a provider refuse Play",
    "T3 RED_BadConstraintIdentityRefusesPlay: malformed constraint identities refuse Play",
    "T3 RED_PhysicsPlayConstructionIsAtomic: late candidate failure leaves zero observable mutation"
)

$configs = if ($Configuration -eq "Both") { @("Release", "Debug") } else { @($Configuration) }

$failed = 0
foreach ($config in $configs) {
    $exe = "bin\$config-windows-x86_64\RT2Tests\RT2Tests.exe"
    if (-not (Test-Path $exe)) {
        Write-Host "[$config] FAIL: test binary not found at $exe" -ForegroundColor Red
        $failed++
        continue
    }
    $listing = & $exe --list-test-cases 2>&1 | Out-String
    $missing = @()
    foreach ($case in $requiredCases) {
        if (-not $listing.Contains($case)) {
            $missing += $case
        }
    }
    if ($missing.Count -gt 0) {
        Write-Host "[$config] FAIL: $($missing.Count) named T1/T2/T3 cases absent from $exe" -ForegroundColor Red
        foreach ($case in $missing) {
            Write-Host "  missing: $case" -ForegroundColor Red
        }
        $failed++
    } else {
        Write-Host "[$config] PASS: all $($requiredCases.Count) named T1/T2/T3 cases present" -ForegroundColor Green
    }
}

if ($failed -gt 0) { exit 1 } else { exit 0 }
