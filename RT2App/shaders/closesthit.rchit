#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : require

#include "pathtracer_shared.glsl"

layout(location = 0) rayPayloadInEXT RayPayload payload;
layout(location = 1) rayPayloadEXT RayPayload nextPayload;

vec3 hitNormal()
{
    uint offset = normalOffsets[gl_InstanceID];
    vec4 n = triangleNormals[offset + uint(gl_PrimitiveID)];
    return normalize(n.xyz);
}

vec2 hitUV()
{
    uint offset = normalOffsets[gl_InstanceID];
    vec4 uv = triangleUVs[offset + uint(gl_PrimitiveID)];
    return uv.xy;
}

void main()
{
    // DEBUG SHADER #2: Display centroid UVs as colors.
    // R = U (left→right), G = V (bottom→top), B = 0
    vec2 uv = hitUV();
    payload.b.xyz = vec3(uv, 0.0);
    payload.c.w = 1.0; // done
}