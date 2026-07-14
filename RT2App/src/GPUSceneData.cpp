#include "GPUSceneData.h"
#include "SceneGraph.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

// Build marginal and conditional CDFs for environment map importance sampling.
// The env map is an equirectangular HDR image. We compute the luminance of each
// pixel, weight by the equirectangular solid-angle Jacobian, and build:
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
        // The shader maps v to latitude phi = (0.5 - v) * PI. Its spherical
        // area Jacobian is cos(phi), not sin(phi). Using sin(phi) suppresses
        // the lower hemisphere and produces rare, enormous-PDF-weighted samples.
        float v = (float(y) + 0.5f) / float(height);
        float latitude = (0.5f - v) * 3.14159265359f;
        float solidAngleWeight = std::cos(latitude);
        if (solidAngleWeight < 1e-6f) solidAngleWeight = 1e-6f;

        float rowSum = 0.0f;
        for (int x = 0; x < width; x++)
        {
            int idx = (y * width + x) * 4;
            float r = floatPixels[idx + 0];
            float g = floatPixels[idx + 1];
            float b = floatPixels[idx + 2];
            if (std::isnan(r) || std::isinf(r)) r = 0.0f;
            if (std::isnan(g) || std::isinf(g)) g = 0.0f;
            if (std::isnan(b) || std::isinf(b)) b = 0.0f;
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            float weight = lum * solidAngleWeight;
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
        geo.vertices = &src.vertices;
        geo.indices  = &src.indices;
        geo.normals  = src.normals.empty() ? nullptr : &src.normals;
        geo.uvs      = src.uvs.empty() ? nullptr : &src.uvs;
        geo.tangents = src.tangents.empty() ? nullptr : &src.tangents;
        geo.materialIndices = src.materialIndices.empty() ? nullptr : &src.materialIndices;

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
        inst.materialIndex = (ref.materialIndex >= 0 &&
            ref.materialIndex < static_cast<int>(gpu.materials.size()))
            ? static_cast<uint32_t>(ref.materialIndex) : 0xFFFFFFFFu;
        inst.worldMatrix = tf.worldMatrix;
        inst.prevWorldMatrix = tf.prevWorldMatrix;

        // Check transparency from material (only for override materials)
        if (inst.materialIndex != 0xFFFFFFFFu && inst.materialIndex < gpu.materials.size())
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

        // Determine effective material index for emissive check
        // For per-triangle materials, check each triangle's material
        if (inst.materialIndex != 0xFFFFFFFFu)
        {
            const auto& mat = gpu.materials[inst.materialIndex];
            glm::vec3 emissive = glm::vec3(mat.emissive_roughness);
            bool isEmissive = (emissive.x > 0.0f || emissive.y > 0.0f || emissive.z > 0.0f);
            if (!isEmissive)
                continue;

            int emissiveTexIdx = mat.textureIndices.z;
            const auto& verts = *mesh.vertices;
            const auto& idxs = *mesh.indices;
            uint32_t triCount = static_cast<uint32_t>(idxs.size() / 3);

            for (uint32_t t = 0; t < triCount; t++)
            {
                uint32_t vi0 = idxs[t * 3 + 0] * 3;
                uint32_t vi1 = idxs[t * 3 + 1] * 3;
                uint32_t vi2 = idxs[t * 3 + 2] * 3;

                glm::vec4 v0 = inst.worldMatrix * glm::vec4(verts[vi0], verts[vi0 + 1], verts[vi0 + 2], 1.0f);
                glm::vec4 v1 = inst.worldMatrix * glm::vec4(verts[vi1], verts[vi1 + 1], verts[vi1 + 2], 1.0f);
                glm::vec4 v2 = inst.worldMatrix * glm::vec4(verts[vi2], verts[vi2 + 1], verts[vi2 + 2], 1.0f);

                glm::vec3 w0(v0), w1(v1), w2(v2);
                glm::vec3 edge1 = w1 - w0;
                glm::vec3 edge2 = w2 - w0;
                float area = 0.5f * std::abs(glm::length(glm::cross(edge1, edge2)));

                if (area < 1e-8f)
                    continue;

                GPUTriangleLight light;
                light.emission_area = glm::vec4(emissive, area);
                light.ids = glm::uvec4(instIdx, t, inst.materialIndex,
                                       emissiveTexIdx >= 0 ? static_cast<uint32_t>(emissiveTexIdx) : 0xFFFFFFFFu);
                gpu.lights.push_back(light);
                gpu.totalLightArea += area;
            }
        }
        else
        {
            // Per-triangle materials: iterate triangles, check each material
            if (!mesh.materialIndices)
                continue;
            const auto& matIndices = *mesh.materialIndices;
            const auto& verts = *mesh.vertices;
            const auto& idxs = *mesh.indices;
            uint32_t triCount = static_cast<uint32_t>(idxs.size() / 3);

            for (uint32_t t = 0; t < triCount; t++)
            {
                uint32_t triMatIdx = matIndices[t];
                if (triMatIdx >= gpu.materials.size())
                    continue;
                const auto& mat = gpu.materials[triMatIdx];
                glm::vec3 emissive = glm::vec3(mat.emissive_roughness);
                bool isEmissive = (emissive.x > 0.0f || emissive.y > 0.0f || emissive.z > 0.0f);
                if (!isEmissive)
                    continue;

                int emissiveTexIdx = mat.textureIndices.z;

                uint32_t vi0 = idxs[t * 3 + 0] * 3;
                uint32_t vi1 = idxs[t * 3 + 1] * 3;
                uint32_t vi2 = idxs[t * 3 + 2] * 3;

                glm::vec4 v0 = inst.worldMatrix * glm::vec4(verts[vi0], verts[vi0 + 1], verts[vi0 + 2], 1.0f);
                glm::vec4 v1 = inst.worldMatrix * glm::vec4(verts[vi1], verts[vi1 + 1], verts[vi1 + 2], 1.0f);
                glm::vec4 v2 = inst.worldMatrix * glm::vec4(verts[vi2], verts[vi2 + 1], verts[vi2 + 2], 1.0f);

                glm::vec3 w0(v0), w1(v1), w2(v2);
                glm::vec3 edge1 = w1 - w0;
                glm::vec3 edge2 = w2 - w0;
                float area = 0.5f * std::abs(glm::length(glm::cross(edge1, edge2)));

                if (area < 1e-8f)
                    continue;

                GPUTriangleLight light;
                light.emission_area = glm::vec4(emissive, area);
                light.ids = glm::uvec4(instIdx, t, triMatIdx,
                                       emissiveTexIdx >= 0 ? static_cast<uint32_t>(emissiveTexIdx) : 0xFFFFFFFFu);
                gpu.lights.push_back(light);
                gpu.totalLightArea += area;
            }
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
        inst.materialIndex = (ref.materialIndex >= 0 &&
            ref.materialIndex < static_cast<int>(gpu.materials.size()))
            ? static_cast<uint32_t>(ref.materialIndex) : 0xFFFFFFFFu;
        inst.worldMatrix = tf.worldMatrix;
        inst.prevWorldMatrix = tf.prevWorldMatrix;

        if (inst.materialIndex != 0xFFFFFFFFu && inst.materialIndex < gpu.materials.size())
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

        if (inst.materialIndex != 0xFFFFFFFFu)
        {
            const auto& mat = gpu.materials[inst.materialIndex];
            glm::vec3 emissive = glm::vec3(mat.emissive_roughness);
            bool isEmissive = (emissive.x > 0.0f || emissive.y > 0.0f || emissive.z > 0.0f);
            if (!isEmissive)
                continue;

            int emissiveTexIdx = mat.textureIndices.z;
            const auto& verts = *mesh.vertices;
            const auto& idxs = *mesh.indices;
            uint32_t triCount = static_cast<uint32_t>(idxs.size() / 3);

            for (uint32_t t = 0; t < triCount; t++)
            {
                uint32_t vi0 = idxs[t * 3 + 0] * 3;
                uint32_t vi1 = idxs[t * 3 + 1] * 3;
                uint32_t vi2 = idxs[t * 3 + 2] * 3;

                glm::vec4 v0 = inst.worldMatrix * glm::vec4(verts[vi0], verts[vi0 + 1], verts[vi0 + 2], 1.0f);
                glm::vec4 v1 = inst.worldMatrix * glm::vec4(verts[vi1], verts[vi1 + 1], verts[vi1 + 2], 1.0f);
                glm::vec4 v2 = inst.worldMatrix * glm::vec4(verts[vi2], verts[vi2 + 1], verts[vi2 + 2], 1.0f);

                glm::vec3 w0(v0), w1(v1), w2(v2);
                glm::vec3 edge1 = w1 - w0;
                glm::vec3 edge2 = w2 - w0;
                float area = 0.5f * std::abs(glm::length(glm::cross(edge1, edge2)));

                if (area < 1e-8f)
                    continue;

                GPUTriangleLight light;
                light.emission_area = glm::vec4(emissive, area);
                light.ids = glm::uvec4(instIdx, t, inst.materialIndex,
                                       emissiveTexIdx >= 0 ? static_cast<uint32_t>(emissiveTexIdx) : 0xFFFFFFFFu);
                gpu.lights.push_back(light);
                gpu.totalLightArea += area;
            }
        }
        else
        {
            if (!mesh.materialIndices)
                continue;
            const auto& matIndices = *mesh.materialIndices;
            const auto& verts = *mesh.vertices;
            const auto& idxs = *mesh.indices;
            uint32_t triCount = static_cast<uint32_t>(idxs.size() / 3);

            for (uint32_t t = 0; t < triCount; t++)
            {
                uint32_t triMatIdx = matIndices[t];
                if (triMatIdx >= gpu.materials.size())
                    continue;
                const auto& mat = gpu.materials[triMatIdx];
                glm::vec3 emissive = glm::vec3(mat.emissive_roughness);
                bool isEmissive = (emissive.x > 0.0f || emissive.y > 0.0f || emissive.z > 0.0f);
                if (!isEmissive)
                    continue;

                int emissiveTexIdx = mat.textureIndices.z;

                uint32_t vi0 = idxs[t * 3 + 0] * 3;
                uint32_t vi1 = idxs[t * 3 + 1] * 3;
                uint32_t vi2 = idxs[t * 3 + 2] * 3;

                glm::vec4 v0 = inst.worldMatrix * glm::vec4(verts[vi0], verts[vi0 + 1], verts[vi0 + 2], 1.0f);
                glm::vec4 v1 = inst.worldMatrix * glm::vec4(verts[vi1], verts[vi1 + 1], verts[vi1 + 2], 1.0f);
                glm::vec4 v2 = inst.worldMatrix * glm::vec4(verts[vi2], verts[vi2 + 1], verts[vi2 + 2], 1.0f);

                glm::vec3 w0(v0), w1(v1), w2(v2);
                glm::vec3 edge1 = w1 - w0;
                glm::vec3 edge2 = w2 - w0;
                float area = 0.5f * std::abs(glm::length(glm::cross(edge1, edge2)));

                if (area < 1e-8f)
                    continue;

                GPUTriangleLight light;
                light.emission_area = glm::vec4(emissive, area);
                light.ids = glm::uvec4(instIdx, t, triMatIdx,
                                       emissiveTexIdx >= 0 ? static_cast<uint32_t>(emissiveTexIdx) : 0xFFFFFFFFu);
                gpu.lights.push_back(light);
                gpu.totalLightArea += area;
            }
        }
    }
}
