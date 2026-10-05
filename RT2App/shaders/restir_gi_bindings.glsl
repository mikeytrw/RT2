// restir_gi_bindings.glsl — shared bindings for ReSTIR GI compute passes.
//
// Included by restir_gi_temporal.comp and restir_gi_history.comp.
// Declares all set 0 and set 1 bindings needed by the GI passes, plus the
// RNG, luminance, diffuse-BRDF, and environment helpers.
//
// Does NOT include restir_shared.glsl (DI-specific). Surface history helpers
// come from surface_history_shared.glsl; GI reservoir operations come from
// restir_gi_shared.glsl.
//
// The GI monolithic buffer at SI_BINDING_GI_DATA contains four regions:
//   reservoirA          pixelCount * 48 bytes
//   reservoirB          pixelCount * 48 bytes
//   receiverHistoryPrev pixelCount * 32 bytes
//   receiverHistoryCur  pixelCount * 32 bytes
// Regions are selected by frame parity (giCurrentRegion / giPreviousRegion).

#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_ray_query : require

#include "shader_interface.h"

#define PI 3.14159265359

// ---- Camera UBO (set 0, binding 1) ------------------------------------------
layout(set = 0, binding = SI_BINDING_CAMERA_UBO, std140) uniform CameraData
{
    vec4 position;
    vec4 forward;
    vec4 right;
    vec4 up;
    vec4 viewportSPP;
    vec4 apertureFocal;
    vec4 envMap;
    mat4 inverseProjection;
    mat4 inverseView;
    mat4 viewToClip;
    mat4 viewToClipPrev;
    mat4 worldToView;
    mat4 worldToViewPrev;
} camera;

// ---- Material buffer (set 0, binding 2) ------------------------------------
struct Material
{
    vec4 baseColor_metallic;
    vec4 emissive_roughness;
    float ior;
    float alphaCutoff;
    float alphaMode;
    float baseAlpha;
    ivec4 textureIndices;
    ivec4 extraIndices;
};

layout(set = 0, binding = SI_BINDING_MATERIAL_BUFFER, std430) readonly buffer MaterialBuffer
{
    Material materials[];
};

// ---- Instance mesh info (set 0, binding 8) ----------------------------------
layout(set = 0, binding = SI_BINDING_INSTANCE_MESH_INFO, std430) readonly buffer InstanceMeshInfo
{
    uvec4 instanceMeshInfo[];
};

// ---- Index buffer (set 0, binding 5) ----------------------------------------
layout(set = 0, binding = SI_BINDING_INDEX_BUFFER, std430) readonly buffer IndexBuffer
{
    uint indices[];
};

// ---- Vertex buffer (set 0, binding 3) ---------------------------------------
layout(set = 0, binding = SI_BINDING_VERTEX_BUFFER, std430) readonly buffer VertexBuffer
{
    vec4 vertices[];
};

// ---- Normal buffer (set 0, binding 6) --------------------------------------
layout(set = 0, binding = SI_BINDING_NORMAL_BUFFER, std430) readonly buffer NormalBuffer
{
    vec4 normals[];
};

// ---- UV buffer (set 0, binding 7) -------------------------------------------
layout(set = 0, binding = SI_BINDING_UV_BUFFER, std430) readonly buffer UVBuffer
{
    vec4 uvs[];
};

// ---- Light buffer (set 0, binding 9) ---------------------------------------
struct TriangleLight
{
    vec4  emission_area;
    uvec4 ids;
};

layout(set = 0, binding = SI_BINDING_LIGHT_BUFFER, std430) readonly buffer LightBuffer
{
    uint  lightCount;
    float totalLightArea;
    uint  _lightPad0;
    uint  _lightPad1;
    TriangleLight lights[];
};

// Probability of selecting triangle NEE (vs env NEE) in stochastic NEE.
float computePTri()
{
    bool hasTri = (lightCount > 0u && totalLightArea > 0.0);
    bool hasEnv = (int(camera.envMap.x) >= 0);
    if (hasTri && hasEnv) return 0.5;
    if (hasTri)            return 1.0;
    return 0.0;
}

// ---- Instance transforms (set 0, binding 10) --------------------------------
layout(set = 0, binding = SI_BINDING_INSTANCE_TRANSFORMS, std430) readonly buffer InstanceTransforms
{
    mat4 instanceTransforms[];
};

// ---- Instance material indices (set 0, binding 13) --------------------------
layout(set = 0, binding = SI_BINDING_INSTANCE_MATERIAL_INDICES, std430) readonly buffer InstanceMaterialIndexBuffer
{
    uint instanceMaterialIndices[];
};

// ---- Instance mat offsets (set 0, binding 17) ------------------------------
layout(set = 0, binding = SI_BINDING_INSTANCE_MAT_OFFSETS, std430) readonly buffer InstanceMatOffsetBuffer
{
    uint instanceMatOffsets[];
};

// ---- Texture array (set 0, binding 18) -------------------------------------
layout(set = 0, binding = SI_BINDING_TEXTURE_ARRAY) uniform sampler2D textures[];

// ---- TLAS (set 0, binding 4) — visible to compute for ray queries -----------
layout(set = 0, binding = SI_BINDING_TLAS) uniform accelerationStructureEXT tlas;

// ---- GI monolithic buffer (set 0, binding 11) -------------------------------
// Four regions: reservoirA, reservoirB, receiverHistoryPrev, receiverHistoryCur.
// Region offsets in uvec4 units (16-byte stride).
layout(set = 0, binding = SI_BINDING_GI_DATA, std430) buffer GIDataBuffer
{
    uvec4 giData[];
};

// ---- G-buffer images (set 1) -----------------------------------------------
layout(set = 1, binding = SI_BINDING_G_PRIM_HIT, rgba32f) uniform readonly image2D gPrimHit;
layout(set = 1, binding = SI_BINDING_G_PRIM_GEO_NORMAL, rgba8) uniform readonly image2D gPrimGeoNormal;
layout(set = 1, binding = SI_BINDING_G_PRIM_UV, rg16f) uniform readonly image2D gPrimUV;
layout(set = 1, binding = SI_BINDING_G_NORMAL_ROUGHNESS, rgb10_a2) uniform readonly image2D gNormalRoughness;
layout(set = 1, binding = SI_BINDING_G_ALBEDO_F0, rgba16f) uniform readonly image2D gAlbedoF0;
layout(set = 1, binding = SI_BINDING_G_DIRECT_EMISSION, rgba16f) uniform readonly image2D gDirectEmission;
layout(set = 1, binding = SI_BINDING_G_VIEWZ, r32f) uniform readonly image2D gViewZ;
layout(set = 1, binding = SI_BINDING_G_MOTION, rg16f) uniform readonly image2D gMotion;

// ---- Push constants ---------------------------------------------------------
layout(push_constant) uniform GIPush
{
    SIGIPushConstants gi;
} pc;

// ---- GI buffer region indexing ----------------------------------------------
// GIReservoir is 48 bytes (3 uvec4); SISurfaceHistory is 32 bytes (2 uvec4).
// sizeof() is not available in GLSL, so use literal byte sizes.
const uint GI_RESERVOIR_BYTES = 48u;
const uint GI_RECEIVER_HISTORY_BYTES = 32u;

// Returns the byte offset of reservoir region (0 or 1) within giData.
uint giReservoirRegionByteOffset(uint region, uint pixelCount)
{
    return region * pixelCount * GI_RESERVOIR_BYTES;
}

// Returns the byte offset of receiver-history region (0 or 1) within giData.
// Receiver history regions start after both reservoir regions.
uint giReceiverHistoryRegionByteOffset(uint region, uint pixelCount)
{
    return 2u * pixelCount * GI_RESERVOIR_BYTES
         + region * pixelCount * GI_RECEIVER_HISTORY_BYTES;
}

// ---- RNG + helpers (shared with DI path) ------------------------------------
uint pcg(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float randomFloat(inout uint state)
{
    return float(pcg(state) >> 8u) * (1.0 / 16777216.0);
}

uint initRNG(ivec2 pixel, float frameIndex)
{
    uint rngState = uint(pixel.x) * 1973u + uint(pixel.y) * 9277u + uint(frameIndex) * 26699u;
    pcg(rngState);
    return rngState;
}

float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// ---- Orthonormal basis + cosine hemisphere sampling (matches pathtracer_shared) ----
void buildONB(vec3 n, out vec3 T, out vec3 B)
{
    if (abs(n.z) > 0.999)
    {
        T = vec3(1.0, 0.0, 0.0);
        B = vec3(0.0, 1.0, 0.0);
    }
    else
    {
        vec3 up = (abs(n.y) < 0.999) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        T = normalize(cross(up, n));
        B = cross(n, T);
    }
}

vec3 sampleCosineHemisphere(vec3 n, inout uint rngState)
{
    float r1 = randomFloat(rngState);
    float r2 = randomFloat(rngState);
    float phi = 2.0 * PI * r1;
    float sr2 = sqrt(r2);
    vec3 T, B;
    buildONB(n, T, B);
    vec3 local = vec3(cos(phi) * sr2, sin(phi) * sr2, sqrt(max(1.0 - r2, 0.0)));
    return normalize(T * local.x + B * local.y + n * local.z);
}

vec4 nrdDecodeNormalRoughness(vec3 p)
{
    float t = p.z * 2.0 - 1.0;
    vec4 r;
    r.x = p.x - p.y;
    r.y = p.x + p.y - 1.0;
    r.z = t < 0.0 ? -1.0 : 1.0;
    r.z *= 1.0 - abs(r.x) - abs(r.y);
    r.w = abs(t);
    r.xyz = normalize(r.xyz);
    return r;
}

// Diffuse BRDF evaluation (diffuse component only — matches pathtracer_shared.glsl).
// GI target uses this, NOT the combined diffuse+specular used by the DI target.
vec3 evalDiffuseBRDF(vec3 wo, vec3 wi, vec3 n,
                     vec3 baseColor, float metallic)
{
    float NdotV = max(dot(wo, n), 0.0);
    float NdotL = max(dot(wi, n), 0.0);
    if (NdotV <= 0.0 || NdotL <= 0.0) return vec3(0.0);

    vec3 F0 = mix(vec3(0.04), baseColor, metallic);
    float VdotH = max(dot(wo, normalize(wo + wi)), 0.0);
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    float specWeight = mix(luminance(F), 1.0, metallic);
    vec3 diffuseAlbedo = baseColor * (1.0 - metallic);
    return (1.0 - specWeight) * diffuseAlbedo / PI;
}

// ---- Lobe selection prediction (matches scatterPrimaryHit in scatter_shared.glsl) ----
// Returns true if the primary scatter will select the diffuse lobe.
// Used by the GI compute pass to skip specular-selected pixels, avoiding
// wasted ray queries. The decision must match scatterPrimaryHit exactly.
//
// ditherMode: 0=white noise, 1=Bayer 4x4, 2=IGN (packed in GI flags bits 1-2).
bool giWillSelectDiffuse(Material mat, vec3 baseColor, float metallic,
                          float roughness, vec3 n, vec3 wo, float NdotV,
                          uvec2 pixelCoord, int ditherMode)
{
    // Delta specular: roughness < 0.001 && metallic >= 0.5 → always specular.
    if (roughness < 0.001 && metallic >= 0.5)
        return false;

    vec3 F0 = mix(vec3(0.04), baseColor, metallic);
    float f = pow(clamp(1.0 - NdotV, 0.0, 1.0), 5.0);
    vec3 F = F0 + (1.0 - F0) * f;
    float P_s = clamp(mix(luminance(F), 1.0, metallic), 0.25, 0.75);

    // Dithered lobe selection (matches scatterPrimaryHit exactly).
    float r;
    if (ditherMode == 1)
    {
        const float bayer4x4[16] = float[16](
            0.0/16.0,  8.0/16.0,  2.0/16.0, 10.0/16.0,
           12.0/16.0,  4.0/16.0, 14.0/16.0,  6.0/16.0,
            3.0/16.0, 11.0/16.0,  1.0/16.0,  9.0/16.0,
           15.0/16.0,  7.0/16.0, 13.0/16.0,  5.0/16.0
        );
        uint bayerIdx = (pixelCoord.y % 4u) * 4u + (pixelCoord.x % 4u);
        r = bayer4x4[bayerIdx];
    }
    else if (ditherMode == 2)
    {
        r = fract(52.9829189 * fract(0.06711056 * float(pixelCoord.x)
                + 0.00583715 * float(pixelCoord.y)));
    }
    else
    {
        // White noise: no deterministic prediction possible. Use the
        // deterministic threshold r = 0.5 as a best-effort heuristic.
        r = 0.5;
    }

    // Transmission materials: 3-way pick (reflect/refract/diffuse).
    float transmissionFactor = intBitsToFloat(mat.textureIndices.w);
    if (mat.alphaMode < 0.5 && transmissionFactor > 0.0)
    {
        float F0_scalar = mix(0.04, luminance(baseColor), metallic);
        float F_scalar = F0_scalar + (1.0 - F0_scalar) * f;
        float P_reflect = F_scalar;
        float P_refract  = (1.0 - F_scalar) * transmissionFactor;
        float P_diffuse = (1.0 - F_scalar) * (1.0 - transmissionFactor) * (1.0 - metallic);
        float Psum = P_reflect + P_refract + P_diffuse;
        if (Psum > 1e-6)
        {
            P_reflect /= Psum;
            P_refract  /= Psum;
            P_diffuse  /= Psum;
        }
        // Diffuse is selected when rTransmission >= P_reflect + P_refract.
        // rTransmission is white noise (randomFloat), not dithered.
        // For prediction, use the deterministic threshold: diffuse if
        // P_diffuse > 0.5 (likely), otherwise predict specular.
        // This is conservative — it only skips when diffuse is clearly unlikely.
        return P_diffuse > 0.5;
    }

    // Standard 2-way pick: diffuse if r >= P_s.
    return r >= P_s;
}

// ---- Environment map helpers (from pathtracer_shared.glsl) -----------------
vec2 directionToEnvUV(vec3 dir)
{
    float u = fract(atan(dir.z, dir.x) * 0.15915494309);
    float v = asin(clamp(dir.y, -1.0, 1.0)) * 0.31830988618;
    return vec2(u, 0.5 - v);
}

vec3 envUVToDirection(vec2 uv)
{
    float theta = uv.x * 2.0 * PI;
    float phi = (0.5 - uv.y) * PI;
    float cosPhi = cos(phi);
    return vec3(cos(theta) * cosPhi, sin(phi), sin(theta) * cosPhi);
}

vec3 envMapRadiance(vec3 dir)
{
    int envIdx = int(camera.envMap.x);
    if (envIdx < 0)
        return vec3(0.0);
    vec2 uv = directionToEnvUV(dir);
    vec3 radiance = texture(textures[nonuniformEXT(envIdx)], uv).rgb;
    return radiance * camera.envMap.y;
}

float envMapPdf(vec3 dir)
{
    int envIdx = int(camera.envMap.x);
    int marginalIdx = int(camera.envMap.z);
    int conditionalIdx = int(camera.envMap.w);
    if (envIdx < 0 || marginalIdx < 0 || conditionalIdx < 0)
        return 0.0;

    ivec2 marginalSize = textureSize(textures[nonuniformEXT(marginalIdx)], 0);
    int marginalLen = marginalSize.x;
    ivec2 condSize = textureSize(textures[nonuniformEXT(conditionalIdx)], 0);
    int condW = condSize.x;

    vec2 uv = directionToEnvUV(dir);
    int vIdx = int(clamp(uv.y * float(marginalLen), 0.0, float(marginalLen - 1)));
    int uIdx = int(clamp(uv.x * float(condW),       0.0, float(condW - 1)));

    float margPrev = (vIdx > 0) ? texelFetch(textures[nonuniformEXT(marginalIdx)], ivec2(vIdx - 1, 0), 0).r : 0.0;
    float margCurr = texelFetch(textures[nonuniformEXT(marginalIdx)], ivec2(vIdx, 0), 0).r;
    float pdfV = max(margCurr - margPrev, 1e-8);

    float condPrev = (uIdx > 0) ? texelFetch(textures[nonuniformEXT(conditionalIdx)], ivec2(uIdx - 1, vIdx), 0).r : 0.0;
    float condCurr = texelFetch(textures[nonuniformEXT(conditionalIdx)], ivec2(uIdx, vIdx), 0).r;
    float pdfU = max(condCurr - condPrev, 1e-8);

    float sinTheta = sqrt(max(1.0 - dir.y * dir.y, 1e-6));
    float texelDensityScale = float(condW) * float(marginalLen);
    return (pdfV * pdfU * texelDensityScale) /
           (sinTheta * 2.0 * PI * PI);
}

// ---- Environment map inverse-CDF sampling (matches sampleEnvMap) ------------
struct EnvSample
{
    vec3  dir;
    float pdf;
    vec3  radiance;
    vec2  uv;
};

EnvSample sampleEnvMap(inout uint rngState)
{
    EnvSample s;
    s.dir = vec3(0.0);
    s.pdf = 0.0;
    s.radiance = vec3(0.0);
    s.uv = vec2(0.0);

    int envIdx = int(camera.envMap.x);
    int marginalIdx = int(camera.envMap.z);
    int conditionalIdx = int(camera.envMap.w);
    if (envIdx < 0 || marginalIdx < 0 || conditionalIdx < 0)
        return s;

    float xi1 = randomFloat(rngState);
    ivec2 marginalSize = textureSize(textures[nonuniformEXT(marginalIdx)], 0);
    int marginalLen = marginalSize.x;

    int lo = 0;
    int hi = marginalLen - 1;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        float cdfVal = texelFetch(textures[nonuniformEXT(marginalIdx)], ivec2(mid, 0), 0).r;
        if (cdfVal < xi1) lo = mid + 1; else hi = mid;
    }
    int vIdx = lo;

    float xi2 = randomFloat(rngState);
    ivec2 condSize = textureSize(textures[nonuniformEXT(conditionalIdx)], 0);
    int condW = condSize.x;

    lo = 0;
    hi = condW - 1;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        float cdfVal = texelFetch(textures[nonuniformEXT(conditionalIdx)], ivec2(mid, vIdx), 0).r;
        if (cdfVal < xi2) lo = mid + 1; else hi = mid;
    }
    int uIdx = lo;

    float u = (float(uIdx) + 0.5) / float(condW);
    float v = (float(vIdx) + 0.5) / float(marginalLen);

    s.uv = vec2(u, v);
    s.dir = envUVToDirection(s.uv);

    float sinTheta = sqrt(max(1.0 - s.dir.y * s.dir.y, 1e-6));

    float margPrev = (vIdx > 0) ? texelFetch(textures[nonuniformEXT(marginalIdx)], ivec2(vIdx - 1, 0), 0).r : 0.0;
    float margCurr = texelFetch(textures[nonuniformEXT(marginalIdx)], ivec2(vIdx, 0), 0).r;
    float pdfV = max(margCurr - margPrev, 1e-8);

    float condPrev = (uIdx > 0) ? texelFetch(textures[nonuniformEXT(conditionalIdx)], ivec2(uIdx - 1, vIdx), 0).r : 0.0;
    float condCurr = texelFetch(textures[nonuniformEXT(conditionalIdx)], ivec2(uIdx, vIdx), 0).r;
    float pdfU = max(condCurr - condPrev, 1e-8);

    float texelDensityScale = float(condW) * float(marginalLen);
    s.pdf = (pdfV * pdfU * texelDensityScale) /
            (sinTheta * 2.0 * PI * PI);
    s.radiance = envMapRadiance(s.dir);

    return s;
}

// ---- Include shared surface history + GI reservoir helpers + ray query ------
// These must be included BEFORE giSecondaryNEE (which calls giTraceShadow).
#include "surface_history_shared.glsl"
#include "material_resolve.glsl"
#include "restir_gi_shared.glsl"
#include "ray_query_scene.glsl"

// ---- Secondary NEE (ray-query based) ----------------------------------------
// Evaluates one direct-light NEE sample at a secondary surface hit, using ray
// queries for visibility (not traceRayEXT). Stochastic triangle/env selection,
// matching the existing path tracer's computeNEE without MIS power/heuristic
// (the GI sample terminates after this one NEE estimate).
//
// Returns the estimated direct radiance contribution at the secondary surface:
//   brdf_sec * Le * NdotL_sec * wLight / pdfOmega
// where brdf_sec is the diffuse-only BRDF at the secondary surface.
//
// shadowSeed is a dedicated salted substream for shadow alpha decisions.
struct GINEEResult
{
    vec3  radiance;       // direct radiance contribution
    bool  valid;          // false = no contribution (back-facing, occluded, etc.)
};

GINEEResult giSecondaryNEE(vec3 P, vec3 N, vec3 wo,
                           vec3 baseColor, float metallic,
                           inout uint rngState, uint shadowSeed)
{
    GINEEResult result;
    result.radiance = vec3(0.0);
    result.valid = false;

    float pTri = computePTri();
    bool hasTri = (pTri > 0.0);
    bool hasEnv = (pTri < 1.0);

    // Stochastic NEE selection (triangle OR env).
    bool sampleTriangle = false;
    bool sampleEnv = false;
    if (hasTri && hasEnv)
    {
        if (randomFloat(rngState) < pTri)
            sampleTriangle = true;
        else
            sampleEnv = true;
    }
    else if (hasTri)
        sampleTriangle = true;
    else if (hasEnv)
        sampleEnv = true;

    if (sampleTriangle)
    {
        if (lightCount == 0u || totalLightArea <= 0.0)
            return result;

        uint lightIdx = pcg(rngState) % lightCount;
        TriangleLight light = lights[lightIdx];

        uvec4 meshInfo = instanceMeshInfo[light.ids.x];
        uint idxBase = meshInfo.y + light.ids.y * 3u;
        uint i0 = indices[idxBase + 0u];
        uint i1 = indices[idxBase + 1u];
        uint i2 = indices[idxBase + 2u];
        mat4 lightWorld = instanceTransforms[light.ids.x];
        vec3 lp0 = vec3(lightWorld * vertices[meshInfo.x + i0]);
        vec3 lp1 = vec3(lightWorld * vertices[meshInfo.x + i1]);
        vec3 lp2 = vec3(lightWorld * vertices[meshInfo.x + i2]);

        float r1 = randomFloat(rngState);
        float r2 = randomFloat(rngState);
        float sqrtR1 = sqrt(r1);
        float b0 = 1.0 - sqrtR1;
        float b1 = sqrtR1 * (1.0 - r2);
        float b2 = sqrtR1 * r2;
        vec3 lightPoint = b0 * lp0 + b1 * lp1 + b2 * lp2;

        vec3 lightN = normalize(cross(lp1 - lp0, lp2 - lp0));

        vec3 toLight = lightPoint - P;
        float dist = length(toLight);
        vec3 L = toLight / max(dist, 1e-6);

        float NdotL = dot(N, L);
        float LNdotL = dot(lightN, -L);

        if (NdotL <= 0.0 || LNdotL <= 0.0)
            return result;

        vec3 brdf = evalDiffuseBRDF(wo, L, N, baseColor, metallic);
        if (dot(brdf, brdf) <= 0.0)
            return result;

        // Shadow ray query (with alpha traversal).
        bool unoccluded = giTraceShadow(P + N * 0.001, L, dist, shadowSeed);
        if (!unoccluded)
            return result;

        vec3 Le = light.emission_area.xyz;
        uint emissiveTexIdx = light.ids.w;
        if (emissiveTexIdx != 0xFFFFFFFFu)
        {
            vec2 luv0 = uvs[meshInfo.w + i0].xy;
            vec2 luv1 = uvs[meshInfo.w + i1].xy;
            vec2 luv2 = uvs[meshInfo.w + i2].xy;
            vec2 lightUV = b0 * luv0 + b1 * luv1 + b2 * luv2;
            Le *= texture(textures[nonuniformEXT(int(emissiveTexIdx))], lightUV).rgb;
        }
        Le *= camera.apertureFocal.w;

        float lightArea = light.emission_area.w;
        float pdfA = (1.0 / float(lightCount)) * (1.0 / lightArea);
        float pdfOmega = pdfA * (dist * dist) / abs(LNdotL);

        // No MIS for GI secondary (single NEE estimate, path terminates).
        float wLight = 1.0;
        vec3 direct = brdf * Le * NdotL * wLight / pdfOmega;

        // Divide by triangle NEE selection probability (stochastic NEE).
        if (hasTri && hasEnv)
            direct /= pTri;

        result.radiance = direct;
        result.valid = true;
        return result;
    }

    if (sampleEnv)
    {
        EnvSample envS = sampleEnvMap(rngState);
        if (envS.pdf <= 0.0)
            return result;

        vec3 L = envS.dir;
        float NdotL = dot(N, L);
        if (NdotL <= 0.0)
            return result;

        vec3 brdf = evalDiffuseBRDF(wo, L, N, baseColor, metallic);
        if (dot(brdf, brdf) <= 0.0)
            return result;

        // Shadow ray query (with alpha traversal).
        bool unoccluded = giTraceShadow(P + N * 0.001, L, 1e9, shadowSeed);
        if (!unoccluded)
            return result;

        vec3 Le = envS.radiance;
        float pdfOmega = envS.pdf;

        // No MIS for GI secondary (single NEE estimate, path terminates).
        float wLight = 1.0;
        vec3 direct = brdf * Le * NdotL * wLight / pdfOmega;

        // Divide by env NEE selection probability (stochastic NEE).
        if (hasTri && hasEnv)
            direct /= (1.0 - pTri);

        result.radiance = direct;
        result.valid = true;
        return result;
    }

    return result;
}

// ---- Salted substream derivation (shared by temporal + history re-evaluation) ----
// From a root seed, derive independent substreams for alpha, NEE, and shadow
// decisions. Replaying the root seed reproduces the exact same stochastic choices.
uint deriveAlphaSeed(uint rootSeed) { return rootSeed ^ 0xA5A5A5A5u; }
uint deriveNEESeed(uint rootSeed)   { return rootSeed ^ 0x5A5A5A5Au; }
uint deriveShadowSeed(uint rootSeed, uint neeSeed)
{ return (rootSeed ^ neeSeed) * 747796405u + 2891336453u; }

// ---- GI radiance evaluation (shared by fresh generation + temporal re-eval) ----
// Traces a radiance ray along wi from worldPos, evaluates Lo (environment miss /
// emissive hit / NEE at secondary), and returns Lo + hitT + flags.
// This is the core "evaluate the sample" operation used by both fresh candidate
// generation and temporal history re-evaluation.
struct GIRadianceResult
{
    vec3  Lo;
    float hitT;
    uint  flags;
};

GIRadianceResult giEvaluateRadiance(vec3 worldPos, vec3 n, vec3 wo,
                                     vec3 wi, float cosTheta, uint rootSeed)
{
    GIRadianceResult result;
    result.Lo = vec3(0.0);
    result.hitT = 0.0;
    result.flags = 0u;

    uint alphaSeed = deriveAlphaSeed(rootSeed);
    RayQueryHit hit = giTraceRadiance(worldPos + n * 0.001, wi, 0.001, 1e9,
                                      alphaSeed);

    if (hit.isEnvironment)
    {
        result.Lo = envMapRadiance(wi);
        result.hitT = 1e4;
        result.flags = GI_FLAG_VALID | GI_FLAG_ENV_MISS;
    }
    else if (hit.committed)
    {
        result.hitT = hit.hitT;
        result.flags = GI_FLAG_VALID | GI_FLAG_GEOMETRY_HIT;

        vec3 secN = hit.frontFace ? hit.shadingNormal : -hit.shadingNormal;
        vec3 secWo = -wi;
        float secNdotV = max(dot(secN, secWo), 0.0);
        if (secNdotV <= 0.0)
            return result;

        vec3 secBaseColor = hit.baseColor;
        float secMetallic = hit.metallic;
        float secRoughness = hit.roughness;

        // Emissive contribution at the secondary surface.
        vec3 emission = hit.emissive;
        if (dot(emission, emission) > 0.0)
        {
            float boost = camera.apertureFocal.w;
            emission *= boost;
            result.Lo += emission;
            result.flags |= GI_FLAG_NEE_VALID;
        }

        // One direct-light NEE sample at the secondary surface.
        float secDiffuseWeight = (1.0 - secMetallic)
                               * max(secBaseColor.x, max(secBaseColor.y, secBaseColor.z));
        if (secDiffuseWeight > 1e-4)
        {
            uint neeSeed = deriveNEESeed(rootSeed);
            uint shadowSeed = deriveShadowSeed(rootSeed, neeSeed);
            uint neeRngState = neeSeed;
            pcg(neeRngState);

            GINEEResult nee = giSecondaryNEE(hit.worldPos, secN, secWo,
                                             secBaseColor, secMetallic,
                                             neeRngState, shadowSeed);
            if (nee.valid)
            {
                result.Lo += nee.radiance;
                result.flags |= GI_FLAG_NEE_VALID;
            }
        }
    }

    return result;
}

// ---- GI receiver history load/store (monolithic buffer) ---------------------
SurfaceHistory loadGIReceiverHistory(uint region, uint pixelLinear, uint pixelCount)
{
    uint byteOffset = giReceiverHistoryRegionByteOffset(region, pixelCount)
                    + pixelLinear * GI_RECEIVER_HISTORY_BYTES;
    uint wordOffset = byteOffset / 16u;
    SurfaceHistory sh;
    sh.data0 = giData[wordOffset + 0u];
    sh.data1 = giData[wordOffset + 1u];
    return sh;
}

void storeGIReceiverHistory(uint region, uint pixelLinear, uint pixelCount,
                            SurfaceHistory sh)
{
    uint byteOffset = giReceiverHistoryRegionByteOffset(region, pixelCount)
                    + pixelLinear * GI_RECEIVER_HISTORY_BYTES;
    uint wordOffset = byteOffset / 16u;
    giData[wordOffset + 0u] = sh.data0;
    giData[wordOffset + 1u] = sh.data1;
}

// ---- GI reservoir load/store (monolithic buffer) -----------------------------
GIReservoir loadGIReservoirCompute(uint region, uint pixelLinear, uint pixelCount)
{
    uint byteOffset = giReservoirRegionByteOffset(region, pixelCount)
                    + pixelLinear * GI_RESERVOIR_BYTES;
    uint wordOffset = byteOffset / 16u;
    GIReservoir r;
    r.data0 = giData[wordOffset + 0u];
    r.data1 = giData[wordOffset + 1u];
    r.data2 = giData[wordOffset + 2u];
    return r;
}

void storeGIReservoirCompute(uint region, uint pixelLinear, uint pixelCount,
                              GIReservoir r)
{
    uint byteOffset = giReservoirRegionByteOffset(region, pixelCount)
                    + pixelLinear * GI_RESERVOIR_BYTES;
    uint wordOffset = byteOffset / 16u;
    giData[wordOffset + 0u] = r.data0;
    giData[wordOffset + 1u] = r.data1;
    giData[wordOffset + 2u] = r.data2;
}