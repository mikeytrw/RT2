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
    vec4 envMap;        // x = envMapIndex (-1=none), y = envIntensity, z = marginalCDFIdx, w = conditionalCDFIdx
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

// Convert a direction to equirectangular UV coordinates
vec2 directionToEnvUV(vec3 dir)
{
    float u = atan(dir.z, dir.x) * 0.15915494309;  // 1/(2π)
    float v = asin(clamp(dir.y, -1.0, 1.0)) * 0.31830988618;  // 1/π
    return vec2(u, 0.5 - v * 0.5);  // flip V for image convention
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
    vec2 envSize = vec2(float(condW), float(marginalLen));
    float sinTheta = sqrt(max(1.0 - s.dir.y * s.dir.y, 1e-6));

    // Marginal PDF
    float margPrev = (vIdx > 0) ? texelFetch(textures[nonuniformEXT(marginalIdx)], ivec2(vIdx - 1, 0), 0).r : 0.0;
    float margCurr = texelFetch(textures[nonuniformEXT(marginalIdx)], ivec2(vIdx, 0), 0).r;
    float pdfV = max(margCurr - margPrev, 1e-8);

    // Conditional PDF
    float condPrev = (uIdx > 0) ? texelFetch(textures[nonuniformEXT(conditionalIdx)], ivec2(uIdx - 1, vIdx), 0).r : 0.0;
    float condCurr = texelFetch(textures[nonuniformEXT(conditionalIdx)], ivec2(uIdx, vIdx), 0).r;
    float pdfU = max(condCurr - condPrev, 1e-8);

    // Convert to solid-angle PDF: p(ω) = p(u,v) / (sinθ * 2π²)
    // The area-to-solid-angle Jacobian for equirect mapping is sinθ * 2π²
    s.pdf = (pdfV * pdfU) / (sinTheta * 2.0 * PI * PI);

    // Sample radiance
    s.radiance = envMapRadiance(s.dir);

    return s;
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
bool checkTIR(vec3 wo, vec3 h, float eta)
{
    float cosTheta_o_h = max(dot(wo, h), 0.0);
    float sin2Theta_o_h = max(1.0 - cosTheta_o_h * cosTheta_o_h, 0.0);
    float sin2Theta_i_h = sin2Theta_o_h / (eta * eta);
    return sin2Theta_i_h >= 1.0;
}

// Refract wo around microfacet normal h using Snell's law.
// eta = eta_i / eta_o. Returns refracted wi (pointing into the other medium).
// Caller must check TIR before calling this.
vec3 refractAroundH(vec3 wo, vec3 h, float eta)
{
    float cosTheta_o_h = max(dot(wo, h), 0.0);
    float sin2Theta_o_h = max(1.0 - cosTheta_o_h * cosTheta_o_h, 0.0);
    float sin2Theta_i_h = sin2Theta_o_h / (eta * eta);
    float cosTheta_i_h = sqrt(max(1.0 - sin2Theta_i_h, 0.0));
    return -wo / eta + (cosTheta_o_h / eta - cosTheta_i_h) * h;
}

// Transmission half-vector: h = normalize(wo + eta * wi), face-forwarded to n.
// For reflection, h = normalize(wo + wi); for transmission, eta weights wi.
vec3 transmissionHalfVector(vec3 wo, vec3 wi, float eta, vec3 n)
{
    vec3 h = normalize(wo + eta * wi);
    return dot(h, n) < 0.0 ? -h : h;
}

// Solid-angle PDF for the transmitted direction wi.
// p(wi) = pdfVNDF(wo, h, n, alpha) * |wo·h| / (wi·h + wo·h/eta)²
// where h = transmissionHalfVector(wo, wi, eta, n).
float pdfBTDF(vec3 wo, vec3 wi, vec3 n, float eta, float alpha)
{
    float NdotL = max(-dot(wi, n), 0.0);  // wi is below surface for transmission
    if (NdotL <= 0.0) return 0.0;

    vec3 h = transmissionHalfVector(wo, wi, eta, n);
    float NdotH = max(dot(n, h), 0.0);
    if (NdotH <= 0.0) return 0.0;

    float VdotH = max(dot(wo, h), 0.0);
    float LdotH = max(dot(wi, h), 0.0);  // wi·h > 0 after face-forwarding
    if (VdotH <= 0.0 || LdotH <= 0.0) return 0.0;

    // Jacobian: dω_h/dω_i = |wo·h| / (wi·h + wo·h/eta)²
    float denom = LdotH + VdotH / eta;
    if (abs(denom) < 1e-8) return 0.0;

    // Raw VNDF PDF for the transmission half-vector h:
    // D(h) * G1(wo) / (4 * NdotV). (Do NOT use pdfVNDF() here — it derives
    // its own h = normalize(wo + wi), which is the reflection half-vector.)
    float NdotV = max(dot(wo, n), 1e-4);
    float D = D_GGX(NdotH, alpha);
    float G1 = G1_Smith(NdotV, alpha);
    float pdf_h_vndf = D * G1 / (4.0 * NdotV);

    float jacobian = abs(VdotH) / (denom * denom);
    return pdf_h_vndf * jacobian;
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