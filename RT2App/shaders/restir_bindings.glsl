// restir_bindings.glsl — shared bindings for ReSTIR compute passes.
//
// Included by restir_temporal.comp and restir_spatial.comp.
// Declares all set 0 and set 1 bindings needed by the ReSTIR passes,
// plus the RNG, luminance, and NRD decode helpers.
// Does NOT include GL_EXT_ray_tracing (compute-only).
//
// Also includes restir_shared.glsl for reservoir operations.

#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require

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

// ---- Instance offsets (set 0, binding 5) ------------------------------------
layout(set = 0, binding = SI_BINDING_INSTANCE_OFFSETS, std430) readonly buffer InstanceNormalOffsets
{
    uint normalOffsets[];
};

// ---- UV buffer (set 0, binding 7) -------------------------------------------
layout(set = 0, binding = SI_BINDING_UV_BUFFER, std430) readonly buffer UVBuffer
{
    vec4 triangleUVs[];
};

// ---- Position buffer (set 0, binding 8) -------------------------------------
layout(set = 0, binding = SI_BINDING_POSITION_BUFFER, std430) readonly buffer PositionBuffer
{
    vec4 trianglePositions[];
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

// ---- Instance transforms (set 0, binding 10) --------------------------------
layout(set = 0, binding = SI_BINDING_INSTANCE_TRANSFORMS, std430) readonly buffer InstanceTransforms
{
    mat4 instanceTransforms[];
};

// ---- Texture array (set 0, binding 11) -------------------------------------
layout(set = 0, binding = SI_BINDING_TEXTURE_ARRAY) uniform sampler2D textures[];

// ---- Reservoir buffers (set 0, bindings 14-15) -----------------------------
// Declared by each pass with the appropriate access qualifiers:
//   temporal: history=readonly, scratch=write
//   spatial:  scratch=readonly, history=write
// Each pass declares these before including restir_bindings.glsl, OR
// restir_bindings.glsl declares defaults and the pass overrides.
// Default: both read-write (works for both passes since they don't alias).

// ---- Surface history buffer (set 0, binding 16) -----------------------------
// Declared by each pass with the appropriate access qualifier.

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
layout(push_constant) uniform Push
{
    SIReSTIRPushConstants restir;
} pc;

// ---- RNG + helpers (same as ris.comp) ---------------------------------------

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

// Diffuse BRDF evaluation (same as pathtracer_shared.glsl)
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

// Probability of selecting triangle NEE (vs env NEE)
float computePTri()
{
    bool hasTri = (lightCount > 0u && totalLightArea > 0.0);
    bool hasEnv = (int(camera.envMap.x) >= 0);
    if (hasTri && hasEnv) return 0.5;
    if (hasTri)            return 1.0;
    return 0.0;
}

// Environment map helpers (from pathtracer_shared.glsl)
vec2 directionToEnvUV(vec3 dir)
{
    float u = atan(dir.z, dir.x) * 0.15915494309;
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
    float uWrapped = fract(uv.x);
    int vIdx = int(clamp(uv.y * float(marginalLen), 0.0, float(marginalLen - 1)));
    int uIdx = int(clamp(uWrapped * float(condW),   0.0, float(condW - 1)));

    float margPrev = (vIdx > 0) ? texelFetch(textures[nonuniformEXT(marginalIdx)], ivec2(vIdx - 1, 0), 0).r : 0.0;
    float margCurr = texelFetch(textures[nonuniformEXT(marginalIdx)], ivec2(vIdx, 0), 0).r;
    float pdfV = max(margCurr - margPrev, 1e-8);

    float condPrev = (uIdx > 0) ? texelFetch(textures[nonuniformEXT(conditionalIdx)], ivec2(uIdx - 1, vIdx), 0).r : 0.0;
    float condCurr = texelFetch(textures[nonuniformEXT(conditionalIdx)], ivec2(uIdx, vIdx), 0).r;
    float pdfU = max(condCurr - condPrev, 1e-8);

    float sinTheta = sqrt(max(1.0 - dir.y * dir.y, 1e-6));
    return (pdfV * pdfU) / (sinTheta * 2.0 * PI * PI);
}

// Environment map inverse-CDF sampling
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

    vec2 envSize = vec2(float(condW), float(marginalLen));
    float sinTheta = sqrt(max(1.0 - s.dir.y * s.dir.y, 1e-6));

    float margPrev = (vIdx > 0) ? texelFetch(textures[nonuniformEXT(marginalIdx)], ivec2(vIdx - 1, 0), 0).r : 0.0;
    float margCurr = texelFetch(textures[nonuniformEXT(marginalIdx)], ivec2(vIdx, 0), 0).r;
    float pdfV = max(margCurr - margPrev, 1e-8);

    float condPrev = (uIdx > 0) ? texelFetch(textures[nonuniformEXT(conditionalIdx)], ivec2(uIdx - 1, vIdx), 0).r : 0.0;
    float condCurr = texelFetch(textures[nonuniformEXT(conditionalIdx)], ivec2(uIdx, vIdx), 0).r;
    float pdfU = max(condCurr - condPrev, 1e-8);

    s.pdf = (pdfV * pdfU) / (sinTheta * 2.0 * PI * PI);
    s.radiance = envMapRadiance(s.dir);

    return s;
}

// ---- Include restir_shared.glsl AFTER all bindings are declared -------------
// restir_shared.glsl references lights, textures, instanceTransforms, etc.
#include "restir_shared.glsl"