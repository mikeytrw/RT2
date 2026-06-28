// Shared declarations for the ray tracing path tracer stages.
// Included by rgen.rgen, miss.rmiss, closesthit.rchit.

#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require

// ---- Bindings (set 0) -------------------------------------------------------

layout(set = 0, binding = 0, rgba32f) uniform image2D outputImage;

layout(set = 0, binding = 1, std140) uniform CameraData
{
    vec4 position;      // xyz = position, w = frameIndex
    vec4 forward;       // xyz = forward, w = pad
    vec4 right;         // xyz = right, w = pad
    vec4 up;            // xyz = up, w = pad
    vec4 viewportSPP;   // x = width, y = height, z = spp, w = maxBounces
    vec4 apertureFocal; // x = aperture, y = focusDistance, z = showBackground, w = emissiveBoost
    mat4 inverseProjection;
    mat4 inverseView;
} camera;

// PBR material — matches GPUMaterial in GPUSceneData.h (64 bytes, std430)
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
};

layout(set = 0, binding = 2, std430) readonly buffer MaterialBuffer
{
    Material materials[];
};

layout(set = 0, binding = 3, std430) readonly buffer NormalBuffer
{
    vec4 triangleNormals[]; // xyz = normal, w = unused (16-byte aligned)
};

layout(set = 0, binding = 5, std430) readonly buffer InstanceNormalOffsets
{
    uint normalOffsets[]; // per-instance offset into triangleNormals
};

// Combined tangent buffer (3 vec4 per triangle, xyz = vertex tangent)
layout(set = 0, binding = 6, std430) readonly buffer TangentBuffer
{
    vec4 triangleTangents[]; // 3 per triangle: xyz = tangent, w = unused
};

// Combined UV buffer (3 vec4 per triangle, xy = vertex UV)
layout(set = 0, binding = 7, std430) readonly buffer UVBuffer
{
    vec4 triangleUVs[]; // 3 per triangle: xy = vertex UV, zw = unused
};

// Combined position buffer (3 vec4 per triangle, xyz = vertex position)
layout(set = 0, binding = 8, std430) readonly buffer PositionBuffer
{
    vec4 trianglePositions[]; // 3 per triangle: xyz = vertex pos, w = unused
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

layout(set = 0, binding = 9, std430) readonly buffer LightBuffer
{
    uint  lightCount;
    float totalLightArea;
    uint  _lightPad0;
    uint  _lightPad1;
    TriangleLight lights[];
};

// Bindless texture array (combined image samplers, variable count)
// Must be the highest binding number for VARIABLE_DESCRIPTOR_COUNT.
layout(set = 0, binding = 10) uniform sampler2D textures[];

layout(set = 0, binding = 4) uniform accelerationStructureEXT topLevelAS;

// ---- Payload ----------------------------------------------------------------
// Packed into explicit vec4 rows to avoid cross-stage std430 alignment
// ambiguity when mixing vec3/scalar fields in rayPayloadEXT.
struct RayPayload
{
    vec4 a; // xyz = throughput, w = rngState
    vec4 b; // xyz = radiance,   w = depth
    vec4 c; // xyz = ray origin, w = done (0/1)
    vec4 d; // xyz = ray dir,    w = pad
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
    return float(pcg(state)) / 4294967295.0;
}

vec3 randomInUnitSphere(inout uint state)
{
    for (int i = 0; i < 10; i++)
    {
        vec3 p = vec3(randomFloat(state) * 2.0 - 1.0,
                      randomFloat(state) * 2.0 - 1.0,
                      randomFloat(state) * 2.0 - 1.0);
        if (dot(p, p) < 1.0)
            return p;
    }
    return vec3(0.0);
}

vec3 randomInUnitDisk(inout uint state)
{
    for (int i = 0; i < 10; i++)
    {
        vec3 p = vec3(randomFloat(state) * 2.0 - 1.0,
                      randomFloat(state) * 2.0 - 1.0,
                      0.0);
        if (dot(p, p) < 1.0)
            return p;
    }
    return vec3(0.0);
}

// ---- Helpers ----------------------------------------------------------------

#define PI 3.14159265359

vec3 skyColor(vec3 direction)
{
    float t = 0.5 * (direction.y + 1.0);
    return (1.0 - t) * vec3(1.0, 1.0, 1.0) + t * vec3(0.5, 0.7, 1.0);
}

float reflectance(float cosine, float refIdx)
{
    float r0 = (1.0 - refIdx) / (1.0 + refIdx);
    r0 = r0 * r0;
    return r0 + (1.0 - r0) * pow(1.0 - cosine, 5.0);
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