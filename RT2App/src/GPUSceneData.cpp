#include "GPUSceneData.h"

GPUSceneData BuildGPUSceneData(const Scene& scene)
{
    GPUSceneData gpu;

    // Copy all textures (decoded RGBA8 pixels)
    gpu.textures = scene.GetTextures();

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

        // Collect 3 vertex UVs per triangle (6 floats: u0,v0,u1,v1,u2,v2)
        uint32_t triCount = static_cast<uint32_t>(sceneMesh.indices.size() / 3);
        geo.vertexUVs.reserve(triCount * 6);
        if (!sceneMesh.uvs.empty())
        {
            for (uint32_t t = 0; t < triCount; t++)
            {
                uint32_t i0 = sceneMesh.indices[t * 3 + 0] * 2;
                uint32_t i1 = sceneMesh.indices[t * 3 + 1] * 2;
                uint32_t i2 = sceneMesh.indices[t * 3 + 2] * 2;

                geo.vertexUVs.push_back(sceneMesh.uvs[i0]);
                geo.vertexUVs.push_back(sceneMesh.uvs[i0 + 1]);
                geo.vertexUVs.push_back(sceneMesh.uvs[i1]);
                geo.vertexUVs.push_back(sceneMesh.uvs[i1 + 1]);
                geo.vertexUVs.push_back(sceneMesh.uvs[i2]);
                geo.vertexUVs.push_back(sceneMesh.uvs[i2 + 1]);
            }
        }
        else
        {
            geo.vertexUVs.resize(triCount * 6, 0.0f);
        }

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