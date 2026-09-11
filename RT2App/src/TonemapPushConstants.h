#pragma once

// ============================================================================
// TonemapPushConstants — typed tone-map push-constant transport.
//
// CPU-only (links into RT2Tests/RT2SliceRunner): no Vulkan, ImGui or Walnut
// dependency. The GPU twin is the push-constant block in
// shaders/tonemap_shared.glsl; the two layouts must match exactly and are
// pinned by the static_asserts below.
//
// Layout (16 bytes, offsets 0/4/8/12):
//   int32  toneOperator       — ToneMapOperator numbering (0/1/2); never
//                               renumber (see CameraPresentation.h).
//   float  exposureMultiplier — exp2(exposureEV), precomputed on the CPU so
//                               both paths scale by the same float.
//   int32  legacyDiagnostic   — 0 for beauty views (camera presentation);
//                               1 for data-oriented debug views, which retain
//                               their current Reinhard-at-0EV diagnostic
//                               mapping and bypass the camera look.
//   float  padding            — reserved, always 0.
// ============================================================================

#include "CameraPresentation.h"

#include <cstddef>
#include <cstdint>

struct TonemapPushConstants
{
    int32_t toneOperator = 0;
    float exposureMultiplier = 1.0f;
    int32_t legacyDiagnostic = 0;
    float padding = 0.0f;
};

static_assert(sizeof(TonemapPushConstants) == 16,
              "TonemapPushConstants must stay one 16-byte push-constant block");
static_assert(offsetof(TonemapPushConstants, toneOperator) == 0,
              "toneOperator must sit at push-constant offset 0");
static_assert(offsetof(TonemapPushConstants, exposureMultiplier) == 4,
              "exposureMultiplier must sit at push-constant offset 4");
static_assert(offsetof(TonemapPushConstants, legacyDiagnostic) == 8,
              "legacyDiagnostic must sit at push-constant offset 8");
static_assert(offsetof(TonemapPushConstants, padding) == 12,
              "padding must sit at push-constant offset 12");

// Data-oriented debug views (gbufferDebugMode >= 0; -1 is the beauty off
// value) keep their current diagnostic mapping: legacy per-channel Reinhard
// at 0 EV, independent of the camera. Beauty views use the presentation.
inline bool IsTonemapDiagnosticView(int gbufferDebugMode)
{
    return gbufferDebugMode >= 0;
}

// Resolve the immutable per-frame push constants from a validated camera
// presentation. Returns false (leaving `out` untouched) on invalid input;
// callers fail loudly rather than submitting a speculative look.
inline bool TryBuildTonemapPushConstants(const CameraPresentation& presentation,
                                         bool diagnosticView,
                                         TonemapPushConstants& out)
{
    if (diagnosticView)
    {
        TonemapPushConstants pc;
        pc.toneOperator = static_cast<int32_t>(ToneMapOperator::Reinhard);
        pc.exposureMultiplier = 1.0f;
        pc.legacyDiagnostic = 1;
        out = pc;
        return true;
    }
    CameraPresentation canonical = presentation;
    if (!TryCanonicalizeCameraPresentation(canonical))
        return false;
    TonemapPushConstants pc;
    pc.toneOperator = static_cast<int32_t>(canonical.toneMap);
    pc.exposureMultiplier = CameraExposureMultiplier(canonical.exposureEV);
    if (!std::isfinite(pc.exposureMultiplier) || pc.exposureMultiplier <= 0.0f)
        return false;
    pc.legacyDiagnostic = 0;
    out = pc;
    return true;
}
