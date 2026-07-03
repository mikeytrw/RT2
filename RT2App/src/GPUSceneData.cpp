#include "GPUSceneData.h"
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