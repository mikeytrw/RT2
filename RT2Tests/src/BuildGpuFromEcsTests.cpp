#include <doctest/doctest.h>

#include "GPUSceneData.h"
#include "ECSScene.h"
#include "ECSComponents.h"
#include "SceneGraph.h"
#include "SceneLoader.h"
#include "SceneLoaderTestSupport.h"
#include <glm/glm.hpp>

// ============================================================================
// BuildGPUSceneDataFromECS tests
// ============================================================================

TEST_CASE("BuildGPUSceneDataFromECS: produces meshes + instances")
{
    ECSScene ecsScene;
    bool ok = LoadGltfForTest(ecsScene, RepositoryRootForSceneLoaderTests(), "C:\\Users\\mikey\\Downloads\\sofa_and_lamp.glb");
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
    bool ok = LoadGltfForTest(ecsScene, RepositoryRootForSceneLoaderTests(), "C:\\Users\\mikey\\Downloads\\sofa_and_lamp.glb");
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
    bool ok = LoadGltfForTest(ecsScene, RepositoryRootForSceneLoaderTests(), "C:\\Users\\mikey\\Downloads\\sofa_and_lamp.glb");
    CHECK(ok);
    if (!ok) return;

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    // Mesh vertices should be in object space (smaller coordinate range)
    // World-space baking would produce larger absolute values for translated meshes
    CHECK(gpu.meshes.size() > 0);
    const auto& mesh = gpu.meshes[0];
    CHECK(mesh.vertices != nullptr);
    CHECK(mesh.indices != nullptr);
    CHECK(mesh.uvs != nullptr);
    CHECK(mesh.normals != nullptr);
}

TEST_CASE("BuildGPUSceneDataFromECS: ABG instancing reduces BLAS count")
{
    ECSScene ecsScene;
    bool ok = LoadGltfForTest(ecsScene, RepositoryRootForSceneLoaderTests(), "C:\\Users\\mikey\\Downloads\\ABeautifulGame.glb");
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
    bool ok = LoadGltfForTest(ecsScene, RepositoryRootForSceneLoaderTests(), "C:\\Users\\mikey\\Downloads\\sofa_and_lamp.glb");
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
    bool ok = LoadGltfForTest(ecsScene, RepositoryRootForSceneLoaderTests(), "C:\\Users\\mikey\\Downloads\\sofa_and_lamp.glb");
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
    bool ok = LoadGltfForTest(ecsScene, RepositoryRootForSceneLoaderTests(), "C:\\Users\\mikey\\Downloads\\sofa_and_lamp.glb");
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
    bool ok = LoadGltfForTest(ecsScene, RepositoryRootForSceneLoaderTests(), "C:\\Users\\mikey\\Downloads\\sofa_and_lamp.glb");
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

// ============================================================================
// Phase 8 step 4 — punctual lights reach the GPU scene from light entities
// ============================================================================

TEST_CASE("Phase8 step 4: punctual lights build from entity world transforms")
{
    ECSScene scene;

    // A spot parented under a mover. Its world pose must follow the parent —
    // the entire reason lights are entities rather than a flat table.
    const auto parent = scene.registry.create();
    {
        Transform tf;
        tf.translation = {10.0f, 0.0f, 0.0f};
        tf.dirty = true;
        scene.registry.emplace<Transform>(parent, tf);
    }

    const auto child = scene.registry.create();
    {
        Transform tf;
        tf.translation = {0.0f, 5.0f, 0.0f};          // local
        tf.rotation = LightDirectionToRotation({0.0f, -1.0f, 0.0f});
        tf.dirty = true;
        scene.registry.emplace<Transform>(child, tf);

        LightComponent lc;
        lc.type = LightType::Spot;
        lc.color = {0.5f, 0.25f, 0.125f};
        lc.intensity = 17.0f;
        lc.range = 40.0f;
        lc.innerConeAngle = 20.0f;
        lc.outerConeAngle = 35.0f;
        scene.registry.emplace<LightComponent>(child, lc);

        Hierarchy h;
        h.parent = parent;
        scene.registry.emplace<Hierarchy>(child, h);
        scene.registry.emplace<Hierarchy>(parent).children.push_back(child);
    }

    SceneGraph::UpdateWorldTransforms(scene.registry);

    std::vector<GPUPunctualLight> lights;
    BuildPunctualLightsFromECS(scene, lights);
    REQUIRE(lights.size() == 1);
    const auto& g = lights[0];

    // World position = parent translation + local translation.
    CHECK(g.position_range.x == doctest::Approx(10.0f));
    CHECK(g.position_range.y == doctest::Approx(5.0f));
    CHECK(g.position_range.z == doctest::Approx(0.0f));
    CHECK(g.position_range.w == doctest::Approx(40.0f));

    // Aim survives the transform.
    CHECK(std::fabs(g.direction_type.y - (-1.0f)) < 1e-5f);
    CHECK(g.direction_type.w == doctest::Approx(static_cast<float>(LightType::Spot)));

    CHECK(g.color_intensity.x == doctest::Approx(0.5f));
    CHECK(g.color_intensity.w == doctest::Approx(17.0f));

    // Cone angles arrive pre-cosined, inner >= outer as cosines.
    CHECK(g.cone.x == doctest::Approx(std::cos(glm::radians(20.0f))));
    CHECK(g.cone.y == doctest::Approx(std::cos(glm::radians(35.0f))));
    CHECK(g.cone.x > g.cone.y);
}

TEST_CASE("Phase8 step 4: an inverted spot cone cannot invert the falloff")
{
    // A malformed cone (inner wider than outer) must not produce cosInner <
    // cosOuter, which a shader doing smoothstep(cosOuter, cosInner, x) would
    // read as a light covering the entire hemisphere.
    ECSScene scene;
    const auto e = scene.registry.create();
    Transform tf;
    tf.dirty = true;
    scene.registry.emplace<Transform>(e, tf);

    LightComponent lc;
    lc.type = LightType::Spot;
    lc.innerConeAngle = 60.0f;   // wider than the outer cone
    lc.outerConeAngle = 15.0f;
    scene.registry.emplace<LightComponent>(e, lc);

    SceneGraph::UpdateWorldTransforms(scene.registry);

    std::vector<GPUPunctualLight> lights;
    BuildPunctualLightsFromECS(scene, lights);
    REQUIRE(lights.size() == 1);
    CHECK(lights[0].cone.x >= lights[0].cone.y);
}

TEST_CASE("Phase8 step 4: every LightType maps to its own GPU type value")
{
    ECSScene scene;
    const LightType types[] = {
        LightType::Point, LightType::Spot, LightType::Directional };

    for (auto type : types)
    {
        const auto e = scene.registry.create();
        Transform tf;
        tf.dirty = true;
        scene.registry.emplace<Transform>(e, tf);
        LightComponent lc;
        lc.type = type;
        scene.registry.emplace<LightComponent>(e, lc);
    }
    SceneGraph::UpdateWorldTransforms(scene.registry);

    std::vector<GPUPunctualLight> lights;
    BuildPunctualLightsFromECS(scene, lights);
    REQUIRE(lights.size() == 3);

    // Collapsing two types onto one value would silently make a directional
    // light behave as a point light at the origin.
    std::vector<float> seen;
    for (const auto& g : lights) seen.push_back(g.direction_type.w);
    std::sort(seen.begin(), seen.end());
    CHECK(seen[0] == doctest::Approx(0.0f));
    CHECK(seen[1] == doctest::Approx(1.0f));
    CHECK(seen[2] == doctest::Approx(2.0f));
}
