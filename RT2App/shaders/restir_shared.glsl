// restir_shared.glsl — unified ReSTIR DI reservoir operations.
//
// Included by restir_temporal.comp, restir_spatial.comp, and the shading
// pass (via scatter_shared.glsl). Provides:
//   - Reservoir struct (matches SIReservoir in shader_interface.h)
//   - SurfaceHistory struct (matches SISurfaceHistory)
//   - Reservoir init, stream update, merge, normalize, validity
//   - Unified target density (p_hat) for triangle and environment samples
//   - Sample reconstruction (triangle point or environment direction)
//   - Candidate proposal PDFs (triangle area-measure, env solid-angle)
//
// All target densities use solid-angle measure for both light types,
// enabling unbiased merging across families.

// ---- Reservoir struct (32 bytes, matches SIReservoir) -----------------------
struct Reservoir
{
    uvec4 data0;  // x = sampleType, y = lightIdx, z = uint(b1/envUV.x), w = uint(b2/envUV.y)
    uvec4 data1;  // x = uint(weightSum), y = uint(targetPdf), z = M, w = packedAgeFlags
};

// ---- SurfaceHistory struct (16 bytes) ---------------------------------------
struct SurfaceHistory
{
    uvec4 data;  // x = packed normal oct, y = uint(viewZ), z = materialID, w = packed flags
};

// ---- Sample type constants --------------------------------------------------
const uint SAMPLE_TRIANGLE = 0u;
const uint SAMPLE_ENV      = 1u;
const uint SAMPLE_EMPTY     = 0xFFFFFFFFu;

// ---- Reservoir field accessors ----------------------------------------------

uint reservoirSampleType(Reservoir r)
{
    return r.data0.x;
}

uint reservoirLightIdx(Reservoir r)
{
    return r.data0.y;
}

vec2 reservoirBarycentrics(Reservoir r)
{
    return vec2(uintBitsToFloat(r.data0.z), uintBitsToFloat(r.data0.w));
}

vec2 reservoirEnvUV(Reservoir r)
{
    return vec2(uintBitsToFloat(r.data0.z), uintBitsToFloat(r.data0.w));
}

float reservoirWeightSum(Reservoir r)
{
    return uintBitsToFloat(r.data1.x);
}

float reservoirTargetPdf(Reservoir r)
{
    return uintBitsToFloat(r.data1.y);
}

uint reservoirM(Reservoir r)
{
    return r.data1.z;
}

uint reservoirAge(Reservoir r)
{
    return r.data1.w >> 8u;
}

uint reservoirFlags(Reservoir r)
{
    return r.data1.w & 0xFFu;
}

bool reservoirValid(Reservoir r)
{
    return (reservoirFlags(r) & 1u) != 0u;
}

// ---- Reservoir constructors --------------------------------------------------

Reservoir makeEmptyReservoir()
{
    Reservoir r;
    r.data0 = uvec4(SAMPLE_EMPTY, 0u, 0u, 0u);
    r.data1 = uvec4(0u, 0u, 0u, 0u);
    return r;
}

Reservoir makeTriangleReservoir(uint lightIdx, float b1, float b2,
                                  float weightSum, float targetPdf, uint M)
{
    Reservoir r;
    r.data0 = uvec4(SAMPLE_TRIANGLE, lightIdx,
                   floatBitsToUint(b1), floatBitsToUint(b2));
    float ws = weightSum;
    float tp = targetPdf;
    r.data1 = uvec4(floatBitsToUint(ws), floatBitsToUint(tp), M, 1u);
    return r;
}

Reservoir makeEnvReservoir(float envU, float envV,
                             float weightSum, float targetPdf, uint M)
{
    Reservoir r;
    r.data0 = uvec4(SAMPLE_ENV, 0u,
                   floatBitsToUint(envU), floatBitsToUint(envV));
    float ws = weightSum;
    float tp = targetPdf;
    r.data1 = uvec4(floatBitsToUint(ws), floatBitsToUint(tp), M, 1u);
    return r;
}

void reservoirSetAge(inout Reservoir r, uint age)
{
    r.data1.w = (r.data1.w & 0xFFu) | (age << 8u);
}

void reservoirSetFlags(inout Reservoir r, uint flags)
{
    r.data1.w = (r.data1.w & ~0xFFu) | (flags & 0xFFu);
}

void reservoirSetM(inout Reservoir r, uint M)
{
    r.data1.z = M;
}

void reservoirSetWeightSum(inout Reservoir r, float ws)
{
    r.data1.x = floatBitsToUint(ws);
}

void reservoirSetTargetPdf(inout Reservoir r, float tp)
{
    r.data1.y = floatBitsToUint(tp);
}

// ---- Reservoir streaming update ---------------------------------------------
// Streaming RIS: for each candidate with weight w_i = p_hat(x_i) / p(x_i),
// accumulate weightSum and probabilistically select the sample.
void reservoirStreamUpdate(inout Reservoir r, float w,
                            uint sampleType, uint lightIdx,
                            float sampleParam1, float sampleParam2,
                            float p_hat, inout uint rngState)
{
    if (isnan(w) || isinf(w) || w <= 0.0) return;

    r.data1.x = floatBitsToUint(uintBitsToFloat(r.data1.x) + w);
    float weightSum = uintBitsToFloat(r.data1.x);

    if (randomFloat(rngState) < w / max(weightSum, 1e-20))
    {
        r.data0.x = sampleType;
        r.data0.y = lightIdx;
        r.data0.z = floatBitsToUint(sampleParam1);
        r.data0.w = floatBitsToUint(sampleParam2);
        r.data1.y = floatBitsToUint(p_hat);
    }
    r.data1.z += 1u;  // M++
    r.data1.w = (r.data1.w & ~0xFFu) | 1u;  // valid
}

// ---- Reservoir merge (canonical ReSTIR) ------------------------------------
// Merge reservoir r_new into r_dst with weight w_merge.
// p_hat_at_dst = target density of r_new's sample evaluated at r_dst's receiver.
// When the sample is adopted, targetPdf is set to p_hat_at_dst (NOT the source's
// targetPdf), so that reservoirW = weightSum / (M * targetPdf) is correct.
void reservoirMerge(inout Reservoir r_dst, Reservoir r_new, float w_merge,
                     float p_hat_at_dst, inout uint rngState)
{
    if (isnan(w_merge) || isinf(w_merge) || w_merge <= 0.0) return;

    float ws_dst = reservoirWeightSum(r_dst);

    float totalWs = ws_dst + w_merge;
    r_dst.data1.x = floatBitsToUint(totalWs);

    if (randomFloat(rngState) < w_merge / max(totalWs, 1e-20))
    {
        // Adopt the new sample — store p_hat evaluated at the DESTINATION receiver
        r_dst.data0 = r_new.data0;
        r_dst.data1.y = floatBitsToUint(p_hat_at_dst);
    }

    r_dst.data1.z += reservoirM(r_new);
    r_dst.data1.w = (r_dst.data1.w & ~0xFFu) | 1u;  // valid
}

// ---- Reservoir final normalization -------------------------------------------
// Returns W = weightSum / (M * targetPdf) — the stochastic replacement for 1/pdf.
float reservoirW(Reservoir r)
{
    float M = float(reservoirM(r));
    float tp = reservoirTargetPdf(r);
    if (M <= 0.0 || tp <= 0.0) return 0.0;
    float ws = reservoirWeightSum(r);
    return ws / (M * tp);
}

// ---- Target density (p_hat) evaluation --------------------------------------
// p_hat = luminance(BRDF * Le * NdotL) — solid-angle measure.
// For triangle samples: includes the area→solid-angle Jacobian (dist² / LNdotL).
// For environment samples: Le and pdf from env map, NdotL from BRDF.
//
// Returns 0.0 for invalid samples (back-facing, below horizon, NaN).

// Triangle target density: p_hat = luminance(brdf * Le * NdotL * LNdotL / dist²)
float evalTriangleTargetPdf(vec3 P, vec3 N, vec3 wo,
                            vec3 baseColor, float metallic,
                            uint lightIdx, float b1, float b2)
{
    TriangleLight light = lights[lightIdx];

    uint triIdx = normalOffsets[light.ids.x] + light.ids.y;
    uint posIdx = triIdx * 3u;
    mat4 lightWorld = instanceTransforms[light.ids.x];
    vec3 lp0 = vec3(lightWorld * trianglePositions[posIdx + 0u]);
    vec3 lp1 = vec3(lightWorld * trianglePositions[posIdx + 1u]);
    vec3 lp2 = vec3(lightWorld * trianglePositions[posIdx + 2u]);

    float b0 = 1.0 - b1 - b2;
    vec3 lightPoint = b0 * lp0 + b1 * lp1 + b2 * lp2;
    vec3 lightN = normalize(cross(lp1 - lp0, lp2 - lp0));

    vec3 toLight = lightPoint - P;
    float dist = length(toLight);
    vec3 L = toLight / max(dist, 1e-6);

    float NdotL = dot(N, L);
    float LNdotL = dot(lightN, -L);

    if (NdotL <= 0.0 || LNdotL <= 0.0) return 0.0;

    vec3 brdf = evalDiffuseBRDF(wo, L, N, baseColor, metallic);
    if (dot(brdf, brdf) <= 0.0) return 0.0;

    vec3 Le = light.emission_area.xyz;
    uint emissiveTexIdx = light.ids.w;
    if (emissiveTexIdx != 0xFFFFFFFFu)
    {
        vec2 luv0 = triangleUVs[posIdx + 0u].xy;
        vec2 luv1 = triangleUVs[posIdx + 1u].xy;
        vec2 luv2 = triangleUVs[posIdx + 2u].xy;
        vec2 lightUV = b0 * luv0 + b1 * luv1 + b2 * luv2;
        Le *= texture(textures[nonuniformEXT(int(emissiveTexIdx))], lightUV).rgb;
    }
    Le *= camera.apertureFocal.w;

    float geom = NdotL * LNdotL / (dist * dist);
    float p_hat = luminance(brdf * Le) * geom;

    if (isnan(p_hat) || isinf(p_hat) || p_hat <= 0.0) return 0.0;
    return p_hat;
}

// Environment target density: p_hat = luminance(brdf * Le * NdotL)
float evalEnvTargetPdf(vec3 P, vec3 N, vec3 wo,
                       vec3 baseColor, float metallic,
                       vec2 envUV)
{
    vec3 L = envUVToDirection(envUV);
    float NdotL = dot(N, L);
    if (NdotL <= 0.0) return 0.0;

    vec3 brdf = evalDiffuseBRDF(wo, L, N, baseColor, metallic);
    if (dot(brdf, brdf) <= 0.0) return 0.0;

    vec3 Le = envMapRadiance(L);
    if (dot(Le, Le) <= 0.0) return 0.0;

    float p_hat = luminance(brdf * Le) * NdotL;
    if (isnan(p_hat) || isinf(p_hat) || p_hat <= 0.0) return 0.0;
    return p_hat;
}

// Unified target density: evaluates p_hat for any reservoir sample type
float evalTargetPdf(Reservoir r, vec3 P, vec3 N, vec3 wo,
                    vec3 baseColor, float metallic)
{
    uint st = reservoirSampleType(r);
    if (st == SAMPLE_TRIANGLE)
    {
        vec2 bary = reservoirBarycentrics(r);
        return evalTriangleTargetPdf(P, N, wo, baseColor, metallic,
                                     reservoirLightIdx(r), bary.x, bary.y);
    }
    else if (st == SAMPLE_ENV)
    {
        vec2 envUV = reservoirEnvUV(r);
        return evalEnvTargetPdf(P, N, wo, baseColor, metallic, envUV);
    }
    return 0.0;
}

// ---- Triangle proposal PDF (solid-angle) ------------------------------------
// q_tri = p(select triangle family) * (1 / lightCount) * (1 / worldArea) * (dist² / |LNdotL|)
// p(select triangle family) = computePTri() when triangle lights exist, else 0.
float triangleProposalPdfOmega(vec3 P, vec3 N,
                                uint lightIdx, float b1, float b2)
{
    float pTri = computePTri();
    if (pTri <= 0.0 || lightCount == 0u) return 0.0;

    TriangleLight light = lights[lightIdx];
    float lightArea = light.emission_area.w;
    if (lightArea <= 0.0) return 0.0;

    uint triIdx = normalOffsets[light.ids.x] + light.ids.y;
    uint posIdx = triIdx * 3u;
    mat4 lightWorld = instanceTransforms[light.ids.x];
    vec3 lp0 = vec3(lightWorld * trianglePositions[posIdx + 0u]);
    vec3 lp1 = vec3(lightWorld * trianglePositions[posIdx + 1u]);
    vec3 lp2 = vec3(lightWorld * trianglePositions[posIdx + 2u]);

    float b0 = 1.0 - b1 - b2;
    vec3 lightPoint = b0 * lp0 + b1 * lp1 + b2 * lp2;
    vec3 lightN = normalize(cross(lp1 - lp0, lp2 - lp0));

    vec3 toLight = lightPoint - P;
    float dist = length(toLight);
    vec3 L = toLight / max(dist, 1e-6);

    float LNdotL = dot(lightN, -L);
    if (LNdotL <= 0.0) return 0.0;

    float pdfArea = pTri * (1.0 / float(lightCount)) * (1.0 / lightArea);
    float pdfOmega = pdfArea * (dist * dist) / LNdotL;

    return pdfOmega;
}

// ---- Environment proposal PDF (solid-angle) ---------------------------------
float envProposalPdfOmega(vec2 envUV)
{
    vec3 dir = envUVToDirection(envUV);
    return envMapPdf(dir);
}

// Unified proposal PDF
float evalProposalPdf(Reservoir r, vec3 P, vec3 N)
{
    uint st = reservoirSampleType(r);
    if (st == SAMPLE_TRIANGLE)
    {
        vec2 bary = reservoirBarycentrics(r);
        return triangleProposalPdfOmega(P, N, reservoirLightIdx(r), bary.x, bary.y);
    }
    else if (st == SAMPLE_ENV)
    {
        vec2 envUV = reservoirEnvUV(r);
        return envProposalPdfOmega(envUV);
    }
    return 0.0;
}

// ---- Reconstruct triangle sample point --------------------------------------
struct TriangleSample
{
    vec3  lightPoint;
    vec3  lightNormal;
    vec3  L;
    float dist;
    float NdotL;
    float LNdotL;
    vec3  Le;
    float lightArea;
    bool  valid;
};

TriangleSample reconstructTriangleSample(Reservoir r, vec3 P, vec3 N, vec3 wo)
{
    TriangleSample s;
    s.valid = false;
    s.lightPoint = vec3(0.0);
    s.lightNormal = vec3(0.0);
    s.L = vec3(0.0);
    s.dist = 0.0;
    s.NdotL = 0.0;
    s.LNdotL = 0.0;
    s.Le = vec3(0.0);
    s.lightArea = 0.0;

    if (reservoirSampleType(r) != SAMPLE_TRIANGLE) return s;

    uint lightIdx = reservoirLightIdx(r);
    if (lightIdx >= lightCount) return s;

    TriangleLight light = lights[lightIdx];
    vec2 bary = reservoirBarycentrics(r);
    float b1 = bary.x;
    float b2 = bary.y;
    float b0 = 1.0 - b1 - b2;

    uint triIdx = normalOffsets[light.ids.x] + light.ids.y;
    uint posIdx = triIdx * 3u;
    mat4 lightWorld = instanceTransforms[light.ids.x];
    vec3 lp0 = vec3(lightWorld * trianglePositions[posIdx + 0u]);
    vec3 lp1 = vec3(lightWorld * trianglePositions[posIdx + 1u]);
    vec3 lp2 = vec3(lightWorld * trianglePositions[posIdx + 2u]);

    s.lightPoint = b0 * lp0 + b1 * lp1 + b2 * lp2;
    s.lightNormal = normalize(cross(lp1 - lp0, lp2 - lp0));

    vec3 toLight = s.lightPoint - P;
    s.dist = length(toLight);
    s.L = toLight / max(s.dist, 1e-6);

    s.NdotL = dot(N, s.L);
    s.LNdotL = dot(s.lightNormal, -s.L);

    if (s.NdotL <= 0.0 || s.LNdotL <= 0.0) return s;

    s.Le = light.emission_area.xyz;
    uint emissiveTexIdx = light.ids.w;
    if (emissiveTexIdx != 0xFFFFFFFFu)
    {
        vec2 luv0 = triangleUVs[posIdx + 0u].xy;
        vec2 luv1 = triangleUVs[posIdx + 1u].xy;
        vec2 luv2 = triangleUVs[posIdx + 2u].xy;
        vec2 lightUV = b0 * luv0 + b1 * luv1 + b2 * luv2;
        s.Le *= texture(textures[nonuniformEXT(int(emissiveTexIdx))], lightUV).rgb;
    }
    s.Le *= camera.apertureFocal.w;
    s.lightArea = light.emission_area.w;
    s.valid = true;
    return s;
}

// ---- Reconstruct environment sample direction -------------------------------
struct EnvSampleRecon
{
    vec3  dir;
    vec3  radiance;
    float pdf;
    float NdotL;
    bool  valid;
};

EnvSampleRecon reconstructEnvSample(Reservoir r, vec3 N)
{
    EnvSampleRecon s;
    s.valid = false;
    s.dir = vec3(0.0);
    s.radiance = vec3(0.0);
    s.pdf = 0.0;
    s.NdotL = 0.0;

    if (reservoirSampleType(r) != SAMPLE_ENV) return s;

    vec2 envUV = reservoirEnvUV(r);
    s.dir = envUVToDirection(envUV);
    s.radiance = envMapRadiance(s.dir);
    s.pdf = envMapPdf(s.dir);
    s.NdotL = dot(N, s.dir);
    if (s.NdotL <= 0.0) return s;
    s.valid = true;
    return s;
}

// ---- Octahedral normal encoding for surface history (preserves full direction) ----
vec2 octEncode(vec3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 o = n.xy;
    if (n.z < 0.0)
        o = (1.0 - abs(o.yx)) * sign(o.xy);
    return o;
}

vec3 octDecode(vec2 o)
{
    vec3 n = vec3(o.x, o.y, 1.0 - abs(o.x) - abs(o.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
    return normalize(n);
}

// ---- Surface history helpers ------------------------------------------------

SurfaceHistory makeSurfaceHistory(vec3 normal, float viewZ, uint matIdx, bool valid)
{
    SurfaceHistory sh;
    vec2 oct = octEncode(normal);
    // Pack oct UV as two unorm16 (0-1 range from -1..1)
    uint ox = uint(clamp(oct.x * 0.5 + 0.5, 0.0, 1.0) * 65535.0);
    uint oy = uint(clamp(oct.y * 0.5 + 0.5, 0.0, 1.0) * 65535.0);
    sh.data.x = ox | (oy << 16u);
    sh.data.y = floatBitsToUint(viewZ);
    sh.data.z = matIdx;
    sh.data.w = valid ? 1u : 0u;
    return sh;
}

vec3 surfaceHistoryNormal(SurfaceHistory sh)
{
    uint ox_bits = sh.data.x & 0xFFFFu;
    uint oy_bits = (sh.data.x >> 16u) & 0xFFFFu;
    float ox = (float(ox_bits) / 65535.0) * 2.0 - 1.0;
    float oy = (float(oy_bits) / 65535.0) * 2.0 - 1.0;
    return octDecode(vec2(ox, oy));
}

float surfaceHistoryViewZ(SurfaceHistory sh)
{
    return uintBitsToFloat(sh.data.y);
}

uint surfaceHistoryMatIdx(SurfaceHistory sh)
{
    return sh.data.z;
}

bool surfaceHistoryValid(SurfaceHistory sh)
{
    return sh.data.w != 0u;
}

// ---- Surface history validation ---------------------------------------------
bool validateSurfaceHistory(SurfaceHistory sh, vec3 currentN, float currentViewZ,
                            uint currentMatIdx, bool currentIsSkyOrEmissive,
                            float depthThreshold, float normalThreshold)
{
    if (currentIsSkyOrEmissive) return false;
    if (!surfaceHistoryValid(sh)) return false;

    uint histMatIdx = surfaceHistoryMatIdx(sh);
    if (histMatIdx != currentMatIdx) return false;
    if (histMatIdx == 0xFFFFFFFFu) return false;  // sky/invalid

    vec3 histN = surfaceHistoryNormal(sh);
    if (dot(histN, currentN) < normalThreshold) return false;

    float histZ = surfaceHistoryViewZ(sh);
    float depthDiff = abs(histZ - currentViewZ) / max(abs(currentViewZ), 1e-6);
    if (depthDiff > depthThreshold) return false;

    return true;
}

// ---- Sanitization -----------------------------------------------------------
Reservoir sanitizeReservoir(Reservoir r)
{
    float ws = reservoirWeightSum(r);
    float tp = reservoirTargetPdf(r);
    uint M = reservoirM(r);
    uint st = reservoirSampleType(r);

    if (isnan(ws) || isinf(ws) || ws <= 0.0 ||
        isnan(tp) || isinf(tp) || tp <= 0.0 ||
        M == 0u || st == SAMPLE_EMPTY)
    {
        return makeEmptyReservoir();
    }
    return r;
}