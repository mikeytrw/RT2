#pragma once

// ============================================================================
// ToneMapMath — CPU reference for camera-owned display tone mapping.
//
// CPU-only (links into RT2Tests/RT2SliceRunner). The GPU twin lives in
// shaders/tonemap_shared.glsl with the same constants and operation order;
// keep the two in lockstep. Final sRGB encoding reuses ColorTransfer so both
// paths share one transfer function.
//
// Reference pins (no runtime download, no OCIO):
//   - AgX: "Minimal AgX Implementation" by Benjamin Wrensch (Missing
//     Deadlines), https://iolite-engine.com/blog_posts/minimal_agx_implementation
//     MIT License, (c) 2024 Missing Deadlines. Values sourced from Troy
//     Sobotka's AgX (https://github.com/sobotka/AgX). Neutral look only
//     (AGX_LOOK 0, the ASC-CDL identity); 6th-order default contrast
//     approximation (mean error^2 3.6705141e-06). This is a compact display
//     transform, not pixel identity with Blender/OCIO AgX configurations.
//     Verified 2026-09-11 against the live post content: agx_mat,
//     agx_mat_inv, min/max EV, all seven sigmoid coefficients, the 2.2
//     EOTF exponent and the MSE figure are identical to the tables below.
//     The post changelog (through 2025.04.20) touches only saturation
//     ordering and the license header, not the pinned constants. The post
//     is a dated blog page with no versioned revision; the pin is the URL
//     plus this verification record (author, MIT license, AGX_LOOK 0,
//     MSE 3.6705141e-06, changelog dates as observed 2026-09-11).
//     The one
//     deliberate deviation: small negative sigmoid residues are clamped
//     before pow() (the reference can feed pow() a negative base).
//   - ACES Fitted: Stephen Hill's BakingLab ACES.hlsl
//     (https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ACES.hlsl),
//     MIT License. Immutable pin: file last touched by upstream commit
//     0f66f50398a372ea27ba0e4d95379d32c83d2e8a ("Initial commit",
//     2016-10-10, MJP); git blob SHA
//     0f8476af81988408484528ec36e44f39447cb985 (1322 bytes), retrieved
//     2026-09-11 and verified constant-identical against the tables below
//     (all 18 input/output matrix entries, all five fit coefficients).
//     Uses the complete
//     ACESInputMat -> RRTAndODTFit -> ACESOutputMat path — NOT the distinct
//     five-coefficient per-channel approximation sometimes given the same name.
//     The input matrix performs the required sRGB/Rec.709-linear to AP1-like
//     working transform; do not feed renderer RGB directly into the scalar fit.
//
// Transform order (both paths):
//   sceneLinearRGB -> nonfinite guard -> max(RGB,0) -> exp2(EV) exposure,
//   clamped to FLT_MAX -> selected operator (each bounds its own arithmetic:
//   guarded log2 floor, ACES fit-domain bound, saturating Reinhard) ->
//   clamp display-linear [0,1] -> piecewise sRGB.
//
// Finite-range guarantee: every finite scene-linear input converts to a
// finite display value at every allowed EV. Only genuinely nonfinite input
// (or a nonfinite/non-positive exposure multiplier) makes the pixel convert
// fail (false).
//
// Nonfinite policy: any nonfinite RGB channel makes the pixel convert fail
// (false) instead of producing a silently-successful NaN export. Beauty
// display substitutes black; checked PNG capture fails; raw EXR/PFM output
// is untouched and may expose the upstream invalid sample.
// Alpha is never tone-mapped: finite source alpha clamps to [0,1],
// nonfinite alpha becomes 1.
// ============================================================================

#include "CameraPresentation.h"
#include "ColorTransfer.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace ToneMapMath
{

namespace Detail
{

// Matrices are stored as ROWS. (The GLSL twin uses column-major mat3
// constructors with transposed initializers; row-dot here equals
// matrix-times-column-vector there.)

// AgX inset (Wrensch agx_mat, transposed to rows).
inline constexpr float kAgXInset[3][3] = {
    { 0.842479062253094f, 0.0784335999999992f, 0.0792237451477643f },
    { 0.0423282422610123f, 0.878468636469772f, 0.0791661274605434f },
    { 0.0423756549057051f, 0.0784336f,         0.879142973793104f },
};

// AgX outset, i.e. inverse input transform (Wrensch agx_mat_inv, rows).
inline constexpr float kAgXOutset[3][3] = {
    { 1.19687900512017f,      -0.0980208811401368f, -0.0990297440797205f },
    { -0.0528968517574562f,    1.15190312990417f,   -0.0989611768448433f },
    { -0.0529716355144438f,   -0.0980434501171241f,  1.15107367264116f },
};

inline constexpr float kAgXMinEV = -12.47393f;
inline constexpr float kAgXMaxEV = 4.026069f;

// Log-domain input floor: the smallest positive value that still maps to
// the toe bound, keeping log2() off the undefined x <= 0 domain in both
// C++ (log2(0) is -inf) and GLSL (log2(x), x <= 0, is undefined). Pinned as
// the decimal that parses to the same float32 bits as exp2f(kAgXMinEV)
// (bit-verified 0x393851F2); the identical literal lives in
// tonemap_shared.glsl. Inputs at/below the floor map exactly where the
// bound clamp below would have put them, so black stays black.
inline constexpr float kAgXLogFloor = 1.7578134e-4f;

// ACES fit-domain bound. Above 1e6 the scalar fit is within ~4e-7 relative
// of its 1/0.983729 asymptote — invisible after 8-bit quantization — while
// the raw v*v terms overflow float32 near 1e19. Clamping here keeps every
// finite input finite without changing normal-range output.
inline constexpr float kACESFitMaxV = 1.0e6f;

// ACES input: sRGB => XYZ => D65_2_D60 => AP1 => RRT_SAT (Hill, rows as listed).
inline constexpr float kACESInput[3][3] = {
    { 0.59719f, 0.35458f, 0.04823f },
    { 0.07600f, 0.90834f, 0.01566f },
    { 0.02840f, 0.13383f, 0.83777f },
};

// ACES output: ODT_SAT => XYZ => D60_2_D65 => sRGB (Hill, rows as listed).
inline constexpr float kACESOutput[3][3] = {
    { 1.60475f, -0.53108f, -0.07367f },
    { -0.10208f, 1.10813f, -0.00605f },
    { -0.00327f, -0.07276f, 1.07602f },
};

inline void MatMulVec(const float m[3][3], float x, float y, float z,
                      float& outX, float& outY, float& outZ)
{
    outX = m[0][0] * x + m[0][1] * y + m[0][2] * z;
    outY = m[1][0] * x + m[1][1] * y + m[1][2] * z;
    outZ = m[2][0] * x + m[2][1] * y + m[2][2] * z;
}

// Wrensch 6th-order default contrast approximation (same operation order).
inline float AgXSigmoid(float x)
{
    const float x2 = x * x;
    const float x4 = x2 * x2;
    return 15.5f * x4 * x2
         - 40.14f * x4 * x
         + 31.96f * x4
         - 6.868f * x2 * x
         + 0.4298f * x2
         + 0.1191f * x
         - 0.00232f;
}

// Hill RRTAndODTFit, one channel.
inline float ACESFit(float v)
{
    const float a = v * (v + 0.0245786f) - 0.000090537f;
    const float b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}

inline float Clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

} // namespace Detail

// AgX neutral display transform. Input is exposed scene-linear RGB with all
// channels finite and >= 0. Output is display-linear sRGB clamped to [0,1],
// ready for the single piecewise sRGB encode (Wrensch linearizes with the
// 2.2-exponent reference EOTF for exactly this non-sRGB-target flow).
inline void AgXNeutralDisplay(float r, float g, float b,
                              float& outR, float& outG, float& outB)
{
    using namespace Detail;
    float x, y, z;
    MatMulVec(kAgXInset, r, g, b, x, y, z);
    // Guarded log2 domain encoding with bounds: inputs at/below the floor
    // (including black) map exactly to the toe, so black stays black
    // through the sigmoid's -0.00232 bias. The upper clamp is retained.
    x = std::log2(std::max(x, kAgXLogFloor));
    y = std::log2(std::max(y, kAgXLogFloor));
    z = std::log2(std::max(z, kAgXLogFloor));
    x = x < kAgXMinEV ? kAgXMinEV : (x > kAgXMaxEV ? kAgXMaxEV : x);
    y = y < kAgXMinEV ? kAgXMinEV : (y > kAgXMaxEV ? kAgXMaxEV : y);
    z = z < kAgXMinEV ? kAgXMinEV : (z > kAgXMaxEV ? kAgXMaxEV : z);
    x = (x - kAgXMinEV) / (kAgXMaxEV - kAgXMinEV);
    y = (y - kAgXMinEV) / (kAgXMaxEV - kAgXMinEV);
    z = (z - kAgXMinEV) / (kAgXMaxEV - kAgXMinEV);
    x = AgXSigmoid(x);
    y = AgXSigmoid(y);
    z = AgXSigmoid(z);
    // Neutral look is the ASC-CDL identity; no Golden/Punchy grading here.
    MatMulVec(kAgXOutset, x, y, z, outR, outG, outB);
    // The sigmoid approximation can leave small negative residues; pow()
    // needs a non-negative base, and the contract clamps display-linear.
    outR = Clamp01(std::pow(outR < 0.0f ? 0.0f : outR, 2.2f));
    outG = Clamp01(std::pow(outG < 0.0f ? 0.0f : outG, 2.2f));
    outB = Clamp01(std::pow(outB < 0.0f ? 0.0f : outB, 2.2f));
}

// ACES Fitted display transform (Hill). Same input/output contract as above;
// the reference saturates the result, mirrored here by the [0,1] clamp.
// The fit input is bounded to kACESFitMaxV so the quadratic terms stay
// finite for every finite HDR input at every allowed EV.
inline void ACESFittedDisplay(float r, float g, float b,
                              float& outR, float& outG, float& outB)
{
    using namespace Detail;
    float x, y, z;
    MatMulVec(kACESInput, r, g, b, x, y, z);
    x = ACESFit(std::min(x, kACESFitMaxV));
    y = ACESFit(std::min(y, kACESFitMaxV));
    z = ACESFit(std::min(z, kACESFitMaxV));
    MatMulVec(kACESOutput, x, y, z, outR, outG, outB);
    outR = Clamp01(outR);
    outG = Clamp01(outG);
    outB = Clamp01(outB);
}

// Legacy per-channel Reinhard display transform. Bit-for-bit the existing
// branch (ColorTransfer::Reinhard + sRGB) at 0 EV; retained for comparison.
inline void ReinhardDisplay(float r, float g, float b,
                            float& outR, float& outG, float& outB)
{
    using namespace Detail;
    outR = Clamp01(ColorTransfer::Reinhard(r));
    outG = Clamp01(ColorTransfer::Reinhard(g));
    outB = Clamp01(ColorTransfer::Reinhard(b));
}

// Full pixel: exposure, operator, display-linear clamp. Returns false when
// any RGB input channel is nonfinite (caller substitutes black / fails the
// capture). Every finite scene-linear input converts to a finite display
// value at every allowed EV: exposure products are clamped to FLT_MAX
// (min() maps +Inf from extreme HDR x maximum exposure onto FLT_MAX, which
// every operator below saturates to white) and each operator bounds its own
// arithmetic (AgX log floor, ACES fit-domain bound, Reinhard saturates).
// A nonfinite or non-positive exposure multiplier still fails loudly rather
// than scaling by garbage.
inline bool ToneMapPixel(float r, float g, float b,
                         ToneMapOperator op, float exposureMult,
                         float& outR, float& outG, float& outB)
{
    if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b))
        return false;
    if (!std::isfinite(exposureMult) || exposureMult <= 0.0f)
        return false;
    constexpr float kMaxExposed = std::numeric_limits<float>::max();
    r = std::min(std::max((r < 0.0f ? 0.0f : r) * exposureMult, 0.0f), kMaxExposed);
    g = std::min(std::max((g < 0.0f ? 0.0f : g) * exposureMult, 0.0f), kMaxExposed);
    b = std::min(std::max((b < 0.0f ? 0.0f : b) * exposureMult, 0.0f), kMaxExposed);
    switch (op)
    {
    case ToneMapOperator::ACESFitted:
        ACESFittedDisplay(r, g, b, outR, outG, outB);
        break;
    case ToneMapOperator::Reinhard:
        ReinhardDisplay(r, g, b, outR, outG, outB);
        break;
    case ToneMapOperator::AgX:
    default:
        AgXNeutralDisplay(r, g, b, outR, outG, outB);
        break;
    }
    return std::isfinite(outR) && std::isfinite(outG) && std::isfinite(outB);
}

// Complete HDR-to-display pixel conversion incl. sRGB quantization and the
// alpha policy (finite alpha clamps to [0,1]; nonfinite alpha becomes 1;
// alpha is never tone-mapped). This IS the PNG readback conversion
// (RendererGPU::ReadbackOutput calls it per pixel).
inline bool ConvertHdrPixelToDisplay8(float r, float g, float b, float a,
                                      ToneMapOperator op, float exposureMult,
                                      uint8_t outRGBA8[4])
{
    float dr = 0.0f, dg = 0.0f, db = 0.0f;
    if (!ToneMapPixel(r, g, b, op, exposureMult, dr, dg, db))
        return false;
    outRGBA8[0] = ColorTransfer::LinearToSRGB8(dr);
    outRGBA8[1] = ColorTransfer::LinearToSRGB8(dg);
    outRGBA8[2] = ColorTransfer::LinearToSRGB8(db);
    const float ac = std::isfinite(a) ? a : 1.0f;
    const float aq = ac > 1.0f ? 1.0f : (ac < 0.0f ? 0.0f : ac);
    // Alpha stays linear (matches the existing readback, which never ran
    // alpha through the sRGB transfer); only RGB sees the display chain.
    outRGBA8[3] = static_cast<uint8_t>(aq * 255.0f + 0.5f);
    return true;
}

} // namespace ToneMapMath
