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

        // ReSTIR DI replaces only the primary diffuse direct-light estimator.
        // Keep rough-specular environment reflection: there is no competing
        // ReSTIR specular estimator, and scatter attenuation already includes
        // stochastic lobe-selection compensation.
        bool restirFirstBounce = camera.up.w > 0.5 &&
                                 uint(payload.b.w) == 1u &&
                                 bsdfPdf >= 0.0;
        bool selectedDiffuse = payload.e.x < 0.5;

        if (restirFirstBounce && selectedDiffuse)
        {
            // ReSTIR replaces primary diffuse environment NEE.
        }
        else if (restirFirstBounce)
        {
            payload.b.xyz += payload.a.xyz * env;
        }
        else if (bsdfPdf >= 0.0)
        {
            // Non-delta bounce: MIS with env PDF.
            // Stochastic NEE: env NEE is selected with probability (1-P_tri),
            // so the effective combined NEE pdf for a direction that misses
            // all geometry is (1-P_tri) * pdfEnv.
            float pdfEnv = envMapPdf(dir);
            float pTri = computePTri();
            float weight = bsdfPdf / (bsdfPdf + (1.0 - pTri) * pdfEnv);
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
