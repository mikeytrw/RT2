#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require

#include "pathtracer_shared.glsl"

layout(location = 0) rayPayloadInEXT RayPayload payload;
layout(location = 1) rayPayloadEXT RayPayload nextPayload;
layout(location = 2) rayPayloadEXT float shadowVisible;

// Get the 3 vertex positions of the current hit triangle
void hitTriPositions(out vec3 p0, out vec3 p1, out vec3 p2)
{
    uint triIdx = normalOffsets[gl_InstanceID] + uint(gl_PrimitiveID);
    uint posIdx = triIdx * 3u;
    p0 = trianglePositions[posIdx + 0u].xyz;
    p1 = trianglePositions[posIdx + 1u].xyz;
    p2 = trianglePositions[posIdx + 2u].xyz;
}

// Compute barycentric coordinates of the hit point
vec3 hitBarycentric()
{
    vec3 p0, p1, p2;
    hitTriPositions(p0, p1, p2);

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

    return vec3(u, v, w);
}

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

    vec2 uv0 = triangleUVs[posIdx + 0u].xy;
    vec2 uv1 = triangleUVs[posIdx + 1u].xy;
    vec2 uv2 = triangleUVs[posIdx + 2u].xy;

    vec3 bary = hitBarycentric();
    return bary.x * uv0 + bary.y * uv1 + bary.z * uv2;
}

vec3 hitTangent()
{
    uint triIdx = normalOffsets[gl_InstanceID] + uint(gl_PrimitiveID);
    uint posIdx = triIdx * 3u;

    vec3 t0 = triangleTangents[posIdx + 0u].xyz;
    vec3 t1 = triangleTangents[posIdx + 1u].xyz;
    vec3 t2 = triangleTangents[posIdx + 2u].xyz;

    vec3 bary = hitBarycentric();
    return normalize(bary.x * t0 + bary.y * t1 + bary.z * t2);
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

// ---- Next-Event Estimation --------------------------------------------------
// Pick a light ~ area, sample a uniform point on its triangle, trace a shadow
// ray (SBT miss index 1 = shadow.rmiss), and return the direct radiance.
// diffuseAlbedo is the diffuse lobe reflectance (already scaled by 1-metallic).
// throughput is the current path throughput. P is the shading point, N is the
// shading normal (oriented toward the viewer).
vec3 sampleNEE(vec3 diffuseAlbedo, vec3 throughput, vec3 P, vec3 N, inout uint rngState)
{
    if (lightCount == 0u || totalLightArea <= 0.0)
        return vec3(0.0);

    // Pick a light uniformly (area-weighted sampling: each light picked with
    // probability 1/lightCount, PDF = 1/totalArea for the chosen light's
    // surface area measure).
    uint lightIdx = pcg(rngState) % lightCount;
    TriangleLight light = lights[lightIdx];

    // Sample a uniform point on the light triangle
    uint triIdx = normalOffsets[light.ids.x] + light.ids.y;  // instanceID + primitiveID
    uint posIdx = triIdx * 3u;
    vec3 lp0 = trianglePositions[posIdx + 0u].xyz;
    vec3 lp1 = trianglePositions[posIdx + 1u].xyz;
    vec3 lp2 = trianglePositions[posIdx + 2u].xyz;

    // Uniform sample on triangle: r1, r2 in [0,1)
    float r1 = randomFloat(rngState);
    float r2 = randomFloat(rngState);
    float sqrtR1 = sqrt(r1);
    float b0 = 1.0 - sqrtR1;
    float b1 = sqrtR1 * (1.0 - r2);
    float b2 = sqrtR1 * r2;
    vec3 lightPoint = b0 * lp0 + b1 * lp1 + b2 * lp2;

    // Light normal (geometric face normal)
    vec3 lightN = normalize(cross(lp1 - lp0, lp2 - lp0));

    // Direction from shading point to light sample
    vec3 toLight = lightPoint - P;
    float dist = length(toLight);
    vec3 L = toLight / max(dist, 1e-6);

    float NdotL = dot(N, L);
    float LNdotL = dot(lightN, -L);

    // Backface culling: both surfaces must face each other
    if (NdotL <= 0.0 || LNdotL <= 0.0)
        return vec3(0.0);

    // Shadow ray: trace from P toward lightPoint, Tmax = dist - epsilon
    shadowVisible = 0.0;
    traceRayEXT(topLevelAS,
                gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT |
                gl_RayFlagsSkipClosestHitShaderEXT,
                0xFF, 0, 0, 1,  // SBT: miss index 1 = shadow.rmiss
                P + N * 0.001, 0.001, L, dist - 0.002, 2);

    if (shadowVisible < 0.5)
        return vec3(0.0);  // occluded

    // Emission: flat fallback or texture-sampled
    vec3 Le = light.emission_area.xyz;
    uint emissiveTexIdx = light.ids.w;
    if (emissiveTexIdx != 0xFFFFFFFFu)
    {
        // Sample emissive texture at the light point's barycentric UV
        vec2 luv0 = triangleUVs[posIdx + 0u].xy;
        vec2 luv1 = triangleUVs[posIdx + 1u].xy;
        vec2 luv2 = triangleUVs[posIdx + 2u].xy;
        vec2 lightUV = b0 * luv0 + b1 * luv1 + b2 * luv2;
        Le *= texture(textures[nonuniformEXT(int(emissiveTexIdx))], lightUV).rgb;
    }

    // Direct lighting: (albedo / PI) * Le * G / pdf_A
    // pdf_A = (1/lightCount) * (1/area_i)  =>  weight = lightCount * area_i
    float G = (NdotL * LNdotL) / (dist * dist);
    float lightArea = light.emission_area.w;
    vec3 direct = (diffuseAlbedo / PI) * Le * G * float(lightCount) * lightArea;

    return throughput * direct;
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

    // If emissive, add contribution only for camera/specular rays.
    // Diffuse bounces (skipEmission = 1.0) skip this — NEE handles direct light.
    if (dot(emissive, emissive) > 0.0)
    {
        float skipEmission = payload.d.w;
        if (skipEmission < 0.5)
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

    // Shading point
    vec3 hitPoint = gl_WorldRayOriginEXT + gl_HitTEXT * rayDir;

    // Simplified PBR scatter
    vec3 attenuation;
    vec3 scatterDir;
    bool doScatter = true;
    bool isDiffuseBounce = false;  // tracks whether this bounce uses the diffuse lobe

    float cosTheta = abs(dot(-rayDir, n));
    float fresnel = reflectance(cosTheta, ior);
    float r = randomFloat(rngState);

    if (metallic >= 0.999)
    {
        // Pure metal: reflect + roughness fuzz (specular — no NEE)
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
            // Specular reflection (no NEE)
            vec3 reflected = reflect(rayDir, n);
            scatterDir = normalize(reflected) + roughness * randomInUnitSphere(rngState);
            attenuation = vec3(1.0);
        }
        else
        {
            // Diffuse
            isDiffuseBounce = true;
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
            // Specular metal lobe (no NEE)
            vec3 reflected = reflect(rayDir, n);
            scatterDir = normalize(reflected) + roughness * randomInUnitSphere(rngState);
            attenuation = baseColor;
        }
        else
        {
            // Diffuse lobe
            isDiffuseBounce = true;
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

    // ---- Next-Event Estimation: direct lighting from a random light ----
    // NEE applies to the diffuse lobe of any non-pure-metal surface.
    // diffuseAlbedo = the diffuse reflectance (already includes 1-metallic factor
    // for mixed surfaces, or is just baseColor for pure dielectric diffuse).
    if (isDiffuseBounce)
    {
        vec3 diffuseAlbedo = attenuation;
        vec3 directLight = sampleNEE(diffuseAlbedo, payload.a.xyz, hitPoint, n, rngState);
        payload.b.xyz += directLight;
    }

    // Update throughput
    payload.a.xyz *= attenuation;
    payload.a.w = uintBitsToFloat(rngState);
    payload.b.w = float(depth + 1);

    // Recursively trace the scattered ray.
    // skipEmission flag: diffuse bounces set 1.0 (NEE handled direct light),
    // specular bounces set 0.0 (still allow finding lights via reflection).
    float nextSkipEmission = isDiffuseBounce ? 1.0 : 0.0;

    nextPayload.a = payload.a;
    nextPayload.b = vec4(vec3(0.0), float(depth + 1));
    nextPayload.c = vec4(hitPoint + n * 0.001, 0.0);
    nextPayload.d = vec4(normalize(scatterDir), nextSkipEmission);

    traceRayEXT(topLevelAS, gl_RayFlagsOpaqueEXT, 0xFF, 0, 0, 0,
                nextPayload.c.xyz, 0.001, nextPayload.d.xyz, 1e9, 0);

    payload.b.xyz += nextPayload.b.xyz;
    payload.c.w = 1.0;
}