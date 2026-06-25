#pragma once

#ifndef GPU_SCENE_DATA_H
#define GPU_SCENE_DATA_H

#include "Scene.h"
#include <glm/glm.hpp>
#include <vector>

// ============================================================================
// GPU-side data structures — plain-old-data, std430 compatible, no Vulkan deps
// ============================================================================

// PBR material packed into 32 bytes (two vec4s) for std430 storage buffer.
// Matches the GLSL Material struct in pathtracer_shared.glsl.
struct GPUMaterial
{
    glm::vec4 baseColor_metallic;   // xyz = base color, w = metallic factor
    glm::vec4 emissive_roughness;   // xyz = emissive * intensity, w = roughness
    float     ior;                  // index of refraction (for dielectric)
    float     _pad0;
    float     _pad1;
    float     _pad2;

    static GPUMaterial fromSceneMaterial(const SceneMaterial& sm)
    {
        GPUMaterial m;
        m.baseColor_metallic = glm::vec4(sm.baseColor, sm.metallic);
        m.emissive_roughness = glm::vec4(
            sm.emissiveColor * sm.emissiveIntensity, sm.roughness);
        m.ior = sm.ior;
        m._pad0 = 0.0f;
        m._pad1 = 0.0f;
        m._pad2 = 0.0f;
        return m;
    }

    GPUMaterial()
        : baseColor_metallic(0.8f, 0.8f, 0.8f, 0.0f)
        , emissive_roughness(0.0f, 0.0f, 0.0f, 0.5f)
        , ior(1.5f), _pad0(0), _pad1(0), _pad2(0)
    {}
};

// One chunk of triangle geometry for a single BLAS build.
struct GPUMeshGeometry
{
    std::vector<float>     vertices;    // position.xyz, stride 3
    std::vector<uint32_t>  indices;     // triangle indices
    uint32_t               materialIndex = 0;
};

// Full scene data ready for GPU upload.
struct GPUSceneData
{
    std::vector<GPUMeshGeometry> meshes;
    std::vector<GPUMaterial>     materials;
};

// Convert a Scene into GPUSceneData: one GPUMeshGeometry per scene mesh
// with geometry, one GPUMaterial per scene material. Meshes without
// geometry are skipped. Out-of-range material indices clamp to 0.
GPUSceneData BuildGPUSceneData(const Scene& scene);

#endif // GPU_SCENE_DATA_H