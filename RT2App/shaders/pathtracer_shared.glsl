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
    vec4 apertureFocal; // x = aperture, y = focusDistance, z = pad, w = pad
    mat4 inverseProjection;
    mat4 inverseView;
} camera;

// PBR material — matches GPUMaterial in GPUSceneData.h (64 bytes, std430)
struct Material
{
    vec4 baseColor_metallic;   // xyz = base color, w = metallic factor
    vec4 emissive_roughness;   // xyz = emissive * intensity, w = roughness
    float ior;
    float _pad0;
    float _pad1;
    float _pad2;
    ivec4 textureIndices;      // x = baseColor, y = normal, z = emissive, w = unused (-1 = none)
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

// Bindless texture array (combined image samplers, variable count)
// Must be the highest binding number for VARIABLE_DESCRIPTOR_COUNT.
layout(set = 0, binding = 9) uniform sampler2D textures[];

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