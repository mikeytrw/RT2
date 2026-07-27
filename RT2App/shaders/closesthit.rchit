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

// Get the 3 vertex positions of the current hit triangle (world space).
// Vertices store object-space positions; transform by the instance's world
// matrix at hit time.
void hitTriPositions(out vec3 p0, out vec3 p1, out vec3 p2)
{
    uvec4 meshInfo = instanceMeshInfo[gl_InstanceID];
    uint idxBase = meshInfo.y + uint(gl_PrimitiveID) * 3u;
    uint i0 = indices[idxBase + 0u];
    uint i1 = indices[idxBase + 1u];
    uint i2 = indices[idxBase + 2u];
    mat4 world = instanceTransforms[gl_InstanceID];
    p0 = vec3(world * vertices[meshInfo.x + i0]);
    p1 = vec3(world * vertices[meshInfo.x + i1]);
    p2 = vec3(world * vertices[meshInfo.x + i2]);
}

// Barycentric coordinates of the hit point, straight from the hardware.
vec3 hitBarycentric()
{
    return vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
}

// Face normal computed from world-space triangle positions.
vec3 hitFaceNormal()
{
    vec3 p0, p1, p2;
    hitTriPositions(p0, p1, p2);
    return normalize(cross(p1 - p0, p2 - p0));
}

// Authored shading normal, barycentrically interpolated exactly like the
// raster path. meshInfo.z is UINT_MAX when a mesh has no normal stream.
vec3 hitVertexNormal()
{
    uvec4 meshInfo = instanceMeshInfo[gl_InstanceID];
    if (meshInfo.z == 0xFFFFFFFFu)
        return hitFaceNormal();

    uint idxBase = meshInfo.y + uint(gl_PrimitiveID) * 3u;
    uint i0 = indices[idxBase + 0u];
    uint i1 = indices[idxBase + 1u];
    uint i2 = indices[idxBase + 2u];

    vec3 n0 = normals[meshInfo.z + i0].xyz;
    vec3 n1 = normals[meshInfo.z + i1].xyz;
    vec3 n2 = normals[meshInfo.z + i2].xyz;
    vec3 bary = hitBarycentric();
    vec3 objectN = bary.x * n0 + bary.y * n1 + bary.z * n2;
    if (dot(objectN, objectN) < 1e-10)
        return hitFaceNormal();

    mat3 normalMatrix = transpose(inverse(mat3(instanceTransforms[gl_InstanceID])));
    return normalize(normalMatrix * objectN);
}

vec2 hitUV()
{
    uvec4 meshInfo = instanceMeshInfo[gl_InstanceID];
    uint idxBase = meshInfo.y + uint(gl_PrimitiveID) * 3u;
    uint i0 = indices[idxBase + 0u];
    uint i1 = indices[idxBase + 1u];
    uint i2 = indices[idxBase + 2u];

    vec2 uv0 = uvs[meshInfo.w + i0].xy;
    vec2 uv1 = uvs[meshInfo.w + i1].xy;
    vec2 uv2 = uvs[meshInfo.w + i2].xy;

    vec3 bary = hitBarycentric();
    return bary.x * uv0 + bary.y * uv1 + bary.z * uv2;
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
vec3 hitTangent()
{
    uvec4 meshInfo = instanceMeshInfo[gl_InstanceID];
    uint idxBase = meshInfo.y + uint(gl_PrimitiveID) * 3u;
    uint i0 = indices[idxBase + 0u];
    uint i1 = indices[idxBase + 1u];
    uint i2 = indices[idxBase + 2u];

    mat4 world = instanceTransforms[gl_InstanceID];
    vec3 p0 = vec3(world * vertices[meshInfo.x + i0]);
    vec3 p1 = vec3(world * vertices[meshInfo.x + i1]);
    vec3 p2 = vec3(world * vertices[meshInfo.x + i2]);

    vec2 uv0 = uvs[meshInfo.w + i0].xy;
    vec2 uv1 = uvs[meshInfo.w + i1].xy;
    vec2 uv2 = uvs[meshInfo.w + i2].xy;

    return computeTangent(p0, p1, p2, uv0, uv1, uv2);
}

// Get the shading normal from authored vertex normals, then apply a normal map.
// Falling back to the face normal is reserved for meshes without usable normals.
vec3 hitShadingNormal(Material mat, vec2 uv)
{
    vec3 baseN = hitVertexNormal();
    int normalTexIdx = mat.textureIndices.y;

    if (normalTexIdx >= 0)
    {
        vec3 tangentN = texture(textures[nonuniformEXT(normalTexIdx)], uv).rgb * 2.0 - 1.0;
        vec3 T = hitTangent();
        vec3 N = baseN;
        T = normalize(T - dot(T, N) * N);
        vec3 B = cross(N, T);
        return normalize(mat3(T, B, N) * tangentN);
    }

    return baseN;
}

void main()
{
    uvec4 meshInfo = instanceMeshInfo[gl_InstanceID];
    uint matIdx = resolveMaterialIndex(gl_InstanceID, uint(gl_PrimitiveID));
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
    vec3 normal = hitShadingNormal(mat, uv);

    vec3 rayDir = gl_WorldRayDirectionEXT;  // always unit-length (normalized at trace)
    bool frontFace = dot(rayDir, normal) < 0.0;
    vec3 n = frontFace ? normal : -normal;

    uint rngState = floatBitsToUint(payload.a.w);
    uint depth = uint(payload.b.w);
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
