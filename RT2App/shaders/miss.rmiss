#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require

#include "pathtracer_shared.glsl"

layout(location = 0) rayPayloadInEXT RayPayload payload;

void main()
{
    vec3 dir = normalize(gl_WorldRayDirectionEXT);
    int envIdx = int(camera.envMap.x);

    if (envIdx >= 0)
    {
        vec3 env = envMapRadiance(dir);
        float bsdfPdf = payload.d.w;

        if (bsdfPdf >= 0.0)
        {
            // Non-delta bounce: MIS with env PDF.
            float pdfEnv = envMapPdf(dir);
            float weight = bsdfPdf / (bsdfPdf + pdfEnv);
            payload.b.xyz += payload.a.xyz * env * weight;
        }
        else
        {
            // Camera ray or delta bounce: full env radiance, no MIS.
            payload.b.xyz += payload.a.xyz * env;
        }
    }
    else
    {
        // Fallback: sky gradient if enabled, black if disabled
        float showBg = camera.apertureFocal.z;
        if (showBg > 0.5)
        {
            vec3 sky = skyColor(dir);
            payload.b.xyz += payload.a.xyz * sky;
        }
    }
    payload.c.w = 1.0; // done
}