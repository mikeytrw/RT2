#pragma once

#ifndef GPU_SCENE_DATA_H
#define GPU_SCENE_DATA_H

#include "Scene.h"
#include <glm/glm.hpp>
#include <vector>

// ============================================================================
// GPU-side data structures — plain-old-data, std430 compatible, no Vulkan deps
// ============================================================================

// PBR material packed into 64 bytes (three vec4s + ivec4) for std430.
// Matches the GLSL Material struct in pathtracer_shared.glsl.
//
// baseAlpha and transmissionFactor are SEPARATE concerns:
//   baseAlpha        = baseColorFactor.a, used by any-hit for alpha opacity (MASK/BLEND).
//   transmissionFactor = KHR_materials_transmission, used by closesthit for refraction.
// transmissionFactor is packed into textureIndices.w via floatBitsToInt.
struct GPUMaterial
{
    glm::vec4 baseColor_metallic;   // xyz = base color, w = metallic factor
    glm::vec4 emissive_roughness;   // xyz = emissive * intensity, w = roughness
    float     ior;                  // index of refraction (for dielectric)
    float     alphaCutoff;          // alpha cutoff (MASK mode)
    float     alphaMode;            // 0=OPAQUE, 1=MASK, 2=BLEND
    float     baseAlpha;             // baseColorFactor.a (1.0 = fully opaque, for any-hit)
    glm::ivec4 textureIndices;      // x = baseColor, y = normal, z = emissive,
                                    // w = floatBitsToInt(transmissionFactor)

    static GPUMaterial fromSceneMaterial(const SceneMaterial& sm)
    {
        GPUMaterial m;
        m.baseColor_metallic = glm::vec4(sm.baseColor, sm.metallic);
        m.emissive_roughness = glm::vec4(
            sm.emissiveColor * sm.emissiveIntensity, sm.roughness);
        m.ior = sm.ior;
        m.alphaCutoff = sm.alphaCutoff;
        if (sm.alphaMode == "MASK")       m.alphaMode = 1.0f;
        else if (sm.alphaMode == "BLEND") m.alphaMode = 2.0f;
        else                              m.alphaMode = 0.0f;
        m.baseAlpha = sm.baseAlpha;
        m.textureIndices = glm::ivec4(
            sm.baseColorTextureIndex,
            sm.normalTextureIndex,
            sm.emissiveTextureIndex,
            glm::floatBitsToInt(sm.transmissionFactor));
        return m;
    }

    GPUMaterial()
        : baseColor_metallic(0.8f, 0.8f, 0.8f, 0.0f)
        , emissive_roughness(0.0f, 0.0f, 0.0f, 0.5f)
        , ior(1.5f), alphaCutoff(0.5f), alphaMode(0.0f), baseAlpha(1.0f)
        , textureIndices(-1, -1, -1, 0)  // w = floatBitsToInt(0.0f) = 0
    {}
};

// One chunk of triangle geometry for a single BLAS build.
struct GPUMeshGeometry
{
    std::vector<float>     vertices;    // position.xyz, stride 3
    std::vector<uint32_t>  indices;     // triangle indices
    std::vector<float>     vertexUVs;   // 6 floats per triangle (3 UVs × xy), interleaved
    std::vector<float>     tangents;    // 9 floats per triangle (3 tangents × xyz), one per vertex
    uint32_t               materialIndex = 0;
};

// A triangle light for Next-Event Estimation. 32 bytes (vec4 + uvec4).
// The shader samples the emissive texture at the picked barycentric point
// using (instanceID, primitiveID) to look up positions/UVs from existing
// buffers. emission_area.xyz is the flat fallback (emissiveColor * intensity)
// used when a material has no emissive texture.
struct GPUTriangleLight
{
    glm::vec4  emission_area;  // xyz = emissiveColor*intensity (flat fallback), w = triangle area
    glm::uvec4 ids;            // x = instanceID, y = primitiveID, z = materialIndex, w = emissiveTexIdx

    GPUTriangleLight()
        : emission_area(0.0f, 0.0f, 0.0f, 0.0f)
        , ids(0, 0, 0, 0xFFFFFFFFu)
    {}
};

// Full scene data ready for GPU upload.
struct GPUSceneData
{
    std::vector<GPUMeshGeometry>  meshes;
    std::vector<GPUMaterial>      materials;
    std::vector<SceneTexture>     textures;
    std::vector<GPUTriangleLight> lights;  // emissive triangles for NEE
    float                         totalLightArea = 0.0f;  // sum of all light triangle areas
};

// Convert a Scene into GPUSceneData: one GPUMeshGeometry per scene mesh
// with geometry, one GPUMaterial per scene material. Meshes without
// geometry are skipped. Out-of-range material indices clamp to 0.
GPUSceneData BuildGPUSceneData(const Scene& scene);

#endif // GPU_SCENE_DATA_H