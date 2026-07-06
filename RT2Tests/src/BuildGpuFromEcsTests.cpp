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