// shader_interface.h — shared between C++ and GLSL via #include.
// glslc supports #include with -I; guard C++-only and GLSL-only sections.
//
// This header defines:
//   - Binding indices (set 0 + set 1) as #defines
//   - CameraData UBO struct layout
//   - Material (GPUMaterial) struct layout
//   - TriangleLight struct layout
//   - NRD UBO struct layout
//
// Including this from both sides ensures C++ and GLSL agree on struct
// sizes, field offsets, and binding numbers. Mismatches become compile
// errors instead of cyan-screen bugs.

#ifndef SHADER_INTERFACE_H
#define SHADER_INTERFACE_H

// ============================================================================
// Type aliases — map GLSL types to glm types in C++
// ============================================================================
#ifdef __cplusplus
#include <glm/glm.hpp>
#define SI_VEC4  glm::vec4
#define SI_UVEC4 glm::uvec4
#define SI_IVEC4 glm::ivec4
#define SI_MAT4  glm::mat4
#define SI_FLOAT float
#define SI_UINT  uint32_t
#define SI_INT   int32_t
#else
#define SI_VEC4  vec4
#define SI_UVEC4 uvec4
#define SI_IVEC4 ivec4
#define SI_MAT4  mat4
#define SI_FLOAT float
#define SI_UINT  uint
#define SI_INT   int
#endif

// ============================================================================
// Binding indices — set 0 (path tracer)
// ============================================================================
#define SI_BINDING_OUTPUT_IMAGE     0
#define SI_BINDING_CAMERA_UBO       1
#define SI_BINDING_MATERIAL_BUFFER  2
#define SI_BINDING_NORMAL_BUFFER    3
#define SI_BINDING_TLAS             4
#define SI_BINDING_INSTANCE_OFFSETS 5
#define SI_BINDING_TANGENT_BUFFER   6
#define SI_BINDING_UV_BUFFER        7
#define SI_BINDING_POSITION_BUFFER  8
#define SI_BINDING_LIGHT_BUFFER          9
#define SI_BINDING_INSTANCE_TRANSFORMS   10
#define SI_BINDING_TEXTURE_ARRAY         11
#define SI_BINDING_INSTANCE_TRANSFORMS_PREV 12
#define SI_BINDING_INSTANCE_MATERIAL_INDICES 13
#define SI_BINDING_RESERVOIR_HISTORY       14  // previous frame's final reservoir (read by temporal, written by spatial)
#define SI_BINDING_RESERVOIR_SCRATCH       15  // temporal output / spatial input
#define SI_BINDING_SURFACE_HISTORY          16  // per-pixel receiver metadata for temporal validation

// ============================================================================
// Binding indices — set 1 (NRD G-buffer)
// ============================================================================
#define SI_BINDING_G_NORMAL_ROUGHNESS  0
#define SI_BINDING_G_VIEWZ             1
#define SI_BINDING_G_MOTION            2
#define SI_BINDING_G_DIFF_RADIANCE     3
#define SI_BINDING_G_SPEC_RADIANCE     4
#define SI_BINDING_G_ALBEDO_F0         5
#define SI_BINDING_NRD_UBO             6
#define SI_BINDING_G_DIRECT_EMISSION   7
#define SI_BINDING_G_PRIM_HIT          8   // rgba32f: xyz = world pos, w = material index (floatBitsToInt)
#define SI_BINDING_G_PRIM_GEO_NORMAL   9   // rgba8: xyz = geo normal (0.5+0.5 encode)
#define SI_BINDING_G_PRIM_UV           10  // rg16f: xy = UV at primary hit

// ============================================================================
// CameraData UBO — std140, matches layout in RendererGPU.cpp UpdateCameraUBO
// ============================================================================
struct SICameraData
{
    SI_VEC4 position;       // xyz = position, w = frameIndex
    SI_VEC4 forward;        // xyz = forward, w = NRD jitter.x
    SI_VEC4 right;          // xyz = right, w = NRD jitter.y
    SI_VEC4 up;             // xyz = up, w = pad
    SI_VEC4 viewportSPP;    // x = width, y = height, z = spp, w = maxBounces
    SI_VEC4 apertureFocal;  // x = aperture, y = focusDistance, z = showBackground, w = emissiveBoost
    SI_VEC4 envMap;         // x = envMapIndex (-1=none), y = envIntensity, z = marginalCDFIdx, w = conditionalCDFIdx
    SI_MAT4 inverseProjection;
    SI_MAT4 inverseView;
    SI_MAT4 viewToClip;     // current frame view-to-clip (for viewZ + motion)
    SI_MAT4 viewToClipPrev; // previous frame view-to-clip
    SI_MAT4 worldToView;    // current world-to-view
    SI_MAT4 worldToViewPrev;// previous world-to-view
};

// ============================================================================
// Material — std430, 80 bytes. Matches GPUMaterial in GPUSceneData.h.
//
// baseAlpha and transmissionFactor are SEPARATE concerns:
//   baseAlpha         = baseColorFactor.a, used by any-hit for alpha opacity.
//   transmissionFactor = KHR_materials_transmission, used by closesthit for refraction.
// transmissionFactor is packed into textureIndices.w via floatBitsToInt.
// ============================================================================
struct SIMaterial
{
    SI_VEC4  baseColor_metallic;   // xyz = base color, w = metallic factor
    SI_VEC4  emissive_roughness;   // xyz = emissive * intensity, w = roughness
    SI_FLOAT ior;                  // index of refraction (for dielectric)
    SI_FLOAT alphaCutoff;          // alpha cutoff (MASK mode)
    SI_FLOAT alphaMode;            // 0=OPAQUE, 1=MASK, 2=BLEND
    SI_FLOAT baseAlpha;            // baseColorFactor.a (1.0 = fully opaque, for any-hit)
    SI_IVEC4 textureIndices;       // x = baseColor, y = normal, z = emissive,
                                   //   w = floatBitsToInt(transmissionFactor)
    SI_IVEC4 extraIndices;         // x = metallicRoughness texture index, yzw = pad
};

// ============================================================================
// TriangleLight — 32 bytes (vec4 + uvec4). Used in LightBuffer after header.
// ============================================================================
struct SITriangleLight
{
    SI_VEC4  emission_area;  // xyz = emissiveColor*intensity (flat fallback), w = area
    SI_UVEC4 ids;            // x = instanceID, y = primitiveID, z = materialIndex, w = emissiveTexIdx
};

// ============================================================================
// Reservoir — per-pixel ReSTIR DI reservoir. 32 bytes (2 × uvec4), std430.
//
// Unified representation for both triangle and environment light samples.
// Stores sample type, light index or environment sentinel, packed barycentrics
// or environment UV, M, weight sum, selected target density, age, and flags.
//
// Sample types:
//   0 = triangle light (lightIdx + barycentrics b1, b2)
//   1 = environment light (envUV packed)
//   0xFFFFFFFF = empty/invalid
//
// M = number of candidates seen (accumulates across temporal/spatial reuse).
// weightSum = Σ w_i = p_hat(x_i) / p(x_i) for accepted candidates.
// targetPdf = p_hat of the selected sample (for W = weightSum / (M * targetPdf)).
// age = frames since the temporal history was first accepted (0 = fresh).
// flags = bit 0 = valid, bits 1-31 = reserved.
//
// Encoding:
//   data0.x = sampleType (uint)
//   data0.y = lightIdx (uint, triangle only; 0 for env)
//   data0.z = floatBitsToUint(b1)  (triangle) or floatBitsToUint(envUV.x) (env)
//   data0.w = floatBitsToUint(b2)  (triangle) or floatBitsToUint(envUV.y) (env)
//   data1.x = floatBitsToUint(weightSum)
//   data1.y = floatBitsToUint(targetPdf)
//   data1.z = M (uint)
//   data1.w = (age << 8) | (flags & 0xFF)
// ============================================================================
struct SIReservoir
{
    SI_UVEC4 data0;
    SI_UVEC4 data1;
};

// Sample type constants
#define SI_SAMPLE_TYPE_TRIANGLE  0u
#define SI_SAMPLE_TYPE_ENV       1u
#define SI_SAMPLE_TYPE_EMPTY     0xFFFFFFFFu

// ============================================================================
// SurfaceHistory — per-pixel receiver metadata for temporal validation.
// 16 bytes (uvec4), std430.
//
// Encodes normal (2 components, oct), view depth, material ID, and validity.
// Written by the spatial pass (overwrites previous frame's history).
//
// data.x = oct-normal packed into 16 bits each (x = n.x oct, y = n.y oct)
// data.y = floatBitsToUint(viewZ)
// data.z = materialID (uint, 0xFFFFFFFF = sky/invalid)
// data.w = (flags << 16) | valid (bit 0 = valid)
// ============================================================================
struct SISurfaceHistory
{
    SI_UVEC4 data;
};

// ============================================================================
// ReSTIR push constants — temporal/spatial pass configuration.
// 48 bytes, fits in the 128-byte push constant limit.
// ============================================================================
struct SIReSTIRPushConstants
{
    SI_UINT freshCandidateCount;   // M: fresh candidates per pixel
    SI_UINT temporalMCap;          // max M from temporal history (e.g. 20 * freshCandidateCount)
    SI_UINT spatialNeighborCount;  // number of neighbors for spatial reuse
    SI_UINT spatialRadius;         // pixel radius for neighbor sampling
    SI_FLOAT depthThreshold;      // relative depth difference threshold
    SI_FLOAT normalThreshold;      // normal similarity threshold (dot product)
    SI_UINT flags;                 // bit 0 = temporal reuse enabled, bit 1 = spatial reuse enabled
    SI_UINT frameIndex;           // frame index for neighbor rotation
    SI_VEC4 jitter;               // xy = current, zw = previous jitter in pixel units
};

// ============================================================================
// NRD UBO — set 1 binding 6, 16 bytes
// ============================================================================
struct SINRDUniformData
{
    SI_UINT nrdEnabled;  // 1 = NRD mode (1 spp, no temporal accum, write G-buffer)
    SI_UINT lobeDither;  // 0=off (white noise), 1=Bayer 4x4, 2=Interleaved Gradient Noise
    SI_UINT pad1;
    SI_UINT pad2;
};

// ============================================================================
// Size assertions (C++ only)
// ============================================================================
#ifdef __cplusplus
static_assert(sizeof(SICameraData) == 496, "SICameraData must be 496 bytes (7 vec4 + 6 mat4)");
static_assert(sizeof(SIMaterial) == 80, "SIMaterial must be 80 bytes (2 vec4 + 4 float + 2 ivec4)");
static_assert(sizeof(SITriangleLight) == 32, "SITriangleLight must be 32 bytes (vec4 + uvec4)");
static_assert(sizeof(SIReservoir) == 32, "SIReservoir must be 32 bytes (2 uvec4)");
static_assert(sizeof(SISurfaceHistory) == 16, "SISurfaceHistory must be 16 bytes (1 uvec4)");
static_assert(sizeof(SIReSTIRPushConstants) == 48, "SIReSTIRPushConstants must be 48 bytes");
static_assert(sizeof(SINRDUniformData) == 16, "SINRDUniformData must be 16 bytes");
#endif

#endif // SHADER_INTERFACE_H
