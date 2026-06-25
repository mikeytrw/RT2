#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : require

#include "pathtracer_shared.glsl"

layout(location = 0) rayPayloadInEXT RayPayload payload;

void main()
{
    // Miss = sky gradient
    vec3 dir = gl_WorldRayDirectionEXT;
    float t = 0.5 * (dir.y + 1.0);
    payload.b.xyz = (1.0 - t) * vec3(1.0, 1.0, 1.0) + t * vec3(0.5, 0.7, 1.0);
}