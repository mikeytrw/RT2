#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : require

#include "pathtracer_shared.glsl"

layout(location = 0) rayPayloadInEXT RayPayload payload;

void main()
{
    // Background: sky gradient if enabled, black if disabled.
    float showBg = camera.apertureFocal.z;
    if (showBg > 0.5)
    {
        vec3 dir = normalize(gl_WorldRayDirectionEXT);
        vec3 sky = skyColor(dir);
        payload.b.xyz += payload.a.xyz * sky;
    }
    // else: black background — add nothing
    payload.c.w = 1.0; // done
}