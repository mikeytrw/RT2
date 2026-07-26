#include <doctest/doctest.h>

#include "SceneTypes.h"
#include "ECSScene.h"
#include "ECSComponents.h"
#include "SceneLoader.h"
#include "SceneLoaderTestSupport.h"

#include <glm/glm.hpp>
#include <filesystem>
#include <iterator>

namespace fs = std::filesystem;

static const char* ROUNDTRIP_GLB = "test_roundtrip_geometry.glb";

static void cleanup()
{
    fs::remove(ROUNDTRIP_GLB);
    fs::remove("test_roundtrip_geometry.bin");
    for (auto& entry : fs::directory_iterator("."))
    {
        std::string name = entry.path().filename().string();
        if (name.find("test_roundtrip") == 0)
            fs::remove(entry.path());
    }
}

// ============================================================================
// glTF Geometry Save/Load Round-Trip Tests (RED phase)
// ============================================================================
// These test that Scene data with inline geometry can be saved to a .glb file
// and loaded back with all geometry preserved.
// ============================================================================

TEST_CASE("Geometry round-trips through glTF save/load")
{
    cleanup();
    ECSScene scene;

    MeshData meshData;
    meshData.vertices = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    meshData.indices = {0, 1, 2};
    uint32_t meshIdx = scene.meshRegistry.AddMesh(std::move(meshData));

    auto entity = scene.registry.create();
    auto& tf = scene.registry.emplace<Transform>(entity);
    tf.translation = {1.0f, 2.0f, 3.0f};
    tf.scale = {2.0f, 2.0f, 2.0f};
    auto& ref = scene.registry.emplace<MeshRef>(entity);
    ref.meshIndex = meshIdx;
    ref.materialIndex = 0;

    scene.materials.push_back(SceneMaterial{});

    REQUIRE(SceneLoader::Save(scene, ROUNDTRIP_GLB));
    ECSScene loaded;
    REQUIRE(LoadGltfForTest(loaded, RepositoryRootForSceneLoaderTests(), ROUNDTRIP_GLB));

    CHECK(loaded.meshRegistry.GetCount() >= 1);
    const auto& m = loaded.meshRegistry.GetMesh(0);
    REQUIRE(m.vertices.size() == 9);
    CHECK(m.vertices[0] == doctest::Approx(0.0f));
    CHECK(m.vertices[3] == doctest::Approx(1.0f));
    CHECK(m.vertices[6] == doctest::Approx(0.0f));
    CHECK(m.vertices[7] == doctest::Approx(1.0f));

    REQUIRE(m.indices.size() == 3);
    CHECK(m.indices[0] == 0);
    CHECK(m.indices[1] == 1);
    CHECK(m.indices[2] == 2);

    auto meshView = loaded.registry.view<MeshRef, Transform>();
    size_t count = std::distance(meshView.begin(), meshView.end());
    REQUIRE(count >= 1);
    auto loadedEntity = *meshView.begin();
    const auto& loadedTf = meshView.get<Transform>(loadedEntity);
    CHECK(loadedTf.translation.x == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(loadedTf.translation.y == doctest::Approx(2.0f).epsilon(0.001));
    CHECK(loadedTf.translation.z == doctest::Approx(3.0f).epsilon(0.001));
    CHECK(loadedTf.scale.x == doctest::Approx(2.0f).epsilon(0.001));

    cleanup();
}

TEST_CASE("Geometry with normals round-trips through glTF save/load")
{
    cleanup();
    ECSScene scene;

    MeshData meshData;
    meshData.vertices = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    meshData.normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    meshData.indices = {0, 1, 2};
    uint32_t meshIdx = scene.meshRegistry.AddMesh(std::move(meshData));

    auto entity = scene.registry.create();
    scene.registry.emplace<Transform>(entity);
    auto& ref = scene.registry.emplace<MeshRef>(entity);
    ref.meshIndex = meshIdx;
    ref.materialIndex = 0;

    scene.materials.push_back(SceneMaterial{});

    REQUIRE(SceneLoader::Save(scene, ROUNDTRIP_GLB));
    ECSScene loaded;
    REQUIRE(LoadGltfForTest(loaded, RepositoryRootForSceneLoaderTests(), ROUNDTRIP_GLB));

    CHECK(loaded.meshRegistry.GetCount() >= 1);
    const auto& m = loaded.meshRegistry.GetMesh(0);
    REQUIRE(m.normals.size() == 9);
    CHECK(m.normals[2] == doctest::Approx(1.0f));
    CHECK(m.normals[5] == doctest::Approx(1.0f));
    CHECK(m.normals[8] == doctest::Approx(1.0f));

    cleanup();
}

TEST_CASE("Geometry with UVs round-trips through glTF save/load")
{
    cleanup();
    ECSScene scene;

    MeshData meshData;
    meshData.vertices = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    meshData.uvs = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    meshData.indices = {0, 1, 2};
    uint32_t meshIdx = scene.meshRegistry.AddMesh(std::move(meshData));

    auto entity = scene.registry.create();
    scene.registry.emplace<Transform>(entity);
    auto& ref = scene.registry.emplace<MeshRef>(entity);
    ref.meshIndex = meshIdx;
    ref.materialIndex = 0;

    scene.materials.push_back(SceneMaterial{});

    REQUIRE(SceneLoader::Save(scene, ROUNDTRIP_GLB));
    ECSScene loaded;
    REQUIRE(LoadGltfForTest(loaded, RepositoryRootForSceneLoaderTests(), ROUNDTRIP_GLB));

    const auto& m = loaded.meshRegistry.GetMesh(0);
    REQUIRE(m.uvs.size() == 6);
    CHECK(m.uvs[0] == doctest::Approx(0.0f));
    CHECK(m.uvs[2] == doctest::Approx(1.0f));
    CHECK(m.uvs[5] == doctest::Approx(1.0f));

    cleanup();
}

TEST_CASE("Multiple geometry meshes round-trip through glTF save/load")
{
    cleanup();
    ECSScene scene;

    MeshData meshData0;
    meshData0.vertices = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    meshData0.indices = {0, 1, 2};
    uint32_t meshIdx0 = scene.meshRegistry.AddMesh(std::move(meshData0));

    MeshData meshData1;
    meshData1.vertices = {5.0f, 0.0f, 0.0f, 6.0f, 0.0f, 0.0f, 5.0f, 1.0f, 0.0f};
    meshData1.indices = {0, 1, 2};
    uint32_t meshIdx1 = scene.meshRegistry.AddMesh(std::move(meshData1));

    {
        auto entity = scene.registry.create();
        auto& tf = scene.registry.emplace<Transform>(entity);
        tf.translation = {0.0f, 0.0f, 0.0f};
        auto& ref = scene.registry.emplace<MeshRef>(entity);
        ref.meshIndex = meshIdx0;
        ref.materialIndex = 0;
    }
    {
        auto entity = scene.registry.create();
        auto& tf = scene.registry.emplace<Transform>(entity);
        tf.translation = {10.0f, 0.0f, 0.0f};
        auto& ref = scene.registry.emplace<MeshRef>(entity);
        ref.meshIndex = meshIdx1;
        ref.materialIndex = 0;
    }

    scene.materials.push_back(SceneMaterial{});

    REQUIRE(SceneLoader::Save(scene, ROUNDTRIP_GLB));
    ECSScene loaded;
    REQUIRE(LoadGltfForTest(loaded, RepositoryRootForSceneLoaderTests(), ROUNDTRIP_GLB));

    CHECK(loaded.meshRegistry.GetCount() >= 1);
    bool foundFirstVertex0 = false;
    bool foundFirstVertex5 = false;
    for (uint32_t i = 0; i < loaded.meshRegistry.GetCount(); i++)
    {
        const auto& m = loaded.meshRegistry.GetMesh(i);
        if (!m.vertices.empty())
        {
            if (m.vertices[0] == doctest::Approx(0.0f)) foundFirstVertex0 = true;
            if (m.vertices[0] == doctest::Approx(5.0f)) foundFirstVertex5 = true;
        }
    }
    CHECK(foundFirstVertex0);
    CHECK(foundFirstVertex5);

    auto meshView = loaded.registry.view<MeshRef, Transform>();
    bool foundPos10 = false;
    for (auto e : meshView)
    {
        const auto& tf = meshView.get<Transform>(e);
        if (tf.translation.x == doctest::Approx(10.0f).epsilon(0.001))
            foundPos10 = true;
    }
    CHECK(foundPos10);

    cleanup();
}

TEST_CASE("Geometry with material assignment round-trips")
{
    cleanup();
    ECSScene scene;

    SceneMaterial mat;
    mat.baseColor = {0.2f, 0.4f, 0.6f};
    scene.materials.push_back(mat);

    MeshData meshData;
    meshData.vertices = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    meshData.indices = {0, 1, 2};
    uint32_t meshIdx = scene.meshRegistry.AddMesh(std::move(meshData));

    auto entity = scene.registry.create();
    scene.registry.emplace<Transform>(entity);
    auto& ref = scene.registry.emplace<MeshRef>(entity);
    ref.meshIndex = meshIdx;
    ref.materialIndex = 0;

    REQUIRE(SceneLoader::Save(scene, ROUNDTRIP_GLB));
    ECSScene loaded;
    REQUIRE(LoadGltfForTest(loaded, RepositoryRootForSceneLoaderTests(), ROUNDTRIP_GLB));

    REQUIRE(loaded.materials.size() >= 1);
    CHECK(loaded.materials[0].baseColor == glm::vec3(0.2f, 0.4f, 0.6f));
    auto meshView = loaded.registry.view<MeshRef>();
    REQUIRE(meshView.size() >= 1);
    auto loadedEntity = *meshView.begin();
    CHECK(meshView.get<MeshRef>(loadedEntity).materialIndex == 0);

    cleanup();
}

TEST_CASE("Multiple inline geometry meshes round-trip")
{
    cleanup();
    ECSScene scene;

    MeshData meshData0;
    meshData0.vertices = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    meshData0.indices = {0, 1, 2};
    uint32_t meshIdx0 = scene.meshRegistry.AddMesh(std::move(meshData0));

    MeshData meshData1;
    meshData1.vertices = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    meshData1.indices = {0, 1, 2};
    uint32_t meshIdx1 = scene.meshRegistry.AddMesh(std::move(meshData1));

    {
        auto entity = scene.registry.create();
        scene.registry.emplace<Transform>(entity);
        auto& ref = scene.registry.emplace<MeshRef>(entity);
        ref.meshIndex = meshIdx0;
        ref.materialIndex = 0;
    }
    {
        auto entity = scene.registry.create();
        auto& tf = scene.registry.emplace<Transform>(entity);
        tf.translation = {5.0f, 0.0f, 0.0f};
        tf.scale = {0.01f, 0.01f, 0.01f};
        auto& ref = scene.registry.emplace<MeshRef>(entity);
        ref.meshIndex = meshIdx1;
        ref.materialIndex = 0;
    }

    scene.materials.push_back(SceneMaterial{});

    REQUIRE(SceneLoader::Save(scene, ROUNDTRIP_GLB));
    ECSScene loaded;
    REQUIRE(LoadGltfForTest(loaded, RepositoryRootForSceneLoaderTests(), ROUNDTRIP_GLB));

    auto meshView = loaded.registry.view<MeshRef, Transform>();
    size_t count = std::distance(meshView.begin(), meshView.end());
    REQUIRE(count >= 2);

    bool foundPos0 = false;
    bool foundPos5 = false;
    for (auto e : meshView)
    {
        const auto& tf = meshView.get<Transform>(e);
        if (tf.translation.x == doctest::Approx(0.0f).epsilon(0.001))
            foundPos0 = true;
        if (tf.translation.x == doctest::Approx(5.0f).epsilon(0.001))
            foundPos5 = true;
    }
    CHECK(foundPos0);
    CHECK(foundPos5);

    cleanup();
}
