// scatter_shared.glsl — BSDF scatter + NEE logic shared between closesthit and
// secondary_raygen (raster-first path). Included only by RT shader stages that
// trace shadow rays (closesthit, secondary_raygen). NOT included by raygen,
// compose, or debug shaders (which don't do NEE).
//
// Includes pathtracer_shared.glsl for all bindings, helpers, and BRDF functions.

#include "pathtracer_shared.glsl"

// Shadow ray payload (location 2) — used by sampleNEE / sampleEnvNEE.
layout(location = 2) rayPayloadEXT float shadowVisible;

// ---- RIS Reservoir (read by shading pass, written by ris.comp) ---------------
// Matches SIReservoir in shader_interface.h (32 bytes, std430).
struct Reservoir
{
    uint  lightIdx;     // index into lights[] array
    float b1;           // post-warp barycentric coord 1
    float b2;           // post-warp barycentric coord 2
    float weightSum;    // Σ w_i = p_hat(x_i) / p(x_i)
    float targetPdf;    // p_hat of selected sample
    uint  M;            // candidates seen
    uint  pad0;
    uint  pad1;
};

layout(set = 0, binding = SI_BINDING_RESERVOIR, std430) readonly buffer ReservoirBuffer
{
    Reservoir reservoirs[];
};

// ---- Scatter result ----------------------------------------------------------

struct ScatterResult
{
    vec3  scatterDir;        // direction of the scattered ray
    vec3  attenuation;       // throughput attenuation for the scattered ray
    bool  doScatter;         // false = invalid scatter direction (absorb)
    bool  isDelta;           // true = delta path (no NEE, full emission at next hit)
    float nextBsdfPdf;       // -1 = delta, >= 0 = combined PDF (for MIS at next hit)
    float P_s;               // specular lobe selection probability
    float P_d;               // diffuse lobe selection probability
    float lobeType;          // 0 = diffuse, 1 = specular (for NRD routing)
    bool  frontFace;         // true = ray hit front face of surface
    float transmissionFactor; // 0 = no transmission, > 0 = dielectric glass
};

// ---- Cook-Torrace GGX scatter (M7) -------------------------------------------
// Stochastic lobe pick: specular with prob P_s (Fresnel-weighted),
// diffuse with prob P_d = 1 - P_s. Metals have P_d = 0.
// Transmission materials use 3-lobe pick (reflect / refract / diffuse).
//
// Inputs:
//   mat          — material struct
//   baseColor    — texture-sampled base color
//   metallic     — metallic factor (texture-modulated)
//   roughness    — roughness factor (texture-modulated)
//   ior          — index of refraction
//   n            — face-forward flipped shading normal (points towards wo side)
//   wo           — outgoing direction (away from surface, towards viewer)
//   NdotV        — dot(n, wo), clamped >= 0
//   rayDir       — incoming ray direction (used for reflect/refract)
//   frontFace    — true if ray hit front face (dot(rayDir, rawNormal) < 0)
//   rngState     — PCG RNG state (inout, consumed for lobe pick + sampling)
ScatterResult scatterPrimaryHit(
    Material mat,
    vec3 baseColor, float metallic, float roughness, float ior,
    vec3 n, vec3 wo, float NdotV, vec3 rayDir, bool frontFace,
    inout uint rngState)
{
    ScatterResult result;
    result.doScatter = true;
    result.isDelta = false;
    result.nextBsdfPdf = -1.0;

    vec3 F0 = computeF0(baseColor, metallic);
    vec3 F = F_Schlick(NdotV, F0);
    // Clamp P_s to [1/4, 3/4] for NRD HitDistanceReconstructionMode::AREA_3X3.
    // NRD requires a valid sample in every 3x3 neighborhood to reconstruct the
    // skipped lobe's hit distance. Without clamping, metals (P_s=1) produce
    // blocks of all-zero diffuse hitT that can't be reconstructed.
    result.P_s = clamp(mix(luminance(F), 1.0, metallic), 0.25, 0.75);
    result.P_d = 1.0 - result.P_s;

    float alpha = roughnessToAlpha(roughness);

    vec3 attenuation;
    vec3 scatterDir;
    bool doScatter = true;
    bool isDelta = false;
    float nextBsdfPdf = -1.0;
    float lobeType = 0.0;  // 0 = diffuse, 1 = specular (set explicitly at each pick)

    float r = randomFloat(rngState);

    // ---- Dielectric transmission (M7 Phase 3) ----
    float transmissionFactor = intBitsToFloat(mat.textureIndices.w);
    result.transmissionFactor = transmissionFactor;

    if (mat.alphaMode < 0.5 && transmissionFactor > 0.0)
    {
        float transmission = transmissionFactor;
        float F0_scalar = mix(0.04, luminance(baseColor), metallic);
        float F_scalar = F_Schlick(NdotV, vec3(F0_scalar)).r;
        float P_reflect = F_scalar;
        float P_refract  = (1.0 - F_scalar) * transmission;
        float P_diffuse = (1.0 - F_scalar) * (1.0 - transmission) * (1.0 - metallic);

        float Psum = P_reflect + P_refract + P_diffuse;
        if (Psum > 1e-6)
        {
            P_reflect /= Psum;
            P_refract  /= Psum;
            P_diffuse  /= Psum;
        }

        if (r < P_reflect)
        {
            if (roughness < 0.001)
            {
                scatterDir = reflect(rayDir, n);
                attenuation = vec3(F_scalar);
                isDelta = true;
                lobeType = 1.0;
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
                    float NdotWi = max(dot(n, scatterDir), 0.0);
                    attenuation = F * G1_Smith(NdotWi, alpha) / P_reflect;
                    nextBsdfPdf = pdfVNDF(wo, scatterDir, n, alpha);
                    lobeType = 1.0;
                }
            }
        }
        else if (r < P_reflect + P_refract)
        {
            float eta = frontFace ? (1.0 / ior) : ior;

            if (roughness < 0.001)
            {
                vec3 refracted = refract(rayDir, n, eta);
                if (length(refracted) < 1e-4)
                {
                    scatterDir = reflect(rayDir, n);
                    attenuation = vec3(1.0);
                }
                else
                {
                    scatterDir = normalize(refracted);
                    attenuation = vec3(1.0);
                }
                isDelta = true;
                lobeType = 1.0;
                nextBsdfPdf = -1.0;
            }
            else
            {
                vec3 h = sampleVNDF(wo, n, alpha, rngState);

                if (checkTIR(wo, h, eta))
                {
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
                        lobeType = 1.0;
                    }
                }
                else
                {
                    vec3 refracted = refractAroundH(wo, h, eta);
                    if (length(refracted) < 1e-4)
                    {
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
                            lobeType = 1.0;
                        }
                    }
                    else
                    {
                        scatterDir = normalize(refracted);
                        float VdotH = max(dot(wo, h), 0.0);
                        float F_h = F_Schlick(VdotH, vec3(F0_scalar)).r;
                        float NdotWi = abs(dot(n, scatterDir));
                        float G1_wi = G1_Smith(NdotWi, alpha);
                        attenuation = vec3(1.0 - F_h) * G1_wi / P_refract;
                        nextBsdfPdf = pdfBTDF(wo, scatterDir, n, eta, alpha);
                        lobeType = 1.0;
                    }
                }
            }
        }
        else
        {
            scatterDir = sampleCosineHemisphere(n, rngState);
            float VdotH_t = max(dot(wo, normalize(wo + scatterDir)), 0.0);
            float F_t = F_Schlick(VdotH_t, vec3(F0_scalar)).r;
            float specW_t = mix(F_t, 1.0, metallic);
            attenuation = (1.0 - specW_t) * baseColor * (1.0 - metallic) / P_diffuse;
            nextBsdfPdf = pdfDiffuse(scatterDir, n);
            lobeType = 0.0;
        }
    }
    else if (roughness < 0.001 && metallic >= 0.5)
    {
        scatterDir = reflect(rayDir, n);
        attenuation = F;
        isDelta = true;
        lobeType = 1.0;
        nextBsdfPdf = -1.0;
    }
    else if (r < result.P_s)
    {
        if (roughness < 0.001)
        {
            scatterDir = reflect(rayDir, n);
            attenuation = F;
            isDelta = true;
            lobeType = 1.0;
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
                float NdotWi = max(dot(n, scatterDir), 0.0);
                float G1_wi = G1_Smith(NdotWi, alpha);
                attenuation = F * G1_wi / result.P_s;
                nextBsdfPdf = pdfVNDF(wo, scatterDir, n, alpha);
                lobeType = 1.0;
            }
        }
    }
    else
    {
        scatterDir = sampleCosineHemisphere(n, rngState);
        float VdotH_d = max(dot(wo, normalize(wo + scatterDir)), 0.0);
        vec3 F_d = F_Schlick(VdotH_d, F0);
        float specWeight_d = mix(luminance(F_d), 1.0, metallic);
        attenuation = (1.0 - specWeight_d) * baseColor * (1.0 - metallic) / result.P_d;
        nextBsdfPdf = pdfDiffuse(scatterDir, n);
        lobeType = 0.0;
    }

    // lobeType set explicitly at each pick branch (0 = diffuse, 1 = specular)
    result.lobeType = lobeType;
    result.frontFace = frontFace;

    result.scatterDir = scatterDir;
    result.attenuation = attenuation;
    result.doScatter = doScatter;
    result.isDelta = isDelta;
    result.nextBsdfPdf = nextBsdfPdf;

    return result;
}

// ---- Next-Event Estimation result struct (M8) --------------------------------
struct NEEResult
{
    vec3  radiance;       // throughput * BRDF(wo,L) * Le * G * wLight / pdfOmega
    float pdfLightOmega;  // solid-angle PDF, 0 if unusable
};

// ---- Environment map NEE (M8) -----------------------------------------------
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

    vec3 brdf = evalDiffuseBRDF(wo, L, N, baseColor, metallic);
    if (dot(brdf, brdf) <= 0.0)
        return result;

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

// ---- Triangle NEE (M8) -------------------------------------------------------
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

    uint lightIdx = pcg(rngState) % lightCount;
    TriangleLight light = lights[lightIdx];

    uint triIdx = normalOffsets[light.ids.x] + light.ids.y;
    uint posIdx = triIdx * 3u;
    mat4 lightWorld = instanceTransforms[light.ids.x];
    vec3 lp0 = vec3(lightWorld * trianglePositions[posIdx + 0u]);
    vec3 lp1 = vec3(lightWorld * trianglePositions[posIdx + 1u]);
    vec3 lp2 = vec3(lightWorld * trianglePositions[posIdx + 2u]);

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

    vec3 brdf = evalDiffuseBRDF(wo, L, N, baseColor, metallic);
    if (dot(brdf, brdf) <= 0.0)
        return result;

    shadowVisible = 0.0;
    traceRayEXT(topLevelAS,
                gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
                0xFF, 2, 1, 1,
                P + N * 0.001, 0.001, L, dist - 0.002, 2);

    if (shadowVisible < 0.5)
        return result;

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
    Le *= camera.apertureFocal.w;

    float lightArea = light.emission_area.w;
    float pdfA = (1.0 / float(lightCount)) * (1.0 / lightArea);
    float pdfOmega = pdfA * (dist * dist) / abs(LNdotL);

    float alpha = roughnessToAlpha(roughness);
    float pdfBsdf;
    if (hasTransmission)
    {
        pdfBsdf = evalTransmissionBSDFPdf(wo, L, N, P_s, P_refract, P_d, alpha, eta);
    }
    else
    {
        pdfBsdf = evalBSDFPdf(wo, L, N, P_s, P_d, alpha);
    }

    float wLight = pdfOmega / (pdfOmega + pdfBsdf);
    vec3 direct = brdf * Le * NdotL * wLight / pdfOmega;

    result.radiance = throughput * direct;
    result.pdfLightOmega = pdfOmega;
    return result;
}

// ---- RIS-based NEE (Phase 1 of ReSTIR) ----------------------------------------
// sampleNEE_RIS: consumes a per-pixel reservoir (written by ris.comp) instead of
// sampling a single uniform light. The reservoir's selected light + barycentrics
// are reused; the shading pass traces the single shadow ray and computes MIS.
//
// Estimator (AREA MEASURE throughout — see plan):
//   W = (1 / M) * (weightSum / targetPdf)     // stochastic replacement for 1/pdf
//   radiance = throughput * brdf * Le * NdotL * (LNdotL / dist²) * W * wLight
//
// NOTE: There is NO division by pdfOmega — W replaces 1/pdf. Dividing by
// pdfOmega would double-divide. The original sampleNEE divides by pdfOmega
// because it samples uniformly (pdfOmega is the source pdf); here the RIS
// weight W already accounts for the source distribution.
//
// MIS WEIGHTS — UNCHANGED from sampleNEE:
//   wLight = pdfOmega / (pdfOmega + pdfBsdf)
// The RIS effective sampling pdf has no closed form, so a "correct" balance
// heuristic against it is not computable. Keeping MIS weights defined against
// the ORIGINAL uniform-light pdf (pdfOmega reconstructed from the reservoir
// sample exactly as in sampleNEE) stays unbiased: MIS weights only need to
// partition unity per direction (they're part of the integrand), and RIS
// estimates any integrand correctly as long as p_hat has matching support.
// Do NOT "fix" the MIS weight for RIS — doing so introduces bias.
NEEResult sampleNEE_RIS(vec3 wo, vec3 N, vec3 P,
                        vec3 baseColor, float metallic, float roughness,
                        float P_s, float P_d,
                        vec3 throughput, inout uint rngState,
                        bool hasTransmission, float P_refract, float eta,
                        uint pixelLinear)
{
    NEEResult result;
    result.radiance = vec3(0.0);
    result.pdfLightOmega = 0.0;

    if (lightCount == 0u || totalLightArea <= 0.0)
        return result;

    Reservoir res = reservoirs[pixelLinear];

    // Guard: empty reservoir (all candidates rejected or no lights at RIS time)
    if (res.M == 0u || res.weightSum <= 0.0 || res.targetPdf <= 0.0)
        return result;

    TriangleLight light = lights[res.lightIdx];

    uint triIdx = normalOffsets[light.ids.x] + light.ids.y;
    uint posIdx = triIdx * 3u;
    mat4 lightWorld = instanceTransforms[light.ids.x];
    vec3 lp0 = vec3(lightWorld * trianglePositions[posIdx + 0u]);
    vec3 lp1 = vec3(lightWorld * trianglePositions[posIdx + 1u]);
    vec3 lp2 = vec3(lightWorld * trianglePositions[posIdx + 2u]);

    // Reconstruct barycentrics from stored post-warp (b1, b2)
    float b1 = res.b1;
    float b2 = res.b2;
    float b0 = 1.0 - b1 - b2;
    vec3 lightPoint = b0 * lp0 + b1 * lp1 + b2 * lp2;

    vec3 lightN = normalize(cross(lp1 - lp0, lp2 - lp0));

    vec3 toLight = lightPoint - P;
    float dist = length(toLight);
    vec3 L = toLight / max(dist, 1e-6);

    float NdotL = dot(N, L);
    float LNdotL = dot(lightN, -L);

    if (NdotL <= 0.0 || LNdotL <= 0.0)
        return result;

    vec3 brdf = evalDiffuseBRDF(wo, L, N, baseColor, metallic);
    if (dot(brdf, brdf) <= 0.0)
        return result;

    // Shadow ray (same as sampleNEE)
    shadowVisible = 0.0;
    traceRayEXT(topLevelAS,
                gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
                0xFF, 2, 1, 1,
                P + N * 0.001, 0.001, L, dist - 0.002, 2);

    if (shadowVisible < 0.5)
        return result;

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
    Le *= camera.apertureFocal.w;

    // pdfOmega — reconstructed from the reservoir sample (for MIS only, NOT for 1/pdf)
    float lightArea = light.emission_area.w;
    float pdfA = (1.0 / float(lightCount)) * (1.0 / lightArea);
    float pdfOmega = pdfA * (dist * dist) / abs(LNdotL);

    // MIS weight — UNCHANGED, defined against original uniform-light pdf
    float alpha = roughnessToAlpha(roughness);
    float pdfBsdf;
    if (hasTransmission)
    {
        pdfBsdf = evalTransmissionBSDFPdf(wo, L, N, P_s, P_refract, P_d, alpha, eta);
    }
    else
    {
        pdfBsdf = evalBSDFPdf(wo, L, N, P_s, P_d, alpha);
    }
    float wLight = pdfOmega / (pdfOmega + pdfBsdf);

    // RIS estimator — AREA MEASURE, NO /pdfOmega (W replaces 1/pdf)
    float W = (1.0 / float(res.M)) * (res.weightSum / res.targetPdf);
    vec3 contribution = brdf * Le * NdotL * (LNdotL / (dist * dist));
    vec3 direct = contribution * W * wLight;

    result.radiance = throughput * direct;
    result.pdfLightOmega = pdfOmega;
    return result;
}

// ---- computeNEE dispatch (shared by closesthit + secondary_raygen) -----------
// Extracted from closesthit lines 669-793. Handles:
// - Delta path skip (no NEE for specular/delta)
// - Transmission material MIS probability recomputation
// - Diffuse weight check (skip NEE if diffuse lobe is black)
// - Stochastic NEE selection (triangle OR env, divided by selection prob)
// - Combined BSDF PDF update for next-hit MIS
//
// Returns radiance to add to payload.b.xyz and potentially-modified nextBsdfPdf.
struct NEEDispatchResult
{
    vec3  radiance;
    float nextBsdfPdf;
};

NEEDispatchResult computeNEE(
    ScatterResult scatter,
    vec3 wo, vec3 n, vec3 hitPoint,
    vec3 baseColor, float metallic, float roughness,
    float ior, float alphaMode,
    vec3 throughput,
    inout uint rngState,
    bool useRIS, uint pixelLinear)
{
    NEEDispatchResult result;
    result.radiance = vec3(0.0);
    result.nextBsdfPdf = scatter.nextBsdfPdf;

    if (scatter.isDelta)
        return result;

    float nee_Ps = scatter.P_s;
    float nee_Pd = scatter.P_d;
    float nee_P_refract = 0.0;
    float nee_eta = 1.0;
    bool isTransmissionMat = (alphaMode < 0.5 && scatter.transmissionFactor > 0.0);
    if (isTransmissionMat)
    {
        float F0_scalar_t = mix(0.04, luminance(baseColor), metallic);
        float NdotV = max(dot(wo, n), 0.0);
        float F_scalar_t = F_Schlick(NdotV, vec3(F0_scalar_t)).r;
        float P_reflect_t = F_scalar_t;
        float P_refract_t = (1.0 - F_scalar_t) * scatter.transmissionFactor;
        float P_diffuse_t = (1.0 - F_scalar_t) * (1.0 - scatter.transmissionFactor) * (1.0 - metallic);
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
        nee_eta = scatter.frontFace ? (1.0 / ior) : ior;
    }

    float diffuseWeight = (1.0 - metallic)
                        * max(baseColor.x, max(baseColor.y, baseColor.z))
                        * (isTransmissionMat ? (1.0 - scatter.transmissionFactor) : 1.0);
    bool neeUseful = diffuseWeight > 1e-4;

    if (neeUseful)
    {
        float pTri = computePTri();
        bool hasTriNee = (pTri > 0.0);
        bool hasEnvNee = (pTri < 1.0);

        if (hasTriNee && hasEnvNee)
        {
            if (randomFloat(rngState) < pTri)
            {
                NEEResult nee;
                if (useRIS)
                    nee = sampleNEE_RIS(wo, n, hitPoint,
                                        baseColor, metallic, roughness,
                                        nee_Ps, nee_Pd,
                                        throughput, rngState,
                                        isTransmissionMat, nee_P_refract, nee_eta,
                                        pixelLinear);
                else
                    nee = sampleNEE(wo, n, hitPoint,
                                    baseColor, metallic, roughness,
                                    nee_Ps, nee_Pd,
                                    throughput, rngState,
                                    isTransmissionMat, nee_P_refract, nee_eta);
                result.radiance = nee.radiance / pTri;
            }
            else
            {
                NEEResult envNee = sampleEnvNEE(wo, n, hitPoint,
                                                baseColor, metallic, roughness,
                                                nee_Ps, nee_Pd,
                                                throughput, rngState,
                                                isTransmissionMat, nee_P_refract, nee_eta);
                result.radiance = envNee.radiance / (1.0 - pTri);
            }
        }
        else if (hasTriNee)
        {
            NEEResult nee;
            if (useRIS)
                nee = sampleNEE_RIS(wo, n, hitPoint,
                                    baseColor, metallic, roughness,
                                    nee_Ps, nee_Pd,
                                    throughput, rngState,
                                    isTransmissionMat, nee_P_refract, nee_eta,
                                    pixelLinear);
            else
                nee = sampleNEE(wo, n, hitPoint,
                                baseColor, metallic, roughness,
                                nee_Ps, nee_Pd,
                                throughput, rngState,
                                isTransmissionMat, nee_P_refract, nee_eta);
            result.radiance = nee.radiance;
        }
        else if (hasEnvNee)
        {
            NEEResult envNee = sampleEnvNEE(wo, n, hitPoint,
                                            baseColor, metallic, roughness,
                                            nee_Ps, nee_Pd,
                                            throughput, rngState,
                                            isTransmissionMat, nee_P_refract, nee_eta);
            result.radiance = envNee.radiance;
        }
    }

    // Combined BSDF PDF for the scattered direction (for next-hit MIS).
    if (result.nextBsdfPdf >= 0.0)
    {
        if (!neeUseful)
        {
            result.nextBsdfPdf = -1.0;
        }
        else if (isTransmissionMat && roughness >= 0.001)
        {
            float alpha = roughnessToAlpha(roughness);
            result.nextBsdfPdf = evalTransmissionBSDFPdf(wo, scatter.scatterDir, n,
                                                          nee_Ps, nee_P_refract, nee_Pd,
                                                          alpha, nee_eta);
        }
        else
        {
            float alpha = roughnessToAlpha(roughness);
            float pdfS = pdfVNDF(wo, scatter.scatterDir, n, alpha);
            float pdfD = pdfDiffuse(scatter.scatterDir, n);
            result.nextBsdfPdf = nee_Ps * pdfS + nee_Pd * pdfD;
        }
    }

    return result;
}