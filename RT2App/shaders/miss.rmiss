#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : require

#include "pathtracer_shared.glsl"

layout(location = 0) rayPayloadInEXT RayPayload payload;

void main()
{
    // Miss = sky gradient. Add sky radiance * throughput to accumulated radiance.
    vec3 dir = normalize(gl_WorldRayDirectionEXT);
    vec3 sky = skyColor(dir);
    payload.b.xyz += payload.a.xyz * sky;
    payload.c.w = 1.0; // done
}