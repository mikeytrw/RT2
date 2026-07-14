// ray_query_scene.glsl — shared ray-query hit reconstruction and alpha traversal.
//
// Used by ReSTIR GI compute passes to trace rays against the TLAS without
// invoking RT closest-hit/any-hit shaders. Reconstructs hit data (positions,
// normals, UVs, tangent, material) from the committed intersection using the
// same buffers and logic as closesthit.rchit, and reproduces the alpha
// MASK/BLEND candidate-acceptance policy from anyhit.rahit / shadow.rahit.
//
// Include this AFTER restir_gi_bindings.glsl (which declares instanceMeshInfo,
// indices, vertices, normals, uvs, instanceTransforms, instanceMaterialIndices,
// instanceMatOffsets, materials, textures, material_resolve.glsl, TLAS, RNG).

#extension GL_EXT_ray_query : require

// ---- Alpha candidate test (shared by radiance + shadow traversal) -----------
// Returns true if the current candidate should be accepted (confirmed).
// Samples base alpha, applies MASK cutoff or BLEND Russian roulette.
// rngState is the salted substream for this ray's alpha decisions.
bool giAlphaAcceptCandidate(rayQueryEXT query, inout uint rngState)
{
    uint instanceID  = rayQueryGetIntersectionInstanceIdEXT(query, false);
    uint primitiveID = rayQueryGetIntersectionPrimitiveIndexEXT(query, false);
    uint matIdx = resolveMaterialIndex(instanceID, primitiveID);
    Material mat = materials[matIdx];

    // OPAQUE: accept.
    if (mat.alphaMode < 0.5)
        return true;

    // Reconstruct UV for this candidate to sample base alpha.
    uvec4 meshInfo = instanceMeshInfo[instanceID];
    uint idxBase = meshInfo.y + primitiveID * 3u;
    uint i0 = indices[idxBase + 0u];
    uint i1 = indices[idxBase + 1u];
    uint i2 = indices[idxBase + 2u];
    vec2 uv0 = uvs[meshInfo.w + i0].xy;
    vec2 uv1 = uvs[meshInfo.w + i1].xy;
    vec2 uv2 = uvs[meshInfo.w + i2].xy;
    vec2 baryCand = rayQueryGetIntersectionBarycentricsEXT(query, false);
    float u = 1.0 - baryCand.x - baryCand.y;
    vec2 uv = u * uv0 + baryCand.x * uv1 + baryCand.y * uv2;

    float alpha = mat.baseAlpha;
    int baseColorTexIdx = mat.textureIndices.x;
    if (baseColorTexIdx >= 0)
        alpha *= texture(textures[nonuniformEXT(baseColorTexIdx)], uv).a;

    // MASK: hard cutoff.
    if (mat.alphaMode < 1.5)
        return alpha >= mat.alphaCutoff;

    // BLEND: Russian roulette via salted substream.
    float r = randomFloat(rngState);
    return r < alpha;
}

// ---- Hit reconstruction result ----------------------------------------------
struct RayQueryHit
{
    bool  committed;       // false = no hit (miss / environment)
    bool  isEnvironment;   // true = ray missed all geometry
    uint  instanceID;      // gl_InstanceID equivalent
    uint  primitiveID;     // gl_PrimitiveID equivalent
    vec2  bary;            // (v, w) — same convention as committed barycentrics
    float hitT;            // ray parameter to the committed hit
    vec3  worldPos;        // hit point in world space
    vec3  geoNormal;      // geometric face normal (world space, normalized)
    vec3  shadingNormal;   // normal-mapped shading normal (world space, normalized)
    vec2  uv;              // interpolated texture coordinate
    vec3  tangent;         // tangent basis (world space, normalized)
    uint  matIdx;          // resolved per-triangle material index
    Material mat;          // resolved material
    vec3  baseColor;       // texture-sampled base color
    float metallic;        // texture-sampled metallic factor
    float roughness;       // texture-sampled roughness factor
    vec3  emissive;        // texture-sampled emissive color (× intensity)
    bool  frontFace;       // true = ray hit front face
};

// ---- Hit reconstruction from committed intersection -------------------------
// Matches closesthit.rchit: hitTriPositions, hitBarycentric, hitFaceNormal,
// hitUV, hitTangent, hitShadingNormal, material resolution, texture sampling.
RayQueryHit giReconstructHit(rayQueryEXT query, vec3 rayOrigin, vec3 rayDir)
{
    RayQueryHit h;
    h.committed = false;
    h.isEnvironment = false;
    h.frontFace = true;

    uint committedType = rayQueryGetIntersectionTypeEXT(query, true);

    if (committedType == gl_RayQueryCommittedIntersectionNoneEXT)
    {
        h.isEnvironment = true;
        return h;
    }

    h.committed = true;
    h.instanceID  = rayQueryGetIntersectionInstanceIdEXT(query, true);
    h.primitiveID = rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
    h.bary        = rayQueryGetIntersectionBarycentricsEXT(query, true);
    h.hitT        = rayQueryGetIntersectionTEXT(query, true);
    h.worldPos    = rayOrigin + h.hitT * rayDir;

    // ---- Triangle vertices (world space) — matches hitTriPositions ----
    uvec4 meshInfo = instanceMeshInfo[h.instanceID];
    uint idxBase = meshInfo.y + h.primitiveID * 3u;
    uint i0 = indices[idxBase + 0u];
    uint i1 = indices[idxBase + 1u];
    uint i2 = indices[idxBase + 2u];
    mat4 world = instanceTransforms[h.instanceID];
    vec3 p0 = vec3(world * vertices[meshInfo.x + i0]);
    vec3 p1 = vec3(world * vertices[meshInfo.x + i1]);
    vec3 p2 = vec3(world * vertices[meshInfo.x + i2]);

    // ---- Barycentrics (hardware convention: attribs = (v, w)) ----
    float b0 = 1.0 - h.bary.x - h.bary.y;
    float b1 = h.bary.x;
    float b2 = h.bary.y;

    // ---- Face normal ----
    h.geoNormal = normalize(cross(p1 - p0, p2 - p0));

    // ---- UV — matches hitUV ----
    vec2 uv0 = uvs[meshInfo.w + i0].xy;
    vec2 uv1 = uvs[meshInfo.w + i1].xy;
    vec2 uv2 = uvs[meshInfo.w + i2].xy;
    h.uv = b0 * uv0 + b1 * uv1 + b2 * uv2;

    // ---- Tangent — matches hitTangent / computeTangent ----
    vec3 edge1 = p1 - p0;
    vec3 edge2 = p2 - p0;
    vec2 dUV1 = uv1 - uv0;
    vec2 dUV2 = uv2 - uv0;
    float det = dUV1.x * dUV2.y - dUV1.y * dUV2.x;
    if (abs(det) < 1e-8)
        h.tangent = vec3(1.0, 0.0, 0.0);
    else
    {
        float r = 1.0 / det;
        h.tangent = normalize(r * (dUV2.y * edge1 - dUV1.y * edge2));
    }

    // ---- Material resolution (canonical per-triangle path) ----
    h.matIdx = resolveMaterialIndex(h.instanceID, h.primitiveID);
    h.mat = materials[h.matIdx];

    // ---- Base color, metallic, roughness, emissive (matches closesthit) ----
    h.baseColor = h.mat.baseColor_metallic.xyz;
    int baseColorTexIdx = h.mat.textureIndices.x;
    if (baseColorTexIdx >= 0)
        h.baseColor *= texture(textures[nonuniformEXT(baseColorTexIdx)], h.uv).rgb;

    h.metallic = h.mat.baseColor_metallic.w;
    h.roughness = h.mat.emissive_roughness.w;
    int metalRoughTexIdx = h.mat.extraIndices.x;
    if (metalRoughTexIdx >= 0)
    {
        vec3 mr = texture(textures[nonuniformEXT(metalRoughTexIdx)], h.uv).rgb;
        h.roughness *= mr.g;
        h.metallic *= mr.b;
    }

    h.emissive = h.mat.emissive_roughness.xyz;
    int emissiveTexIdx = h.mat.textureIndices.z;
    if (emissiveTexIdx >= 0)
        h.emissive *= texture(textures[nonuniformEXT(emissiveTexIdx)], h.uv).rgb;

    // ---- Shading normal (normal-mapped or geometric) — matches hitShadingNormal ----
    int normalTexIdx = h.mat.textureIndices.y;
    if (normalTexIdx >= 0)
    {
        vec3 tangentN = texture(textures[nonuniformEXT(normalTexIdx)], h.uv).rgb * 2.0 - 1.0;
        vec3 T = h.tangent;
        vec3 N = h.geoNormal;
        T = normalize(T - dot(T, N) * N);
        vec3 B = cross(N, T);
        h.shadingNormal = normalize(mat3(T, B, N) * tangentN);
    }
    else
    {
        h.shadingNormal = h.geoNormal;
    }

    // ---- Front/back face — matches closesthit ----
    h.frontFace = dot(rayDir, h.shadingNormal) < 0.0;

    return h;
}

// ---- Radiance ray query (with alpha traversal) ------------------------------
// Traces a radiance ray from origin along dir (up to tMax), performs alpha
// traversal, and returns the reconstructed hit. If the ray misses all
// geometry, h.isEnvironment is true.
RayQueryHit giTraceRadiance(vec3 origin, vec3 dir, float tMin, float tMax,
                            uint alphaSeed)
{
    rayQueryEXT query;
    rayQueryInitializeEXT(query, tlas,
                          gl_RayFlagsNoneEXT,
                          0xFF,
                          origin, tMin,
                          dir, tMax);

    uint rngState = alphaSeed;
    while (rayQueryProceedEXT(query))
    {
        if (rayQueryGetIntersectionTypeEXT(query, false) ==
            gl_RayQueryCandidateIntersectionTriangleEXT)
        {
            if (giAlphaAcceptCandidate(query, rngState))
                rayQueryConfirmIntersectionEXT(query);
        }
    }

    return giReconstructHit(query, origin, dir);
}

// ---- Shadow ray query (with alpha traversal) -------------------------------
// Returns true if the path from origin to targetPoint is unoccluded.
// Uses gl_RayFlagsTerminateOnFirstHitEXT so the first accepted candidate
// terminates the query (matches shadow.rahit "accept = blocks light").
bool giTraceShadow(vec3 origin, vec3 dir, float dist, uint alphaSeed)
{
    rayQueryEXT query;
    rayQueryInitializeEXT(query, tlas,
                          gl_RayFlagsTerminateOnFirstHitEXT,
                          0xFF,
                          origin, 0.001,
                          dir, dist - 0.002);

    uint rngState = alphaSeed;
    while (rayQueryProceedEXT(query))
    {
        if (rayQueryGetIntersectionTypeEXT(query, false) ==
            gl_RayQueryCandidateIntersectionTriangleEXT)
        {
            if (giAlphaAcceptCandidate(query, rngState))
                rayQueryConfirmIntersectionEXT(query);
        }
    }

    uint committedType = rayQueryGetIntersectionTypeEXT(query, true);
    // If nothing was committed, the path is unoccluded.
    return committedType == gl_RayQueryCommittedIntersectionNoneEXT;
}