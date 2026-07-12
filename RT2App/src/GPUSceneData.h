#pragma once

#ifndef GPU_SCENE_DATA_H
#define GPU_SCENE_DATA_H

#include "SceneTypes.h"
#include "ECSScene.h"
#include <glm/glm.hpp>
#include <vector>

// Include the shared shader interface header to keep C++/GLSL structs in sync.
// The GPUMaterial and GPUTriangleLight structs below must match the Material
// and TriangleLight structs defined in shader_interface.h.
#include "../shaders/shader_interface.h"

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
    int       metallicRoughnessTextureIndex; // -1 = no texture
    int       pad0;
    int       pad1;
    int       pad2;

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
        m.metallicRoughnessTextureIndex = sm.metallicRoughnessTextureIndex;
        return m;
    }

    GPUMaterial()
        : baseColor_metallic(0.8f, 0.8f, 0.8f, 0.0f)
        , emissive_roughness(0.0f, 0.0f, 0.0f, 0.5f)
        , ior(1.5f), alphaCutoff(0.5f), alphaMode(0.0f), baseAlpha(1.0f)
        , textureIndices(-1, -1, -1, 0)  // w = floatBitsToInt(0.0f) = 0
        , metallicRoughnessTextureIndex(-1), pad0(0), pad1(0), pad2(0)
    {}
};

// Non-owning reference to mesh geometry stored in MeshRegistry.
// GPUSceneData does NOT own mesh data — it points into ECSScene::meshRegistry.
// The MeshRegistry must outlive any GPUSceneData that references it.
struct GPUMeshGeometry
{
    const std::vector<float>*    vertices = nullptr;    // position.xyz, stride 3
    const std::vector<uint32_t>* indices = nullptr;     // triangle indices
    const std::vector<float>*    normals = nullptr;     // normal.xyz, stride 3 (may be null)
    const std::vector<float>*    uvs = nullptr;         // texcoord.xy, stride 2 (may be null)
    const std::vector<uint32_t>* materialIndices = nullptr; // per-triangle material index (may be null)
    uint32_t                     materialIndex = 0;
};

// One instance of a mesh in the world. Multiple instances can reference
// the same meshIndex (BLAS) with different transforms — instancing.
struct GPUInstance
{
    uint32_t     meshIndex = 0;       // index into GPUSceneData::meshes
    uint32_t     materialIndex = 0;   // material for this instance
    glm::mat4    worldMatrix = glm::mat4(1.0f);       // object-to-world
    glm::mat4    prevWorldMatrix = glm::mat4(1.0f);   // previous frame (for motion vectors)
    bool         isTransparent = false;  // alpha mode > 0.5 → any-hit SBT group
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
    std::vector<GPUMeshGeometry>  meshes;     // unique geometry (one BLAS per entry)
    std::vector<GPUInstance>      instances;  // TLAS instances (one per entity with MeshRef)
    std::vector<GPUMaterial>      materials;
    std::vector<SceneTexture>     textures;
    std::vector<GPUTriangleLight> lights;  // emissive triangles for NEE
    float                         totalLightArea = 0.0f;  // sum of all light triangle areas

    // Environment map (M8). When envMapIndex >= 0, the miss shader samples
    // this texture from the bindless array instead of the sky gradient.
    // CDF textures are used for importance-sampled NEE of the env map.
    int  envMapIndex = -1;      // index into textures[] (or -1 if no env map)
    float envIntensity = 1.0f;  // multiplier for env map radiance
    // CDF data (uploaded as separate GPU textures via RendererGPU)
    std::vector<float> marginalCDF;  // height entries, CDF over rows
    std::vector<float> conditionalCDF; // width*height entries, CDF within each row
    int cdfWidth = 0;
    int cdfHeight = 0;
};

// Convert an ECSScene into GPUSceneData: one GPUMeshGeometry per unique mesh
// in the MeshRegistry, one GPUInstance per entity with MeshRef. World matrices
// are read from Transform components (must be updated by SceneGraph first).
GPUSceneData BuildGPUSceneDataFromECS(const ECSScene& ecsScene);

// Update only the instances[] and lights[] arrays in an existing GPUSceneData
// from the current ECS transforms. Meshes and materials are unchanged.
// Call after SceneGraph::UpdateWorldTransforms when transforms have changed.
void UpdateInstancesFromECS(GPUSceneData& gpu, const ECSScene& ecsScene);

// Build marginal and conditional CDFs for env map importance sampling (M8).
void BuildEnvMapCDF(const std::vector<float>& floatPixels, int width, int height,
                    std::vector<float>& marginalCDF, std::vector<float>& conditionalCDF);

// ============================================================================
// Verify C++ structs match the shared shader_interface.h layouts
// ============================================================================
static_assert(sizeof(GPUMaterial) == sizeof(SIMaterial),
    "GPUMaterial size mismatch with shader_interface.h SIMaterial");
static_assert(sizeof(GPUTriangleLight) == sizeof(SITriangleLight),
    "GPUTriangleLight size mismatch with shader_interface.h SITriangleLight");
static_assert(offsetof(GPUMaterial, baseColor_metallic) == offsetof(SIMaterial, baseColor_metallic),
    "GPUMaterial baseColor_metallic offset mismatch");
static_assert(offsetof(GPUMaterial, textureIndices) == offsetof(SIMaterial, textureIndices),
    "GPUMaterial textureIndices offset mismatch");
static_assert(offsetof(GPUMaterial, metallicRoughnessTextureIndex) == offsetof(SIMaterial, extraIndices),
    "GPUMaterial metallicRoughnessTextureIndex offset mismatch with SIMaterial.extraIndices.x");

#endif // GPU_SCENE_DATA_H