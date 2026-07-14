// restir_gi_shared.glsl — ReSTIR GI reservoir operations.
//
// Included by restir_gi_temporal.comp, restir_gi_history.comp, and the
// shading pass (via scatter_shared.glsl) for GI consumption. Provides:
//   - GIReservoir struct (matches SIGIReservoir in shader_interface.h)
//   - Field accessors (direction, Lo, hitT, weightSum, targetPdf, M, age,
//     flags, seed)
//   - Streaming update and canonical merge (diffuse-only target)
//   - Parity helpers (giCurrentRegion / giPreviousRegion)
//   - Sanitization and empty-reservoir construction
//
// Target density uses the diffuse primary BRDF component only — NOT the
// combined diffuse+specular used by the DI target. The cosine term is part
// of both the target and the final vector contribution (the proposal is
// cosine-weighted, but the cosine must still be included explicitly).
//
// Include this AFTER restir_gi_bindings.glsl (which declares lights, textures,
// instanceTransforms, evalDiffuseBRDF, luminance, randomFloat, etc.) and
// surface_history_shared.glsl.

#ifndef RESTIR_GI_SHARED_GLSL
#define RESTIR_GI_SHARED_GLSL

// ---- GI reservoir flag constants (match shader_interface.h) -----------------
#define GI_FLAG_VALID         1u
#define GI_FLAG_ENV_MISS      2u
#define GI_FLAG_GEOMETRY_HIT  4u
#define GI_FLAG_HISTORY       8u
#define GI_FLAG_NEE_VALID    16u

// ---- Reservoir struct (48 bytes, 3 × uvec4, matches SIGIReservoir) -----------
struct GIReservoir
{
    uvec4 data0;  // xyz = floatBitsToUint(direction.xyz), w = floatBitsToUint(hitT)
    uvec4 data1;  // xyz = floatBitsToUint(Lo.xyz),       w = floatBitsToUint(weightSum)
    uvec4 data2;  // x = floatBitsToUint(targetPdf), y = M,
                  // z = packed (age << 16) | flags, w = root sample seed
};

// ---- Parity helpers ---------------------------------------------------------
// Drives compute output region, temporal input region, receiver-history
// regions, and SINRDUniformData::restirGIReservoirIndex read by raygen.
// The same convention is mirrored in C++ (RendererGPU::m_GIFrameIndex).
uint giCurrentRegion(uint frameIndex)  { return frameIndex & 1u; }
uint giPreviousRegion(uint frameIndex) { return frameIndex ^ 1u; }

// ---- Field accessors --------------------------------------------------------
vec3 giDirection(GIReservoir r)
{
    return vec3(uintBitsToFloat(r.data0.x),
                uintBitsToFloat(r.data0.y),
                uintBitsToFloat(r.data0.z));
}

float giHitT(GIReservoir r)
{
    return uintBitsToFloat(r.data0.w);
}

vec3 giLo(GIReservoir r)
{
    return vec3(uintBitsToFloat(r.data1.x),
                uintBitsToFloat(r.data1.y),
                uintBitsToFloat(r.data1.z));
}

float giWeightSum(GIReservoir r)
{
    return uintBitsToFloat(r.data1.w);
}

float giTargetPdf(GIReservoir r)
{
    return uintBitsToFloat(r.data2.x);
}

uint giM(GIReservoir r)
{
    return r.data2.y;
}

uint giFlags(GIReservoir r)
{
    return r.data2.z & 0xFFFFu;
}

uint giAge(GIReservoir r)
{
    return r.data2.z >> 16u;
}

uint giSeed(GIReservoir r)
{
    return r.data2.w;
}

bool giValid(GIReservoir r)
{
    return (giFlags(r) & GI_FLAG_VALID) != 0u;
}

// ---- Mutators ---------------------------------------------------------------
void giSetFlags(inout GIReservoir r, uint flags)
{
    r.data2.z = (r.data2.z & 0xFFFF0000u) | (flags & 0xFFFFu);
}

void giSetAge(inout GIReservoir r, uint age)
{
    r.data2.z = (r.data2.z & 0xFFFFu) | (age << 16u);
}

void giSetM(inout GIReservoir r, uint M)
{
    r.data2.y = M;
}

void giSetWeightSum(inout GIReservoir r, float ws)
{
    r.data1.w = floatBitsToUint(ws);
}

void giSetTargetPdf(inout GIReservoir r, float tp)
{
    r.data2.x = floatBitsToUint(tp);
}

void giSetPayload(inout GIReservoir r, vec3 direction, vec3 Lo, float hitT,
                  uint seed)
{
    r.data0 = uvec4(floatBitsToUint(direction.x),
                   floatBitsToUint(direction.y),
                   floatBitsToUint(direction.z),
                   floatBitsToUint(hitT));
    r.data1.x = floatBitsToUint(Lo.x);
    r.data1.y = floatBitsToUint(Lo.y);
    r.data1.z = floatBitsToUint(Lo.z);
    r.data2.w = seed;
}

// ---- Constructors -----------------------------------------------------------
GIReservoir makeEmptyGIReservoir()
{
    GIReservoir r;
    r.data0 = uvec4(0u);
    r.data1 = uvec4(0u);
    r.data2 = uvec4(0u);
    return r;
}

// A zeroed GIReservoir has flags = 0 → valid bit clear, and M = 0.
// vkCmdFillBuffer(..., 0) therefore produces invalid reservoirs by construction.
bool giIsEmpty(GIReservoir r)
{
    return !giValid(r) && giM(r) == 0u;
}

// ---- Reservoir W (stochastic replacement for 1/pdf) -------------------------
float giReservoirW(GIReservoir r)
{
    float M  = float(giM(r));
    float tp = giTargetPdf(r);
    if (M <= 0.0 || tp <= 0.0) return 0.0;
    float ws = giWeightSum(r);
    return ws / (M * tp);
}

// ---- Streaming update -------------------------------------------------------
// Streaming RIS for GI: for each positive finite candidate weight
// w_i = p_hat(x_i) / q(x_i), accumulate weightSum and probabilistically
// select the sample. q is the cosine-weighted diffuse proposal (cosTheta/pi).
// The caller accounts for the proposal in M before any rejection.
//
// payloadArgs: direction, Lo, hitT, seed — copied together when selected.
// flags: the candidate's flag bits (env miss / geometry hit / NEE valid).
void giStreamUpdate(inout GIReservoir r, float w,
                    vec3 direction, vec3 Lo, float hitT, uint seed, uint flags,
                    float p_hat, inout uint rngState)
{
    if (isnan(w) || isinf(w) || w <= 0.0) return;

    float ws = giWeightSum(r) + w;
    giSetWeightSum(r, ws);

    if (randomFloat(rngState) < w / max(ws, 1e-20))
    {
        giSetPayload(r, direction, Lo, hitT, seed);
        giSetTargetPdf(r, p_hat);
        // Preserve age (set by caller / merge); only set flags here.
        giSetFlags(r, flags | GI_FLAG_VALID);
    }
}

// ---- Canonical merge --------------------------------------------------------
// Merge r_new into r_dst with weight w_merge. p_hat_at_dst is the target
// density of r_new's sample evaluated at r_dst's receiver. representedM is
// the effective candidate count to add to r_dst.M (may be capped). sourceAge
// follows the selected sample so reuse cannot relabel an old source sample
// as fresh at the destination. Returns true when r_new is selected.
//
// When the source is adopted, targetPdf is set to p_hat_at_dst (NOT the
// source's targetPdf) so that giReservoirW = weightSum / (M * targetPdf)
// is correct at the destination.
bool giMerge(inout GIReservoir r_dst, GIReservoir r_new, float w_merge,
             uint representedM, uint sourceAge, float p_hat_at_dst,
             inout uint rngState)
{
    if (isnan(w_merge) || isinf(w_merge) || w_merge <= 0.0) return false;
    if (representedM == 0u) return false;

    float ws_dst = giWeightSum(r_dst);
    float totalWs = ws_dst + w_merge;
    giSetWeightSum(r_dst, totalWs);

    bool sourceSelected = randomFloat(rngState) < w_merge / max(totalWs, 1e-20);
    if (sourceSelected)
    {
        // Copy full sample payload (direction, Lo, hitT, seed, flags).
        r_dst.data0 = r_new.data0;
        r_dst.data1.x = r_new.data1.x;  // Lo.xyz
        r_dst.data1.y = r_new.data1.y;
        r_dst.data1.z = r_new.data1.z;
        r_dst.data2.w = r_new.data2.w;  // seed
        uint age = sourceAge;
        uint flags = giFlags(r_new) | GI_FLAG_HISTORY;
        r_dst.data2.z = (age << 16u) | (flags & 0xFFFFu);
        giSetTargetPdf(r_dst, p_hat_at_dst);
    }

    giSetM(r_dst, giM(r_dst) + representedM);
    giSetFlags(r_dst, giFlags(r_dst) | GI_FLAG_VALID);
    return sourceSelected;
}

// ---- Sanitization -----------------------------------------------------------
GIReservoir sanitizeGIReservoir(GIReservoir r)
{
    float ws = giWeightSum(r);
    float tp = giTargetPdf(r);
    uint  M  = giM(r);

    if (isnan(ws) || isinf(ws) || ws <= 0.0 ||
        isnan(tp) || isinf(tp) || tp <= 0.0 ||
        M == 0u || !giValid(r))
    {
        return makeEmptyGIReservoir();
    }
    return r;
}

#endif // RESTIR_GI_SHARED_GLSL