// Shared declarations for the ray tracing path tracer stages.
// Included by rgen.rgen, miss.rmiss, closesthit.rchit.

#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require

// Include shared C++/GLSL interface header for binding #defines
#include "shader_interface.h"
#include "rr_guide_shared.glsl"

// ---- Bindings (set 0) -------------------------------------------------------

layout(set = 0, binding = SI_BINDING_OUTPUT_IMAGE, rgba32f) uniform image2D outputImage;

layout(set = 0, binding = SI_BINDING_CAMERA_UBO, std140) uniform CameraData
{
    vec4 position;      // xyz = position, w = frameIndex
    vec4 forward;       // xyz = forward, w = NRD jitter.x
    vec4 right;         // xyz = right, w = NRD jitter.y
    vec4 up;            // xyz = up, w = pad
    vec4 viewportSPP;   // x = width, y = height, z = spp, w = maxBounces
    vec4 apertureFocal; // x = aperture, y = focusDistance, z = showBackground, w = emissiveBoost
    vec4 envMap;        // x = envMapIndex (-1=none), y = envIntensity, z = marginalCDFIdx, w = conditionalCDFIdx
    mat4 inverseProjection;
    mat4 inverseView;
    // NRD G-buffer inputs (set 1, bindings 0-4):
    mat4 viewToClip;        // current frame view-to-clip (for viewZ + motion)
    mat4 viewToClipPrev;    // previous frame view-to-clip
    mat4 worldToView;       // current world-to-view
    mat4 worldToViewPrev;   // previous world-to-view
} camera;

// PBR material — matches GPUMaterial in GPUSceneData.h (80 bytes, std430)
// Struct layout is verified by static_asserts in GPUSceneData.h against
// SIMaterial in shader_interface.h.
struct Material
{
    vec4 baseColor_metallic;   // xyz = base color, w = metallic factor
    vec4 emissive_roughness;   // xyz = emissive * intensity, w = roughness
    float ior;                 // index of refraction (for dielectric)
    float alphaCutoff;         // alpha cutoff (MASK mode)
    float alphaMode;           // 0=OPAQUE, 1=MASK, 2=BLEND
    float baseAlpha;           // baseColorFactor.a (1.0 = fully opaque, for any-hit)
    ivec4 textureIndices;      // x = baseColor, y = normal, z = emissive,
                               // w = floatBitsToInt(transmissionFactor)
    ivec4 extraIndices;        // x = metallicRoughness texture index, yzw = pad
};

layout(set = 0, binding = SI_BINDING_MATERIAL_BUFFER, std430) readonly buffer MaterialBuffer
{
    Material materials[];
};

layout(set = 0, binding = SI_BINDING_VERTEX_BUFFER, std430) readonly buffer VertexBuffer
{
    vec4 vertices[]; // xyz = position, w = 1.0
};

layout(set = 0, binding = SI_BINDING_INDEX_BUFFER, std430) readonly buffer IndexBuffer
{
    uint indices[];
};

layout(set = 0, binding = SI_BINDING_NORMAL_BUFFER, std430) readonly buffer NormalBuffer
{
    vec4 normals[]; // xyz = normal, w = 0.0
};

layout(set = 0, binding = SI_BINDING_UV_BUFFER, std430) readonly buffer UVBuffer
{
    vec4 uvs[]; // xy = UV, zw = 0.0
};

layout(set = 0, binding = SI_BINDING_INSTANCE_MESH_INFO, std430) readonly buffer InstanceMeshInfo
{
    uvec4 instanceMeshInfo[]; // x = vertOffset, y = idxOffset, z = normOffset, w = uvOffset
};

// ---- Light buffer (NEE) -----------------------------------------------------
// std430 buffer with a 16-byte header followed by a flat array of TriangleLight.
// The shader picks a light ~ area, samples a point on its triangle, and traces
// a shadow ray to test visibility.
struct TriangleLight
{
    vec4  emission_area;  // xyz = emissiveColor*intensity (flat fallback), w = area
    uvec4 ids;            // x = instanceID, y = primitiveID, z = materialIndex, w = emissiveTexIdx
};

layout(set = 0, binding = SI_BINDING_LIGHT_BUFFER, std430) readonly buffer LightBuffer
{
    uint  lightCount;
    float totalLightArea;
    uint  _lightPad0;
    uint  _lightPad1;
    TriangleLight lights[];
};

// Punctual lights (Phase 8) — point, spot and directional. Deliberately NOT
// part of the triangle-vs-env stochastic selection above.
//
// That selection exists because both of those arms draw a random sample and
// need MIS to combine. A punctual light has no area: given a shading point
// there is exactly one direction to it, so its contribution is a
// deterministic, zero-variance term. Making it a third arm of computePTri
// would mean touching every consumer of pTri across four shader files that
// each carry their own copy — and missing one biases the estimator silently,
// producing a plausible-looking picture with the wrong energy.
//
// Adding it as an independent term instead cannot double-count: punctual
// lights appear in neither the triangle light list nor the environment map.
struct PunctualLight
{
    vec4 position_range;   // xyz = world position, w = range (0 = unbounded)
    vec4 direction_type;   // xyz = world direction (unit), w = type
    vec4 color_intensity;  // xyz = colour, w = intensity
    vec4 cone;             // x = cos(inner), y = cos(outer), zw = pad
};

layout(set = 0, binding = SI_BINDING_PUNCTUAL_LIGHT_BUFFER, std430) readonly buffer PunctualLightBuffer
{
    uint  punctualLightCount;
    uint  _punctualPad0;
    uint  _punctualPad1;
    uint  _punctualPad2;
    PunctualLight punctualLights[];
};

// Direction to the light, its distance, and the intensity arriving before
// visibility. Distance is +inf for directional lights so the shadow ray runs
// to the far plane.
struct PunctualSample
{
    vec3  toLight;    // unit direction from the shading point to the light
    float distance;   // shadow ray length
    vec3  radiance;   // colour * intensity * falloff, before visibility
    bool  valid;
};

PunctualSample evalPunctualLight(uint index, vec3 worldPos)
{
    PunctualSample s;
    s.toLight = vec3(0.0, 1.0, 0.0);
    s.distance = 0.0;
    s.radiance = vec3(0.0);
    s.valid = false;

    PunctualLight L = punctualLights[index];
    float type = L.direction_type.w;
    vec3 intensity = L.color_intensity.xyz * L.color_intensity.w;

    if (type == SI_LIGHT_TYPE_DIRECTIONAL)
    {
        // Parallel rays: no position, no distance falloff.
        s.toLight = normalize(-L.direction_type.xyz);
        s.distance = 1e30;
        s.radiance = intensity;
        s.valid = true;
        return s;
    }

    vec3 delta = L.position_range.xyz - worldPos;
    float dist2 = dot(delta, delta);
    if (dist2 < 1e-12) return s;   // shading point coincides with the light

    float dist = sqrt(dist2);
    s.toLight = delta / dist;
    s.distance = dist;

    // Inverse-square falloff. This is why a small emissive needs a radiance in
    // the thousands to rival an environment map, and why a punctual light —
    // authored as intensity, with no area term — does not.
    vec3 radiance = intensity / dist2;

    float range = L.position_range.w;
    if (range > 0.0)
    {
        if (dist >= range) return s;
        // Windowed falloff (glTF): fades to exactly zero at `range` rather
        // than clipping, so a light does not end in a visible hard edge.
        float t = dist / range;
        float window = clamp(1.0 - t * t * t * t, 0.0, 1.0);
        radiance *= window * window;
    }

    if (type == SI_LIGHT_TYPE_SPOT)
    {
        // cone.x = cos(inner) >= cone.y = cos(outer), clamped CPU-side so a
        // malformed cone cannot invert this and light the whole hemisphere.
        float cosAngle = dot(normalize(L.direction_type.xyz), -s.toLight);
        float denom = max(L.cone.x - L.cone.y, 1e-4);
        float spot = clamp((cosAngle - L.cone.y) / denom, 0.0, 1.0);
        if (spot <= 0.0) return s;
        radiance *= spot * spot;
    }

    s.radiance = radiance;
    s.valid = true;
    return s;
}

// Probability of selecting triangle NEE (vs env NEE) in stochastic NEE.
// Returns 1.0 if only triangle lights, 0.0 if only env, 0.5 if both.
float computePTri()
{
    bool hasTri = (lightCount > 0u && totalLightArea > 0.0);
    bool hasEnv = (int(camera.envMap.x) >= 0);
    if (hasTri && hasEnv) return 0.5;
    if (hasTri)            return 1.0;
    return 0.0;
}

// Per-instance world transform matrices (object-to-world).
// Combined buffers store object-space data; the shader transforms to
// world space at hit time using instanceTransforms[gl_InstanceID].
layout(set = 0, binding = SI_BINDING_INSTANCE_TRANSFORMS, std430) readonly buffer InstanceTransformBuffer
{
    mat4 instanceTransforms[];
};

// Per-triangle material indices (binding 13 — shared with raster path).
// When the buffer has per-triangle entries, overrides gl_InstanceCustomIndexEXT.
layout(set = 0, binding = SI_BINDING_INSTANCE_MATERIAL_INDICES, std430) readonly buffer InstanceMaterialIndexBuffer
{
    uint instanceMaterialIndices[];
};

// Per-instance material index offsets (binding 17).
// Each instance has its own section in instanceMaterialIndices, starting at this offset.
layout(set = 0, binding = SI_BINDING_INSTANCE_MAT_OFFSETS, std430) readonly buffer InstanceMatOffsetBuffer
{
    uint instanceMatOffsets[];
};

// Bindless texture array (combined image samplers, variable count)
// Must be the highest binding number for VARIABLE_DESCRIPTOR_COUNT.
layout(set = 0, binding = SI_BINDING_TEXTURE_ARRAY) uniform sampler2D textures[];

layout(set = 0, binding = SI_BINDING_TLAS) uniform accelerationStructureEXT topLevelAS;

// ---- NRD G-buffer outputs (set 1) -------------------------------------------
// Storage images written by closesthit at the primary hit (depth=0) and
// read by the NRD denoiser. When NRD is disabled, these are unused.
// NRD expects A2B10G10R10_UNORM_PACK32 + oct-packed normals.
layout(set = 1, binding = SI_BINDING_G_NORMAL_ROUGHNESS, rgb10_a2) uniform image2D gNormalRoughness;  // xyz = oct-packed normal+roughness, w = unused
layout(set = 1, binding = SI_BINDING_G_VIEWZ, r32f)  uniform image2D gViewZ;            // view-space Z (fp32 — fp16 overflows for large scenes)
layout(set = 1, binding = SI_BINDING_G_MOTION, rg16f) uniform image2D gMotion;           // 2D screen-space motion vector
layout(set = 1, binding = SI_BINDING_G_DIFF_RADIANCE, rgba16f) uniform image2D gDiffRadianceHitDist; // rgb = diffuse radiance, a = hitT
layout(set = 1, binding = SI_BINDING_G_SPEC_RADIANCE, rgba16f) uniform image2D gSpecRadianceHitDist; // rgb = specular radiance, a = hitT
layout(set = 1, binding = SI_BINDING_G_ALBEDO_F0, rgba16f) uniform image2D gAlbedoF0;            // rgb = demod albedo, a = F0 scalar

// Direct emission (emissive surfaces + sky) — bypasses NRD, added in compose
layout(set = 1, binding = SI_BINDING_G_DIRECT_EMISSION, rgba16f) uniform image2D gDirectEmission;
layout(set = 1, binding = SI_BINDING_RR_NOISY_HDR, r11f_g11f_b10f) uniform image2D rrNoisyHdr;
layout(set = 1, binding = SI_BINDING_RR_HIT_DISTANCE, r32f) uniform image2D rrHitDistance;

// NRD enable flag (1 = NRD mode, 0 = normal temporal accumulation)
layout(set = 1, binding = SI_BINDING_NRD_UBO) uniform NRDUniform
{
    uint nrdEnabled;            // 1 = NRD mode (1 spp, no temporal accum, write G-buffer)
    uint lobeDither;            // 0=off (white noise), 1=Bayer 4x4, 2=Interleaved Gradient Noise
    uint restirGIEnabled;       // 1 = ReSTIR GI active (consume stored GI sample in raygen)
    uint restirGIReservoirIndex;// current GI reservoir region index (frame parity)
} nrdData;

// ---- Payload ----------------------------------------------------------------
// Packed into explicit vec4 rows to avoid cross-stage std430 alignment
// ambiguity when mixing vec3/scalar fields in rayPayloadEXT.
//
// NOTE: a closest-hit shader may declare exactly ONE rayPayloadInEXT variable
// (VUID-StandaloneSpirv-IncomingRayPayloadKHR-04700) and it aliases the payload
// referenced by the caller's traceRayEXT. Any data the primary hit must return
// to raygen therefore lives HERE (row e), not in a second payload.
struct RayPayload
{
    vec4 a; // xyz = throughput, w = rngState
    vec4 b; // xyz = radiance,   w = depth
    vec4 c; // xyz = ray origin, w = done (0/1)
    vec4 d; // xyz = ray dir,    w = bsdfPdf
    vec4 e; // NRD primary-hit info (written by closesthit at depth 0 only):
            // x = uintBitsToFloat(packUnorm4x8(vec4(demodAlbedo, lobeType)))
            // y = uintBitsToFloat(packUnorm2x16(vec2(F0, roughness)))
            // z = viewZ (view-space Z at primary hit)
            // w = primary hitT (0 = primary ray missed or hit an emitter)
};

// ---- RNG (PCG) --------------------------------------------------------------

uint pcg(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float randomFloat(inout uint state)
{
    // Use the top 24 bits so the result is exactly representable in float
    // and strictly < 1.0 (safe for "r < P" lobe-selection comparisons).
    return float(pcg(state) >> 8u) * (1.0 / 16777216.0);
}

// ---- Helpers ----------------------------------------------------------------

#define PI 3.14159265359

// Branchless-ish orthonormal basis around unit vector n (n = local z-axis).
void buildONB(vec3 n, out vec3 T, out vec3 B)
{
    if (abs(n.z) > 0.999)
    {
        T = vec3(1.0, 0.0, 0.0);
        B = vec3(0.0, 1.0, 0.0);
    }
    else
    {
        vec3 up = (abs(n.y) < 0.999) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        T = normalize(cross(up, n));
        B = cross(n, T);
    }
}

// Analytic cosine-weighted hemisphere sample around n.
// pdf(wi) = dot(n, wi) / PI — matches pdfDiffuse() exactly.
// (Replaces the old rejection-sampled n + randomInUnitSphere, which was
// both slower — divergent loop, up to 30 RNG draws — and not exactly
// cosine-distributed.)
vec3 sampleCosineHemisphere(vec3 n, inout uint rngState)
{
    float r1 = randomFloat(rngState);
    float r2 = randomFloat(rngState);
    float phi = 2.0 * PI * r1;
    float sr2 = sqrt(r2);
    vec3 T, B;
    buildONB(n, T, B);
    vec3 local = vec3(cos(phi) * sr2, sin(phi) * sr2, sqrt(max(1.0 - r2, 0.0)));
    return normalize(T * local.x + B * local.y + n * local.z);
}

// Analytic uniform sample on the unit disk (for depth of field).
vec2 sampleUnitDisk(inout uint rngState)
{
    float r1 = randomFloat(rngState);
    float r2 = randomFloat(rngState);
    float r = sqrt(r1);
    float phi = 2.0 * PI * r2;
    return vec2(r * cos(phi), r * sin(phi));
}

vec3 skyColor(vec3 direction)
{
    float t = 0.5 * (direction.y + 1.0);
    return (1.0 - t) * vec3(1.0, 1.0, 1.0) + t * vec3(0.5, 0.7, 1.0);
}

// ---- Environment map (M8) ---------------------------------------------------

// Convert a direction to equirectangular UV coordinates.
// Exact inverse of envUVToDirection: u ∈ [-0.5, 0.5] (wraps), v ∈ [0, 1].
vec2 directionToEnvUV(vec3 dir)
{
    // The bindless sampler clamps U, so wrap explicitly. Radiance and PDF
    // must address the same texel or HDR importance weights become biased.
    float u = fract(atan(dir.z, dir.x) * 0.15915494309);  // [0, 1)
    float v = asin(clamp(dir.y, -1.0, 1.0)) * 0.31830988618;  // asin/π ∈ [-0.5, 0.5]
    return vec2(u, 0.5 - v);  // flip V for image convention, spans [0, 1]
}

// Convert equirectangular UV to a direction
vec3 envUVToDirection(vec2 uv)
{
    float theta = uv.x * 2.0 * PI;          // azimuth
    float phi = (0.5 - uv.y) * PI;           // polar angle from horizon
    float cosPhi = cos(phi);
    return vec3(cos(theta) * cosPhi, sin(phi), sin(theta) * cosPhi);
}

// Sample the environment map radiance for a given direction.
// Returns vec3(0) if no env map is loaded.
vec3 envMapRadiance(vec3 dir)
{
    int envIdx = int(camera.envMap.x);
    if (envIdx < 0)
        return vec3(0.0);
    vec2 uv = directionToEnvUV(dir);
    vec3 radiance = texture(textures[nonuniformEXT(envIdx)], uv).rgb;
    return radiance * camera.envMap.y;  // envIntensity
}

// Sample the environment map importance-sampled using CDFs.
// Returns (direction, pdf) where pdf is the solid-angle PDF.
// Uses marginal + conditional CDF textures for 2D inverse CDF sampling.
struct EnvSample
{
    vec3  dir;
    float pdf;
    vec3  radiance;
};

EnvSample sampleEnvMap(inout uint rngState)
{
    EnvSample s;
    s.dir = vec3(0.0);
    s.pdf = 0.0;
    s.radiance = vec3(0.0);

    int envIdx = int(camera.envMap.x);
    int marginalIdx = int(camera.envMap.z);
    int conditionalIdx = int(camera.envMap.w);
    if (envIdx < 0 || marginalIdx < 0 || conditionalIdx < 0)
        return s;

    // Inverse CDF sampling via binary search on the CDF textures.
    // The CDFs are monotonically increasing, so binary search works.

    // Sample marginal CDF (1D texture, height entries) — binary search
    float xi1 = randomFloat(rngState);
    ivec2 marginalSize = textureSize(textures[nonuniformEXT(marginalIdx)], 0);
    int marginalLen = marginalSize.x;

    int lo = 0;
    int hi = marginalLen - 1;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        float cdfVal = texelFetch(textures[nonuniformEXT(marginalIdx)], ivec2(mid, 0), 0).r;
        if (cdfVal < xi1)
            lo = mid + 1;
        else
            hi = mid;
    }
    int vIdx = lo;

    // Sample conditional CDF (2D texture, width×height) for row vIdx — binary search
    float xi2 = randomFloat(rngState);
    ivec2 condSize = textureSize(textures[nonuniformEXT(conditionalIdx)], 0);
    int condW = condSize.x;

    lo = 0;
    hi = condW - 1;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        float cdfVal = texelFetch(textures[nonuniformEXT(conditionalIdx)], ivec2(mid, vIdx), 0).r;
        if (cdfVal < xi2)
            lo = mid + 1;
        else
            hi = mid;
    }
    int uIdx = lo;

    // Convert pixel indices to UV
    float u = (float(uIdx) + 0.5) / float(condW);
    float v = (float(vIdx) + 0.5) / float(marginalLen);

    // Convert UV to direction
    s.dir = envUVToDirection(vec2(u, v));

    // Compute PDF: p(u,v) = luminance(envMap(u,v)) / totalLuminance
    // The CDF stores cumulative probability, so the PDF is:
    // p(v) = marginalCDF[v] - marginalCDF[v-1]
    // p(u|v) = conditionalCDF[u,v] - conditionalCDF[u-1,v]
    // p(dir) = p(u,v) / (2π² * sin(θ))
    // where θ is the polar angle and the Jacobian of the spherical mapping.
    float sinTheta = sqrt(max(1.0 - s.dir.y * s.dir.y, 1e-6));

    // Marginal PDF
    float margPrev = (vIdx > 0) ? texelFetch(textures[nonuniformEXT(marginalIdx)], ivec2(vIdx - 1, 0), 0).r : 0.0;
    float margCurr = texelFetch(textures[nonuniformEXT(marginalIdx)], ivec2(vIdx, 0), 0).r;
    float pdfV = max(margCurr - margPrev, 1e-8);

    // Conditional PDF
    float condPrev = (uIdx > 0) ? texelFetch(textures[nonuniformEXT(conditionalIdx)], ivec2(uIdx - 1, vIdx), 0).r : 0.0;
    float condCurr = texelFetch(textures[nonuniformEXT(conditionalIdx)], ivec2(uIdx, vIdx), 0).r;
    float pdfU = max(condCurr - condPrev, 1e-8);

    // pdfV*pdfU is probability mass for one texel. Convert it to UV density
    // by dividing by the texel area (1 / width*height), then apply the
    // equirectangular UV-to-solid-angle Jacobian.
    float texelDensityScale = float(condW) * float(marginalLen);
    s.pdf = (pdfV * pdfU * texelDensityScale) /
            (sinTheta * 2.0 * PI * PI);

    // Sample radiance
    s.radiance = envMapRadiance(s.dir);

    return s;
}

// Compute the env map solid-angle PDF for an arbitrary direction.
// This is the inverse of sampleEnvMap: given a direction, look up the
// CDF density at its corresponding pixel and convert to solid-angle PDF.
float envMapPdf(vec3 dir)
{
    int envIdx = int(camera.envMap.x);
    int marginalIdx = int(camera.envMap.z);
    int conditionalIdx = int(camera.envMap.w);
    if (envIdx < 0 || marginalIdx < 0 || conditionalIdx < 0)
        return 0.0;

    ivec2 marginalSize = textureSize(textures[nonuniformEXT(marginalIdx)], 0);
    int marginalLen = marginalSize.x;
    ivec2 condSize = textureSize(textures[nonuniformEXT(conditionalIdx)], 0);
    int condW = condSize.x;

    vec2 uv = directionToEnvUV(dir);
    // Convert UV to pixel indices (same quantization as sampleEnvMap).
    // uv.x from atan() is in [-0.5, 0.5] — wrap to [0, 1) before quantizing
    // (clamping would collapse half the sphere onto column 0).
    int vIdx = int(clamp(uv.y * float(marginalLen), 0.0, float(marginalLen - 1)));
    int uIdx = int(clamp(uv.x * float(condW),       0.0, float(condW - 1)));

    // Marginal PDF: p(v) = marginalCDF[v] - marginalCDF[v-1]
    float margPrev = (vIdx > 0) ? texelFetch(textures[nonuniformEXT(marginalIdx)], ivec2(vIdx - 1, 0), 0).r : 0.0;
    float margCurr = texelFetch(textures[nonuniformEXT(marginalIdx)], ivec2(vIdx, 0), 0).r;
    float pdfV = max(margCurr - margPrev, 1e-8);

    // Conditional PDF: p(u|v) = conditionalCDF[u,v] - conditionalCDF[u-1,v]
    float condPrev = (uIdx > 0) ? texelFetch(textures[nonuniformEXT(conditionalIdx)], ivec2(uIdx - 1, vIdx), 0).r : 0.0;
    float condCurr = texelFetch(textures[nonuniformEXT(conditionalIdx)], ivec2(uIdx, vIdx), 0).r;
    float pdfU = max(condCurr - condPrev, 1e-8);

    // CDF differences are discrete texel probability masses, so multiply by
    // width*height to obtain a density over normalized UV before converting
    // to a density over solid angle.
    float sinTheta = sqrt(max(1.0 - dir.y * dir.y, 1e-6));
    float texelDensityScale = float(condW) * float(marginalLen);
    return (pdfV * pdfU * texelDensityScale) /
           (sinTheta * 2.0 * PI * PI);
}

float reflectance(float cosine, float refIdx)
{
    float r0 = (1.0 - refIdx) / (1.0 + refIdx);
    r0 = r0 * r0;
    return r0 + (1.0 - r0) * pow(1.0 - cosine, 5.0);
}

// ---- NRD helpers (oct normal packing, YCoCg, hit distance normalization) -----

// NRD normal/roughness packing (matches NRD_NORMAL_ENCODING=2, NRD_ROUGHNESS_ENCODING=1)
// Ported from NRD.hlsli _NRD_EncodeNormalRoughness101010 / _NRD_DecodeNormalRoughness101010.
// The packed image format is A2B10G10R10_UNORM_PACK32, but storage image format qualifier
// is rgba8 in GLSL (GL does not expose A2B10G10R10 as a storage format qualifier; the
// Vulkan backend reinterprets). We pack into a vec3 (rgb) and store .xyz, leaving .w=0.
// The pack stores: xy = octahedral normal, z = signed roughness (encodes n.z sign).

// Encode: N (unit normal, world space) + linear roughness → vec3 for imageStore
// NRD_ROUGHNESS_ENCODING=1 (LINEAR): stores linear roughness directly.
vec3 nrdEncodeNormalRoughness(vec3 n, float roughness)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);

    vec3 r;
    r.y = n.y * 0.5 + 0.5;
    r.x = n.x * 0.5 + r.y;
    r.y -= n.x * 0.5;

    // LINEAR encoding: store linear roughness in the z channel
    roughness = max(roughness, 1.5 / 512.0); // can't be 0 to not ruin n.z sign bit
    float s = n.z < 0.0 ? -roughness : roughness;
    r.z = s * 0.5 + 0.5;

    return r;
}

// Decode: vec3 from imageLoad → vec4(xyz = unit normal, w = linear roughness)
vec4 nrdDecodeNormalRoughness(vec3 p)
{
    float t = p.z * 2.0 - 1.0; // signed roughness

    vec4 r;
    r.x = p.x - p.y;
    r.y = p.x + p.y - 1.0;
    r.z = t < 0.0 ? -1.0 : 1.0;
    r.z *= 1.0 - abs(r.x) - abs(r.y);

    r.w = abs(t); // linear roughness (NRD_ROUGHNESS_ENCODING=1 = LINEAR)

    // Normalize normal
    r.xyz = normalize(r.xyz);

    return r;
}

// NRD REBLUR uses YCoCg color space for radiance packing.
vec3 linearToYCoCg(vec3 c)
{
    float Y  = dot(c, vec3(0.25, 0.5, 0.25));
    float Co = dot(c, vec3(0.5, 0.0, -0.5));
    float Cg = dot(c, vec3(-0.25, 0.5, -0.25));
    return vec3(Y, Co, Cg);
}

vec3 YCoCgToLinear(vec3 c)
{
    float t = c.x - c.z;
    return max(vec3(t + c.y, c.x + c.z, t - c.y), vec3(0.0));
}

// NRD spec magic curve (for hit distance normalization)
float nrdSpecMagicCurve(float roughness, float power)
{
    float f = 1.0 - exp2(-200.0 * roughness * roughness);
    f *= pow(clamp(roughness, 0.0, 1.0), power);
    return f;
}

// NRD REBLUR hit distance normalization
// hitDistParams = (A, B, C) from ReblurHitDistanceParameters (defaults: 3.0, 0.1, 20.0)
// For diffuse: roughness = 1.0
float nrdGetHitDistanceNormalization(float viewZ, vec3 hitDistParams, float roughness)
{
    float smc = nrdSpecMagicCurve(roughness, 0.5);
    return (hitDistParams.x + abs(viewZ) * hitDistParams.y) * mix(hitDistParams.z, 1.0, smc);
}

float nrdGetNormHitDist(float hitDist, float viewZ, vec3 hitDistParams, float roughness)
{
    float f = nrdGetHitDistanceNormalization(viewZ, hitDistParams, roughness);
    return clamp(hitDist / f, 0.0, 1.0);
}

// NRD environment term (Ray Tracing Gems Ch32, Eq 4) — GGX VNDF + Schlick.
// Ported from NRD.hlsli _NRD_EnvironmentTerm_Rtg.
// Used by NRD_MaterialFactors for proper demodulation/remodulation.
vec3 nrdEnvironmentTerm(vec3 Rf0, float NoV, float roughness)
{
    float m = clamp(roughness * roughness, 0.0, 1.0);

    vec4 X = vec4(1.0, NoV, NoV * NoV, NoV * NoV * NoV);
    vec4 Y = vec4(1.0, m, m * m, m * m * m);

    mat2 M1 = mat2(0.99044, -1.28514, 1.29678, -0.755907);
    mat3 M2 = mat3(1.0, 2.92338, 59.4188, 20.3225, -27.0302, 222.592, 121.563, 626.13, 316.627);

    mat2 M3 = mat2(0.0365463, 3.32707, 9.0632, -9.04756);
    mat3 M4 = mat3(1.0, 3.59685, -1.36772, 9.04401, -16.3174, 9.22949, 5.56589, 19.7886, -20.2123);

    vec2 M1X = M1 * X.xy;
    vec3 M2X = M2 * vec3(X.xyw);
    float bias = dot(M1X, Y.xy) / max(dot(M2X, Y.xyw), 1e-6);

    vec2 M3X = M3 * X.xy;
    vec3 M4X = M4 * vec3(X.xzw);
    float scale = dot(M3X, Y.xy) / max(dot(M4X, Y.xyw), 1e-6);

    return clamp(Rf0 * scale + vec3(bias), 0.0, 1.0);
}

// NRD material factors (NRD.hlsli:728-745).
// diffFactor = (1 - Fenv) * albedo, clamped to [MIN_SCALE, 1]
// specFactor = Fenv * lerp(ROUGHNESS_FACTOR_MIN_SCALE, 1, roughness), clamped to [MIN_SCALE, 1]
// Both demodulation (before NRD) and remodulation (after NRD) must use the same factors.
#define NRD_MATERIAL_FACTOR_MIN_SCALE 0.02
#define NRD_ROUGHNESS_FACTOR_MIN_SCALE 0.1

void nrdMaterialFactors(vec3 N, vec3 V, vec3 albedo, vec3 Rf0, float roughness,
                       out vec3 diffFactor, out vec3 specFactor)
{
    float NoV = abs(dot(N, V));
    vec3 Fenv = nrdEnvironmentTerm(Rf0, NoV, roughness);

    diffFactor = (1.0 - Fenv) * albedo;
    diffFactor = mix(vec3(NRD_MATERIAL_FACTOR_MIN_SCALE), vec3(1.0), diffFactor);

    specFactor = Fenv * mix(vec3(NRD_ROUGHNESS_FACTOR_MIN_SCALE), vec3(1.0), roughness);
    specFactor = mix(vec3(NRD_MATERIAL_FACTOR_MIN_SCALE), vec3(1.0), specFactor);
}

// ---- Cook-Torrance GGX BRDF (M7) -------------------------------------------
// Reference: Heitz 2018 "Sampling the GGX visible distribution of normals"
// and Walter et al. 2007 for the GGX distribution.

float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Roughness -> alpha (GGX uses squared roughness, clamped to avoid singularity)
float roughnessToAlpha(float roughness)
{
    return max(roughness * roughness, 1e-4);
}

// Schlick Fresnel with vec3 F0 (tinted for metals)
vec3 F_Schlick(float cosTheta, vec3 F0)
{
    float f = pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
    return F0 + (1.0 - F0) * f;
}

// Compute F0 (Fresnel reflectance at normal incidence) from base color + metallic
vec3 computeF0(vec3 baseColor, float metallic)
{
    return mix(vec3(0.04), baseColor, metallic);
}

// GGX/Trowbridge-Reitz normal distribution function
float D_GGX(float NdotH, float alpha)
{
    float a2 = alpha * alpha;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

// Smith G1 (masking term for one direction), separable form
float G1_Smith(float NdotX, float alpha)
{
    float Nx = max(NdotX, 0.0);
    float a2 = alpha * alpha;
    return 2.0 * Nx / (Nx + sqrt(a2 + (1.0 - a2) * Nx * Nx));
}

// Separable Smith G2 (masking + shadowing)
float G2_Smith(float NdotV, float NdotL, float alpha)
{
    return G1_Smith(NdotV, alpha) * G1_Smith(NdotL, alpha);
}

// Sample a visible microfacet normal using VNDF (Heitz 2018).
// wo = outgoing direction (away from surface, towards viewer).
// n  = shading normal (unit, points away from surface).
// Returns microfacet normal h in world space.
vec3 sampleVNDF(vec3 wo, vec3 n, float alpha, inout uint rngState)
{
    // Build orthonormal basis with n as z-axis
    vec3 T, B;
    buildONB(n, T, B);

    // Transform wo into the tangent frame (z along n)
    vec3 wo_t = vec3(dot(wo, T), dot(wo, B), dot(wo, n));

    // Stretch configuration (isotropic: alpha_x = alpha_y = alpha)
    vec3 wo_p = normalize(vec3(alpha * wo_t.x, alpha * wo_t.y, wo_t.z));

    // Sample a point on the unit disk
    float r1 = randomFloat(rngState);
    float r2 = randomFloat(rngState);
    float r  = sqrt(r1);
    float phi = 2.0 * PI * r2;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);

    // Slide the sample along the view direction (Heitz 2018)
    float s = 0.5 * (1.0 + wo_p.z);
    t1 = (1.0 - s) * t1 + s * wo_p.x;
    t2 = (1.0 - s) * t2 + s * wo_p.y;

    // Microfacet normal in stretched half-space
    float z_sq = max(1.0 - t1 * t1 - t2 * t2, 0.0);
    vec3 Nh = normalize(vec3(t1, t2, sqrt(z_sq)));

    // Unstretch back to the original configuration
    vec3 Ne = normalize(vec3(alpha * Nh.x, alpha * Nh.y, Nh.z));

    // Transform back to world space
    return Ne.x * T + Ne.y * B + Ne.z * n;
}

// VNDF solid-angle PDF for direction wi.
// p(wi) = D(h) * G1(wo) / (4 * NdotV),  where h = normalize(wo + wi)
float pdfVNDF(vec3 wo, vec3 wi, vec3 n, float alpha)
{
    float NdotV = max(dot(wo, n), 1e-4);
    float NdotL = max(dot(wi, n), 0.0);
    if (NdotL <= 0.0) return 0.0;

    vec3 h = normalize(wo + wi);
    float NdotH = max(dot(n, h), 0.0);
    if (NdotH <= 0.0) return 0.0;

    float D = D_GGX(NdotH, alpha);
    float G1 = G1_Smith(NdotV, alpha);
    return D * G1 / (4.0 * NdotV);
}

// Cosine-weighted diffuse solid-angle PDF
float pdfDiffuse(vec3 wi, vec3 n)
{
    return max(dot(wi, n), 0.0) / PI;
}

// Full Cook-Torrance BRDF evaluation (diffuse + specular).
// wo, wi both point away from the surface.
vec3 evalBRDF(vec3 wo, vec3 wi, vec3 n,
              vec3 baseColor, float metallic, float roughness)
{
    float alpha = roughnessToAlpha(roughness);
    float NdotV = max(dot(wo, n), 0.0);
    float NdotL = max(dot(wi, n), 0.0);
    if (NdotV <= 0.0 || NdotL <= 0.0) return vec3(0.0);

    vec3 F0 = computeF0(baseColor, metallic);
    vec3 h = normalize(wo + wi);
    float NdotH = max(dot(n, h), 0.0);
    float VdotH = max(dot(wo, h), 0.0);

    float D = D_GGX(NdotH, alpha);
    float G = G2_Smith(NdotV, NdotL, alpha);
    vec3 F = F_Schlick(VdotH, F0);

    // Specular: Cook-Torrance microfacet
    vec3 specular = D * G * F / (4.0 * NdotV * NdotL);

    // Diffuse: Lambertian, reduced by Fresnel reflection and metallic factor.
    // Metals have no diffuse; non-metals lose energy to specular (1 - F).
    float specWeight = mix(luminance(F), 1.0, metallic);
    vec3 diffuseAlbedo = baseColor * (1.0 - metallic);
    vec3 diffuse = (1.0 - specWeight) * diffuseAlbedo / PI;

    return diffuse + specular;
}

// Diffuse-only BRDF evaluation for NEE.
// NEE only evaluates the diffuse term — the specular lobe is efficiently
// importance-sampled by VNDF, so NEE for specular produces fireflies.
// The specular direct lighting comes from BSDF-sampled rays hitting lights,
// MIS-weighted at the emissive hit (w_bsdf in the emission block).
vec3 evalDiffuseBRDF(vec3 wo, vec3 wi, vec3 n,
                     vec3 baseColor, float metallic)
{
    float NdotV = max(dot(wo, n), 0.0);
    float NdotL = max(dot(wi, n), 0.0);
    if (NdotV <= 0.0 || NdotL <= 0.0) return vec3(0.0);

    vec3 F0 = computeF0(baseColor, metallic);
    float VdotH = max(dot(wo, normalize(wo + wi)), 0.0);
    vec3 F = F_Schlick(VdotH, F0);
    float specWeight = mix(luminance(F), 1.0, metallic);
    vec3 diffuseAlbedo = baseColor * (1.0 - metallic);
    return (1.0 - specWeight) * diffuseAlbedo / PI;
}

// Combined BSDF solid-angle PDF for MIS (stochastic lobe mixture).
// P_s, P_d = lobe selection probabilities (must sum to ~1).
float evalBSDFPdf(vec3 wo, vec3 wi, vec3 n,
                  float P_s, float P_d, float alpha)
{
    float pdf_s = pdfVNDF(wo, wi, n, alpha);
    float pdf_d = pdfDiffuse(wi, n);
    return P_s * pdf_s + P_d * pdf_d;
}

// ---- Rough Dielectric BTDF (Walter et al. 2007, M7.5) -------------------------
// For rough transmission: refract around a sampled microfacet normal h, not the
// macro normal n. The half-vector for transmission differs from reflection.

// Check if total internal reflection occurs at the microfacet level.
// eta = eta_i / eta_o (incident medium IOR / transmitted medium IOR).
// wo points away from surface (towards viewer).
// h is the microfacet normal (oriented towards the upper hemisphere).
// Snell with eta = eta_i/eta_t: sin²θ_t = eta² · sin²θ_o
bool checkTIR(vec3 wo, vec3 h, float eta)
{
    float c = max(dot(wo, h), 0.0);
    float sin2Theta_t = (1.0 - c * c) * eta * eta;
    return sin2Theta_t >= 1.0;
}

// Refract wo around microfacet normal h using Snell's law.
// eta = eta_i / eta_o. Returns refracted wi (pointing into the other medium).
// Walter 2007 eq. 40 — identical math to GLSL refract(-wo, h, eta).
// Caller must check TIR before calling this.
vec3 refractAroundH(vec3 wo, vec3 h, float eta)
{
    float c = max(dot(wo, h), 0.0);
    float k = 1.0 - eta * eta * (1.0 - c * c);
    if (k < 0.0) return vec3(0.0);             // TIR (caller already handles)
    return (eta * c - sqrt(k)) * h - eta * wo; // unit length by construction
}

// Transmission half-vector: h = normalize(eta * wo + wi), face-forwarded to n.
// For reflection, h = normalize(wo + wi); for transmission, eta weights wo.
vec3 transmissionHalfVector(vec3 wo, vec3 wi, float eta, vec3 n)
{
    vec3 h = normalize(eta * wo + wi);
    return dot(h, n) < 0.0 ? -h : h;
}

// Solid-angle PDF for the transmitted direction wi.
// p(wi) = pdf_h(h) * |wi·h| / (eta*(wo·h) + (wi·h))²
// where h = transmissionHalfVector(wo, wi, eta, n) and pdf_h is the raw
// VNDF half-vector density (NOT the reflection-mapped /4·NdotV form).
float pdfBTDF(vec3 wo, vec3 wi, vec3 n, float eta, float alpha)
{
    float NdotL = max(-dot(wi, n), 0.0);  // wi is below surface for transmission
    if (NdotL <= 0.0) return 0.0;

    vec3 h = transmissionHalfVector(wo, wi, eta, n);
    float NdotH = max(dot(n, h), 0.0);
    if (NdotH <= 0.0) return 0.0;

    float VdotH = dot(wo, h);   // > 0 for valid transmission
    float LdotH = dot(wi, h);   // < 0 for valid transmission
    if (VdotH <= 0.0 || LdotH >= 0.0) return 0.0;

    // Signed denominator: eta*(wo·h) + (wi·h)
    float denom = eta * VdotH + LdotH;
    if (abs(denom) < 1e-8) return 0.0;

    // Raw VNDF density of h: D(h) * G1(wo) * (wo·h) / NdotV
    float NdotV = max(dot(wo, n), 1e-4);
    float pdf_h = D_GGX(NdotH, alpha) * G1_Smith(NdotV, alpha) * VdotH / NdotV;

    // Transmission Jacobian (Walter 2007 eq. 17): dωh/dωi = |wi·h| / denom²
    float jacobian = abs(LdotH) / (denom * denom);
    return pdf_h * jacobian;
}

// Combined BSDF PDF for transmission materials (3 lobes: reflect, refract, diffuse).
// P_reflect, P_refract, P_diffuse = lobe selection probabilities.
// eta = eta_i / eta_o for the transmission lobe.
float evalTransmissionBSDFPdf(vec3 wo, vec3 wi, vec3 n,
                               float P_reflect, float P_refract, float P_diffuse,
                               float alpha, float eta)
{
    float pdf_r = pdfVNDF(wo, wi, n, alpha);       // reflection lobe
    float pdf_t = pdfBTDF(wo, wi, n, eta, alpha);   // transmission lobe
    float pdf_d = pdfDiffuse(wi, n);                // diffuse lobe
    return P_reflect * pdf_r + P_refract * pdf_t + P_diffuse * pdf_d;
}

// ---- Shared raygen helpers (used by raygen.rgen + secondary_raygen.rgen) ----

// Initialize RNG from pixel coords + frame index (camera.position.w).
// Returns warmed-up rngState ready for randomFloat/pcg calls.
uint initRNG(ivec2 pixel, float frameIndex)
{
    uint seed = uint(abs(frameIndex));
    uint rngState = uint(pixel.x) * 1973u + uint(pixel.y) * 9277u + seed * 26699u;
    pcg(rngState);
    return rngState;
}

// Temporal accumulation: blend current color with previous frame.
// frameIndex = camera.position.w (accumulation counter, 1 = first frame).
// Negative frameIndex = accumulation disabled (per-frame Monte Carlo noise).
vec3 temporalAccumulate(ivec2 pixel, vec3 color, float frameIndex)
{
    if (frameIndex < 0.0)
        return color;
    if (frameIndex > 1.0)
    {
        vec3 prevColor = imageLoad(outputImage, pixel).xyz;
        return (prevColor * (frameIndex - 1.0) + color) / frameIndex;
    }
    return color;
}

// Write sky-pixel NRD defaults: oct-packed up normal, roughness=1, viewZ=1e6,
// white albedo/F0, zero diff/spec radiance, camera-derived sky motion, sky radiance as
// direct emission + beauty output.
void writeNRDSkyDefaults(ivec2 pixel, vec3 skyRadiance, vec3 skyDirection)
{
    vec3 skyOct = nrdEncodeNormalRoughness(vec3(0.0, 0.0, 1.0), 1.0);
    imageStore(gNormalRoughness, pixel, vec4(skyOct, 0.0));
    imageStore(gViewZ, pixel, vec4(1e6, 0.0, 0.0, 0.0));
    imageStore(gAlbedoF0, pixel, vec4(1.0, 1.0, 1.0, 1.0));
    imageStore(gDirectEmission, pixel, vec4(skyRadiance, 0.0));
    imageStore(gDiffRadianceHitDist, pixel, vec4(0.0));
    imageStore(gSpecRadianceHitDist, pixel, vec4(0.0));
    vec2 currUv = (vec2(pixel) + vec2(0.5)) / camera.viewportSPP.xy;
    vec4 previousClip = camera.viewToClipPrev * camera.worldToViewPrev * vec4(skyDirection, 0.0);
    vec2 previousUv = (previousClip.xy / previousClip.w) * 0.5 + 0.5;
    imageStore(gMotion, pixel, vec4(previousUv - currUv, 0.0, 0.0));
    imageStore(rrNoisyHdr, pixel, vec4(skyRadiance, 1.0));
    imageStore(rrHitDistance, pixel, vec4(0.0));
    imageStore(outputImage, pixel, vec4(skyRadiance, 1.0));
}

// NRD hit distance params (REBLUR defaults: A=3, B=0.1, C=20).
const vec3 NRD_HIT_DIST_PARAMS = vec3(3.0, 0.1, 20.0);
// Keep stochastic outliers within the range REBLUR's anti-firefly pass can
// robustly sanitize. FP16-scale impulses turn into visible multi-pixel blooms.
const float NRD_FIREFLY_CLAMP = 100.0;

// Pack diffuse lobe NRD output: demodulate, clamp fireflies, normalize hit dist,
// convert to YCoCg, and store to gDiffRadianceHitDist. The caller owns the
// opposite signal: raster-first shading can produce diffuse direct and specular
// indirect radiance for the same pixel, so this helper must not erase it.
void writeNRDDiffuse(ivec2 pixel, vec3 radiance, vec3 diffFactor, float hitT, float viewZ)
{
    vec3 demod = clamp(radiance / max(diffFactor, vec3(NRD_MATERIAL_FACTOR_MIN_SCALE)),
                       vec3(0.0), vec3(NRD_FIREFLY_CLAMP));
    float normHitT = nrdGetNormHitDist(hitT, viewZ, NRD_HIT_DIST_PARAMS, 1.0);
    vec3 ycocg = linearToYCoCg(demod);
    imageStore(gDiffRadianceHitDist, pixel, vec4(ycocg, normHitT));
}

// Pack specular lobe NRD output: demodulate, clamp fireflies, normalize hit dist,
// convert to YCoCg, and store to gSpecRadianceHitDist. The caller owns the
// opposite signal for the same reason as writeNRDDiffuse.
void writeNRDSpecular(ivec2 pixel, vec3 radiance, vec3 specFactor, float hitT, float viewZ, float roughness)
{
    vec3 demod = clamp(radiance / max(specFactor, vec3(NRD_MATERIAL_FACTOR_MIN_SCALE)),
                       vec3(0.0), vec3(NRD_FIREFLY_CLAMP));
    float normHitT = nrdGetNormHitDist(hitT, viewZ, NRD_HIT_DIST_PARAMS, roughness);
    vec3 ycocg = linearToYCoCg(demod);
    imageStore(gSpecRadianceHitDist, pixel, vec4(ycocg, normHitT));
}
