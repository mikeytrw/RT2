// ============================================================================
// tonemap_shared.glsl — one camera-presentation implementation for the
// native (RGBA32F) and RR (RGBA16F) tone-map variants.
//
// The CPU twin lives in src/ToneMapMath.h with the same constants and
// operation order; keep the two in lockstep. Final sRGB encoding is the one
// shared piecewise transfer (see src/ColorTransfer.h).
//
// Reference pins (no runtime download, no OCIO):
//   - AgX: "Minimal AgX Implementation" by Benjamin Wrensch (Missing
//     Deadlines), MIT License (c) 2024, values from Troy Sobotka's AgX.
//     Neutral look only (ASC-CDL identity); 6th-order default contrast
//     approximation. Verified 2026-09-11 against the live post: matrices,
//     EV bounds, sigmoid coefficients, 2.2 EOTF and MSE identical to the
//     initializers below. Small negative sigmoid residues are clamped
//     before pow() (the reference can feed pow() a negative base).
//   - ACES Fitted: Stephen Hill's BakingLab ACES.hlsl, MIT License,
//     retrieved 2026-09-11 from upstream master and verified
//     constant-identical (all 18 matrix entries, all five fit
//     coefficients). Complete ACESInputMat -> RRTAndODTFit ->
//     ACESOutputMat path, NOT the five-coefficient per-channel
//     approximation.
//
// Matrix note: the CPU tables are stored as ROWS. GLSL mat3() takes COLUMNS,
// so every initializer below is the transpose of its ToneMapMath.h twin:
// column j holds the C++ table's column j, making (M * v) here equal the
// row-dot there. Do not "simplify" the order.
//
// Transform order (both paths):
//   sceneLinearRGB -> nonfinite guard -> max(RGB,0) -> exposure multiplier,
//   clamped to FLT_MAX -> selected operator (each bounds its own arithmetic:
//   guarded log2 floor, ACES fit-domain bound, saturating Reinhard) ->
//   clamp display-linear [0,1] -> piecewise sRGB.
// Finite-range guarantee: every finite scene-linear input converts to a
// finite display value at every allowed EV. Only genuinely nonfinite input
// becomes black (shaders cannot report; checked PNG capture detects the
// scene-linear sample on the CPU and fails instead). Alpha is never
// tone-mapped: finite alpha clamps to [0,1], nonfinite alpha is 1.
// ============================================================================

layout(push_constant) uniform TonemapPC
{
    int toneOperator;       // ToneMapOperator numbering: 0 AgX, 1 ACES, 2 Reinhard
    float exposureMultiplier; // exp2(exposureEV), precomputed on the CPU
    int legacyDiagnostic;   // nonzero forces the legacy Reinhard-at-0EV
                            // diagnostic mapping (data-oriented debug views)
    float padding;          // reserved
} pc;

// AgX inset (transpose of ToneMapMath kAgXInset rows).
const mat3 kAgXInset = mat3(
    0.842479062253094, 0.0423282422610123, 0.0423756549057051,
    0.0784335999999992, 0.878468636469772, 0.0784336,
    0.0792237451477643, 0.0791661274605434, 0.879142973793104);

// AgX outset (transpose of ToneMapMath kAgXOutset rows).
const mat3 kAgXOutset = mat3(
    1.19687900512017, -0.0528968517574562, -0.0529716355144438,
    -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
    -0.0990297440797205, -0.0989611768448433, 1.15107367264116);

const float kAgXMinEV = -12.47393;
const float kAgXMaxEV = 4.026069;

// Log-domain input floor: the smallest positive value that still maps to
// the toe bound, keeping log2() off the undefined x <= 0 domain. Pinned as
// the decimal that parses to the same float32 bits as the CPU table's
// exp2f(kAgXMinEV) (bit-verified 0x393851F2); the identical literal lives in
// ToneMapMath.h as kAgXLogFloor. Inputs at/below the floor map exactly
// where the bound clamp would have put them, so black stays black.
const float kAgXLogFloor = 1.7578134e-4;

// ACES fit-domain bound (see ToneMapMath kACESFitMaxV): above 1e6 the fit is
// within ~4e-7 relative of its asymptote (invisible after quantization)
// while raw v*v terms overflow float32 near 1e19.
const float kACESFitMaxV = 1.0e6;

// ACES input (transpose of ToneMapMath kACESInput rows).
const mat3 kACESInput = mat3(
    0.59719, 0.07600, 0.02840,
    0.35458, 0.90834, 0.13383,
    0.04823, 0.01566, 0.83777);

// ACES output (transpose of ToneMapMath kACESOutput rows).
const mat3 kACESOutput = mat3(
    1.60475, -0.10208, -0.00327,
    -0.53108, 1.10813, -0.07276,
    -0.07367, -0.00605, 1.07602);

float agxSigmoid(float x)
{
    float x2 = x * x;
    float x4 = x2 * x2;
    return 15.5 * x4 * x2
         - 40.14 * x4 * x
         + 31.96 * x4
         - 6.868 * x2 * x
         + 0.4298 * x2
         + 0.1191 * x
         - 0.00232;
}

float acesFit(float v)
{
    float a = v * (v + 0.0245786) - 0.000090537;
    float b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

vec3 linearToSRGB(vec3 linearColor)
{
    linearColor = max(linearColor, vec3(0.0));
    bvec3 useLinearSegment = lessThanEqual(linearColor, vec3(0.0031308));
    vec3 low = linearColor * 12.92;
    vec3 high = 1.055 * pow(linearColor, vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, useLinearSegment);
}

vec3 agxNeutralDisplay(vec3 rgb)
{
    vec3 v = kAgXInset * rgb;
    // Guarded log2 domain encoding with bounds: inputs at/below the floor
    // (including black) map exactly to the toe, so black stays black
    // through the sigmoid's -0.00232 bias (same as the CPU reference).
    // log2() is only ever evaluated on strictly positive values.
    v = log2(max(v, vec3(kAgXLogFloor)));
    v = clamp(v, vec3(kAgXMinEV), vec3(kAgXMaxEV));
    v = (v - kAgXMinEV) / (kAgXMaxEV - kAgXMinEV);
    v = vec3(agxSigmoid(v.x), agxSigmoid(v.y), agxSigmoid(v.z));
    v = kAgXOutset * v;
    // Small negative residues would poison pow(); the contract clamps
    // display-linear, matching the CPU reference exactly.
    v = max(v, vec3(0.0));
    return clamp(pow(v, vec3(2.2)), vec3(0.0), vec3(1.0));
}

vec3 acesFittedDisplay(vec3 rgb)
{
    vec3 v = kACESInput * rgb;
    // Bound the rational fit domain so the quadratic terms stay finite for
    // every finite HDR input at every allowed EV (same as the CPU reference).
    v = min(v, vec3(kACESFitMaxV));
    v = vec3(acesFit(v.x), acesFit(v.y), acesFit(v.z));
    v = kACESOutput * v;
    return clamp(v, vec3(0.0), vec3(1.0));
}

vec3 reinhardDisplay(vec3 rgb)
{
    // Legacy per-channel Reinhard. Bit-for-bit the pre-presentation branch
    // at 0 EV; retained for comparison and diagnostic views.
    return clamp(rgb / (vec3(1.0) + rgb), vec3(0.0), vec3(1.0));
}

// Full HDR-to-display-linear conversion for one beauty pixel. The caller
// has already rejected nonfinite input (black) and clamped negatives.
vec3 tonemapDisplay(vec3 exposed, int toneOperator)
{
    if (toneOperator == 1)
        return acesFittedDisplay(exposed);
    if (toneOperator == 2)
        return reinhardDisplay(exposed);
    return agxNeutralDisplay(exposed);
}
