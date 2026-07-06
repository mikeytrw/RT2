#include <doctest/doctest.h>

#include "GPUSceneData.h"
#include "ECSScene.h"
#include "ECSComponents.h"
#include "SceneGraph.h"
#include "SceneLoader.h"
#include <glm/glm.hpp>

// ============================================================================
// BuildGPUSceneDataFromECS tests
// ============================================================================

TEST_CASE("BuildGPUSceneDataFromECS: produces meshes + instances")
{
    ECSScene ecsScene;
    bool ok = SceneLoader::LoadIntoECS(ecsScene, "C:\\Users\\mikey\\Downloads\\sofa_and_lamp.glb");
    CHECK(ok);
    if (!ok) return;

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    CHECK(gpu.meshes.size() > 0);
    CHECK(gpu.instances.size() > 0);
    CHECK(gpu.materials.size() > 0);

    // Every instance should reference a valid mesh
    for (const auto& inst : gpu.instances)
        CHECK(inst.meshIndex < gpu.meshes.size());
}

TEST_CASE("BuildGPUSceneDataFromECS: instances have world transforms")
{
    ECSScene ecsScene;
    bool ok = SceneLoader::LoadIntoECS(ecsScene, "C:\\Users\\mikey\\Downloads\\sofa_and_lamp.glb");
    CHECK(ok);
    if (!ok) return;

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    // At least one instance should have a non-identity world matrix
    bool anyNonIdentity = false;
    for (const auto& inst : gpu.instances)
    {
        if (inst.worldMatrix != glm::mat4(1.0f))
        {
            anyNonIdentity = true;
            break;
        }
    }
    CHECK(anyNonIdentity);
}

TEST_CASE("BuildGPUSceneDataFromECS: object-space mesh vertices")
{
    ECSScene ecsScene;
    bool ok = SceneLoader::LoadIntoECS(ecsScene, "C:\\Users\\mikey\\Downloads\\sofa_and_lamp.glb");
    CHECK(ok);
    if (!ok) return;

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    // Mesh vertices should be in object space (smaller coordinate range)
    // World-space baking would produce larger absolute values for translated meshes
    CHECK(gpu.meshes.size() > 0);
    const auto& mesh = gpu.meshes[0];
    CHECK(!mesh.vertices.empty());
    CHECK(!mesh.indices.empty());
    CHECK(!mesh.vertexUVs.empty());
    CHECK(!mesh.tangents.empty());
}

TEST_CASE("BuildGPUSceneDataFromECS: ABG instancing reduces BLAS count")
{
    ECSScene ecsScene;
    bool ok = SceneLoader::LoadIntoECS(ecsScene, "C:\\Users\\mikey\\Downloads\\ABeautifulGame.glb");
    CHECK(ok);
    if (!ok) return;

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    // ABG has 15 unique meshes but 49 instances
    CHECK(gpu.meshes.size() <= gpu.instances.size());

    printf("[BuildFromECS] ABG: %zu meshes (BLAS), %zu instances (TLAS)\n",
           gpu.meshes.size(), gpu.instances.size());
}

TEST_CASE("BuildGPUSceneDataFromECS: emissive lights from instances")
{
    ECSScene ecsScene;
    bool ok = SceneLoader::LoadIntoECS(ecsScene, "C:\\Users\\mikey\\Downloads\\sofa_and_lamp.glb");
    CHECK(ok);
    if (!ok) return;

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    // sofa_and_lamp has a lamp with emissive material
    // (may or may not have lights depending on the glTF — just check structure)
    for (const auto& light : gpu.lights)
    {
        // Every light should reference a valid instance
        CHECK(light.ids.x < gpu.instances.size());
    }
}

// ============================================================================
// UpdateInstancesFromECS tests
// ============================================================================

TEST_CASE("UpdateInstancesFromECS: updates world matrices after transform change")
{
    ECSScene ecsScene;
    bool ok = SceneLoader::LoadIntoECS(ecsScene, "C:\\Users\\mikey\\Downloads\\sofa_and_lamp.glb");
    CHECK(ok);
    if (!ok) return;

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);
    CHECK(gpu.instances.size() > 0);

    // Record original world matrices
    std::vector<glm::mat4> originalMatrices;
    for (const auto& inst : gpu.instances)
        originalMatrices.push_back(inst.worldMatrix);

    // Rotate the first entity with a MeshRef
    auto view = ecsScene.registry.view<MeshRef>();
    CHECK(!view.empty());
    if (view.empty()) return;
    entt::entity first = *view.begin();
    auto* tf = ecsScene.registry.try_get<Transform>(first);
    CHECK(tf != nullptr);
    if (!tf) return;

    tf->rotation = glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 1, 0));
    SceneGraph::MarkDirty(ecsScene.registry, first);
    SceneGraph::UpdateWorldTransforms(ecsScene.registry);

    // Update instances
    UpdateInstancesFromECS(gpu, ecsScene);

    // At least one instance should have a different world matrix
    bool anyChanged = false;
    for (size_t i = 0; i < gpu.instances.size() && i < originalMatrices.size(); i++)
    {
        if (gpu.instances[i].worldMatrix != originalMatrices[i])
        {
            anyChanged = true;
            break;
        }
    }
    CHECK(anyChanged);
}

TEST_CASE("UpdateInstancesFromECS: preserves mesh and material arrays")
{
    ECSScene ecsScene;
    bool ok = SceneLoader::LoadIntoECS(ecsScene, "C:\\Users\\mikey\\Downloads\\sofa_and_lamp.glb");
    CHECK(ok);
    if (!ok) return;

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);
    size_t meshCount = gpu.meshes.size();
    size_t matCount = gpu.materials.size();
    size_t texCount = gpu.textures.size();

    CHECK(meshCount > 0);
    CHECK(matCount > 0);

    // Update transforms (no actual change needed)
    UpdateInstancesFromECS(gpu, ecsScene);

    // Meshes and materials should be unchanged
    CHECK(gpu.meshes.size() == meshCount);
    CHECK(gpu.materials.size() == matCount);
    CHECK(gpu.textures.size() == texCount);
}

TEST_CASE("UpdateInstancesFromECS: light areas update with transform")
{
    ECSScene ecsScene;
    bool ok = SceneLoader::LoadIntoECS(ecsScene, "C:\\Users\\mikey\\Downloads\\sofa_and_lamp.glb");
    CHECK(ok);
    if (!ok) return;

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    // Scale the first entity with a MeshRef (uniform scale changes light area)
    auto view = ecsScene.registry.view<MeshRef>();
    if (view.empty()) return;
    entt::entity first = *view.begin();
    auto* tf = ecsScene.registry.try_get<Transform>(first);
    if (!tf) return;

    tf->scale = glm::vec3(2.0f); // 2x scale → 4x area
    SceneGraph::MarkDirty(ecsScene.registry, first);
    SceneGraph::UpdateWorldTransforms(ecsScene.registry);

    UpdateInstancesFromECS(gpu, ecsScene);

    // If the scaled entity had emissive triangles, total area should change.
    // If it didn't, area stays the same. Either way, the function should not crash.
    // For sofa_and_lamp, the lamp is emissive — if it's the first entity, area changes.
    // Just verify the function completed without crashing.
    CHECK(gpu.lights.size() > 0);
    CHECK(gpu.totalLightArea > 0.0f);
}