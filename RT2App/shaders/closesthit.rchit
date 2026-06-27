#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require

#include "pathtracer_shared.glsl"

layout(location = 0) rayPayloadInEXT RayPayload payload;
layout(location = 1) rayPayloadEXT RayPayload nextPayload;

vec3 hitNormal()
{
    uint offset = normalOffsets[gl_InstanceID];
    vec4 n = triangleNormals[offset + uint(gl_PrimitiveID)];
    return normalize(n.xyz);
}

// Compute barycentric-interpolated UV at the hit point.
// Uses stored per-triangle vertex positions + UVs.
vec2 hitUV()
{
    uint triIdx = normalOffsets[gl_InstanceID] + uint(gl_PrimitiveID);
    uint posIdx = triIdx * 3u;

    vec3 p0 = trianglePositions[posIdx + 0u].xyz;
    vec3 p1 = trianglePositions[posIdx + 1u].xyz;
    vec3 p2 = trianglePositions[posIdx + 2u].xyz;

    vec2 uv0 = triangleUVs[posIdx + 0u].xy;
    vec2 uv1 = triangleUVs[posIdx + 1u].xy;
    vec2 uv2 = triangleUVs[posIdx + 2u].xy;

    // Hit point in world space
    vec3 rayDir = normalize(gl_WorldRayDirectionEXT);
    vec3 hitPoint = gl_WorldRayOriginEXT + gl_HitTEXT * rayDir;

    // Compute barycentric coordinates using the area method
    vec3 edge0 = p1 - p0;
    vec3 edge1 = p2 - p0;
    vec3 edge2 = hitPoint - p0;

    float d00 = dot(edge0, edge0);
    float d01 = dot(edge0, edge1);
    float d11 = dot(edge1, edge1);
    float d20 = dot(edge2, edge0);
    float d21 = dot(edge2, edge1);
    float denom = d00 * d11 - d01 * d01;

    float v = (d11 * d20 - d01 * d21) / denom;
    float w = (d00 * d21 - d01 * d20) / denom;
    float u = 1.0 - v - w;

    return u * uv0 + v * uv1 + w * uv2;
}

void main()
{
    uint matIdx = gl_InstanceCustomIndexEXT;
    Material mat = materials[matIdx];

    // DEBUG SHADER #3: Sample base color texture at barycentric-interpolated UV.
    int texIdx = mat.textureIndices.x;
    vec3 color;
    if (texIdx >= 0)
    {
        vec2 uv = hitUV();
        color = texture(textures[nonuniformEXT(texIdx)], uv).rgb * mat.baseColor_metallic.xyz;
    }
    else
    {
        color = mat.baseColor_metallic.xyz;
    }

    payload.b.xyz = color;
    payload.c.w = 1.0; // done
}