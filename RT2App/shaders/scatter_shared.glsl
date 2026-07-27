// scatter_shared.glsl — BSDF scatter + NEE logic shared between closesthit and
// secondary_raygen (raster-first path). Included only by RT shader stages that
// trace shadow rays (closesthit, secondary_raygen). NOT included by raygen,
// compose, or debug shaders (which don't do NEE).
//
// Includes pathtracer_shared.glsl for all bindings, helpers, and BRDF functions.

#include "pathtracer_shared.glsl"
#include "restir_shared.glsl"
#include "surface_history_shared.glsl"
#include "restir_gi_shared.glsl"

// Shadow ray payload (location 2) — used by sampleNEE / sampleEnvNEE.
layout(location = 2) rayPayloadEXT float shadowVisible;

// ---- Reservoir buffer (set 0, binding 14 — history buffer, read by shading) ----
layout(set = 0, binding = SI_BINDING_RESERVOIR_HISTORY, std430) readonly buffer ReservoirBuffer
{
    Reservoir reservoirs[];
};

// ---- GI reservoir buffer (set 0, binding 11 — monolithic, read by shading) ----
// The GI buffer contains four regions (reservoirA/B + receiverHistory prev/cur).
// Raygen reads the current reservoir region selected by nrdData.restirGIReservoirIndex.
layout(set = 0, binding = SI_BINDING_GI_DATA, std430) readonly buffer GIReservoirReadBuffer
{
    uvec4 giReservoirData[];
};

// GIReservoir is 48 bytes (3 uvec4). sizeof() is not available in GLSL.
const uint GI_RESERVOIR_BYTES_RT = 48u;

// Load a GI reservoir from the monolithic buffer by region + pixel.
// region = nrdData.restirGIReservoirIndex (frame parity).
GIReservoir loadGIReservoir(uint region, uint pixelLinear, uint pixelCount)
{
    uint byteOffset = region * pixelCount * GI_RESERVOIR_BYTES_RT
                    + pixelLinear * GI_RESERVOIR_BYTES_RT;
    uint wordOffset = byteOffset / 16u;
    GIReservoir r;
    r.data0 = giReservoirData[wordOffset + 0u];
    r.data1 = giReservoirData[wordOffset + 1u];
    r.data2 = giReservoirData[wordOffset + 2u];
    return r;
}

// ---- Scatter result ----------------------------------------------------------

struct ScatterResult
{
    vec3  scatterDir;        // direction of the scattered ray
    vec3  attenuation;       // throughput attenuation for the scattered ray
    bool  doScatter;         // false = invalid scatter direction (absorb)
    bool  isDelta;           // true = delta path (no NEE, full emission at next hit)
    float nextBsdfPdf;       // -1 = no diffuse-NEE competitor; >= 0 = competing PDF
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
//   pixelCoord   — pixel coordinates (for Bayer/IGN dithering of lobe selection,
//                  required by NRD HitDistanceReconstructionMode::AREA_3X3)
//   ditherMode   — 0=white noise, 1=Bayer 4x4, 2=Interleaved Gradient Noise
//   temporalShift — frame-global Cranley-Patterson rotation in [0, 1). NRD
//                   requires this for Bayer-based probabilistic lobe selection:
//                   a static screen-space pattern develops directional bias and
//                   shimmers when camera motion scans it across world surfaces.
ScatterResult scatterPrimaryHit(
    Material mat,
    vec3 baseColor, float metallic, float roughness, float ior,
    vec3 n, vec3 wo, float NdotV, vec3 rayDir, bool frontFace,
    inout uint rngState, uvec2 pixelCoord, int ditherMode, float temporalShift)
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

    // Dithered lobe selection for NRD HitDistanceReconstructionMode::AREA_3X3.
    // NRD requires a low-discrepancy pattern (not white noise) to guarantee
    // a valid sample in every 3x3 neighborhood for both lobes.
    // 0 = white noise (no NRD guarantee), 1 = Bayer 4x4, 2 = IGN
    float r;
    if (ditherMode == 1)
    {
        const float bayer4x4[16] = float[16](
            0.0/16.0,  8.0/16.0,  2.0/16.0, 10.0/16.0,
           12.0/16.0,  4.0/16.0, 14.0/16.0,  6.0/16.0,
            3.0/16.0, 11.0/16.0,  1.0/16.0,  9.0/16.0,
           15.0/16.0,  7.0/16.0, 13.0/16.0,  5.0/16.0
        );
        uint bayerIdx = (pixelCoord.y % 4u) * 4u + (pixelCoord.x % 4u);
        // A global temporal rotation preserves the Bayer pattern's spatial
        // coverage while turning its static directional bias into temporal
        // variance that REBLUR can filter.
        r = fract(bayer4x4[bayerIdx] + temporalShift);
    }
    else if (ditherMode == 2)
    {
        // Interleaved Gradient Noise (Jorge Jimenez) — less structured than
        // Bayer, quasi-random, still good spatial coverage for 3x3 recon.
        r = fract(52.9829189 * fract(0.06711056 * float(pixelCoord.x)
                + 0.00583715 * float(pixelCoord.y)) + temporalShift);
    }
    else
    {
        r = randomFloat(rngState);
    }

    // Transmission path uses white noise (separate from NRD lobe reconstruction)
    float rTransmission = randomFloat(rngState);

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

        if (rTransmission < P_reflect)
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
        else if (rTransmission < P_reflect + P_refract)
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
                       float familyProbability,
                       bool jointLobeEstimator,
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
    // The environment is selected stochastically against triangle lights.
    // MIS and the estimator denominator must both use the complete proposal.
    float pdfOmega = familyProbability * envS.pdf;
    if (pdfOmega <= 0.0)
        return result;

    float alpha = roughnessToAlpha(roughness);
    // NEE evaluates only the diffuse integrand. Ordinary lobe sampling can
    // therefore compete only through its diffuse branch. The raster-first NRD
    // joint estimator evaluates diffuse for every mixture-sampled direction,
    // so its competing density is the full mixture PDF.
    float pdfBsdf = P_d * pdfDiffuse(L, N);
    if (jointLobeEstimator)
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
                    float familyProbability,
                    bool jointLobeEstimator,
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

    uvec4 meshInfo = instanceMeshInfo[light.ids.x];
    uint idxBase = meshInfo.y + light.ids.y * 3u;
    uint i0 = indices[idxBase + 0u];
    uint i1 = indices[idxBase + 1u];
    uint i2 = indices[idxBase + 2u];
    mat4 lightWorld = instanceTransforms[light.ids.x];
    vec3 lp0 = vec3(lightWorld * vertices[meshInfo.x + i0]);
    vec3 lp1 = vec3(lightWorld * vertices[meshInfo.x + i1]);
    vec3 lp2 = vec3(lightWorld * vertices[meshInfo.x + i2]);

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
        vec2 luv0 = uvs[meshInfo.w + i0].xy;
        vec2 luv1 = uvs[meshInfo.w + i1].xy;
        vec2 luv2 = uvs[meshInfo.w + i2].xy;
        vec2 lightUV = b0 * luv0 + b1 * luv1 + b2 * luv2;
        Le *= texture(textures[nonuniformEXT(int(emissiveTexIdx))], lightUV).rgb;
    }
    Le *= camera.apertureFocal.w;

    float lightArea = light.emission_area.w;
    float pdfA = (1.0 / float(lightCount)) * (1.0 / lightArea);
    // Include the stochastic triangle-vs-environment family selection in the
    // proposal PDF. The BSDF-hit side uses the same effective density.
    float pdfOmega = familyProbability * pdfA * (dist * dist) / abs(LNdotL);
    if (pdfOmega <= 0.0)
        return result;

    float alpha = roughnessToAlpha(roughness);
    float pdfBsdf = P_d * pdfDiffuse(L, N);
    if (jointLobeEstimator)
        pdfBsdf = evalBSDFPdf(wo, L, N, P_s, P_d, alpha);

    float wLight = pdfOmega / (pdfOmega + pdfBsdf);
    vec3 direct = brdf * Le * NdotL * wLight / pdfOmega;

    result.radiance = throughput * direct;
    result.pdfLightOmega = pdfOmega;
    return result;
}

// ---- ReSTIR DI: unified sampling from final reservoir ------------------------
// sampleReSTIRDI: consumes the final reservoir (written by restir_spatial.comp
// into reservoirHistory) and traces a single visibility ray.
//
// Handles both triangle and environment samples in the unified reservoir.
// Reconstructs the sample, traces one shadow ray, computes the contribution
// using the RIS weight W = weightSum / (M * targetPdf).
//
// Estimator (solid-angle measure throughout):
//   W = weightSum / (M * targetPdf)     — stochastic replacement for 1/pdf
//   For triangle: radiance = throughput * brdf * Le * NdotL * W
//   For env:      radiance = throughput * brdf * Le * NdotL * W
//
// The area→solid-angle Jacobian (LNdotL/dist²) is in the proposal PDF only,
// NOT in p_hat or the final contribution. This avoids double-application.
//
// MIS: The conventional light-vs-BSDF balance heuristic is NOT valid for a
// RIS/ReSTIR-selected sample — the sample follows the reservoir distribution,
// not the base light proposal. MIS is disabled (wLight = 1.0) until a
// reservoir-aware MIS derivation is implemented.
NEEResult sampleReSTIRDI(vec3 wo, vec3 N, vec3 P,
                         vec3 baseColor, float metallic, float roughness,
                         float P_s, float P_d,
                         vec3 throughput, inout uint rngState,
                         bool hasTransmission, float P_refract, float eta,
                         uint pixelLinear)
{
    NEEResult result;
    result.radiance = vec3(0.0);
    // Once ReSTIR DI is selected for this pixel, its reservoir outcome is the
    // complete estimator. An empty reservoir is a legitimate zero-valued RIS
    // estimate (all M candidates had zero weight), not a reason to draw a
    // conditional conventional NEE fallback sample.
    result.pdfLightOmega = -1.0;

    Reservoir res = reservoirs[pixelLinear];
    res = sanitizeReservoir(res);

    if (reservoirSampleType(res) == SAMPLE_EMPTY)
        return result;

    uint sampleType = reservoirSampleType(res);

    if (sampleType == SAMPLE_TRIANGLE)
    {
        TriangleSample ts = reconstructTriangleSample(res, P, N, wo);
        if (!ts.valid)
            return result;

        vec3 brdf = evalDiffuseBRDF(wo, ts.L, N, baseColor, metallic);
        if (dot(brdf, brdf) <= 0.0)
            return result;

        // A reconstructed reservoir sample is a complete estimator even when
        // its visibility ray returns zero. Mark it handled before tracing so
        // the caller does not conditionally draw a second, biased NEE sample.
        shadowVisible = 0.0;
        traceRayEXT(topLevelAS,
                    gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
                    0xFF, 2, 1, 1,
                    P + N * 0.001, 0.001, ts.L, ts.dist - 0.002, 2);

        if (shadowVisible < 0.5)
            return result;

        float W = reservoirW(res);
        vec3 direct = brdf * ts.Le * ts.NdotL * W;

        result.radiance = throughput * direct;
    }
    else if (sampleType == SAMPLE_ENV)
    {
        EnvSampleRecon es = reconstructEnvSample(res, N);
        if (!es.valid)
            return result;

        vec3 brdf = evalDiffuseBRDF(wo, es.dir, N, baseColor, metallic);
        if (dot(brdf, brdf) <= 0.0)
            return result;

        shadowVisible = 0.0;
        traceRayEXT(topLevelAS,
                    gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
                    0xFF, 2, 1, 1,
                    P + N * 0.001, 0.001, es.dir, 1e9, 2);

        if (shadowVisible < 0.5)
            return result;

        float W = reservoirW(res);
        vec3 direct = brdf * es.radiance * es.NdotL * W;

        result.radiance = throughput * direct;
    }

    return result;
}

// ---- computeNEE dispatch (shared by closesthit + secondary_raygen) -----------
// Extracted from closesthit lines 669-793. Handles:
// - Delta path skip (no NEE for specular/delta)
// - Transmission material MIS probability recomputation
// - Diffuse weight check (skip NEE if diffuse lobe is black)
// - Stochastic NEE selection (triangle OR env, divided by selection prob)
// - Lobe-aware competing BSDF PDF update for next-hit MIS
//
// Returns radiance to add to payload.b.xyz and potentially-modified nextBsdfPdf.
struct NEEDispatchResult
{
    vec3  radiance;
    float nextBsdfPdf;
};

// Direct lighting from punctual lights (Phase 8).
//
// Independent of the triangle-vs-env stochastic selection, so it needs no
// share of pTri and cannot bias it. One light is chosen per shading point,
// weighted by its unshadowed contribution, and the estimator divides by that
// selection probability — so this stays one shadow ray regardless of how many
// lights the scene has, and remains unbiased.
vec3 samplePunctualNEE(
    vec3 wo, vec3 N, vec3 P,
    vec3 baseColor, float metallic,
    inout uint rngState)
{
    if (punctualLightCount == 0u)
        return vec3(0.0);

    // Pass 1: weight each light by the luminance it would deliver here,
    // ignoring visibility. A light behind the surface or outside its spot
    // cone weighs zero and is never chosen.
    const uint kMaxConsidered = 64u;
    uint count = min(punctualLightCount, kMaxConsidered);

    float weights[kMaxConsidered];
    float total = 0.0;
    for (uint i = 0u; i < count; i++)
    {
        PunctualSample ps = evalPunctualLight(i, P);
        float w = 0.0;
        if (ps.valid)
        {
            float NdotL = dot(N, ps.toLight);
            if (NdotL > 0.0)
                w = dot(ps.radiance, vec3(0.2126, 0.7152, 0.0722)) * NdotL;
        }
        weights[i] = max(w, 0.0);
        total += weights[i];
    }

    if (total <= 0.0)
        return vec3(0.0);

    // Pass 2: pick one proportionally.
    float r = randomFloat(rngState) * total;
    uint chosen = count - 1u;
    float running = 0.0;
    for (uint i = 0u; i < count; i++)
    {
        running += weights[i];
        if (r <= running) { chosen = i; break; }
    }

    float selectPdf = weights[chosen] / total;
    if (selectPdf <= 0.0)
        return vec3(0.0);

    PunctualSample ps = evalPunctualLight(chosen, P);
    if (!ps.valid)
        return vec3(0.0);

    float NdotL = dot(N, ps.toLight);
    if (NdotL <= 0.0)
        return vec3(0.0);

    vec3 brdf = evalDiffuseBRDF(wo, ps.toLight, N, baseColor, metallic);
    if (dot(brdf, brdf) <= 0.0)
        return vec3(0.0);

    // Shadow ray stops just short of the light so the light's own geometry,
    // if any, does not occlude it.
    float tmax = (ps.distance > 1e29) ? 1e9 : (ps.distance - 0.002);
    if (tmax <= 0.001)
        return vec3(0.0);

    shadowVisible = 0.0;
    traceRayEXT(topLevelAS,
                gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
                0xFF, 2, 1, 1,
                P + N * 0.001, 0.001, ps.toLight, tmax, 2);

    if (shadowVisible < 0.5)
        return vec3(0.0);

    // No MIS weight: a punctual light is a delta distribution, so a BSDF
    // sample can never hit it and there is no second strategy to balance.
    return brdf * ps.radiance * NdotL / selectPdf;
}

NEEDispatchResult computeNEE(
    ScatterResult scatter,
    vec3 wo, vec3 n, vec3 hitPoint,
    vec3 baseColor, float metallic, float roughness,
    float ior, float alphaMode,
    vec3 throughput,
    inout uint rngState,
    bool useReSTIR, bool jointLobeEstimator, uint pixelLinear)
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
        if (useReSTIR)
        {
            // ReSTIR DI: use the unified reservoir (handles both triangle + env).
            // Empty, rejected, and occluded reservoir outcomes are valid zero
            // estimates and must not conditionally draw another NEE sample.
            NEEResult restirResult = sampleReSTIRDI(wo, n, hitPoint,
                                             baseColor, metallic, roughness,
                                             nee_Ps, nee_Pd,
                                             throughput, rngState,
                                             isTransmissionMat, nee_P_refract, nee_eta,
                                             pixelLinear);
            if (restirResult.pdfLightOmega < 0.0)
            {
                result.radiance = restirResult.radiance;
            }
            else
            {
                // Fallback: standard NEE path.
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
                                                  pTri,
                                                  jointLobeEstimator,
                                                  throughput, rngState,
                                                  isTransmissionMat, nee_P_refract, nee_eta);
                        result.radiance = nee.radiance;
                    }
                    else
                    {
                        NEEResult envNee = sampleEnvNEE(wo, n, hitPoint,
                                                        baseColor, metallic, roughness,
                                                        nee_Ps, nee_Pd,
                                                        1.0 - pTri,
                                                        jointLobeEstimator,
                                                        throughput, rngState,
                                                        isTransmissionMat, nee_P_refract, nee_eta);
                        result.radiance = envNee.radiance;
                    }
                }
                else if (hasTriNee)
                {
                    NEEResult nee = sampleNEE(wo, n, hitPoint,
                                              baseColor, metallic, roughness,
                                              nee_Ps, nee_Pd,
                                              1.0,
                                              jointLobeEstimator,
                                              throughput, rngState,
                                              isTransmissionMat, nee_P_refract, nee_eta);
                    result.radiance = nee.radiance;
                }
                else if (hasEnvNee)
                {
                    NEEResult envNee = sampleEnvNEE(wo, n, hitPoint,
                                                    baseColor, metallic, roughness,
                                                    nee_Ps, nee_Pd,
                                                    1.0,
                                                    jointLobeEstimator,
                                                    throughput, rngState,
                                                    isTransmissionMat, nee_P_refract, nee_eta);
                    result.radiance = envNee.radiance;
                }
            }
        }
        else
        {
            // Standard NEE path (stochastic triangle/env selection)
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
                                              pTri,
                                              jointLobeEstimator,
                                              throughput, rngState,
                                              isTransmissionMat, nee_P_refract, nee_eta);
                    result.radiance = nee.radiance;
                }
                else
                {
                    NEEResult envNee = sampleEnvNEE(wo, n, hitPoint,
                                                    baseColor, metallic, roughness,
                                                    nee_Ps, nee_Pd,
                                                    1.0 - pTri,
                                                    jointLobeEstimator,
                                                    throughput, rngState,
                                                    isTransmissionMat, nee_P_refract, nee_eta);
                    result.radiance = envNee.radiance;
                }
            }
            else if (hasTriNee)
            {
                NEEResult nee = sampleNEE(wo, n, hitPoint,
                                          baseColor, metallic, roughness,
                                          nee_Ps, nee_Pd,
                                          1.0,
                                          jointLobeEstimator,
                                          throughput, rngState,
                                          isTransmissionMat, nee_P_refract, nee_eta);
                result.radiance = nee.radiance;
            }
            else if (hasEnvNee)
            {
                NEEResult envNee = sampleEnvNEE(wo, n, hitPoint,
                                                baseColor, metallic, roughness,
                                                nee_Ps, nee_Pd,
                                                1.0,
                                                jointLobeEstimator,
                                                throughput, rngState,
                                                isTransmissionMat, nee_P_refract, nee_eta);
                result.radiance = envNee.radiance;
            }
        }
    }

    // PDF for the continuation technique that competes with diffuse-only NEE.
    if (result.nextBsdfPdf >= 0.0)
    {
        if (!neeUseful)
        {
            result.nextBsdfPdf = -1.0;
        }
        else if (jointLobeEstimator)
        {
            float alpha = roughnessToAlpha(roughness);
            result.nextBsdfPdf = evalBSDFPdf(wo, scatter.scatterDir, n,
                                              nee_Ps, nee_Pd, alpha);
        }
        else if (scatter.lobeType < 0.5)
        {
            result.nextBsdfPdf = nee_Pd * pdfDiffuse(scatter.scatterDir, n);
        }
        else
        {
            // Specular/refraction continuations do not estimate the diffuse
            // integrand sampled by NEE and therefore have no competing light
            // technique at their terminal hit.
            result.nextBsdfPdf = -1.0;
        }
    }

    return result;
}
