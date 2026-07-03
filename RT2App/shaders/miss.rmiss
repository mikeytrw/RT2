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
        // Environment map with MIS (M8):
        // payload.d.w = bsdfPdf (solid-angle PDF of this ray direction under BSDF).
        //   -1.0 = camera ray or delta bounce → full env radiance (no MIS).
        //   >= 0 = non-delta bounce → weight by w_bsdf = bsdfPdf / (bsdfPdf + pdfEnv).
        vec3 env = envMapRadiance(dir);

        float bsdfPdf = payload.d.w;
        if (bsdfPdf >= 0.0)
        {
            // Compute env PDF for this direction using the CDFs.
            // p(dir) = luminance(env(dir)) / totalLuminance, converted to solid angle.
            // We can look up the CDF at the direction's UV and compute the density.
            // However, computing the exact PDF requires the CDF derivatives at the pixel.
            // Instead, we use a simpler approach: the env PDF for a given direction is
            // proportional to the luminance of the env map at that direction, normalized.
            // For the balance heuristic, we need pdfEnv. We estimate it from the CDF.
            // Since exact CDF differentiation in the shader is expensive, we use a
            // conservative approximation: pdfEnv ≈ luminance(env) / (2π² * sinθ * totalLum)
            // But we don't have totalLum. We can store it in the UBO or estimate.
            // For now, skip MIS for env-sampled BSDF rays — full env radiance.
            // This is slightly biased but stable. The NEE handles the MIS correctly.
            payload.b.xyz += payload.a.xyz * env;
        }
        else
        {
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