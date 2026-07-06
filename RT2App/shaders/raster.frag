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

// Instance offset buffer (set 0, binding 5) — maps instance index to BLAS mesh index
layout(set = 0, binding = SI_BINDING_INSTANCE_OFFSETS, std430) readonly buffer InstanceOffsets
{
    uint normalOffsets[];
};

// Bindless textures (set 0, binding 11)
layout(set = 0, binding = SI_BINDING_TEXTURE_ARRAY) uniform sampler2D textures[];

// G-buffer outputs (set 1 — color attachments via dynamic rendering)
layout(set = 1, binding = 0, rgba8) uniform image2D gNormalRoughness;
layout(set = 1, binding = 1, r16f) uniform image2D gViewZ;
layout(set = 1, binding = 2, rg16f) uniform image2D gMotion;
layout(set = 1, binding = 5, rgba16f) uniform image2D gAlbedoF0;
layout(set = 1, binding = 7, rgba16f) uniform image2D gDirectEmission;

// New: primary hit data for path tracer
layout(set = 1, binding = 8, rgba32f) uniform image2D gPrimHit;       // xyz = world pos, w = material index
layout(set = 1, binding = 9, rgba8) uniform image2D gPrimGeoNormal;   // xyz = geo normal (0.5+0.5), w = unused
layout(set = 1, binding = 10, rg16f) uniform image2D gPrimUV;         // xy = UV at primary hit

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inWorldPosPrev;
layout(location = 2) in vec2 inUV;
layout(location = 3) flat in uint inInstanceIndex;

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

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    uint matIdx = instanceMaterialIndices[inInstanceIndex];
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
        // Need tangent — compute from screen-space derivatives of UV and position
        vec3 dp1 = dFdx(inWorldPos);
        vec3 dp2 = dFdy(inWorldPos);
        vec2 duv1 = dFdx(uv);
        vec2 duv2 = dFdy(uv);

        vec3 dp2perp = cross(geoN, dp2);
        vec3 dp1perp = cross(dp1, geoN);
        vec3 T = normalize(dp2perp * duv1.x + dp1perp * duv2.x);
        // Gram-Schmidt orthogonalize
        T = normalize(T - dot(T, geoN) * geoN);
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

    // F0
    float f0Scalar = max(mix(0.04, dot(baseColor, vec3(0.2126, 0.7152, 0.0722)), metallic), 0.01);
    vec3 diffFactor = max(baseColor * (1.0 - metallic), vec3(0.01));

    // View-space Z
    vec4 viewPos = camera.worldToView * vec4(inWorldPos, 1.0);
    float viewZ = viewPos.z;

    // Motion vector: reproject world position into previous and current screen space
    vec4 currClip = camera.viewToClip * viewPos;
    vec4 prevView = camera.worldToViewPrev * vec4(inWorldPosPrev, 1.0);
    vec4 prevClip = camera.viewToClipPrev * prevView;
    vec2 currUv = (currClip.xy / currClip.w) * 0.5 + 0.5;
    vec2 prevUv = (prevClip.xy / prevClip.w) * 0.5 + 0.5;

    // Write G-buffer
    imageStore(gNormalRoughness, pixel, vec4(shadingN * 0.5 + 0.5, roughness));
    imageStore(gViewZ, pixel, vec4(viewZ, 0.0, 0.0, 0.0));
    imageStore(gMotion, pixel, vec4(prevUv - currUv, 0.0, 0.0));
    imageStore(gAlbedoF0, pixel, vec4(diffFactor, f0Scalar));
    imageStore(gDirectEmission, pixel, vec4(emissive, 0.0));
    imageStore(gPrimHit, pixel, vec4(inWorldPos, floatBitsToInt(matIdx)));
    imageStore(gPrimGeoNormal, pixel, vec4(geoN * 0.5 + 0.5, 0.0));
    imageStore(gPrimUV, pixel, vec4(uv, 0.0, 0.0));
}