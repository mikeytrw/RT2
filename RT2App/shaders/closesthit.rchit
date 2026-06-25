#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : require

#include "pathtracer_shared.glsl"

layout(location = 0) rayPayloadInEXT RayPayload payload;

void main()
{
    // Shade by geometric normal
    uint primitiveIndex = gl_PrimitiveID;
    vec3 normal = triangleNormals[primitiveIndex].xyz;
    vec3 n = normalize(normal) * 0.5 + 0.5;
    payload.b.xyz = n;
}