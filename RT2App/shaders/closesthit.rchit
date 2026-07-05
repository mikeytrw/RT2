#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require

#include "pathtracer_shared.glsl"

layout(location = 0) rayPayloadInEXT RayPayload payload;
layout(location = 1) rayPayloadEXT RayPayload nextPayload;
layout(location = 2) rayPayloadEXT float shadowVisible;

// Hardware-provided barycentric hit attributes (core GL_EXT_ray_tracing).
// attribs = (v, w) weights for vertices 1 and 2; u = 1 - v - w.
hitAttributeEXT vec2 attribs;

// Get the 3 vertex positions of the current hit triangle
void hitTriPositions(out vec3 p0, out vec3 p1, out vec3 p2)
{
    uint triIdx = normalOffsets[gl_InstanceID] + uint(gl_PrimitiveID);
    uint posIdx = triIdx * 3u;
    p0 = trianglePositions[posIdx + 0u].xyz;
    p1 = trianglePositions[posIdx + 1u].xyz;
    p2 = trianglePositions[posIdx + 2u].xyz;
}

// Barycentric coordinates of the hit point, straight from the hardware.
vec3 hitBarycentric()
{
    return vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
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

// ---- Next-Event Estimation result struct (M8) --------------------------------
struct NEEResult
{
    vec3  radiance;       // throughput * BRDF(wo,L) * Le * G * wLight / pdfOmega
    float pdfLightOmega;  // solid-angle PDF, 0 if unusable
};

// ---- Environment map NEE (M8) -----------------------------------------------
// Sample the env map using importance sampling (CDF), trace a shadow ray,
// and return (directRadiance, pdfOmega) for MIS weighting.
NEEResult sampleEnvNEE(vec3 wo, vec3 N, vec3 P,
                       vec3 baseColor, float metallic, float roughness,
                       float P_s, float P_d,
                       vec3 throughput, inout uint rngState,
                       bool hasTransmission, float P_refract, float eta)
{
    NEEResult result;
    result.radiance = vec3(0.0);
    result.pdfLightOmega = 0.0;

    EnvSample envS = sampleEnvMap(rngState);
    if (envS.pdf <= 0.0)
        return result;

    vec3 L = envS.dir;
    float NdotL = dot(N, L);
    if (NdotL <= 0.0)
        return result;

    // Diffuse BRDF evaluation (same as triangle NEE — avoids specular fireflies).
    // Evaluate BEFORE tracing the shadow ray: if the diffuse lobe is black
    // (e.g. metals), the shadow ray is pure waste.
    vec3 brdf = evalDiffuseBRDF(wo, L, N, baseColor, metallic);
    if (dot(brdf, brdf) <= 0.0)
        return result;

    // Shadow ray to the sky (distance = large).
    // SkipClosestHitShader: only the any-hit/miss decide visibility.
    shadowVisible = 0.0;
    traceRayEXT(topLevelAS,
                gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
                0xFF, 2, 1, 1,
                P + N * 0.001, 0.001, L, 1e9, 2);

    if (shadowVisible < 0.5)
        return result;

    vec3 Le = envS.radiance;
    float pdfOmega = envS.pdf;

    float alpha = roughnessToAlpha(roughness);
    float pdfBsdf;
    if (hasTransmission)
        pdfBsdf = evalTransmissionBSDFPdf(wo, L, N, P_s, P_refract, P_d, alpha, eta);
    else
        pdfBsdf = evalBSDFPdf(wo, L, N, P_s, P_d, alpha);

    float wLight = pdfOmega / (pdfOmega + pdfBsdf);
    vec3 direct = brdf * Le * NdotL * wLight / pdfOmega;

    result.radiance = throughput * direct;
    result.pdfLightOmega = pdfOmega;
    return result;
}

// ---- Next-Event Estimation with MIS -----------------------------------------
// Pick a light uniformly, sample a point on its triangle, trace a shadow ray,
// and return (directRadiance, pdfLightOmega) for MIS weighting.
// directRadiance already includes throughput. pdfLightOmega is the solid-angle
// PDF of the light-sampled direction, needed for the balance heuristic.
// Returns pdfLightOmega = 0.0 if the sample is invalid (backface, occluded).

// NEE with diffuse BRDF evaluation and full-combined-PDF MIS weighting.
// P_s, P_d = lobe selection probabilities for MIS combined-PDF evaluation.
// For transmission materials (hasTransmission=true), includes BTDF lobe.
NEEResult sampleNEE(vec3 wo, vec3 N, vec3 P,
                    vec3 baseColor, float metallic, float roughness,
                    float P_s, float P_d,
                    vec3 throughput, inout uint rngState,
                    bool hasTransmission, float P_refract, float eta)
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

    // NEE evaluates ONLY the diffuse BRDF term to avoid GGX specular fireflies.
    // Evaluate BEFORE the shadow ray: if the diffuse lobe is black (metals,
    // black albedo), skip the shadow ray entirely.
    vec3 brdf = evalDiffuseBRDF(wo, L, N, baseColor, metallic);
    if (dot(brdf, brdf) <= 0.0)
        return result;

    // Shadow ray: SBT hit offset 2 = shadow hit group.
    // SkipClosestHitShader: only the any-hit/miss decide visibility.
    shadowVisible = 0.0;
    traceRayEXT(topLevelAS,
                gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
                0xFF, 2, 1, 1,
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

    // The MIS weight MUST use the full combined BSDF PDF so that
    // w_light + w_bsdf = 1 at the emissive hit.
    float alpha = roughnessToAlpha(roughness);
    float pdfBsdf;
    if (hasTransmission)
    {
        // 3-lobe: reflect(VNDF) + refract(BTDF) + diffuse
        pdfBsdf = evalTransmissionBSDFPdf(wo, L, N, P_s, P_refract, P_d, alpha, eta);
    }
    else
    {
        // 2-lobe: reflect(VNDF) + diffuse
        pdfBsdf = evalBSDFPdf(wo, L, N, P_s, P_d, alpha);
    }

    // MIS weight (balance heuristic)
    float wLight = pdfOmega / (pdfOmega + pdfBsdf);

    // Direct radiance (solid-angle form): BRDF(wo, L) * Le * NdotL * w_light / pdfOmega
    vec3 direct = brdf * Le * NdotL * wLight / pdfOmega;

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

    // Metallic-roughness texture: glTF convention — G=roughness, B=metallic
    int metalRoughTexIdx = mat.extraIndices.x;
    if (metalRoughTexIdx >= 0)
    {
        vec3 mr = texture(textures[nonuniformEXT(metalRoughTexIdx)], uv).rgb;
        roughness *= mr.g;
        metallic *= mr.b;
    }

    // Emissive contribution: texture-sampled or flat
    int emissiveTexIdx = mat.textureIndices.z;
    if (emissiveTexIdx >= 0)
        emissive *= texture(textures[nonuniformEXT(emissiveTexIdx)], uv).rgb;

    // If emissive, add contribution with MIS weight.
    // payload.d.w = bsdfPdf (solid-angle PDF of the incoming ray).
    //   -1.0 = camera ray or delta/specular bounce → no MIS, full emission.
    //   >= 0  = diffuse bounce → weight by w_bsdf = bsdfPdf / (bsdfPdf + pdfLight)
    // With stochastic NEE selection, the effective light-sampling PDF for a
    // direction hitting a triangle is P_tri * pdfLight (env NEE can't sample
    // this direction), so the MIS weight becomes:
    //   w_bsdf = bsdfPdf / (bsdfPdf + P_tri * pdfLight)
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
            vec3 rayD = gl_WorldRayDirectionEXT;  // always unit-length (normalized at trace)
            float hitDist = gl_HitTEXT;
            float LNdotL = abs(dot(lightN, -rayD));

            float pdfLight = 0.0;
            if (LNdotL > 1e-6 && lightArea > 1e-6)
            {
                pdfLight = (1.0 / float(lightCount)) * (1.0 / lightArea)
                         * (hitDist * hitDist) / LNdotL;
            }
            // Stochastic NEE: triangle NEE is selected with probability P_tri
            // when both triangle and env NEE are available. Scale pdfLight
            // by P_tri to get the effective combined NEE pdf for this direction.
            float pTri = computePTri();
            weight = bsdfPdf / (bsdfPdf + pTri * pdfLight);
        }
        payload.b.xyz += payload.a.xyz * emissive * weight * boost;
        payload.c.w = 1.0;

        // NRD: tag emissive hits at depth 0 with lobeType=2 so raygen routes
        // their radiance to gDirectEmission (bypassing NRD entirely).
        if (uint(payload.b.w) == 0u && nrdData.nrdEnabled != 0u)
        {
            ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
            vec3 geoN = hitFaceNormal();
            imageStore(gNormalRoughness, pixel, vec4(geoN * 0.5 + 0.5, 1.0));
            vec3 worldPos = gl_WorldRayOriginEXT + gl_HitTEXT * gl_WorldRayDirectionEXT;
            float viewZE = (camera.worldToView * vec4(worldPos, 1.0)).z;
            imageStore(gViewZ, pixel, vec4(viewZE, 0.0, 0.0, 0.0));
            imageStore(gMotion, pixel, vec4(0.0, 0.0, 0.0, 0.0));
            imageStore(gAlbedoF0, pixel, vec4(1.0, 1.0, 1.0, 1.0));
            // lobeType=2 (emissive), viewZ in z, hitT in w
            payload.e = vec4(0.0, 0.0, 2.0, gl_HitTEXT);
        }
        return;
    }

    // Shading normal (normal-mapped or geometric)
    vec3 normal = hitShadingNormal(mat, uv);

    vec3 rayDir = gl_WorldRayDirectionEXT;  // always unit-length (normalized at trace)
    bool frontFace = dot(rayDir, normal) < 0.0;
    vec3 n = frontFace ? normal : -normal;

    uint rngState = floatBitsToUint(payload.a.w);
    uint depth = uint(payload.b.w);
    uint maxBounces = uint(camera.viewportSPP.w);

    vec3 hitPoint = gl_WorldRayOriginEXT + gl_HitTEXT * rayDir;

    // ---- Cook-Torrance GGX scatter (M7) ----
    // Stochastic lobe pick: specular with prob P_s (Fresnel-weighted),
    // diffuse with prob P_d = 1 - P_s. Metals have P_d = 0.
    vec3 wo = -rayDir;  // outgoing direction (away from surface, towards viewer)
    float NdotV = max(dot(n, wo), 0.0);

    vec3 F0 = computeF0(baseColor, metallic);
    vec3 F = F_Schlick(NdotV, F0);
    float P_s = mix(luminance(F), 1.0, metallic);  // metals: specular only
    float P_d = 1.0 - P_s;

    float alpha = roughnessToAlpha(roughness);

    vec3 attenuation;
    vec3 scatterDir;
    bool doScatter = true;
    bool isDelta = false;   // delta path: no NEE, full emission at next hit
    float nextBsdfPdf = -1.0;  // -1 = delta (specular), >= 0 = combined PDF

    float r = randomFloat(rngState);

    // ---- Dielectric transmission (M7 Phase 3) ----
    // transmissionFactor > 0 with OPAQUE alpha mode = physical glass (KHR_materials_transmission).
    // MASK/BLEND alpha materials use any-hit for opacity, NOT refraction here.
    // Smooth (delta) refraction — rough glass uses delta too (M7.5 will add GGX BTDF).
    float transmissionFactor = intBitsToFloat(mat.textureIndices.w);
    if (mat.alphaMode < 0.5 && transmissionFactor > 0.0)
    {
        float transmission = transmissionFactor;
        // Scalar dielectric Fresnel (F0 = 0.04 for non-metals)
        float F0_scalar = mix(0.04, luminance(baseColor), metallic);
        float F_scalar = F_Schlick(NdotV, vec3(F0_scalar)).r;
        float P_reflect = F_scalar;
        float P_refract  = (1.0 - F_scalar) * transmission;
        float P_diffuse = (1.0 - F_scalar) * (1.0 - transmission) * (1.0 - metallic);

        // Renormalize probabilities (should sum to ~1)
        float Psum = P_reflect + P_refract + P_diffuse;
        if (Psum > 1e-6)
        {
            P_reflect /= Psum;
            P_refract  /= Psum;
            P_diffuse  /= Psum;
        }

        if (r < P_reflect)
        {
            // Reflect (delta for smooth, or GGX for rough)
            if (roughness < 0.001)
            {
                scatterDir = reflect(rayDir, n);
                attenuation = vec3(F_scalar);
                isDelta = true;
                nextBsdfPdf = -1.0;
            }
            else
            {
                vec3 h = sampleVNDF(wo, n, alpha, rngState);
                scatterDir = reflect(rayDir, h);
                if (dot(scatterDir, n) <= 0.0)
                {
                    doScatter = false;
                }
                else
                {
                    // Stochastic lobe pick: divide by P_reflect
                    float NdotWi = max(dot(n, scatterDir), 0.0);
                    attenuation = F * G1_Smith(NdotWi, alpha) / P_reflect;
                    nextBsdfPdf = pdfVNDF(wo, scatterDir, n, alpha);
                }
            }
        }
        else if (r < P_reflect + P_refract)
        {
            // Refract via Snell's law
            // eta = eta_i / eta_o (incident medium IOR / transmitted medium IOR)
            // frontFace=true: ray enters from air (1.0) into glass (ior), eta = 1.0/ior
            // frontFace=false: ray exits from glass (ior) into air (1.0), eta = ior/1.0
            float eta = frontFace ? (1.0 / ior) : ior;

            if (roughness < 0.001)
            {
                // SMOOTH delta refraction (M7): refract around macro normal n
                // GLSL refract() uses eta = eta_i / eta_t (same convention)
                vec3 refracted = refract(rayDir, n, eta);
                if (length(refracted) < 1e-4)
                {
                    // Total internal reflection → reflect
                    scatterDir = reflect(rayDir, n);
                    attenuation = vec3(1.0);
                }
                else
                {
                    scatterDir = normalize(refracted);
                    attenuation = vec3(1.0);  // dielectric transmits all colors equally
                }
                isDelta = true;
                nextBsdfPdf = -1.0;
            }
            else
            {
                // ROUGH BTDF (M7.5): refract around sampled microfacet normal h
                vec3 h = sampleVNDF(wo, n, alpha, rngState);

                if (checkTIR(wo, h, eta))
                {
                    // TIR at microfacet level → rough reflect around h
                    scatterDir = reflect(rayDir, h);
                    if (dot(scatterDir, n) <= 0.0)
                    {
                        doScatter = false;
                    }
                    else
                    {
                        float VdotH = max(dot(wo, h), 0.0);
                        float F_h = F_Schlick(VdotH, vec3(F0_scalar)).r;
                        float NdotWi = max(dot(n, scatterDir), 0.0);
                        attenuation = vec3(F_h) * G1_Smith(NdotWi, alpha) / P_refract;
                        nextBsdfPdf = pdfVNDF(wo, scatterDir, n, alpha);
                        // NOT delta — rough TIR reflection participates in NEE
                    }
                }
                else
                {
                    // Refract around h
                    vec3 refracted = refractAroundH(wo, h, eta);
                    if (length(refracted) < 1e-4)
                    {
                        // Degenerate → treat as TIR reflect
                        scatterDir = reflect(rayDir, h);
                        if (dot(scatterDir, n) <= 0.0)
                        {
                            doScatter = false;
                        }
                        else
                        {
                            float VdotH = max(dot(wo, h), 0.0);
                            float F_h = F_Schlick(VdotH, vec3(F0_scalar)).r;
                            float NdotWi = max(dot(n, scatterDir), 0.0);
                            attenuation = vec3(F_h) * G1_Smith(NdotWi, alpha) / P_refract;
                            nextBsdfPdf = pdfVNDF(wo, scatterDir, n, alpha);
                        }
                    }
                    else
                    {
                        scatterDir = normalize(refracted);
                        float VdotH = max(dot(wo, h), 0.0);
                        float F_h = F_Schlick(VdotH, vec3(F0_scalar)).r;
                        float NdotWi = abs(dot(n, scatterDir));  // transmitted ray below surface
                        float G1_wi = G1_Smith(NdotWi, alpha);
                        // Attenuation: (1-F_h) * G1(wi) / P_refract
                        // No color tint (non-absorbing dielectric)
                        attenuation = vec3(1.0 - F_h) * G1_wi / P_refract;
                        nextBsdfPdf = pdfBTDF(wo, scatterDir, n, eta, alpha);
                        // NOT delta — rough refraction participates in NEE (diffuse only)
                    }
                }
            }
        }
        else
        {
            // Diffuse lobe (for non-metallic transmission materials with roughness)
            // Must subtract Fresnel energy to match evalDiffuseBRDF in NEE.
            scatterDir = sampleCosineHemisphere(n, rngState);
            float VdotH_t = max(dot(wo, normalize(wo + scatterDir)), 0.0);
            float F_t = F_Schlick(VdotH_t, vec3(F0_scalar)).r;
            float specW_t = mix(F_t, 1.0, metallic);
            attenuation = (1.0 - specW_t) * baseColor * (1.0 - metallic) / P_diffuse;
            nextBsdfPdf = pdfDiffuse(scatterDir, n);
        }
    }
    else if (roughness < 0.001 && metallic >= 0.5)
    {
        // Perfect mirror (smooth metal): delta reflection
        scatterDir = reflect(rayDir, n);
        attenuation = F;  // Fresnel-tinted
        isDelta = true;
        nextBsdfPdf = -1.0;
    }
    else if (r < P_s)
    {
        // Specular lobe: GGX VNDF sampling
        if (roughness < 0.001)
        {
            // Smooth dielectric specular: delta reflection
            scatterDir = reflect(rayDir, n);
            attenuation = F;
            isDelta = true;
            nextBsdfPdf = -1.0;
        }
        else
        {
            // Rough specular: sample visible microfacet normal h, reflect around h
            vec3 h = sampleVNDF(wo, n, alpha, rngState);
            scatterDir = reflect(rayDir, h);
            if (dot(scatterDir, n) <= 0.0)
            {
                doScatter = false;
            }
            else
            {
                // Importance sampling weight for stochastic lobe pick:
                // BRDF_s * cos(wi) / (P_s * pdfVNDF) = F * G1(wi) / P_s
                float NdotWi = max(dot(n, scatterDir), 0.0);
                float G1_wi = G1_Smith(NdotWi, alpha);
                attenuation = F * G1_wi / P_s;
                nextBsdfPdf = pdfVNDF(wo, scatterDir, n, alpha);
            }
        }
    }
    else
    {
        // Diffuse lobe: analytic cosine-weighted hemisphere sampling
        // attenuation = BRDF_d(wo,wi) * cos(wi) / (P_d * pdfDiffuse(wi))
        // BRDF_d = (1 - specWeight) * baseColor * (1-metallic) / PI
        // cos(wi) = NdotWi, pdfDiffuse = NdotWi / PI
        // => (1 - specWeight) * baseColor * (1-metallic) / P_d
        // where specWeight = luminance(F(VdotH)) and VdotH = dot(wo, normalize(wo+wi))
        scatterDir = sampleCosineHemisphere(n, rngState);
        float VdotH_d = max(dot(wo, normalize(wo + scatterDir)), 0.0);
        vec3 F_d = F_Schlick(VdotH_d, F0);
        float specWeight_d = mix(luminance(F_d), 1.0, metallic);
        attenuation = (1.0 - specWeight_d) * baseColor * (1.0 - metallic) / P_d;
        nextBsdfPdf = pdfDiffuse(scatterDir, n);
    }

    // ---- NRD G-buffer capture at primary hit (depth=0) ----
    // Per-pixel data (normal/roughness, viewZ) is written straight to the
    // G-buffer images here — this shader knows the pixel via gl_LaunchIDEXT.
    // Data raygen needs for radiance routing/demodulation goes back through
    // payload.e (the ONLY legal return channel — see RayPayload note).
    if (depth == 0u && nrdData.nrdEnabled != 0u)
    {
        ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);

        // Determine lobe type: 0 = diffuse, 1 = specular (reflect/refract/delta)
        // Based on what actually scattered, not just material type.
        // Diffuse = cosine hemisphere sample (nextBsdfPdf == pdfDiffuse, not delta)
        // Specular = everything else (delta reflect/refract, GGX VNDF, BTDF)
        bool isSpecularLobe = isDelta || (nextBsdfPdf != pdfDiffuse(scatterDir, n));
        float lobeType = isSpecularLobe ? 1.0 : 0.0;

        // World-space normal + roughness. gNormalRoughness is rgba8 UNORM, so
        // the signed normal must be encoded to [0,1] (NRD RGBA8 unorm encoding).
        imageStore(gNormalRoughness, pixel, vec4(n * 0.5 + 0.5, roughness));

        // View-space Z: transform world position to view space
        vec3 worldPos = gl_WorldRayOriginEXT + gl_HitTEXT * rayDir;
        vec4 viewPos = camera.worldToView * vec4(worldPos, 1.0);
        float viewZ = viewPos.z;
        imageStore(gViewZ, pixel, vec4(viewZ, 0.0, 0.0, 0.0));

        // Motion vector: reproject world position into previous and current
        // screen space. NRD expects MV in UV space: pixelUvPrev = pixelUv + mv.
        // The projection matrix has a Y-flip (m_Projection[1][1] *= -1), so
        // both current and previous frames use the same flip — the delta
        // cancels the flip, so standard NDC-to-UV conversion works.
        vec4 currClip = camera.viewToClip * viewPos;
        vec4 prevView = camera.worldToViewPrev * vec4(worldPos, 1.0);
        vec4 prevClip = camera.viewToClipPrev * prevView;
        vec2 currUv = (currClip.xy / currClip.w) * 0.5 + 0.5;
        vec2 prevUv = (prevClip.xy / prevClip.w) * 0.5 + 0.5;
        imageStore(gMotion, pixel, vec4(prevUv - currUv, 0.0, 0.0));

        // Demodulation factors:
        // Diffuse: divide by albedo = baseColor * (1 - metallic)
        // Specular: divide by F0 (Schlick fresnel at normal incidence)
        vec3 diffFactor = max(baseColor * (1.0 - metallic), vec3(0.01));
        float f0Scalar = max(mix(0.04, luminance(baseColor), metallic), 0.01);

        // Store demod factors for the compose compute shader
        imageStore(gAlbedoF0, pixel, vec4(diffFactor, f0Scalar));

        // Return routing/demod info to raygen through the payload.
        // Pack lobeType into the 4th component of the albedo pack (0-1 range).
        // Pack F0 + roughness into y via packUnorm2x16.
        // hitT = gl_HitTEXT (> 0); raygen treats e.w == 0 as "primary miss".
        payload.e = vec4(uintBitsToFloat(packUnorm4x8(vec4(diffFactor, lobeType))),
                         uintBitsToFloat(packUnorm2x16(vec2(clamp(f0Scalar, 0.0, 1.0), clamp(roughness, 0.0, 1.0)))),
                         viewZ, gl_HitTEXT);
    }

    if (!doScatter || depth >= maxBounces)
    {
        payload.c.w = 1.0;
        payload.a.w = uintBitsToFloat(rngState);
        return;
    }

    // ---- NEE + MIS: direct lighting from a random light ----
    // Fire for all non-delta bounces (diffuse AND rough specular/refraction paths).
    // NEE evaluates only the diffuse BRDF — specular/BTDF is handled by BSDF sampling.
    if (!isDelta)
    {
        // For transmission materials, recompute MIS probabilities for the diffuse path.
        float nee_Ps = P_s;
        float nee_Pd = P_d;
        float nee_P_refract = 0.0;
        float nee_eta = 1.0;
        bool isTransmissionMat = (mat.alphaMode < 0.5 && transmissionFactor > 0.0);
        if (isTransmissionMat)
        {
            // Transmission material: NEE evaluates diffuse only, but MIS weight
            // uses the full 3-lobe combined PDF (reflect + refract + diffuse).
            float F0_scalar_t = mix(0.04, luminance(baseColor), metallic);
            float F_scalar_t = F_Schlick(NdotV, vec3(F0_scalar_t)).r;
            float P_reflect_t = F_scalar_t;
            float P_refract_t = (1.0 - F_scalar_t) * transmissionFactor;
            float P_diffuse_t = (1.0 - F_scalar_t) * (1.0 - transmissionFactor) * (1.0 - metallic);
            float Psum = P_reflect_t + P_refract_t + P_diffuse_t;
            if (Psum > 1e-6)
            {
                P_reflect_t /= Psum;
                P_refract_t /= Psum;
                P_diffuse_t /= Psum;
            }
            nee_Ps = P_reflect_t;
            nee_Pd = P_diffuse_t;
            nee_P_refract = P_refract_t;
            nee_eta = frontFace ? (1.0 / ior) : ior;
        }

        // NEE only ever evaluates the diffuse lobe, so if the material has no
        // diffuse energy (metals, pure glass, black albedo) skip both shadow
        // rays — they can only ever return zero. BSDF sampling then becomes
        // the sole direct-light strategy, so the next emissive/env hit must
        // use full weight (nextBsdfPdf = -1, same as delta paths).
        float diffuseWeight = (1.0 - metallic)
                            * max(baseColor.x, max(baseColor.y, baseColor.z))
                            * (isTransmissionMat ? (1.0 - transmissionFactor) : 1.0);
        bool neeUseful = diffuseWeight > 1e-4;

        if (neeUseful)
        {
            // Stochastic NEE selection: pick triangle OR env NEE per bounce.
            // Only one shadow ray is traced instead of two. The selected
            // NEE result is divided by its selection probability P to stay
            // unbiased (it only fires P fraction of the time).
            //
            // The MIS weights at the emissive hit / env miss must also account
            // for the selection probability: at a triangle hit, the effective
            // NEE pdf is P_tri * pdfLight (env can't sample this direction);
            // at env miss, it's (1-P_tri) * pdfEnv (tri can't sample a miss).
            float pTri = computePTri();
            bool hasTriNee = (pTri > 0.0);
            bool hasEnvNee = (pTri < 1.0);

            if (hasTriNee && hasEnvNee)
            {
                if (randomFloat(rngState) < pTri)
                {
                    NEEResult nee = sampleNEE(wo, n, hitPoint,
                                              baseColor, metallic, roughness,
                                              nee_Ps, nee_Pd,
                                              payload.a.xyz, rngState,
                                              isTransmissionMat, nee_P_refract, nee_eta);
                    payload.b.xyz += nee.radiance / pTri;
                }
                else
                {
                    NEEResult envNee = sampleEnvNEE(wo, n, hitPoint,
                                                    baseColor, metallic, roughness,
                                                    nee_Ps, nee_Pd,
                                                    payload.a.xyz, rngState,
                                                    isTransmissionMat, nee_P_refract, nee_eta);
                    payload.b.xyz += envNee.radiance / (1.0 - pTri);
                }
            }
            else if (hasTriNee)
            {
                NEEResult nee = sampleNEE(wo, n, hitPoint,
                                          baseColor, metallic, roughness,
                                          nee_Ps, nee_Pd,
                                          payload.a.xyz, rngState,
                                          isTransmissionMat, nee_P_refract, nee_eta);
                payload.b.xyz += nee.radiance;
            }
            else if (hasEnvNee)
            {
                NEEResult envNee = sampleEnvNEE(wo, n, hitPoint,
                                                baseColor, metallic, roughness,
                                                nee_Ps, nee_Pd,
                                                payload.a.xyz, rngState,
                                                isTransmissionMat, nee_P_refract, nee_eta);
                payload.b.xyz += envNee.radiance;
            }
        }

        // Combined BSDF PDF for the scattered direction (for next-hit MIS).
        // For transmission materials with rough refraction, include the BTDF lobe.
        if (nextBsdfPdf >= 0.0)
        {
            if (!neeUseful)
            {
                // No NEE was performed — BSDF sampling is the only strategy,
                // so the emissive hit must not be MIS-down-weighted.
                nextBsdfPdf = -1.0;
            }
            else if (isTransmissionMat && roughness >= 0.001)
            {
                // 3-lobe combined PDF: reflect(VNDF) + refract(BTDF) + diffuse
                nextBsdfPdf = evalTransmissionBSDFPdf(wo, scatterDir, n,
                                                      nee_Ps, nee_P_refract, nee_Pd,
                                                      alpha, nee_eta);
            }
            else
            {
                // 2-lobe combined PDF: reflect(VNDF) + diffuse
                float pdfS = pdfVNDF(wo, scatterDir, n, alpha);
                float pdfD = pdfDiffuse(scatterDir, n);
                nextBsdfPdf = nee_Ps * pdfS + nee_Pd * pdfD;
            }
        }
    }

    // Update throughput
    vec3 newThroughput = payload.a.xyz * attenuation;

    // ---- Russian roulette path termination ----
    // After a few bounces, probabilistically kill low-throughput paths and
    // compensate the survivors. Unbiased, and drastically shortens paths in
    // dark/absorbing scenes instead of always tracing to maxBounces.
    if (depth >= 3u)
    {
        float pContinue = clamp(max(newThroughput.x, max(newThroughput.y, newThroughput.z)),
                                0.05, 0.95);
        if (randomFloat(rngState) >= pContinue)
        {
            payload.c.w = 1.0;
            payload.a.w = uintBitsToFloat(rngState);
            return;
        }
        newThroughput /= pContinue;
    }

    payload.a.xyz = newThroughput;
    payload.a.w = uintBitsToFloat(rngState);
    payload.b.w = float(depth + 1);

    // Recursively trace the scattered ray.
    // d.w = bsdfPdf (solid-angle): -1.0 for specular/delta, cos(θ)/pi for diffuse.
    // Offset origin along the shading normal's side of the scatter direction:
    // refraction goes into the surface (dot < 0), so offset by -n; others by +n.
    vec3 offsetN = dot(scatterDir, n) > 0.0 ? n : -n;
    nextPayload.a = payload.a;
    nextPayload.b = vec4(vec3(0.0), float(depth + 1));
    nextPayload.c = vec4(hitPoint + offsetN * 0.001, 0.0);
    nextPayload.d = vec4(normalize(scatterDir), nextBsdfPdf);

    traceRayEXT(topLevelAS, gl_RayFlagsNoneEXT, 0xFF, 0, 0, 0,
                nextPayload.c.xyz, 0.001, nextPayload.d.xyz, 1e9, 0);

    payload.b.xyz += nextPayload.b.xyz;
    payload.c.w = 1.0;
}