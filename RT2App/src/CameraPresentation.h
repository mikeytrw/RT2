#pragma once

// ============================================================================
// CameraPresentation — camera-owned display look (tone operator + exposure).
//
// CPU-only vocabulary shared by CameraComponent, SceneCamera, EditorCameraPose
// and Camera. No Vulkan, ImGui or Walnut dependency so it links into RT2Tests
// and RT2SliceRunner.
//
// Contracts (see the camera-owned filmic tone mapping technical plan):
//   - Owner: each camera owns its presentation; the active Camera supplies it.
//   - Default: AgX, neutral look, 0 EV — including files lacking the fields.
//   - Serialized tokens are stable: "agx" | "aces" | "reinhard" (exact,
//     case-sensitive; never silently remap an unknown token to AgX).
//   - Valid EV is finite and within [-8,+8]; canonicalize negative zero.
//   - The integer operator numbering (0/1/2) is shared with the GPU
//     tone-map push constants (tonemap_shared.glsl). Do not renumber.
// ============================================================================

#include <cmath>
#include <string>

enum class ToneMapOperator : int
{
    AgX = 0,
    ACESFitted = 1,
    Reinhard = 2,
};

struct CameraPresentation
{
    ToneMapOperator toneMap = ToneMapOperator::AgX;
    float exposureEV = 0.0f;

    bool operator==(const CameraPresentation& other) const
    {
        return toneMap == other.toneMap && exposureEV == other.exposureEV;
    }
    bool operator!=(const CameraPresentation& other) const
    {
        return !(*this == other);
    }
};

inline constexpr float kMinCameraExposureEV = -8.0f;
inline constexpr float kMaxCameraExposureEV = 8.0f;

inline CameraPresentation DefaultCameraPresentation()
{
    return CameraPresentation{};
}

// Stable serialization token. Never-localized, never-renamed. Returns
// "unknown" for out-of-range input so an invalid enum cannot silently
// become AgX at a display boundary; callers must validate first.
inline const char* ToneMapOperatorName(ToneMapOperator op)
{
    switch (op)
    {
    case ToneMapOperator::AgX:      return "agx";
    case ToneMapOperator::ACESFitted: return "aces";
    case ToneMapOperator::Reinhard: return "reinhard";
    default:                       return "unknown";
    }
}

// Human-facing label for inspector/editor controls.
inline const char* ToneMapOperatorLabel(ToneMapOperator op)
{
    switch (op)
    {
    case ToneMapOperator::AgX:      return "AgX";
    case ToneMapOperator::ACESFitted: return "ACES Fitted";
    case ToneMapOperator::Reinhard: return "Reinhard (Legacy)";
    default:                       return "Unknown";
    }
}

// Exact, case-sensitive match. Unknown input is rejected (false), never
// defaulted — callers must fail loudly rather than silently adopt AgX.
inline bool TryParseToneMapOperator(const char* name, ToneMapOperator& out)
{
    if (!name)
        return false;
    const std::string token(name);
    if (token == "agx")      { out = ToneMapOperator::AgX; return true; }
    if (token == "aces")     { out = ToneMapOperator::ACESFitted; return true; }
    if (token == "reinhard") { out = ToneMapOperator::Reinhard; return true; }
    return false;
}

inline bool IsValidExposureEV(float ev)
{
    return std::isfinite(ev) && ev >= kMinCameraExposureEV && ev <= kMaxCameraExposureEV;
}

inline bool IsValidCameraPresentation(const CameraPresentation& value)
{
    return (value.toneMap == ToneMapOperator::AgX ||
            value.toneMap == ToneMapOperator::ACESFitted ||
            value.toneMap == ToneMapOperator::Reinhard) &&
           IsValidExposureEV(value.exposureEV);
}

// Canonical form: negative-zero EV becomes positive zero so commands,
// equality and prefab canonicalization cannot diverge on -0.0f. Note that
// IEEE (-0.0f == +0.0f) already compares equal; canonicalization keeps the
// stored bit pattern deterministic. (-0.0f + 0.0f is +0.0f, but the
// explicit branch below states the intent.)
inline CameraPresentation CanonicalCameraPresentation(CameraPresentation value)
{
    if (value.exposureEV == 0.0f)
        value.exposureEV = 0.0f; // +0.0f
    return value;
}

// Shared validator used by pose normalization, commands and CLI: rejects
// invalid input without mutating it, otherwise stores the canonical form.
inline bool TryCanonicalizeCameraPresentation(CameraPresentation& value)
{
    if (!IsValidCameraPresentation(value))
        return false;
    value = CanonicalCameraPresentation(value);
    return true;
}

// One-shot CLI seed overlay: each present flag replaces its own axis while
// absent flags keep the resolved camera value. Canonicalizes the result.
// Returns false (leaving `out` untouched) on invalid input; callers consume
// the seed only on success so a failed seed can never defeat later UI edits.
inline bool TryApplyPresentationSeed(const CameraPresentation& current,
                                     bool hasToneMap, ToneMapOperator toneMap,
                                     bool hasExposureEV, float exposureEV,
                                     CameraPresentation& out)
{
    CameraPresentation seeded = current;
    if (hasToneMap)
        seeded.toneMap = toneMap;
    if (hasExposureEV)
        seeded.exposureEV = exposureEV;
    if (!TryCanonicalizeCameraPresentation(seeded))
        return false;
    out = seeded;
    return true;
}

// Manual display exposure: scene-linear multiplier applied BEFORE the
// display transform. Valid range cannot overflow finite HDR inputs:
// exp2(+8) is 256x, and float range covers any sane renderer output.
inline float CameraExposureMultiplier(float exposureEV)
{
    return std::exp2(exposureEV);
}
