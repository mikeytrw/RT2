#version 460
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require

#include "shader_interface.h"

// Camera UBO (set 0, binding 1)
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

// Material buffer (set 0, binding 2)
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

// Bindless textures (set 0, binding 11)
layout(set = 0, binding = SI_BINDING_TEXTURE_ARRAY) uniform sampler2D textures[];

// G-buffer outputs (MRT color attachments — rasterization-ordered, no imageStore race)
// Location mapping: 0=gNormalRoughness, 1=gViewZ, 2=gMotion, 3=gAlbedoF0,
//                   4=gDirectEmission, 5=gPrimHit, 6=gPrimGeoNormal, 7=gPrimUV
layout(location = 0) out vec4 outNormalRoughness;   // rgba8
layout(location = 1) out vec4 outViewZ;             // r32f
layout(location = 2) out vec4 outMotion;            // rg16f
layout(location = 3) out vec4 outAlbedoF0;          // rgba16f
layout(location = 4) out vec4 outDirectEmission;    // rgba16f
layout(location = 5) out vec4 outPrimHit;           // rgba32f: xyz=worldPos, w=floatBitsToInt(matIdx)
layout(location = 6) out vec4 outPrimGeoNormal;     // rgba8
layout(location = 7) out vec4 outPrimUV;            // rg16f

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inWorldPosPrev;
layout(location = 2) in vec2 inUV;
layout(location = 3) flat in uint inInstanceIndex;
layout(location = 4) in vec3 inWorldTangent;

// Material index is passed via the instance's custom index.
// We store it in a separate SSBO or derive from instance data.
// For now, use a push constant or instance buffer.
// Actually, we need the material index per instance. The BLASInstance has
// customIndex = materialIndex, but that's for RT. For raster, we need
// a separate instance data SSBO mapping instance index → material index.
layout(set = 0, binding = 13, std430) readonly buffer InstanceMaterialIndices
{
    uint instanceMaterialIndices[];
};

layout(set = 0, binding = 17, std430) readonly buffer InstanceMatOffsets
{
    uint instanceMatOffsets[];
};

// NRD oct-packing (matches NRD_NORMAL_ENCODING=2, NRD_ROUGHNESS_ENCODING=1)
// Ported from NRD.hlsli _NRD_EncodeNormalRoughness101010.
vec3 nrdEncodeNormalRoughness(vec3 n, float roughness)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec3 r;
    r.y = n.y * 0.5 + 0.5;
    r.x = n.x * 0.5 + r.y;
    r.y -= n.x * 0.5;
    roughness = max(roughness, 1.5 / 512.0);
    float s = n.z < 0.0 ? -roughness : roughness;
    r.z = s * 0.5 + 0.5;
    return r;
}

void main()
{
    uint matIdx = instanceMaterialIndices[instanceMatOffsets[inInstanceIndex] + uint(gl_PrimitiveID)];
    Material mat = materials[matIdx];

    vec2 uv = inUV;

    // Base color
    vec3 baseColor = mat.baseColor_metallic.xyz;
    int baseColorTexIdx = mat.textureIndices.x;
    if (baseColorTexIdx >= 0)
        baseColor *= texture(textures[nonuniformEXT(baseColorTexIdx)], uv).rgb;

    // Alpha test for MASK materials
    if (mat.alphaMode > 0.5 && mat.alphaMode < 1.5)
    {
        float alpha = mat.baseAlpha;
        if (baseColorTexIdx >= 0)
            alpha *= texture(textures[nonuniformEXT(baseColorTexIdx)], uv).a;
        if (alpha < mat.alphaCutoff)
            discard;
    }

    float metallic = mat.baseColor_metallic.w;
    float roughness = mat.emissive_roughness.w;

    // Metallic-roughness texture: glTF convention — G=roughness, B=metallic
    int metalRoughTexIdx = mat.extraIndices.x;
    if (metalRoughTexIdx >= 0)
    {
        vec3 mr = texture(textures[nonuniformEXT(metalRoughTexIdx)], uv).rgb;
        roughness *= mr.g;
        metallic *= mr.b;
    }

    // Geometric normal from screen-space derivatives (flat, matches RT path)
    vec3 geoN = normalize(cross(dFdx(inWorldPos), dFdy(inWorldPos)));

    // Shading normal (normal-mapped or geometric)
    vec3 shadingN = geoN;
    int normalTexIdx = mat.textureIndices.y;
    if (normalTexIdx >= 0)
    {
        // Use precomputed vertex tangent (transformed to world space in vert shader)
        vec3 T = normalize(inWorldTangent - dot(inWorldTangent, geoN) * geoN);
        vec3 B = cross(geoN, T);

        vec3 tangentN = texture(textures[nonuniformEXT(normalTexIdx)], uv).rgb * 2.0 - 1.0;
        shadingN = normalize(mat3(T, B, geoN) * tangentN);
    }

    // Emissive
    vec3 emissive = mat.emissive_roughness.xyz;
    int emissiveTexIdx = mat.textureIndices.z;
    if (emissiveTexIdx >= 0)
        emissive *= texture(textures[nonuniformEXT(emissiveTexIdx)], uv).rgb;
    float boost = camera.apertureFocal.w;
    emissive *= boost;

    // F0 (Fresnel reflectance at normal incidence) — vec3 for colored metals
    vec3 F0 = mix(vec3(0.04), baseColor, metallic);
    // Store albedo.rgb + metallic in gAlbedoF0 for NRD material factor computation.
    // Compose pass reconstructs Rf0 = mix(0.04, albedo, metallic) and computes
    // NRD_MaterialFactors(N, V, albedo, Rf0, roughness).
    vec3 diffAlbedo = baseColor;

    // View-space Z
    vec4 viewPos = camera.worldToView * vec4(inWorldPos, 1.0);
    float viewZ = viewPos.z;

    // Motion vector: reproject world position into previous and current screen space
    vec4 currClip = camera.viewToClip * viewPos;
    vec4 prevView = camera.worldToViewPrev * vec4(inWorldPosPrev, 1.0);
    vec4 prevClip = camera.viewToClipPrev * prevView;
    vec2 currUv = (currClip.xy / currClip.w) * 0.5 + 0.5;
    vec2 prevUv = (prevClip.xy / prevClip.w) * 0.5 + 0.5;

    // Write G-buffer via MRT color attachments (rasterization-ordered, no race)
    // matIdx sentinel: store matIdx + 1 in gPrimHit.w (0 = miss/sky, used by secondary_raygen)
    outPrimHit = vec4(inWorldPos, uintBitsToFloat(matIdx + 1u));
    outPrimGeoNormal = vec4(geoN * 0.5 + 0.5, 0.0);
    outPrimUV = vec4(uv, 0.0, 0.0);

    // Emissive special-casing: match RT closesthit's emissive path (lines 351-363).
    // Emissive surfaces terminate — no bounce lighting, no NRD demod.
    // Override G-buffer to signal "direct emission only" to secondary_raygen + NRD.
    if (dot(emissive, emissive) > 0.0)
    {
        vec3 octE = nrdEncodeNormalRoughness(geoN, 1.0);
        outNormalRoughness = vec4(octE, 1.0);  // oct-packed geo normal, roughness=1.0
        outViewZ = vec4(viewZ, 0.0, 0.0, 0.0);
        outMotion = vec4(0.0, 0.0, 0.0, 0.0);              // zero motion (emissive bypasses NRD)
        outAlbedoF0 = vec4(1.0, 1.0, 1.0, 1.0);            // white albedo, metallic=1 (no demod)
        outDirectEmission = vec4(emissive, 0.0);
        return;
    }

    // NRD expects oct-packed normal + linear roughness (encoding=2, LINEAR)
    vec3 octNR = nrdEncodeNormalRoughness(shadingN, roughness);
    outNormalRoughness = vec4(octNR, 0.0);
    outViewZ = vec4(viewZ, 0.0, 0.0, 0.0);
    outMotion = vec4(prevUv - currUv, 0.0, 0.0);
    outAlbedoF0 = vec4(diffAlbedo, metallic);
    outDirectEmission = vec4(0.0);  // non-emissive: no direct emission
}