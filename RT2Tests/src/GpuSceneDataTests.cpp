#include <doctest/doctest.h>

#include "GPUSceneData.h"
#include "SceneTypes.h"
#include "ECSScene.h"
#include "ECSComponents.h"
#include "SceneGraph.h"
#include <glm/glm.hpp>
#include <cstddef>

// ============================================================================
// shader_interface.h struct layout tests (shared C++/GLSL)
// ============================================================================

TEST_CASE("SICameraData is 496 bytes (7 vec4 + 6 mat4)")
{
    CHECK(sizeof(SICameraData) == 496);
}

TEST_CASE("SIMaterial is 80 bytes")
{
    CHECK(sizeof(SIMaterial) == 80);
}

TEST_CASE("SITriangleLight is 32 bytes")
{
    CHECK(sizeof(SITriangleLight) == 32);
}

TEST_CASE("SINRDUniformData is 16 bytes")
{
    CHECK(sizeof(SINRDUniformData) == 16);
}

TEST_CASE("SIGIReservoir is 48 bytes (3 uvec4, std430)")
{
    CHECK(sizeof(SIGIReservoir) == 48);
}

TEST_CASE("SIGIPushConstants is 48 bytes")
{
    CHECK(sizeof(SIGIPushConstants) == 48);
}

TEST_CASE("Zeroed SIGIReservoir decodes as invalid")
{
    // vkCmdFillBuffer(..., 0) must produce an invalid reservoir: valid bit = 0
    // and M = 0. This is the contract that ClearAll relies on.
    SIGIReservoir r{};
    // data2.z packs (age << 16) | flags; zeroed → flags = 0 → valid bit clear.
    uint32_t flags = r.data2.z & 0xFFFFu;
    CHECK((flags & SI_GI_FLAG_VALID) == 0u);
    CHECK(r.data2.y == 0u);  // M == 0
}

TEST_CASE("SIReSTIRPushConstants carries both jitter samples")
{
    CHECK(sizeof(SIReSTIRPushConstants) == 60);
    CHECK(offsetof(SIReSTIRPushConstants, jitter) == 44);
}

TEST_CASE("GPUMaterial matches SIMaterial layout")
{
    CHECK(sizeof(GPUMaterial) == sizeof(SIMaterial));
    CHECK(offsetof(GPUMaterial, baseColor_metallic) == offsetof(SIMaterial, baseColor_metallic));
    CHECK(offsetof(GPUMaterial, textureIndices) == offsetof(SIMaterial, textureIndices));
}

TEST_CASE("GPUTriangleLight matches SITriangleLight layout")
{
    CHECK(sizeof(GPUTriangleLight) == sizeof(SITriangleLight));
}

// ============================================================================
// GPUMaterial struct layout tests (std430 / 32-byte alignment)
// ============================================================================

TEST_CASE("GPUMaterial is 80 bytes (2 vec4 + 4 float + 2 ivec4, std430)")
{
    CHECK(sizeof(GPUMaterial) == 80);
}

TEST_CASE("GPUMaterial default values are sensible")
{
    GPUMaterial mat;
    CHECK(mat.baseColor_metallic == glm::vec4(0.8f, 0.8f, 0.8f, 0.0f));
    CHECK(mat.emissive_roughness == glm::vec4(0.0f, 0.0f, 0.0f, 0.5f));
    CHECK(mat.ior == 1.5f);
    CHECK(mat.alphaCutoff == 0.5f);
    CHECK(mat.alphaMode == 0.0f); // OPAQUE
    CHECK(mat.baseAlpha == 1.0f);
    CHECK(mat.textureIndices.x == -1); // no baseColor texture
    CHECK(mat.textureIndices.y == -1); // no normal texture
    CHECK(mat.textureIndices.z == -1); // no emissive texture
}

TEST_CASE("GPUMaterial from SceneMaterial preserves texture indices")
{
    SceneMaterial sm;
    sm.baseColorTextureIndex = 3;
    sm.normalTextureIndex = 7;
    sm.emissiveTextureIndex = 12;

    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);

    CHECK(gm.textureIndices.x == 3);
    CHECK(gm.textureIndices.y == 7);
    CHECK(gm.textureIndices.z == 12);
}

TEST_CASE("GPUMaterial from SceneMaterial preserves PBR fields")
{
    SceneMaterial sm;
    sm.type = MaterialType::Lambertian;
    sm.baseColor = {0.9f, 0.2f, 0.1f};
    sm.metallic = 0.8f;
    sm.roughness = 0.3f;
    sm.ior = 1.45f;
    sm.emissiveColor = {1.0f, 0.5f, 0.2f};
    sm.emissiveIntensity = 3.0f;

    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);

    CHECK(gm.baseColor_metallic.x == 0.9f);
    CHECK(gm.baseColor_metallic.y == 0.2f);
    CHECK(gm.baseColor_metallic.z == 0.1f);
    CHECK(gm.baseColor_metallic.w == 0.8f);
    CHECK(gm.emissive_roughness.x == 1.0f * 3.0f);
    CHECK(gm.emissive_roughness.y == 0.5f * 3.0f);
    CHECK(gm.emissive_roughness.z == 0.2f * 3.0f);
    CHECK(gm.emissive_roughness.w == 0.3f);
    CHECK(gm.ior == 1.45f);
}

TEST_CASE("GPUMaterial emissive is zero when intensity is zero")
{
    SceneMaterial sm;
    sm.emissiveColor = {1.0f, 0.5f, 0.2f};
    sm.emissiveIntensity = 0.0f;

    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);

    CHECK(gm.emissive_roughness.x == 0.0f);
    CHECK(gm.emissive_roughness.y == 0.0f);
    CHECK(gm.emissive_roughness.z == 0.0f);
}

// ============================================================================
// GPUMeshGeometry tests
// ============================================================================

TEST_CASE("GPUMeshGeometry stores vertices and indices")
{
    std::vector<float> verts = {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f};
    std::vector<uint32_t> idxs = {0, 1, 2};
    GPUMeshGeometry geo;
    geo.vertices = &verts;
    geo.indices = &idxs;
    geo.materialIndex = 3;

    CHECK(geo.vertices->size() == 9);
    CHECK(geo.indices->size() == 3);
    CHECK(geo.materialIndex == 3);
}

TEST_CASE("GPUMeshGeometry default materialIndex is 0")
{
    GPUMeshGeometry geo;
    CHECK(geo.materialIndex == 0);
}

// ============================================================================
// GPUSceneData tests
// ============================================================================

TEST_CASE("GPUSceneData starts empty")
{
    GPUSceneData scene;
    CHECK(scene.meshes.empty());
    CHECK(scene.materials.empty());
}

TEST_CASE("GPUSceneData holds multiple meshes and materials")
{
    GPUSceneData scene;

    GPUMeshGeometry geo1;
    geo1.materialIndex = 0;
    scene.meshes.push_back(geo1);

    GPUMeshGeometry geo2;
    geo2.materialIndex = 1;
    scene.meshes.push_back(geo2);

    scene.materials.push_back(GPUMaterial::fromSceneMaterial(SceneMaterial{}));
    scene.materials.push_back(GPUMaterial::fromSceneMaterial(SceneMaterial{}));

    CHECK(scene.meshes.size() == 2);
    CHECK(scene.materials.size() == 2);
    CHECK(scene.meshes[0].materialIndex == 0);
    CHECK(scene.meshes[1].materialIndex == 1);
}

// ============================================================================
// BuildGPUSceneDataFromECS from ECSScene (integration of scene → GPU data)
// ============================================================================

TEST_CASE("BuildGPUSceneDataFromECS converts ECSScene meshes and materials")
{
    ECSScene ecsScene;

    SceneMaterial mat0;
    mat0.baseColor = {1.0f, 0.0f, 0.0f};
    mat0.metallic = 0.0f;
    ecsScene.materials.push_back(mat0);

    SceneMaterial mat1;
    mat1.baseColor = {0.0f, 0.0f, 1.0f};
    mat1.metallic = 1.0f;
    ecsScene.materials.push_back(mat1);

    MeshData meshData0;
    meshData0.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    meshData0.indices = {0, 1, 2};
    uint32_t meshIdx0 = ecsScene.meshRegistry.AddMesh(std::move(meshData0));

    MeshData meshData1;
    meshData1.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    meshData1.indices = {0, 1, 2};
    uint32_t meshIdx1 = ecsScene.meshRegistry.AddMesh(std::move(meshData1));

    {
        auto entity = ecsScene.registry.create();
        ecsScene.registry.emplace<Transform>(entity);
        auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
        ref.meshIndex = meshIdx0;
        ref.materialIndex = 0;
    }
    {
        auto entity = ecsScene.registry.create();
        ecsScene.registry.emplace<Transform>(entity);
        auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
        ref.meshIndex = meshIdx1;
        ref.materialIndex = 1;
    }

    SceneGraph::UpdateWorldTransforms(ecsScene.registry);

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    CHECK(gpu.meshes.size() == 2);
    CHECK(gpu.materials.size() == 2);
    CHECK(gpu.instances.size() == 2);

    bool foundMat0 = false, foundMat1 = false;
    for (const auto& inst : gpu.instances)
    {
        if (inst.materialIndex == 0) foundMat0 = true;
        if (inst.materialIndex == 1) foundMat1 = true;
    }
    CHECK(foundMat0);
    CHECK(foundMat1);

    CHECK(gpu.materials[0].baseColor_metallic.x == 1.0f);
    CHECK(gpu.materials[0].baseColor_metallic.w == 0.0f);

    CHECK(gpu.materials[1].baseColor_metallic.z == 1.0f);
    CHECK(gpu.materials[1].baseColor_metallic.w == 1.0f);
}

TEST_CASE("BuildGPUSceneDataFromECS handles mesh with invalid materialIndex")
{
    ECSScene ecsScene;
    ecsScene.materials.push_back(SceneMaterial{});

    MeshData meshData;
    meshData.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    meshData.indices = {0, 1, 2};
    uint32_t meshIdx = ecsScene.meshRegistry.AddMesh(std::move(meshData));

    auto entity = ecsScene.registry.create();
    ecsScene.registry.emplace<Transform>(entity);
    auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
    ref.meshIndex = meshIdx;
    ref.materialIndex = 99;

    SceneGraph::UpdateWorldTransforms(ecsScene.registry);

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    CHECK(gpu.instances[0].materialIndex == 0xFFFFFFFFu);
}

TEST_CASE("BuildGPUSceneDataFromECS only creates instances for entities with MeshRef")
{
    ECSScene ecsScene;
    ecsScene.materials.push_back(SceneMaterial{});

    MeshData meshData;
    meshData.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    meshData.indices = {0, 1, 2};
    uint32_t meshIdx = ecsScene.meshRegistry.AddMesh(std::move(meshData));

    {
        auto entity = ecsScene.registry.create();
        ecsScene.registry.emplace<Transform>(entity);
        auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
        ref.meshIndex = meshIdx;
        ref.materialIndex = 0;
    }
    {
        auto entity = ecsScene.registry.create();
        ecsScene.registry.emplace<Transform>(entity);
    }

    SceneGraph::UpdateWorldTransforms(ecsScene.registry);

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    CHECK(gpu.instances.size() == 1);
}

// ============================================================================
// GPUTriangleLight struct layout tests
// ============================================================================

TEST_CASE("GPUTriangleLight is 32 bytes (vec4 + uvec4, std430)")
{
    CHECK(sizeof(GPUTriangleLight) == 32);
}

TEST_CASE("GPUTriangleLight default values")
{
    GPUTriangleLight light;
    CHECK(light.emission_area == glm::vec4(0.0f));
    CHECK(light.ids == glm::uvec4(0, 0, 0, 0xFFFFFFFFu));
}

// ============================================================================
// Light list building tests (NEE Phase 1)
// ============================================================================

TEST_CASE("BuildGPUSceneDataFromECS finds no lights when no emissive materials")
{
    ECSScene ecsScene;

    SceneMaterial mat;
    mat.emissiveColor = {0.0f, 0.0f, 0.0f};
    mat.emissiveIntensity = 0.0f;
    ecsScene.materials.push_back(mat);

    MeshData meshData;
    meshData.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    meshData.indices = {0, 1, 2};
    uint32_t meshIdx = ecsScene.meshRegistry.AddMesh(std::move(meshData));

    auto entity = ecsScene.registry.create();
    ecsScene.registry.emplace<Transform>(entity);
    auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
    ref.meshIndex = meshIdx;
    ref.materialIndex = 0;

    SceneGraph::UpdateWorldTransforms(ecsScene.registry);

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    CHECK(gpu.lights.empty());
    CHECK(gpu.totalLightArea == 0.0f);
}

TEST_CASE("BuildGPUSceneDataFromECS finds one light per emissive triangle")
{
    ECSScene ecsScene;

    SceneMaterial mat;
    mat.emissiveColor = {1.0f, 0.8f, 0.4f};
    mat.emissiveIntensity = 5.0f;
    ecsScene.materials.push_back(mat);

    MeshData meshData;
    meshData.vertices = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    meshData.indices = {0, 1, 2, 0, 2, 3};
    uint32_t meshIdx = ecsScene.meshRegistry.AddMesh(std::move(meshData));

    auto entity = ecsScene.registry.create();
    ecsScene.registry.emplace<Transform>(entity);
    auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
    ref.meshIndex = meshIdx;
    ref.materialIndex = 0;

    SceneGraph::UpdateWorldTransforms(ecsScene.registry);

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    CHECK(gpu.lights.size() == 2);
    CHECK(gpu.totalLightArea == doctest::Approx(1.0f).epsilon(0.001f));
}

TEST_CASE("GPUTriangleLight stores correct ids and emission")
{
    ECSScene ecsScene;

    SceneMaterial mat;
    mat.emissiveColor = {1.0f, 0.5f, 0.2f};
    mat.emissiveIntensity = 3.0f;
    ecsScene.materials.push_back(mat);

    MeshData meshData;
    meshData.vertices = {0, 0, 0, 2, 0, 0, 0, 2, 0};
    meshData.indices = {0, 1, 2};
    uint32_t meshIdx = ecsScene.meshRegistry.AddMesh(std::move(meshData));

    auto entity = ecsScene.registry.create();
    ecsScene.registry.emplace<Transform>(entity);
    auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
    ref.meshIndex = meshIdx;
    ref.materialIndex = 0;

    SceneGraph::UpdateWorldTransforms(ecsScene.registry);

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    REQUIRE(gpu.lights.size() == 1);
    const auto& light = gpu.lights[0];

    CHECK(light.emission_area.x == doctest::Approx(3.0f));
    CHECK(light.emission_area.y == doctest::Approx(1.5f));
    CHECK(light.emission_area.z == doctest::Approx(0.6f));

    CHECK(light.emission_area.w == doctest::Approx(2.0f).epsilon(0.001f));

    CHECK(light.ids.x == 0);
    CHECK(light.ids.y == 0);
    CHECK(light.ids.z == 0);
    CHECK(light.ids.w == 0xFFFFFFFFu);
}

TEST_CASE("BuildGPUSceneDataFromECS light ids map to correct instanceID and primitiveID")
{
    ECSScene ecsScene;

    SceneMaterial mat0;
    ecsScene.materials.push_back(mat0);

    SceneMaterial mat1;
    mat1.emissiveColor = {1.0f, 1.0f, 1.0f};
    mat1.emissiveIntensity = 2.0f;
    ecsScene.materials.push_back(mat1);

    MeshData meshData0;
    meshData0.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    meshData0.indices = {0, 1, 2};
    uint32_t meshIdx0 = ecsScene.meshRegistry.AddMesh(std::move(meshData0));

    MeshData meshData1;
    meshData1.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0,
                          1, 0, 0, 1, 1, 0, 0, 1, 0,
                          0, 0, 0, 1, 0, 0, 1, 1, 0};
    meshData1.indices = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    uint32_t meshIdx1 = ecsScene.meshRegistry.AddMesh(std::move(meshData1));

    {
        auto entity = ecsScene.registry.create();
        ecsScene.registry.emplace<Transform>(entity);
        auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
        ref.meshIndex = meshIdx0;
        ref.materialIndex = 0;
    }
    {
        auto entity = ecsScene.registry.create();
        ecsScene.registry.emplace<Transform>(entity);
        auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
        ref.meshIndex = meshIdx1;
        ref.materialIndex = 1;
    }

    SceneGraph::UpdateWorldTransforms(ecsScene.registry);

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    CHECK(gpu.lights.size() == 3);
    CHECK(gpu.meshes.size() == 2);
    CHECK(gpu.instances.size() == 2);

    uint32_t emissiveInstIdx = 0;
    for (uint32_t i = 0; i < gpu.instances.size(); i++)
    {
        if (gpu.instances[i].materialIndex == 1)
        {
            emissiveInstIdx = i;
            break;
        }
    }

    for (uint32_t i = 0; i < gpu.lights.size(); i++)
    {
        CHECK(gpu.lights[i].ids.x == emissiveInstIdx);
        CHECK(gpu.lights[i].ids.z == 1);
        CHECK(gpu.lights[i].ids.y == i);
    }
}

TEST_CASE("BuildGPUSceneDataFromECS totalLightArea sums all light areas")
{
    ECSScene ecsScene;

    SceneMaterial mat;
    mat.emissiveColor = {1.0f, 1.0f, 1.0f};
    mat.emissiveIntensity = 1.0f;
    ecsScene.materials.push_back(mat);

    MeshData meshData0;
    meshData0.vertices = {0, 0, 0, 2, 0, 0, 0, 2, 0};
    meshData0.indices = {0, 1, 2};
    uint32_t meshIdx0 = ecsScene.meshRegistry.AddMesh(std::move(meshData0));

    MeshData meshData1;
    meshData1.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    meshData1.indices = {0, 1, 2};
    uint32_t meshIdx1 = ecsScene.meshRegistry.AddMesh(std::move(meshData1));

    {
        auto entity = ecsScene.registry.create();
        ecsScene.registry.emplace<Transform>(entity);
        auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
        ref.meshIndex = meshIdx0;
        ref.materialIndex = 0;
    }
    {
        auto entity = ecsScene.registry.create();
        ecsScene.registry.emplace<Transform>(entity);
        auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
        ref.meshIndex = meshIdx1;
        ref.materialIndex = 0;
    }

    SceneGraph::UpdateWorldTransforms(ecsScene.registry);

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    CHECK(gpu.lights.size() == 2);
    CHECK(gpu.totalLightArea == doctest::Approx(2.5f).epsilon(0.001f));
}

TEST_CASE("BuildGPUSceneDataFromECS light stores emissiveTexIdx when material has emissive texture")
{
    ECSScene ecsScene;

    SceneMaterial mat;
    mat.emissiveColor = {1.0f, 1.0f, 1.0f};
    mat.emissiveIntensity = 2.0f;
    mat.emissiveTextureIndex = 5;
    ecsScene.materials.push_back(mat);

    MeshData meshData;
    meshData.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    meshData.indices = {0, 1, 2};
    uint32_t meshIdx = ecsScene.meshRegistry.AddMesh(std::move(meshData));

    auto entity = ecsScene.registry.create();
    ecsScene.registry.emplace<Transform>(entity);
    auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
    ref.meshIndex = meshIdx;
    ref.materialIndex = 0;

    SceneGraph::UpdateWorldTransforms(ecsScene.registry);

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    REQUIRE(gpu.lights.size() == 1);
    CHECK(gpu.lights[0].ids.w == 5u);
}

// ============================================================================
// Alpha mode / translucency tests (M6 Phase 1)
// ============================================================================

TEST_CASE("GPUMaterial alphaMode defaults to OPAQUE (0.0)")
{
    GPUMaterial mat;
    CHECK(mat.alphaMode == 0.0f);
    CHECK(mat.alphaCutoff == 0.5f);
}

TEST_CASE("GPUMaterial fromSceneMaterial converts alphaMode MASK")
{
    SceneMaterial sm;
    sm.alphaMode = "MASK";
    sm.alphaCutoff = 0.3f;

    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);

    CHECK(gm.alphaMode == 1.0f);
    CHECK(gm.alphaCutoff == 0.3f);
}

TEST_CASE("GPUMaterial fromSceneMaterial converts alphaMode BLEND")
{
    SceneMaterial sm;
    sm.alphaMode = "BLEND";

    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);

    CHECK(gm.alphaMode == 2.0f);
}

TEST_CASE("GPUMaterial fromSceneMaterial converts alphaMode OPAQUE")
{
    SceneMaterial sm;
    sm.alphaMode = "OPAQUE";

    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);

    CHECK(gm.alphaMode == 0.0f);
}

TEST_CASE("GPUMaterial is 80 bytes with extraIndices for metallicRoughness")
{
    CHECK(sizeof(GPUMaterial) == 80);
}

// ============================================================================
// M7: Dielectric transmission — KHR_materials_transmission import tests
// ============================================================================

TEST_CASE("Transmission material stores transmissionFactor (not in baseAlpha)")
{
    // M7 fix: transmissionFactor is separate from baseAlpha (opacity).
    SceneMaterial sm;
    sm.alphaMode = "OPAQUE";
    sm.transmissionFactor = 0.85f;
    sm.baseAlpha = 1.0f;  // fully opaque (no alpha transparency)
    sm.ior = 1.5f;

    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);

    CHECK(gm.alphaMode == 0.0f);       // OPAQUE
    CHECK(gm.baseAlpha == 1.0f);      // opacity unchanged
    CHECK(gm.ior == 1.5f);
    // transmissionFactor packed into textureIndices.w
    CHECK(glm::intBitsToFloat(gm.textureIndices.w) == 0.85f);
}

TEST_CASE("Transmission material with transmissionFactor=1 is fully transmissive")
{
    SceneMaterial sm;
    sm.transmissionFactor = 1.0f;  // 100% transmission
    sm.baseAlpha = 1.0f;           // still opaque for alpha purposes
    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);
    CHECK(glm::intBitsToFloat(gm.textureIndices.w) == 1.0f);
    CHECK(gm.baseAlpha == 1.0f);  // opacity unchanged
    CHECK(gm.alphaMode == 0.0f);  // OPAQUE
}

TEST_CASE("Opaque material with transmissionFactor=0 is not transmissive")
{
    SceneMaterial sm;
    sm.transmissionFactor = 0.0f;
    sm.baseAlpha = 1.0f;
    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);
    CHECK(glm::intBitsToFloat(gm.textureIndices.w) == 0.0f);
    CHECK(gm.baseAlpha == 1.0f);
    CHECK(gm.alphaMode == 0.0f);
}

TEST_CASE("Alpha-blended material does NOT trigger transmission")
{
    // A BLEND alpha material (e.g. bottle) should NOT enter the refraction branch.
    // baseAlpha < 1 here means partial opacity, NOT physical transmission.
    SceneMaterial sm;
    sm.alphaMode = "BLEND";
    sm.baseAlpha = 0.5f;        // 50% transparent (alpha opacity)
    sm.transmissionFactor = 0.0f;  // NOT physical glass
    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);
    CHECK(gm.alphaMode == 2.0f);  // BLEND
    CHECK(gm.baseAlpha == 0.5f); // opacity
    CHECK(glm::intBitsToFloat(gm.textureIndices.w) == 0.0f);  // no transmission
}

// ============================================================================
// Punctual lights must follow the transform-only sync path.
//
// A light is an entity with a Transform and no MeshRef, so it never appears in
// the renderable loop that UpdateInstancesFromECS iterates. Its position and
// aim come straight from the world matrix -- exactly what a transform edit
// changes -- so if this path does not rebuild them, dragging a light in the
// editor leaves it casting from where it used to be until some unrelated full
// sync happens to run.
// ============================================================================
TEST_CASE("UpdateInstancesFromECS moves punctual lights with their transform")
{
    ECSScene scene;
    scene.materials.push_back(SceneMaterial{});

    // One renderable, so there are instances to update at all.
    {
        MeshData mesh;
        mesh.vertices = {0,0,0, 1,0,0, 0,1,0};
        mesh.indices = {0, 1, 2};
        const uint32_t meshIdx = scene.meshRegistry.AddMesh(std::move(mesh));
        const auto e = scene.registry.create();
        scene.registry.emplace<Transform>(e);
        scene.registry.emplace<MeshRef>(e, meshIdx, 0);
        scene.registry.emplace<VisibleComponent>(e);
    }

    const auto lightEntity = scene.registry.create();
    {
        Transform tf;
        tf.translation = {1.0f, 2.0f, 3.0f};
        scene.registry.emplace<Transform>(lightEntity, tf);
        LightComponent light;
        light.type = LightType::Point;
        light.intensity = 50.0f;
        scene.registry.emplace<LightComponent>(lightEntity, light);
        scene.registry.emplace<VisibleComponent>(lightEntity);
    }

    SceneGraph::UpdateWorldTransforms(scene.registry);
    GPUSceneData gpu = BuildGPUSceneDataFromECS(scene, nullptr);
    REQUIRE(gpu.punctualLights.size() == 1);
    CHECK(gpu.punctualLights[0].position_range.x == doctest::Approx(1.0f));
    CHECK(gpu.punctualLights[0].position_range.z == doctest::Approx(3.0f));

    // Move the light, then run only the transform-sync path.
    scene.registry.get<Transform>(lightEntity).translation = {-4.0f, 5.0f, 6.0f};
    SceneGraph::MarkDirty(scene.registry, lightEntity);
    SceneGraph::UpdateWorldTransforms(scene.registry);
    UpdateInstancesFromECS(gpu, scene, nullptr);

    REQUIRE(gpu.punctualLights.size() == 1);
    CHECK(gpu.punctualLights[0].position_range.x == doctest::Approx(-4.0f));
    CHECK(gpu.punctualLights[0].position_range.y == doctest::Approx(5.0f));
    CHECK(gpu.punctualLights[0].position_range.z == doctest::Approx(6.0f));
}
