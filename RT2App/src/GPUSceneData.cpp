#include "GPUSceneData.h"
#include "SceneGraph.h"
#include "SceneVisibility.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace
{
constexpr uint32_t kEmissiveOccupancyBlockSize = 8;

void BuildEmissiveTextureOccupancy(GPUSceneData& gpu)
{
    gpu.emissiveTextureOccupancy.clear();
    gpu.emissiveTextureOccupancy.resize(gpu.textures.size());
    std::vector<bool> referenced(gpu.textures.size(), false);
    for (const auto& material : gpu.materials)
    {
        const glm::vec3 emission(material.emissive_roughness);
        const int textureIndex = material.textureIndices.z;
        if ((emission.x > 0.0f || emission.y > 0.0f || emission.z > 0.0f) &&
            textureIndex >= 0 && textureIndex < static_cast<int>(referenced.size()))
            referenced[textureIndex] = true;
    }

    for (size_t textureIndex = 0; textureIndex < gpu.textures.size(); textureIndex++)
    {
        if (!referenced[textureIndex])
            continue;
        const SceneTexture& texture = gpu.textures[textureIndex];
        if (texture.width <= 0 || texture.height <= 0 || texture.channels <= 0 || texture.pixels.empty())
            continue;
        const size_t requiredBytes = static_cast<size_t>(texture.width) * texture.height * texture.channels;
        if (texture.pixels.size() < requiredBytes)
            continue;

        EmissiveTextureOccupancy& occupancy = gpu.emissiveTextureOccupancy[textureIndex];
        occupancy.blockWidth = (static_cast<uint32_t>(texture.width) + kEmissiveOccupancyBlockSize - 1) /
                               kEmissiveOccupancyBlockSize;
        occupancy.blockHeight = (static_cast<uint32_t>(texture.height) + kEmissiveOccupancyBlockSize - 1) /
                                kEmissiveOccupancyBlockSize;
        const uint32_t stride = occupancy.blockWidth + 1;
        occupancy.summedArea.assign(static_cast<size_t>(stride) * (occupancy.blockHeight + 1), 0);

        for (uint32_t blockY = 0; blockY < occupancy.blockHeight; blockY++)
        {
            uint32_t rowSum = 0;
            for (uint32_t blockX = 0; blockX < occupancy.blockWidth; blockX++)
            {
                bool emits = false;
                const uint32_t xBegin = blockX * kEmissiveOccupancyBlockSize;
                const uint32_t yBegin = blockY * kEmissiveOccupancyBlockSize;
                const uint32_t xEnd = std::min(xBegin + kEmissiveOccupancyBlockSize, static_cast<uint32_t>(texture.width));
                const uint32_t yEnd = std::min(yBegin + kEmissiveOccupancyBlockSize, static_cast<uint32_t>(texture.height));
                for (uint32_t y = yBegin; y < yEnd && !emits; y++)
                {
                    for (uint32_t x = xBegin; x < xEnd; x++)
                    {
                        const size_t pixel = (static_cast<size_t>(y) * texture.width + x) * texture.channels;
                        const int colorChannels = std::min(texture.channels, 3);
                        for (int channel = 0; channel < colorChannels; channel++)
                        {
                            if (texture.pixels[pixel + channel] != 0)
                            {
                                emits = true;
                                break;
                            }
                        }
                        if (emits) break;
                    }
                }
                rowSum += emits ? 1u : 0u;
                occupancy.summedArea[static_cast<size_t>(blockY + 1) * stride + blockX + 1] =
                    occupancy.summedArea[static_cast<size_t>(blockY) * stride + blockX + 1] + rowSum;
            }
        }
    }
}

bool TriangleTextureMayEmit(const GPUSceneData& gpu, const GPUMeshGeometry& mesh,
                            uint32_t triangleIndex, int textureIndex)
{
    if (textureIndex < 0 || textureIndex >= static_cast<int>(gpu.textures.size()) ||
        textureIndex >= static_cast<int>(gpu.emissiveTextureOccupancy.size()) || !mesh.uvs)
        return true;
    const EmissiveTextureOccupancy& occupancy = gpu.emissiveTextureOccupancy[textureIndex];
    if (occupancy.summedArea.empty() || occupancy.blockWidth == 0 || occupancy.blockHeight == 0)
        return true;

    const SceneTexture& texture = gpu.textures[textureIndex];
    const auto& indices = *mesh.indices;
    const auto& uvs = *mesh.uvs;
    const size_t indexBase = static_cast<size_t>(triangleIndex) * 3;
    if (indexBase + 2 >= indices.size())
        return true;
    const uint32_t vertices[3] = { indices[indexBase], indices[indexBase + 1], indices[indexBase + 2] };
    glm::vec2 triangleUV[3];
    for (int vertex = 0; vertex < 3; vertex++)
    {
        const size_t uvIndex = static_cast<size_t>(vertices[vertex]) * 2;
        if (uvIndex + 1 >= uvs.size())
            return true;
        triangleUV[vertex] = glm::vec2(uvs[uvIndex], uvs[uvIndex + 1]);
        if (!std::isfinite(triangleUV[vertex].x) || !std::isfinite(triangleUV[vertex].y) ||
            triangleUV[vertex].x < 0.0f || triangleUV[vertex].x > 1.0f ||
            triangleUV[vertex].y < 0.0f || triangleUV[vertex].y > 1.0f)
            return true; // Unknown wrap behavior: retain conservatively.
    }

    const float minU = std::min({ triangleUV[0].x, triangleUV[1].x, triangleUV[2].x });
    const float maxU = std::max({ triangleUV[0].x, triangleUV[1].x, triangleUV[2].x });
    const float minV = std::min({ triangleUV[0].y, triangleUV[1].y, triangleUV[2].y });
    const float maxV = std::max({ triangleUV[0].y, triangleUV[1].y, triangleUV[2].y });
    const int x0 = std::clamp(static_cast<int>(std::floor(minU * texture.width)) - 1, 0, texture.width - 1);
    const int x1 = std::clamp(static_cast<int>(std::ceil(maxU * texture.width)) + 1, 0, texture.width - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(minV * texture.height)) - 1, 0, texture.height - 1);
    const int y1 = std::clamp(static_cast<int>(std::ceil(maxV * texture.height)) + 1, 0, texture.height - 1);
    const uint32_t blockX0 = static_cast<uint32_t>(x0) / kEmissiveOccupancyBlockSize;
    const uint32_t blockX1 = static_cast<uint32_t>(x1) / kEmissiveOccupancyBlockSize;
    const uint32_t blockY0 = static_cast<uint32_t>(y0) / kEmissiveOccupancyBlockSize;
    const uint32_t blockY1 = static_cast<uint32_t>(y1) / kEmissiveOccupancyBlockSize;
    const uint32_t stride = occupancy.blockWidth + 1;
    const auto sample = [&](uint32_t x, uint32_t y) {
        return occupancy.summedArea[static_cast<size_t>(y) * stride + x];
    };
    const uint32_t occupiedBlocks = sample(blockX1 + 1, blockY1 + 1) - sample(blockX0, blockY1 + 1) -
                                    sample(blockX1 + 1, blockY0) + sample(blockX0, blockY0);
    return occupiedBlocks != 0;
}
}

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

GPUSceneData BuildGPUSceneDataFromECS(const ECSScene& ecsScene,
                                     RenderInstanceMap* instanceMap)
{
    GPUSceneData gpu;
	if (instanceMap)
		instanceMap->clear();

    // Copy textures and materials (same as BuildGPUSceneData)
    gpu.textures = ecsScene.textures;
    gpu.materials.reserve(ecsScene.materials.size());
    for (const auto& sm : ecsScene.materials)
        gpu.materials.push_back(GPUMaterial::fromSceneMaterial(sm));
    if (gpu.materials.empty())
        gpu.materials.push_back(GPUMaterial{});
    BuildEmissiveTextureOccupancy(gpu);

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
    const auto renderables = SceneVisibility::CollectVisibleRenderables(ecsScene);

    for (auto entity : renderables)
    {
        const auto& ref = ecsScene.registry.get<MeshRef>(entity);
        const auto& tf = ecsScene.registry.get<Transform>(entity);

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
		if (instanceMap)
		{
			const auto* identity = ecsScene.registry.try_get<EntityIdComponent>(entity);
			instanceMap->push_back(identity ? identity->id : rt2::core::UUID::Nil());
		}
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
                gpu.sourceEmissiveTriangleCount++;
                if (!TriangleTextureMayEmit(gpu, mesh, t, emissiveTexIdx))
                {
                    gpu.filteredBlackEmissiveTriangleCount++;
                    continue;
                }
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

                gpu.sourceEmissiveTriangleCount++;
                if (!TriangleTextureMayEmit(gpu, mesh, t, emissiveTexIdx))
                {
                    gpu.filteredBlackEmissiveTriangleCount++;
                    continue;
                }

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

void UpdateInstancesFromECS(GPUSceneData& gpu, const ECSScene& ecsScene,
                            RenderInstanceMap* instanceMap)
{
    // Update instance world matrices from ECS transforms
    const auto renderables = SceneVisibility::CollectVisibleRenderables(ecsScene);

    // The instance order must match BuildGPUSceneDataFromECS exactly
    // (entt view iteration order is deterministic for the same registry state).
    gpu.instances.clear();
	if (instanceMap)
		instanceMap->clear();

    for (auto entity : renderables)
    {
        const auto& ref = ecsScene.registry.get<MeshRef>(entity);
        const auto& tf = ecsScene.registry.get<Transform>(entity);

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
		if (instanceMap)
		{
			const auto* identity = ecsScene.registry.try_get<EntityIdComponent>(entity);
			instanceMap->push_back(identity ? identity->id : rt2::core::UUID::Nil());
		}
    }

    // Rebuild light list (areas change with transforms)
    gpu.lights.clear();
    gpu.totalLightArea = 0.0f;
    gpu.sourceEmissiveTriangleCount = 0;
    gpu.filteredBlackEmissiveTriangleCount = 0;

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
                gpu.sourceEmissiveTriangleCount++;
                if (!TriangleTextureMayEmit(gpu, mesh, t, emissiveTexIdx))
                {
                    gpu.filteredBlackEmissiveTriangleCount++;
                    continue;
                }
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

                gpu.sourceEmissiveTriangleCount++;
                if (!TriangleTextureMayEmit(gpu, mesh, t, emissiveTexIdx))
                {
                    gpu.filteredBlackEmissiveTriangleCount++;
                    continue;
                }

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
