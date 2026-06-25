#include "GPUSceneData.h"

GPUSceneData BuildGPUSceneData(const Scene& scene)
{
    GPUSceneData gpu;

    // Convert all materials
    for (const auto& sm : scene.GetMaterials())
        gpu.materials.push_back(GPUMaterial::fromSceneMaterial(sm));

    // If no materials, add a default so index 0 is always valid
    if (gpu.materials.empty())
        gpu.materials.push_back(GPUMaterial());

    // Convert meshes that have inline geometry
    for (const auto& sceneMesh : scene.GetMeshes())
    {
        if (!sceneMesh.HasGeometry())
            continue;

        GPUMeshGeometry geo;
        geo.vertices = sceneMesh.vertices;
        geo.indices  = sceneMesh.indices;

        // Clamp material index to valid range
        uint32_t matIdx = 0;
        if (sceneMesh.materialIndex >= 0 &&
            sceneMesh.materialIndex < static_cast<int>(gpu.materials.size()))
            matIdx = static_cast<uint32_t>(sceneMesh.materialIndex);
        geo.materialIndex = matIdx;

        gpu.meshes.push_back(std::move(geo));
    }

    return gpu;
}