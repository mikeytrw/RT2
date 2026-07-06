#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require

// scatter_shared.glsl includes pathtracer_shared.glsl + scatter/NEE functions
#include "scatter_shared.glsl"

layout(location = 0) rayPayloadInEXT RayPayload payload;
layout(location = 1) rayPayloadEXT RayPayload nextPayload;
// shadowVisible (location 2) is declared in scatter_shared.glsl

// Hardware-provided barycentric hit attributes (core GL_EXT_ray_tracing).
// attribs = (v, w) weights for vertices 1 and 2; u = 1 - v - w.
hitAttributeEXT vec2 attribs;

// Get the 3 vertex positions of the current hit triangle (world space).
// Combined buffers store object-space positions; transform by the
// instance's world matrix at hit time.
void hitTriPositions(out vec3 p0, out vec3 p1, out vec3 p2)
{
    uint triIdx = normalOffsets[gl_InstanceID] + uint(gl_PrimitiveID);
    uint posIdx = triIdx * 3u;
    mat4 world = instanceTransforms[gl_InstanceID];
    p0 = vec3(world * trianglePositions[posIdx + 0u]);
    p1 = vec3(world * trianglePositions[posIdx + 1u]);
    p2 = vec3(world * trianglePositions[posIdx + 2u]);
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

// Vertex tangents transformed to world space via the instance matrix.
vec3 hitTangent()
{
    uint triIdx = normalOffsets[gl_InstanceID] + uint(gl_PrimitiveID);
    uint posIdx = triIdx * 3u;

    mat3 worldMat3 = mat3(instanceTransforms[gl_InstanceID]);
    vec3 t0 = normalize(worldMat3 * triangleTangents[posIdx + 0u].xyz);
    vec3 t1 = normalize(worldMat3 * triangleTangents[posIdx + 1u].xyz);
    vec3 t2 = normalize(worldMat3 * triangleTangents[posIdx + 2u].xyz);

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

    // ---- Cook-Torrance GGX scatter (via shared function) ----
    vec3 wo = -rayDir;  // outgoing direction (away from surface, towards viewer)
    float NdotV = max(dot(n, wo), 0.0);

    ScatterResult scatter = scatterPrimaryHit(
        mat, baseColor, metallic, roughness, ior,
        n, wo, NdotV, rayDir, frontFace, rngState);

    // ---- NRD G-buffer capture at primary hit (depth=0) ----
    // Per-pixel data (normal/roughness, viewZ) is written straight to the
    // G-buffer images here — this shader knows the pixel via gl_LaunchIDEXT.
    // Data raygen needs for radiance routing/demodulation goes back through
    // payload.e (the ONLY legal return channel — see RayPayload note).
    if (depth == 0u && nrdData.nrdEnabled != 0u)
    {
        ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);

        // World-space normal + roughness. gNormalRoughness is rgba8 UNORM, so
        // the signed normal must be encoded to [0,1] (NRD RGBA8 unorm encoding).
        imageStore(gNormalRoughness, pixel, vec4(n * 0.5 + 0.5, roughness));

        // View-space Z: transform world position to view space
        vec4 viewPos = camera.worldToView * vec4(hitPoint, 1.0);
        float viewZ = viewPos.z;
        imageStore(gViewZ, pixel, vec4(viewZ, 0.0, 0.0, 0.0));

        // Motion vector: reproject world position into previous and current
        // screen space. NRD expects MV in UV space: pixelUvPrev = pixelUv + mv.
        vec4 currClip = camera.viewToClip * viewPos;
        vec4 prevView = camera.worldToViewPrev * vec4(hitPoint, 1.0);
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
        payload.e = vec4(uintBitsToFloat(packUnorm4x8(vec4(diffFactor, scatter.lobeType))),
                         uintBitsToFloat(packUnorm2x16(vec2(clamp(f0Scalar, 0.0, 1.0), clamp(roughness, 0.0, 1.0)))),
                         viewZ, gl_HitTEXT);
    }

    if (!scatter.doScatter || depth >= maxBounces)
    {
        payload.c.w = 1.0;
        payload.a.w = uintBitsToFloat(rngState);
        return;
    }

    // ---- NEE + MIS: direct lighting from a random light ----
    NEEDispatchResult nee = computeNEE(
        scatter, wo, n, hitPoint,
        baseColor, metallic, roughness, ior, mat.alphaMode,
        payload.a.xyz, rngState);

    payload.b.xyz += nee.radiance;

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
    // d.w = bsdfPdf (solid-angle): -1.0 for specular/delta, cos(θ)/pi for diffuse.
    // Offset origin along the shading normal's side of the scatter direction:
    // refraction goes into the surface (dot < 0), so offset by -n; others by +n.
    vec3 offsetN = dot(scatter.scatterDir, n) > 0.0 ? n : -n;
    nextPayload.a = payload.a;
    nextPayload.b = vec4(vec3(0.0), float(depth + 1));
    nextPayload.c = vec4(hitPoint + offsetN * 0.001, 0.0);
    nextPayload.d = vec4(normalize(scatter.scatterDir), nextBsdfPdf);

    traceRayEXT(topLevelAS, gl_RayFlagsNoneEXT, 0xFF, 0, 0, 0,
                nextPayload.c.xyz, 0.001, nextPayload.d.xyz, 1e9, 0);

    payload.b.xyz += nextPayload.b.xyz;
    payload.c.w = 1.0;
}