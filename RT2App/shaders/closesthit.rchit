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

void main()
{
    uint matIdx = gl_InstanceCustomIndexEXT;
    Material mat = materials[matIdx];

    vec3 baseColor = mat.baseColor_metallic.xyz;
    float metallic = mat.baseColor_metallic.w;
    float roughness = mat.emissive_roughness.w;
    vec3 emissive = mat.emissive_roughness.xyz;
    float ior = mat.ior;

    // Geometric normal from hit triangle
    vec3 normal = hitNormal();

    // If emissive, add emissive contribution and stop
    if (dot(emissive, emissive) > 0.0)
    {
        payload.b.xyz += payload.a.xyz * emissive;
        payload.c.w = 1.0; // done
        return;
    }

    // Determine if we're hitting the front face
    vec3 rayDir = normalize(gl_WorldRayDirectionEXT);
    bool frontFace = dot(rayDir, normal) < 0.0;
    vec3 n = frontFace ? normal : -normal;

    uint rngState = floatBitsToUint(payload.a.w);
    uint depth = uint(payload.b.w);
    uint maxBounces = uint(camera.viewportSPP.w);

    // Simplified PBR scatter:
    //   metallic ≈ 0 → diffuse (Lambertian cosine hemisphere)
    //   metallic ≈ 1 → specular reflection + roughness fuzz
    //   0 < metallic < 1 → lerp between diffuse and specular
    // Non-metals also get dielectric refraction via ior (Schlick fresnel)

    vec3 attenuation;
    vec3 scatterDir;
    bool doScatter = true;

    // Fresnel for dielectric (Schlick approximation)
    float cosTheta = abs(dot(-rayDir, n));
    float fresnel = reflectance(cosTheta, ior);

    // Decide: reflect or refract for dielectric
    float r = randomFloat(rngState);

    if (metallic >= 0.999)
    {
        // Pure metal: reflect + roughness fuzz
        vec3 reflected = reflect(rayDir, n);
        scatterDir = normalize(reflected) + roughness * randomInUnitSphere(rngState);
        if (dot(scatterDir, n) <= 0.0)
        {
            doScatter = false;
        }
        attenuation = baseColor;
    }
    else if (metallic <= 0.001)
    {
        // Non-metal (dielectric): use fresnel to choose reflect vs refract
        if (r < fresnel)
        {
            // Specular reflection
            vec3 reflected = reflect(rayDir, n);
            scatterDir = normalize(reflected) + roughness * randomInUnitSphere(rngState);
            attenuation = vec3(1.0); // dielectric reflection is white
        }
        else
        {
            // Diffuse (Lambertian)
            scatterDir = n + randomInUnitSphere(rngState);
            if (dot(scatterDir, n) <= 0.0)
                scatterDir = n;
            attenuation = baseColor;
        }
    }
    else
    {
        // Mixed: lerp between diffuse and metal specular by metallic factor
        if (r < metallic)
        {
            // Metal specular
            vec3 reflected = reflect(rayDir, n);
            scatterDir = normalize(reflected) + roughness * randomInUnitSphere(rngState);
            attenuation = baseColor;
        }
        else
        {
            // Diffuse
            scatterDir = n + randomInUnitSphere(rngState);
            if (dot(scatterDir, n) <= 0.0)
                scatterDir = n;
            attenuation = baseColor * (1.0 - metallic);
        }
    }

    if (!doScatter || depth >= maxBounces)
    {
        // Ray absorbed or max depth reached
        payload.c.w = 1.0; // done
        payload.a.w = uintBitsToFloat(rngState);
        return;
    }

    // Update throughput
    payload.a.xyz *= attenuation;
    payload.a.w = uintBitsToFloat(rngState);
    payload.b.w = float(depth + 1);

    // Recursively trace the scattered ray
    vec3 hitPoint = gl_WorldRayOriginEXT + gl_HitTEXT * rayDir;

    nextPayload.a = payload.a;
    nextPayload.b = vec4(vec3(0.0), float(depth + 1));
    nextPayload.c = vec4(hitPoint + n * 0.001, 0.0);
    nextPayload.d = vec4(normalize(scatterDir), 0.0);

    traceRayEXT(topLevelAS, gl_RayFlagsOpaqueEXT, 0xFF, 0, 0, 0,
                nextPayload.c.xyz, 0.001, nextPayload.d.xyz, 1e9, 0);

    // Accumulate radiance from the bounce
    payload.b.xyz += nextPayload.b.xyz;
    payload.c.w = 1.0; // done
}