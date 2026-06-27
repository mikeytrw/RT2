#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require

#include "pathtracer_shared.glsl"

layout(location = 0) rayPayloadInEXT RayPayload payload;
layout(location = 1) rayPayloadEXT RayPayload nextPayload;

vec3 hitFaceNormal()
{
    uint offset = normalOffsets[gl_InstanceID];
    vec4 n = triangleNormals[offset + uint(gl_PrimitiveID)];
    return normalize(n.xyz);
}

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

    vec3 rayDir = normalize(gl_WorldRayDirectionEXT);
    vec3 hitPoint = gl_WorldRayOriginEXT + gl_HitTEXT * rayDir;

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

vec3 hitTangent()
{
    uint triIdx = normalOffsets[gl_InstanceID] + uint(gl_PrimitiveID);
    uint posIdx = triIdx * 3u;

    vec3 p0 = trianglePositions[posIdx + 0u].xyz;
    vec3 p1 = trianglePositions[posIdx + 1u].xyz;
    vec3 p2 = trianglePositions[posIdx + 2u].xyz;

    vec3 t0 = triangleTangents[posIdx + 0u].xyz;
    vec3 t1 = triangleTangents[posIdx + 1u].xyz;
    vec3 t2 = triangleTangents[posIdx + 2u].xyz;

    vec3 rayDir = normalize(gl_WorldRayDirectionEXT);
    vec3 hitPoint = gl_WorldRayOriginEXT + gl_HitTEXT * rayDir;

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

    return normalize(u * t0 + v * t1 + w * t2);
}

// Get the shading normal: geometric face normal, or normal-mapped if texture exists
vec3 hitShadingNormal(Material mat, vec2 uv)
{
    vec3 geoN = hitFaceNormal();
    int normalTexIdx = mat.textureIndices.y;

    if (normalTexIdx >= 0)
    {
        vec3 tangentN = texture(textures[nonuniformEXT(normalTexIdx)], uv).rgb * 2.0 - 1.0;
        vec3 T = hitTangent();
        vec3 N = geoN;
        T = normalize(T - dot(T, N) * N);
        vec3 B = cross(N, T);
        return normalize(mat3(T, B, N) * tangentN);
    }

    return geoN;
}

void main()
{
    uint matIdx = gl_InstanceCustomIndexEXT;
    Material mat = materials[matIdx];

    vec2 uv = hitUV();

    // Base color: texture-sampled or flat
    vec3 baseColor = mat.baseColor_metallic.xyz;
    int baseColorTexIdx = mat.textureIndices.x;
    if (baseColorTexIdx >= 0)
        baseColor *= texture(textures[nonuniformEXT(baseColorTexIdx)], uv).rgb;

    float metallic = mat.baseColor_metallic.w;
    float roughness = mat.emissive_roughness.w;
    vec3 emissive = mat.emissive_roughness.xyz;
    float ior = mat.ior;

    // Emissive contribution: texture-sampled or flat
    int emissiveTexIdx = mat.textureIndices.z;
    if (emissiveTexIdx >= 0)
        emissive *= texture(textures[nonuniformEXT(emissiveTexIdx)], uv).rgb;

    // If emissive, add contribution and stop
    if (dot(emissive, emissive) > 0.0)
    {
        payload.b.xyz += payload.a.xyz * emissive;
        payload.c.w = 1.0;
        return;
    }

    // Shading normal (normal-mapped or geometric)
    vec3 normal = hitShadingNormal(mat, uv);

    vec3 rayDir = normalize(gl_WorldRayDirectionEXT);
    bool frontFace = dot(rayDir, normal) < 0.0;
    vec3 n = frontFace ? normal : -normal;

    uint rngState = floatBitsToUint(payload.a.w);
    uint depth = uint(payload.b.w);
    uint maxBounces = uint(camera.viewportSPP.w);

    // Simplified PBR scatter
    vec3 attenuation;
    vec3 scatterDir;
    bool doScatter = true;

    float cosTheta = abs(dot(-rayDir, n));
    float fresnel = reflectance(cosTheta, ior);
    float r = randomFloat(rngState);

    if (metallic >= 0.999)
    {
        // Pure metal: reflect + roughness fuzz
        vec3 reflected = reflect(rayDir, n);
        scatterDir = normalize(reflected) + roughness * randomInUnitSphere(rngState);
        if (dot(scatterDir, n) <= 0.0) doScatter = false;
        attenuation = baseColor;
    }
    else if (metallic <= 0.001)
    {
        // Non-metal (dielectric)
        if (r < fresnel)
        {
            vec3 reflected = reflect(rayDir, n);
            scatterDir = normalize(reflected) + roughness * randomInUnitSphere(rngState);
            attenuation = vec3(1.0);
        }
        else
        {
            scatterDir = n + randomInUnitSphere(rngState);
            if (dot(scatterDir, n) <= 0.0) scatterDir = n;
            attenuation = baseColor;
        }
    }
    else
    {
        // Mixed
        if (r < metallic)
        {
            vec3 reflected = reflect(rayDir, n);
            scatterDir = normalize(reflected) + roughness * randomInUnitSphere(rngState);
            attenuation = baseColor;
        }
        else
        {
            scatterDir = n + randomInUnitSphere(rngState);
            if (dot(scatterDir, n) <= 0.0) scatterDir = n;
            attenuation = baseColor * (1.0 - metallic);
        }
    }

    if (!doScatter || depth >= maxBounces)
    {
        payload.c.w = 1.0;
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

    payload.b.xyz += nextPayload.b.xyz;
    payload.c.w = 1.0;
}