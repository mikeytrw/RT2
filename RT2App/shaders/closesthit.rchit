#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require

// scatter_shared.glsl includes pathtracer_shared.glsl + scatter/NEE functions
#include "scatter_shared.glsl"
#include "material_resolve.glsl"

layout(location = 0) rayPayloadInEXT RayPayload payload;
layout(location = 1) rayPayloadEXT RayPayload nextPayload;
// shadowVisible (location 2) is declared in scatter_shared.glsl

// Hardware-provided barycentric hit attributes (core GL_EXT_ray_tracing).
// attribs = (v, w) weights for vertices 1 and 2; u = 1 - v - w.
hitAttributeEXT vec2 attribs;

// Single fetch-and-interpolate of the hit triangle's base attributes.
//
// RT4-T1: replaces the four duplicated loaders (hitTriPositions,
// hitVertexNormal, hitUV, hitTangent), each of which re-fetched
// instanceMeshInfo + indices x3 + vertices/uvs/normals x3 + transforms.
// Now: one instanceMeshInfo load, one index-triplet load, one transform
// load; world-space positions and UVs each fetched once and interpolated
// once with the hardware barycentrics.
//
// Deliberately excludes normals and tangent math. Emissive hits return
// before mapped-normal work (see main), so those stay lazy behind the
// early-out via hitVertexNormalFrom()/hitTangentFrom() — an upfront load
// of the normal stream would make the emissive path pay for data it never
// uses. RNG order is untouched (this loader consumes no random state);
// interpolation expressions match the originals term-for-term.
struct HitTriangle
{
    uvec4 meshInfo;
    uint idxBase;
    uint i0;
    uint i1;
    uint i2;
    vec3 bary;
    mat4 world;
    vec3 p0;
    vec3 p1;
    vec3 p2;
    vec2 uv0;
    vec2 uv1;
    vec2 uv2;
    vec2 uv;
};

HitTriangle loadHitAttributes()
{
    HitTriangle h;
    h.meshInfo = instanceMeshInfo[gl_InstanceID];
    h.idxBase = h.meshInfo.y + uint(gl_PrimitiveID) * 3u;
    h.i0 = indices[h.idxBase + 0u];
    h.i1 = indices[h.idxBase + 1u];
    h.i2 = indices[h.idxBase + 2u];
    h.bary = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
    h.world = instanceTransforms[gl_InstanceID];
    h.p0 = vec3(h.world * vertices[h.meshInfo.x + h.i0]);
    h.p1 = vec3(h.world * vertices[h.meshInfo.x + h.i1]);
    h.p2 = vec3(h.world * vertices[h.meshInfo.x + h.i2]);
    h.uv0 = uvs[h.meshInfo.w + h.i0].xy;
    h.uv1 = uvs[h.meshInfo.w + h.i1].xy;
    h.uv2 = uvs[h.meshInfo.w + h.i2].xy;
    h.uv = h.bary.x * h.uv0 + h.bary.y * h.uv1 + h.bary.z * h.uv2;
    return h;
}

// Face normal computed from the cached world-space triangle positions.
// Same expression as the original hitFaceNormal().
vec3 hitFaceNormalFrom(HitTriangle h)
{
    return normalize(cross(h.p1 - h.p0, h.p2 - h.p0));
}

// Authored shading normal, barycentrically interpolated exactly like the
// raster path. meshInfo.z is UINT_MAX when a mesh has no normal stream.
// Reuses the cached indices/barycentrics/transform; falls back to the face
// normal for missing or degenerate normals, as before.
vec3 hitVertexNormalFrom(HitTriangle h)
{
    if (h.meshInfo.z == 0xFFFFFFFFu)
        return hitFaceNormalFrom(h);

    vec3 n0 = normals[h.meshInfo.z + h.i0].xyz;
    vec3 n1 = normals[h.meshInfo.z + h.i1].xyz;
    vec3 n2 = normals[h.meshInfo.z + h.i2].xyz;
    vec3 objectN = h.bary.x * n0 + h.bary.y * n1 + h.bary.z * n2;
    if (dot(objectN, objectN) < 1e-10)
        return hitFaceNormalFrom(h);

    mat3 normalMatrix = transpose(inverse(mat3(h.world)));
    return normalize(normalMatrix * objectN);
}

vec3 computeTangent(vec3 p0, vec3 p1, vec3 p2, vec2 uv0, vec2 uv1, vec2 uv2)
{
    vec3 edge1 = p1 - p0;
    vec3 edge2 = p2 - p0;
    vec2 dUV1 = uv1 - uv0;
    vec2 dUV2 = uv2 - uv0;
    float det = dUV1.x * dUV2.y - dUV1.y * dUV2.x;
    if (abs(det) < 1e-8) return vec3(1.0, 0.0, 0.0);
    float r = 1.0 / det;
    return normalize(r * (dUV2.y * edge1 - dUV1.y * edge2));
}

// Tangent computed inline from UV gradients, transformed to world space.
// Reuses the cached positions/UVs from loadHitAttributes() — no refetch.
vec3 hitTangentFrom(HitTriangle h)
{
    return computeTangent(h.p0, h.p1, h.p2, h.uv0, h.uv1, h.uv2);
}

// Get the shading normal from authored vertex normals, then apply a normal map.
// Falling back to the face normal is reserved for meshes without usable normals.
// The normal-mapped path (texture fetch + tangent) runs only here, after the
// emissive early-out in main() — emissive hits never pay for it.
vec3 hitShadingNormalFrom(Material mat, HitTriangle h)
{
    vec3 baseN = hitVertexNormalFrom(h);
    int normalTexIdx = mat.textureIndices.y;

    if (normalTexIdx >= 0)
    {
        vec3 tangentN = texture(textures[nonuniformEXT(normalTexIdx)], h.uv).rgb * 2.0 - 1.0;
        vec3 T = hitTangentFrom(h);
        vec3 N = baseN;
        T = normalize(T - dot(T, N) * N);
        vec3 B = cross(N, T);
        return normalize(mat3(T, B, N) * tangentN);
    }

    return baseN;
}

void main()
{
    uint matIdx = resolveMaterialIndex(gl_InstanceID, uint(gl_PrimitiveID));
    Material mat = materials[matIdx];

    // Single fetch-and-interpolate of positions + UVs (see loadHitAttributes).
    // Normals/tangent stay lazy: hitShadingNormalFrom() runs only after the
    // emissive early-out below.
    HitTriangle hit = loadHitAttributes();
    vec2 uv = hit.uv;

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
    //   -1.0 = camera or a continuation outside diffuse NEE → full emission.
    //   >= 0  = a diffuse-capable estimator → balance against light sampling.
    // With stochastic NEE selection, the effective light-sampling PDF for a
    // direction hitting a triangle is P_tri * pdfLight (env NEE can't sample
    // this direction), so the MIS weight becomes:
    //   w_bsdf = bsdfPdf / (bsdfPdf + P_tri * pdfLight)
    if (dot(emissive, emissive) > 0.0)
    {
        if (uint(payload.b.w) == 1u)
        {
            payload.e.z = 1.0;
            payload.e.w = gl_HitTEXT;
        }
        else if (uint(payload.b.w) == 0u)
        {
            // Primary emissive terminal marker: a primary emissive hit
            // reads (e.w == 0, e.z == 1.0), distinct from a true miss
            // (e.w == 0, e.z == 0.0). Uses the established "emissive
            // terminal recorded" value; raygen routes marked pixels to
            // zero motion instead of sky motion.
            payload.e.z = 1.0;
        }
        float boost = camera.apertureFocal.w;
        float bsdfPdf = payload.d.w;
        float weight = 1.0;  // default: full emission (camera/specular)
        if (bsdfPdf >= 0.0)
        {
            // Compute the light-sampling PDF for THIS specific hit.
            // Only the light that was actually hit contributes to pdf_light(ω);
            // all other lights have pdf_i(ω)=0 for this direction.
            // pdf_light = (1/lightCount) * (1/area) * dist² / |LNdotL|
            // Reuses the cached positions from loadHitAttributes() — same
            // normalize(cross)/length(cross) expressions as before, no refetch.
            vec3 lightN = normalize(cross(hit.p1 - hit.p0, hit.p2 - hit.p0));
            float lightArea = 0.5 * length(cross(hit.p1 - hit.p0, hit.p2 - hit.p0));

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

        bool firstBounce = uint(payload.b.w) == 1u;
        bool selectedDiffuse = payload.e.x < 0.5;
        bool jointLobeEstimator = payload.e.y > 0.5;
        bool restirFirstBounce = camera.up.w > 0.5 && firstBounce;

        if (restirFirstBounce && selectedDiffuse && !jointLobeEstimator)
        {
            // ReSTIR DI is the complete primary diffuse direct estimator.
            // Suppress the competing BSDF-sampled emissive terminal, matching
            // the environment-miss behavior.
        }
        else if (jointLobeEstimator && firstBounce)
        {
            // Preserve unweighted incident radiance for the later lobe split.
            // Store the conventional diffuse MIS weight in the terminal marker;
            // ReSTIR consumes the diffuse component without conventional MIS.
            payload.b.xyz += payload.a.xyz * emissive * boost;
            payload.e.z = restirFirstBounce ? 1.0 : 2.0 + weight;
        }
        else
        {
            payload.b.xyz += payload.a.xyz * emissive * weight * boost;
        }
        payload.c.w = 1.0;
        return;
    }

    // Shading normal (normal-mapped or geometric)
    vec3 normal = hitShadingNormalFrom(mat, hit);

    vec3 rayDir = gl_WorldRayDirectionEXT;  // always unit-length (normalized at trace)
    bool frontFace = dot(rayDir, normal) < 0.0;
    vec3 n = frontFace ? normal : -normal;

    uint rngState = floatBitsToUint(payload.a.w);
    uint depth = uint(payload.b.w); ST_SET_DEPTH(depth == 0u ? 0u : 1u);
    uint maxBounces = uint(camera.viewportSPP.w);

    vec3 hitPoint = gl_WorldRayOriginEXT + gl_HitTEXT * rayDir;

    // ---- Cook-Torrance GGX scatter (via shared function) ----
    vec3 wo = -rayDir;  // outgoing direction (away from surface, towards viewer)
    float NdotV = max(dot(n, wo), 0.0);

    // Only the primary lobe participates in NRD's probabilistic split. Use the
    // configured spatial dither there and apply the same frame-global temporal
    // rotation as the raster-first path. Later bounces keep ordinary RNG.
    int primaryDitherMode = (depth == 0u && nrdData.nrdEnabled != 0u)
        ? int(nrdData.lobeDither) : 0;
    float lobeTemporalShift = fract(camera.position.w * 0.6180339887498948);

    ScatterResult scatter = scatterPrimaryHit(
        mat, baseColor, metallic, roughness, ior,
        n, wo, NdotV, rayDir, frontFace, rngState,
        gl_LaunchIDEXT.xy, primaryDitherMode, lobeTemporalShift);

    // Store hitT for secondary_raygen (depth=1 = first bounce after raster primary)
    if (depth == 1u)
        payload.e.w = gl_HitTEXT;

    if (!scatter.doScatter || depth >= maxBounces)
    {
        payload.c.w = 1.0;
        payload.a.w = uintBitsToFloat(rngState);
        return;
    }

    // ---- NEE + MIS: direct lighting from a random light ----
    // Bounces always use uniform sampleNEE — RIS reservoirs are screen-space
    // (primary-hit only). Standard ReSTIR-DI scope.
    NEEDispatchResult nee = computeNEE(
        scatter, wo, n, hitPoint,
        baseColor, metallic, roughness, ior, mat.alphaMode,
        payload.a.xyz, rngState, false, false, 0u);

    payload.b.xyz += nee.radiance;

    // Punctual lights contribute a separate additive term. Independent of the
    // triangle/env selection above, so it needs no MIS weight and cannot
    // perturb those probabilities.
    payload.b.xyz += payload.a.xyz *
        samplePunctualNEE(wo, n, hitPoint, baseColor, metallic, rngState);

    float nextBsdfPdf = nee.nextBsdfPdf;

    // Update throughput
    vec3 newThroughput = payload.a.xyz * scatter.attenuation;

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
    // d.w = effective competing BSDF PDF, or -1 when diffuse NEE cannot compete.
    // Offset origin along the shading normal's side of the scatter direction:
    // refraction goes into the surface (dot < 0), so offset by -n; others by +n.
    vec3 offsetN = dot(scatter.scatterDir, n) > 0.0 ? n : -n;
    nextPayload.a = payload.a;
    nextPayload.b = vec4(vec3(0.0), float(depth + 1));
    nextPayload.c = vec4(hitPoint + offsetN * 0.001, 0.0);
    nextPayload.d = vec4(normalize(scatter.scatterDir), nextBsdfPdf);
    // e.x/e.y carry lobe metadata only for the immediate continuation. Always
    // initialize recursive payloads so stale register contents cannot be
    // mistaken for the raster-first joint estimator.
    nextPayload.e = vec4(scatter.lobeType, 0.0, 0.0, 0.0);

    traceRayEXT(topLevelAS, gl_RayFlagsNoneEXT, 0xFF, 0, 0, 0,
                nextPayload.c.xyz, 0.001, nextPayload.d.xyz, 1e9, 0);

    payload.b.xyz += nextPayload.b.xyz;
    payload.c.w = 1.0;
}
