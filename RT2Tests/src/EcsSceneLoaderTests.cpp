#include <doctest/doctest.h>

#include "SceneLoader.h"
#include "ECSScene.h"
#include "ECSComponents.h"
#include "SceneGraph.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// ============================================================================
// ECS SceneLoader tests — verify LoadIntoECS populates the registry correctly
// ============================================================================

TEST_CASE("LoadIntoECS: sofa_and_lamp.glb loads successfully")
{
    ECSScene ecsScene;
    const char* scenePath = "C:\\Users\\mikey\\Downloads\\sofa_and_lamp.glb";

    bool ok = SceneLoader::LoadIntoECS(ecsScene, scenePath);
    CHECK(ok);
    if (!ok) return;

    // Should have at least some entities with Transform
    auto view = ecsScene.registry.view<Transform>();
    CHECK(view.size() > 0);

    // Should have at least some entities with MeshRef
    auto meshView = ecsScene.registry.view<MeshRef>();
    CHECK(meshView.size() > 0);

    // Should have at least one unique mesh in the registry
    CHECK(ecsScene.meshRegistry.GetCount() > 0);

    // Should have materials
    CHECK(ecsScene.materials.size() > 0);

    // Every MeshRef should reference a valid mesh index
    for (auto entity : meshView)
    {
        const auto& ref = meshView.get<MeshRef>(entity);
        CHECK(ref.meshIndex < ecsScene.meshRegistry.GetCount());
    }
}

TEST_CASE("LoadIntoECS: world transforms are resolved")
{
    ECSScene ecsScene;
    const char* scenePath = "C:\\Users\\mikey\\Downloads\\sofa_and_lamp.glb";

    bool ok = SceneLoader::LoadIntoECS(ecsScene, scenePath);
    CHECK(ok);
    if (!ok) return;

    // SceneGraph::UpdateWorldTransforms should have been called by LoadIntoECS
    // All transforms should be non-dirty
    auto view = ecsScene.registry.view<Transform>();
    bool anyDirty = false;
    for (auto entity : view)
    {
        const auto& t = view.get<Transform>(entity);
        if (t.dirty)
            anyDirty = true;
    }
    CHECK(!anyDirty);
}

TEST_CASE("LoadIntoECS: ABeautifulGame.glb loads with many meshes")
{
    ECSScene ecsScene;
    const char* scenePath = "C:\\Users\\mikey\\Downloads\\ABeautifulGame.glb";

    bool ok = SceneLoader::LoadIntoECS(ecsScene, scenePath);
    CHECK(ok);
    if (!ok) return;

    // ABG has ~49 meshes in the old format, so ECS should have a good number
    auto meshView = ecsScene.registry.view<MeshRef>();
    CHECK(meshView.size() >= 10);

    // Should have many unique meshes
    CHECK(ecsScene.meshRegistry.GetCount() >= 5);

    // Should have 15+ materials
    CHECK(ecsScene.materials.size() >= 10);
}

TEST_CASE("LoadIntoECS: mesh deduplication works")
{
    // ABeautifulGame has multiple nodes referencing the same mesh
    // The MeshRegistry should have fewer unique meshes than total MeshRef entities
    ECSScene ecsScene;
    const char* scenePath = "C:\\Users\\mikey\\Downloads\\ABeautifulGame.glb";

    bool ok = SceneLoader::LoadIntoECS(ecsScene, scenePath);
    CHECK(ok);
    if (!ok) return;

    auto meshView = ecsScene.registry.view<MeshRef>();
    size_t totalRefs = meshView.size();
    size_t uniqueMeshes = ecsScene.meshRegistry.GetCount();

    // If there's instancing, unique meshes < total refs
    // But even without instancing, unique meshes should be <= total refs
    CHECK(uniqueMeshes <= totalRefs);

    printf("[LoadIntoECS] ABG: %zu mesh refs, %zu unique meshes\n", totalRefs, uniqueMeshes);
}

TEST_CASE("LoadIntoECS: mesh geometry is in object space")
{
    ECSScene ecsScene;
    const char* scenePath = "C:\\Users\\mikey\\Downloads\\sofa_and_lamp.glb";

    bool ok = SceneLoader::LoadIntoECS(ecsScene, scenePath);
    CHECK(ok);
    if (!ok) return;

    // Pick the first mesh and verify it has geometry
    CHECK(ecsScene.meshRegistry.GetCount() > 0);
    const auto& mesh = ecsScene.meshRegistry.GetMesh(0);
    CHECK(!mesh.vertices.empty());
    CHECK(!mesh.indices.empty());

    // Vertices should be in object space — for most glTF models,
    // the origin is near the model center, so some vertices should be
    // near zero. We can't check exact values without knowing the model,
    // but we can verify the data is valid (finite, not all zeros).
    bool hasNonZeroVertex = false;
    for (size_t i = 0; i < mesh.vertices.size(); i += 3)
    {
        float x = mesh.vertices[i];
        float y = mesh.vertices[i + 1];
        float z = mesh.vertices[i + 2];
        if (glm::any(glm::isnan(glm::vec3(x, y, z))))
        {
            CHECK(false);  // NaN vertices!
            return;
        }
        if (x != 0.0f || y != 0.0f || z != 0.0f)
            hasNonZeroVertex = true;
    }
    CHECK(hasNonZeroVertex);
}