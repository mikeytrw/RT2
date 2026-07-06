#include "GPUSceneData.h"
#include "SceneGraph.h"
#include <glm/glm.hpp>
#include <cmath>

// Build marginal and conditional CDFs for environment map importance sampling.
// The env map is an equirectangular HDR image. We compute the luminance of each
// pixel, weight by sin(theta) (solid-angle correction), and build:
//   - conditionalCDF[row][col]: CDF over columns within each row
//   - marginalCDF[row]: CDF over rows (using row sums)
// Both CDFs are normalized to [0, 1].
void BuildEnvMapCDF(const std::vector<float>& floatPixels, int width, int height,
                    std::vector<float>& marginalCDF, std::vector<float>& conditionalCDF)
{
    if (width <= 0 || height <= 0 || floatPixels.empty())
        return;

    marginalCDF.resize(height);
    conditionalCDF.resize(width * height);

    std::vector<float> rowSums(height, 0.0f);

    for (int y = 0; y < height; y++)
    {
        // Solid-angle weight: sin(theta) where theta = (0.5 - v) * PI
        float v = (float(y) + 0.5f) / float(height);
        float theta = (0.5f - v) * 3.14159265359f;
        float sinTheta = std::sin(theta);
        if (sinTheta < 1e-6f) sinTheta = 1e-6f;

        float rowSum = 0.0f;
        for (int x = 0; x < width; x++)
        {
            int idx = (y * width + x) * 4;
            float r = floatPixels[idx + 0];
            float g = floatPixels[idx + 1];
            float b = floatPixels[idx + 2];
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            float weight = lum * sinTheta;
            rowSum += weight;
            conditionalCDF[y * width + x] = rowSum;
            rowSums[y] = rowSum;
        }

        // Normalize conditional CDF for this row
        if (rowSum > 1e-8f)
        {
            float invRowSum = 1.0f / rowSum;
            for (int x = 0; x < width; x++)
                conditionalCDF[y * width + x] *= invRowSum;
        }
        else
        {
            // Uniform fallback for zero-luminance rows
            for (int x = 0; x < width; x++)
                conditionalCDF[y * width + x] = float(x + 1) / float(width);
        }
    }

    // Build marginal CDF from row sums
    float totalSum = 0.0f;
    for (int y = 0; y < height; y++)
    {
        totalSum += rowSums[y];
        marginalCDF[y] = totalSum;
    }

    // Normalize marginal CDF
    if (totalSum > 1e-8f)
    {
        float invTotal = 1.0f / totalSum;
        for (int y = 0; y < height; y++)
            marginalCDF[y] *= invTotal;
    }
    else
    {
        for (int y = 0; y < height; y++)
            marginalCDF[y] = float(y + 1) / float(height);
    }
}

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
    uint32_t instanceID = 0;  // TLAS instance index = mesh index in gpu.meshes
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

        // --- Build light list: one GPUTriangleLight per emissive triangle ---
        // A material is emissive if emissiveColor * emissiveIntensity != 0.
        const GPUMaterial& mat = gpu.materials[matIdx];
        glm::vec3 emissive = glm::vec3(mat.emissive_roughness);  // pre-baked color*intensity
        bool isEmissive = (emissive.x > 0.0f || emissive.y > 0.0f || emissive.z > 0.0f);

        if (isEmissive)
        {
            int emissiveTexIdx = mat.textureIndices.z;
            for (uint32_t t = 0; t < triCount; t++)
            {
                uint32_t vi0 = sceneMesh.indices[t * 3 + 0] * 3;
                uint32_t vi1 = sceneMesh.indices[t * 3 + 1] * 3;
                uint32_t vi2 = sceneMesh.indices[t * 3 + 2] * 3;

                glm::vec3 v0(sceneMesh.vertices[vi0], sceneMesh.vertices[vi0 + 1], sceneMesh.vertices[vi0 + 2]);
                glm::vec3 v1(sceneMesh.vertices[vi1], sceneMesh.vertices[vi1 + 1], sceneMesh.vertices[vi1 + 2]);
                glm::vec3 v2(sceneMesh.vertices[vi2], sceneMesh.vertices[vi2 + 1], sceneMesh.vertices[vi2 + 2]);

                glm::vec3 edge1 = v1 - v0;
                glm::vec3 edge2 = v2 - v0;
                float area = 0.5f * std::abs(glm::length(glm::cross(edge1, edge2)));

                GPUTriangleLight light;
                light.emission_area = glm::vec4(emissive, area);
                light.ids = glm::uvec4(instanceID, t, matIdx,
                                        emissiveTexIdx >= 0 ? static_cast<uint32_t>(emissiveTexIdx) : 0xFFFFFFFFu);
                gpu.lights.push_back(light);
                gpu.totalLightArea += area;
            }
        }

        gpu.meshes.push_back(std::move(geo));
        ++instanceID;
    }

    return gpu;
}

GPUSceneData BuildGPUSceneDataFromECS(const ECSScene& ecsScene)
{
    GPUSceneData gpu;

    // Copy textures and materials (same as BuildGPUSceneData)
    gpu.textures = ecsScene.textures;
    gpu.materials.reserve(ecsScene.materials.size());
    for (const auto& sm : ecsScene.materials)
        gpu.materials.push_back(GPUMaterial::fromSceneMaterial(sm));
    if (gpu.materials.empty())
        gpu.materials.push_back(GPUMaterial{});

    // Build one GPUMeshGeometry per unique mesh in the MeshRegistry.
    // Vertices stay in object space — transforms are applied via TLAS instances.
    uint32_t meshCount = ecsScene.meshRegistry.GetCount();
    gpu.meshes.reserve(meshCount);

    for (uint32_t m = 0; m < meshCount; m++)
    {
        const auto& src = ecsScene.meshRegistry.GetMesh(m);
        GPUMeshGeometry geo;
        geo.vertices = src.vertices;
        geo.indices  = src.indices;

        uint32_t triCount = static_cast<uint32_t>(src.indices.size() / 3);
        geo.vertexUVs.reserve(triCount * 6);
        geo.tangents.reserve(triCount * 12);

        if (!src.uvs.empty())
        {
            for (uint32_t t = 0; t < triCount; t++)
            {
                uint32_t vi0 = src.indices[t * 3 + 0] * 3;
                uint32_t vi1 = src.indices[t * 3 + 1] * 3;
                uint32_t vi2 = src.indices[t * 3 + 2] * 3;
                uint32_t ui0 = src.indices[t * 3 + 0] * 2;
                uint32_t ui1 = src.indices[t * 3 + 1] * 2;
                uint32_t ui2 = src.indices[t * 3 + 2] * 2;

                // UVs
                geo.vertexUVs.push_back(src.uvs[ui0]);
                geo.vertexUVs.push_back(src.uvs[ui0 + 1]);
                geo.vertexUVs.push_back(src.uvs[ui1]);
                geo.vertexUVs.push_back(src.uvs[ui1 + 1]);
                geo.vertexUVs.push_back(src.uvs[ui2]);
                geo.vertexUVs.push_back(src.uvs[ui2 + 1]);

                // Tangent from edges and UV derivatives
                glm::vec3 v0(src.vertices[vi0], src.vertices[vi0 + 1], src.vertices[vi0 + 2]);
                glm::vec3 v1(src.vertices[vi1], src.vertices[vi1 + 1], src.vertices[vi1 + 2]);
                glm::vec3 v2(src.vertices[vi2], src.vertices[vi2 + 1], src.vertices[vi2 + 2]);

                glm::vec2 uv0(src.uvs[ui0], src.uvs[ui0 + 1]);
                glm::vec2 uv1(src.uvs[ui1], src.uvs[ui1 + 1]);
                glm::vec2 uv2(src.uvs[ui2], src.uvs[ui2 + 1]);

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

        gpu.meshes.push_back(std::move(geo));
    }

    // Build instance list: one GPUInstance per entity with MeshRef
    // World matrices come from Transform (already resolved by SceneGraph)
    auto meshView = ecsScene.registry.view<MeshRef, Transform>();

    for (auto entity : meshView)
    {
        const auto& ref = meshView.get<MeshRef>(entity);
        const auto& tf = meshView.get<Transform>(entity);

        GPUInstance inst;
        inst.meshIndex = ref.meshIndex;
        inst.materialIndex = static_cast<uint32_t>(
            ref.materialIndex < static_cast<int>(gpu.materials.size()) ? ref.materialIndex : 0);
        inst.worldMatrix = tf.worldMatrix;
        inst.prevWorldMatrix = tf.prevWorldMatrix;

        // Check transparency from material
        if (inst.materialIndex < gpu.materials.size())
        {
            float alphaMode = gpu.materials[inst.materialIndex].alphaMode;
            inst.isTransparent = (alphaMode > 0.5f);
        }

        gpu.instances.push_back(inst);
    }

    // Build emissive triangle lights from instances with emissive materials.
    // Each emissive triangle stores (instanceID, primitiveID) so the shader
    // can look up positions from the combined buffers.
    // NOTE: Positions are in object space — the shader must transform them
    // to world space using the instance's world matrix. This will be handled
    // in Phase 2.6 (shader changes). For now, we bake world-space positions
    // like BuildGPUSceneData does, using the instance world matrix.
    for (uint32_t instIdx = 0; instIdx < gpu.instances.size(); instIdx++)
    {
        const auto& inst = gpu.instances[instIdx];
        if (inst.meshIndex >= gpu.meshes.size())
            continue;

        const auto& mesh = gpu.meshes[inst.meshIndex];
        const auto& mat = gpu.materials[inst.materialIndex];
        glm::vec3 emissive = glm::vec3(mat.emissive_roughness);
        bool isEmissive = (emissive.x > 0.0f || emissive.y > 0.0f || emissive.z > 0.0f);
        if (!isEmissive)
            continue;

        int emissiveTexIdx = mat.textureIndices.z;
        uint32_t triCount = static_cast<uint32_t>(mesh.indices.size() / 3);

        for (uint32_t t = 0; t < triCount; t++)
        {
            uint32_t vi0 = mesh.indices[t * 3 + 0] * 3;
            uint32_t vi1 = mesh.indices[t * 3 + 1] * 3;
            uint32_t vi2 = mesh.indices[t * 3 + 2] * 3;

            // Object-space vertices → world space via instance transform
            glm::vec4 v0 = inst.worldMatrix * glm::vec4(mesh.vertices[vi0], mesh.vertices[vi0 + 1], mesh.vertices[vi0 + 2], 1.0f);
            glm::vec4 v1 = inst.worldMatrix * glm::vec4(mesh.vertices[vi1], mesh.vertices[vi1 + 1], mesh.vertices[vi1 + 2], 1.0f);
            glm::vec4 v2 = inst.worldMatrix * glm::vec4(mesh.vertices[vi2], mesh.vertices[vi2 + 1], mesh.vertices[vi2 + 2], 1.0f);

            glm::vec3 w0(v0), w1(v1), w2(v2);
            glm::vec3 edge1 = w1 - w0;
            glm::vec3 edge2 = w2 - w0;
            float area = 0.5f * std::abs(glm::length(glm::cross(edge1, edge2)));

            GPUTriangleLight light;
            light.emission_area = glm::vec4(emissive, area);
            light.ids = glm::uvec4(instIdx, t, inst.materialIndex,
                                   emissiveTexIdx >= 0 ? static_cast<uint32_t>(emissiveTexIdx) : 0xFFFFFFFFu);
            gpu.lights.push_back(light);
            gpu.totalLightArea += area;
        }
    }

    return gpu;
}

void UpdateInstancesFromECS(GPUSceneData& gpu, const ECSScene& ecsScene)
{
    // Update instance world matrices from ECS transforms
    auto meshView = ecsScene.registry.view<MeshRef, Transform>();

    // The instance order must match BuildGPUSceneDataFromECS exactly
    // (entt view iteration order is deterministic for the same registry state).
    gpu.instances.clear();

    for (auto entity : meshView)
    {
        const auto& ref = meshView.get<MeshRef>(entity);
        const auto& tf = meshView.get<Transform>(entity);

        GPUInstance inst;
        inst.meshIndex = ref.meshIndex;
        inst.materialIndex = static_cast<uint32_t>(
            ref.materialIndex < static_cast<int>(gpu.materials.size()) ? ref.materialIndex : 0);
        inst.worldMatrix = tf.worldMatrix;
        inst.prevWorldMatrix = tf.prevWorldMatrix;

        if (inst.materialIndex < gpu.materials.size())
        {
            float alphaMode = gpu.materials[inst.materialIndex].alphaMode;
            inst.isTransparent = (alphaMode > 0.5f);
        }

        gpu.instances.push_back(inst);
    }

    // Rebuild light list (areas change with transforms)
    gpu.lights.clear();
    gpu.totalLightArea = 0.0f;

    for (uint32_t instIdx = 0; instIdx < gpu.instances.size(); instIdx++)
    {
        const auto& inst = gpu.instances[instIdx];
        if (inst.meshIndex >= gpu.meshes.size())
            continue;

        const auto& mesh = gpu.meshes[inst.meshIndex];
        const auto& mat = gpu.materials[inst.materialIndex];
        glm::vec3 emissive = glm::vec3(mat.emissive_roughness);
        bool isEmissive = (emissive.x > 0.0f || emissive.y > 0.0f || emissive.z > 0.0f);
        if (!isEmissive)
            continue;

        int emissiveTexIdx = mat.textureIndices.z;
        uint32_t triCount = static_cast<uint32_t>(mesh.indices.size() / 3);

        for (uint32_t t = 0; t < triCount; t++)
        {
            uint32_t vi0 = mesh.indices[t * 3 + 0] * 3;
            uint32_t vi1 = mesh.indices[t * 3 + 1] * 3;
            uint32_t vi2 = mesh.indices[t * 3 + 2] * 3;

            glm::vec4 v0 = inst.worldMatrix * glm::vec4(mesh.vertices[vi0], mesh.vertices[vi0 + 1], mesh.vertices[vi0 + 2], 1.0f);
            glm::vec4 v1 = inst.worldMatrix * glm::vec4(mesh.vertices[vi1], mesh.vertices[vi1 + 1], mesh.vertices[vi1 + 2], 1.0f);
            glm::vec4 v2 = inst.worldMatrix * glm::vec4(mesh.vertices[vi2], mesh.vertices[vi2 + 1], mesh.vertices[vi2 + 2], 1.0f);

            glm::vec3 w0(v0), w1(v1), w2(v2);
            glm::vec3 edge1 = w1 - w0;
            glm::vec3 edge2 = w2 - w0;
            float area = 0.5f * std::abs(glm::length(glm::cross(edge1, edge2)));

            GPUTriangleLight light;
            light.emission_area = glm::vec4(emissive, area);
            light.ids = glm::uvec4(instIdx, t, inst.materialIndex,
                                   emissiveTexIdx >= 0 ? static_cast<uint32_t>(emissiveTexIdx) : 0xFFFFFFFFu);
            gpu.lights.push_back(light);
            gpu.totalLightArea += area;
        }
    }
}