#include "GPUSceneData.h"
#include <glm/glm.hpp>

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
        // and compute per-triangle tangent (3 tangents × xyz = 12 floats)
        uint32_t triCount = static_cast<uint32_t>(sceneMesh.indices.size() / 3);
        geo.vertexUVs.reserve(triCount * 6);
        geo.tangents.reserve(triCount * 12);
        if (!sceneMesh.uvs.empty())
        {
            for (uint32_t t = 0; t < triCount; t++)
            {
                uint32_t vi0 = sceneMesh.indices[t * 3 + 0] * 3;
                uint32_t vi1 = sceneMesh.indices[t * 3 + 1] * 3;
                uint32_t vi2 = sceneMesh.indices[t * 3 + 2] * 3;
                uint32_t ui0 = sceneMesh.indices[t * 3 + 0] * 2;
                uint32_t ui1 = sceneMesh.indices[t * 3 + 1] * 2;
                uint32_t ui2 = sceneMesh.indices[t * 3 + 2] * 2;

                // UVs
                geo.vertexUVs.push_back(sceneMesh.uvs[ui0]);
                geo.vertexUVs.push_back(sceneMesh.uvs[ui0 + 1]);
                geo.vertexUVs.push_back(sceneMesh.uvs[ui1]);
                geo.vertexUVs.push_back(sceneMesh.uvs[ui1 + 1]);
                geo.vertexUVs.push_back(sceneMesh.uvs[ui2]);
                geo.vertexUVs.push_back(sceneMesh.uvs[ui2 + 1]);

                // Tangent: computed from edges and UV derivatives
                glm::vec3 v0(sceneMesh.vertices[vi0], sceneMesh.vertices[vi0 + 1], sceneMesh.vertices[vi0 + 2]);
                glm::vec3 v1(sceneMesh.vertices[vi1], sceneMesh.vertices[vi1 + 1], sceneMesh.vertices[vi1 + 2]);
                glm::vec3 v2(sceneMesh.vertices[vi2], sceneMesh.vertices[vi2 + 1], sceneMesh.vertices[vi2 + 2]);

                glm::vec2 uv0(sceneMesh.uvs[ui0], sceneMesh.uvs[ui0 + 1]);
                glm::vec2 uv1(sceneMesh.uvs[ui1], sceneMesh.uvs[ui1 + 1]);
                glm::vec2 uv2(sceneMesh.uvs[ui2], sceneMesh.uvs[ui2 + 1]);

                glm::vec3 edge1 = v1 - v0;
                glm::vec3 edge2 = v2 - v0;
                glm::vec2 dUV1 = uv1 - uv0;
                glm::vec2 dUV2 = uv2 - uv0;

                float det = dUV1.x * dUV2.y - dUV1.y * dUV2.x;
                glm::vec3 tangent(1.0f, 0.0f, 0.0f);
                if (std::abs(det) > 1e-8f)
                {
                    float r = 1.0f / det;
                    tangent = normalize(r * (dUV2.y * edge1 - dUV1.y * edge2));
                }

                // Same tangent for all 3 vertices (flat per-triangle)
                for (int v = 0; v < 3; v++)
                {
                    geo.tangents.push_back(tangent.x);
                    geo.tangents.push_back(tangent.y);
                    geo.tangents.push_back(tangent.z);
                }
            }
        }
        else
        {
            geo.vertexUVs.resize(triCount * 6, 0.0f);
            geo.tangents.resize(triCount * 9, 0.0f);
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