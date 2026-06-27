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

// ---- Next-Event Estimation with MIS -----------------------------------------
// Pick a light uniformly, sample a point on its triangle, trace a shadow ray,
// and return (directRadiance, pdfLightOmega) for MIS weighting.
// directRadiance already includes throughput. pdfLightOmega is the solid-angle
// PDF of the light-sampled direction, needed for the balance heuristic.
// Returns pdfLightOmega = 0.0 if the sample is invalid (backface, occluded).
struct NEEResult
{
    vec3  radiance;       // throughput * (albedo/pi) * Le * G * weight
    float pdfLightOmega;  // solid-angle PDF, 0 if unusable
};

NEEResult sampleNEE(vec3 diffuseAlbedo, vec3 throughput, vec3 P, vec3 N, inout uint rngState)
{
    NEEResult result;
    result.radiance = vec3(0.0);
    result.pdfLightOmega = 0.0;

    if (lightCount == 0u || totalLightArea <= 0.0)
        return result;

    // Pick a light uniformly: prob = 1/lightCount
    uint lightIdx = pcg(rngState) % lightCount;
    TriangleLight light = lights[lightIdx];

    // Sample a uniform point on the light triangle
    uint triIdx = normalOffsets[light.ids.x] + light.ids.y;
    uint posIdx = triIdx * 3u;
    vec3 lp0 = trianglePositions[posIdx + 0u].xyz;
    vec3 lp1 = trianglePositions[posIdx + 1u].xyz;
    vec3 lp2 = trianglePositions[posIdx + 2u].xyz;

    float r1 = randomFloat(rngState);
    float r2 = randomFloat(rngState);
    float sqrtR1 = sqrt(r1);
    float b0 = 1.0 - sqrtR1;
    float b1 = sqrtR1 * (1.0 - r2);
    float b2 = sqrtR1 * r2;
    vec3 lightPoint = b0 * lp0 + b1 * lp1 + b2 * lp2;

    vec3 lightN = normalize(cross(lp1 - lp0, lp2 - lp0));

    vec3 toLight = lightPoint - P;
    float dist = length(toLight);
    vec3 L = toLight / max(dist, 1e-6);

    float NdotL = dot(N, L);
    float LNdotL = dot(lightN, -L);

    if (NdotL <= 0.0 || LNdotL <= 0.0)
        return result;

    // Shadow ray: SBT hit offset 2 = shadow hit group.
    // Instance adds 0 (opaque) or 1 (alpha) → shadow_opaque or shadow_alpha.
    // TerminateOnFirstHit: first accepted hit = occluder.
    shadowVisible = 0.0;
    traceRayEXT(topLevelAS,
                gl_RayFlagsTerminateOnFirstHitEXT,
                0xFF, 2, 1, 1,  // SBT hit offset=2, stride=1, miss index=1
                P + N * 0.001, 0.001, L, dist - 0.002, 2);

    if (shadowVisible < 0.5)
        return result;

    // Emission: flat fallback or texture-sampled
    vec3 Le = light.emission_area.xyz;
    uint emissiveTexIdx = light.ids.w;
    if (emissiveTexIdx != 0xFFFFFFFFu)
    {
        vec2 luv0 = triangleUVs[posIdx + 0u].xy;
        vec2 luv1 = triangleUVs[posIdx + 1u].xy;
        vec2 luv2 = triangleUVs[posIdx + 2u].xy;
        vec2 lightUV = b0 * luv0 + b1 * luv1 + b2 * luv2;
        Le *= texture(textures[nonuniformEXT(int(emissiveTexIdx))], lightUV).rgb;
    }
    Le *= camera.apertureFocal.w;  // emissive boost

    // Solid-angle PDF: pdf_A = (1/lightCount) * (1/area_i)
    // Convert to solid angle: pdf_ω = pdf_A * dist² / |LNdotL|
    float lightArea = light.emission_area.w;
    float pdfA = (1.0 / float(lightCount)) * (1.0 / lightArea);
    float pdfOmega = pdfA * (dist * dist) / abs(LNdotL);

    // MIS weight (balance heuristic): w_light = pdf_light / (pdf_light + pdf_bsdf)
    // BSDF PDF for cosine-weighted Lambertian: pdf_bsdf = NdotL / pi
    float pdfBsdf = NdotL / PI;
    float wLight = pdfOmega / (pdfOmega + pdfBsdf);

    // Radiance = (albedo / PI) * Le * G * wLight / pdfOmega
    //   where G = NdotL * LNdotL / dist²
    // Simplify: (albedo/PI) * Le * (NdotL*LNdotL/dist²) * wLight / pdfOmega
    //   and pdfOmega = pdfA * dist² / LNdotL = dist² / (lightCount * area * LNdotL)
    //   => (albedo/PI) * Le * NdotL * LNdotL * lightCount * area / dist²  (the old formula)
    //   × wLight
    float G = (NdotL * LNdotL) / (dist * dist);
    vec3 direct = (diffuseAlbedo / PI) * Le * G * float(lightCount) * lightArea * wLight;

    result.radiance = throughput * direct;
    result.pdfLightOmega = pdfOmega;
    return result;
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

    // If emissive, add contribution with MIS weight.
    // payload.d.w = bsdfPdf (solid-angle PDF of the incoming ray).
    //   -1.0 = camera ray or delta/specular bounce → no MIS, full emission.
    //   >= 0  = diffuse bounce → weight by w_bsdf = bsdfPdf / (bsdfPdf + pdfLight)
    if (dot(emissive, emissive) > 0.0)
    {
        float boost = camera.apertureFocal.w;
        float bsdfPdf = payload.d.w;
        float weight = 1.0;  // default: full emission (camera/specular)
        if (bsdfPdf >= 0.0)
        {
            // Compute the light-sampling PDF for THIS specific hit.
            // Only the light that was actually hit contributes to pdf_light(ω);
            // all other lights have pdf_i(ω)=0 for this direction.
            // pdf_light = (1/lightCount) * (1/area) * dist² / |LNdotL|
            vec3 p0, p1, p2;
            hitTriPositions(p0, p1, p2);
            vec3 lightN = normalize(cross(p1 - p0, p2 - p0));
            float lightArea = 0.5 * length(cross(p1 - p0, p2 - p0));

            vec3 origin = gl_WorldRayOriginEXT;
            vec3 rayD = normalize(gl_WorldRayDirectionEXT);
            float hitDist = gl_HitTEXT;
            float LNdotL = abs(dot(lightN, -rayD));

            float pdfLight = 0.0;
            if (LNdotL > 1e-6 && lightArea > 1e-6)
            {
                pdfLight = (1.0 / float(lightCount)) * (1.0 / lightArea)
                         * (hitDist * hitDist) / LNdotL;
            }
            weight = bsdfPdf / (bsdfPdf + pdfLight);
        }
        payload.b.xyz += payload.a.xyz * emissive * weight * boost;
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

    vec3 hitPoint = gl_WorldRayOriginEXT + gl_HitTEXT * rayDir;

    // Simplified PBR scatter
    vec3 attenuation;
    vec3 scatterDir;
    bool doScatter = true;
    bool isDiffuseBounce = false;
    float nextBsdfPdf = -1.0;  // -1 = delta (specular), >= 0 = diffuse cosine PDF

    float cosTheta = abs(dot(-rayDir, n));
    float fresnel = reflectance(cosTheta, ior);
    float r = randomFloat(rngState);

    if (metallic >= 0.999)
    {
        vec3 reflected = reflect(rayDir, n);
        scatterDir = normalize(reflected) + roughness * randomInUnitSphere(rngState);
        if (dot(scatterDir, n) <= 0.0) doScatter = false;
        attenuation = baseColor;
    }
    else if (metallic <= 0.001)
    {
        if (r < fresnel)
        {
            vec3 reflected = reflect(rayDir, n);
            scatterDir = normalize(reflected) + roughness * randomInUnitSphere(rngState);
            attenuation = vec3(1.0);
        }
        else
        {
            isDiffuseBounce = true;
            scatterDir = n + randomInUnitSphere(rngState);
            if (dot(scatterDir, n) <= 0.0) scatterDir = n;
            attenuation = baseColor;
        }
    }
    else
    {
        if (r < metallic)
        {
            vec3 reflected = reflect(rayDir, n);
            scatterDir = normalize(reflected) + roughness * randomInUnitSphere(rngState);
            attenuation = baseColor;
        }
        else
        {
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

    // ---- NEE + MIS: direct lighting from a random light ----
    if (isDiffuseBounce)
    {
        // Cosine-weighted BSDF PDF for the scatter direction (computed later
        // for the recursive ray, but needed here for NEE MIS weight)
        NEEResult nee = sampleNEE(attenuation, payload.a.xyz, hitPoint, n, rngState);
        payload.b.xyz += nee.radiance;

        // BSDF PDF for the scattered direction (cosine / pi)
        float cosScatter = max(dot(normalize(scatterDir), n), 0.0);
        nextBsdfPdf = cosScatter / PI;
    }

    // Update throughput
    payload.a.xyz *= attenuation;
    payload.a.w = uintBitsToFloat(rngState);
    payload.b.w = float(depth + 1);

    // Recursively trace the scattered ray.
    // d.w = bsdfPdf (solid-angle): -1.0 for specular/delta, cos(θ)/pi for diffuse.
    nextPayload.a = payload.a;
    nextPayload.b = vec4(vec3(0.0), float(depth + 1));
    nextPayload.c = vec4(hitPoint + n * 0.001, 0.0);
    nextPayload.d = vec4(normalize(scatterDir), nextBsdfPdf);

    traceRayEXT(topLevelAS, gl_RayFlagsNoneEXT, 0xFF, 0, 0, 0,
                nextPayload.c.xyz, 0.001, nextPayload.d.xyz, 1e9, 0);

    payload.b.xyz += nextPayload.b.xyz;
    payload.c.w = 1.0;
}