#include <doctest/doctest.h>

#include "SceneTypes.h"
#include "ECSScene.h"
#include "ECSComponents.h"
#include "SceneLoader.h"
#include "SceneLoaderTestSupport.h"
#include "GltfBuilder.h"  // helper to construct test glTF files

#include <glm/glm.hpp>
#include <filesystem>
#include <iterator>

namespace fs = std::filesystem;

static const char* TEST_GLB = "test_geometry.glb";
static const char* TEST_GLTF = "test_geometry.gltf";

static void cleanup()
{
    fs::remove(TEST_GLB);
    fs::remove(TEST_GLTF);
    fs::remove("test_geometry.bin");
    for (auto& entry : fs::directory_iterator("."))
    {
        std::string name = entry.path().filename().string();
        if (name.find("test_geometry") == 0)
            fs::remove(entry.path());
    }
}

// ============================================================================
// glTF Geometry Loading Tests (RED phase)
// ============================================================================

TEST_CASE("Load glTF with single triangle extracts positions and indices")
{
    cleanup();
    GltfBuilder builder;
    builder.AddTriangle(
        {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {0, 1, 2}
    );
    REQUIRE(builder.Write(TEST_GLB));

    ECSScene scene;
    REQUIRE(LoadGltfForTest(scene, RepositoryRootForSceneLoaderTests(), TEST_GLB));
    CHECK(scene.meshRegistry.GetCount() >= 1);

    const auto& mesh = scene.meshRegistry.GetMesh(0);
    REQUIRE(mesh.vertices.size() == 9);
    CHECK(mesh.vertices[0] == 0.0f);
    CHECK(mesh.vertices[1] == 0.0f);
    CHECK(mesh.vertices[2] == 0.0f);
    CHECK(mesh.vertices[3] == 1.0f);
    CHECK(mesh.vertices[4] == 0.0f);
    CHECK(mesh.vertices[5] == 0.0f);
    CHECK(mesh.vertices[6] == 0.0f);
    CHECK(mesh.vertices[7] == 1.0f);
    CHECK(mesh.vertices[8] == 0.0f);

    REQUIRE(mesh.indices.size() == 3);
    CHECK(mesh.indices[0] == 0);
    CHECK(mesh.indices[1] == 1);
    CHECK(mesh.indices[2] == 2);

    cleanup();
}

TEST_CASE("Load glTF with normals extracts normal data")
{
    cleanup();
    GltfBuilder builder;
    builder.AddTriangleWithNormals(
        {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},
        {0, 1, 2}
    );
    REQUIRE(builder.Write(TEST_GLB));

    ECSScene scene;
    REQUIRE(LoadGltfForTest(scene, RepositoryRootForSceneLoaderTests(), TEST_GLB));
    CHECK(scene.meshRegistry.GetCount() >= 1);

    const auto& mesh = scene.meshRegistry.GetMesh(0);
    REQUIRE(mesh.normals.size() == 9);
    CHECK(mesh.normals[0] == doctest::Approx(0.0f));
    CHECK(mesh.normals[1] == doctest::Approx(0.0f));
    CHECK(mesh.normals[2] == doctest::Approx(1.0f));

    cleanup();
}

TEST_CASE("Load glTF with UVs extracts texcoord data")
{
    cleanup();
    GltfBuilder builder;
    builder.AddTriangleWithUVs(
        {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}},
        {0, 1, 2}
    );
    REQUIRE(builder.Write(TEST_GLB));

    ECSScene scene;
    REQUIRE(LoadGltfForTest(scene, RepositoryRootForSceneLoaderTests(), TEST_GLB));
    CHECK(scene.meshRegistry.GetCount() >= 1);

    const auto& mesh = scene.meshRegistry.GetMesh(0);
    REQUIRE(mesh.uvs.size() == 6);
    CHECK(mesh.uvs[0] == 0.0f);
    CHECK(mesh.uvs[1] == 0.0f);
    CHECK(mesh.uvs[2] == 1.0f);
    CHECK(mesh.uvs[3] == 0.0f);
    CHECK(mesh.uvs[4] == 0.0f);
    CHECK(mesh.uvs[5] == 1.0f);

    cleanup();
}

TEST_CASE("Load glTF with uint16 indices widens to uint32")
{
    cleanup();
    GltfBuilder builder;
    builder.AddTriangle(
        {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {0, 1, 2},
        GltfBuilder::IndexType::UInt16
    );
    REQUIRE(builder.Write(TEST_GLB));

    ECSScene scene;
    REQUIRE(LoadGltfForTest(scene, RepositoryRootForSceneLoaderTests(), TEST_GLB));
    const auto& mesh = scene.meshRegistry.GetMesh(0);
    REQUIRE(mesh.indices.size() == 3);
    CHECK(mesh.indices[0] == 0);
    CHECK(mesh.indices[1] == 1);
    CHECK(mesh.indices[2] == 2);

    cleanup();
}

TEST_CASE("Load glTF with non-indexed primitive generates sequential indices")
{
    cleanup();
    GltfBuilder builder;
    builder.AddTriangleNonIndexed(
        {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}
    );
    REQUIRE(builder.Write(TEST_GLB));

    ECSScene scene;
    REQUIRE(LoadGltfForTest(scene, RepositoryRootForSceneLoaderTests(), TEST_GLB));
    const auto& mesh = scene.meshRegistry.GetMesh(0);
    REQUIRE(mesh.vertices.size() == 9);
    REQUIRE(mesh.indices.size() == 3);
    CHECK(mesh.indices[0] == 0);
    CHECK(mesh.indices[1] == 1);
    CHECK(mesh.indices[2] == 2);

    cleanup();
}

TEST_CASE("Load glTF with node transform applies translation to mesh")
{
    cleanup();
    GltfBuilder builder;
    builder.AddTriangle(
        {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {0, 1, 2}
    );
    builder.SetNodeTranslation({5.0f, 10.0f, -3.0f});
    REQUIRE(builder.Write(TEST_GLB));

    ECSScene scene;
    REQUIRE(LoadGltfForTest(scene, RepositoryRootForSceneLoaderTests(), TEST_GLB));
    auto meshView = scene.registry.view<MeshRef, Transform>();
    size_t count = std::distance(meshView.begin(), meshView.end());
    REQUIRE(count >= 1);
    auto entity = *meshView.begin();
    const auto& tf = meshView.get<Transform>(entity);
    CHECK(tf.translation == glm::vec3(5.0f, 10.0f, -3.0f));

    cleanup();
}

TEST_CASE("Load glTF with node scale applies scale to mesh")
{
    cleanup();
    GltfBuilder builder;
    builder.AddTriangle(
        {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {0, 1, 2}
    );
    builder.SetNodeScale({2.0f, 3.0f, 4.0f});
    REQUIRE(builder.Write(TEST_GLB));

    ECSScene scene;
    REQUIRE(LoadGltfForTest(scene, RepositoryRootForSceneLoaderTests(), TEST_GLB));
    auto meshView = scene.registry.view<MeshRef, Transform>();
    size_t count = std::distance(meshView.begin(), meshView.end());
    REQUIRE(count >= 1);
    auto entity = *meshView.begin();
    const auto& tf = meshView.get<Transform>(entity);
    CHECK(tf.scale.x == doctest::Approx(2.0f).epsilon(0.001));

    cleanup();
}

TEST_CASE("Load glTF with node rotation quaternion stores rotation")
{
    cleanup();
    GltfBuilder builder;
    builder.AddTriangle(
        {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {0, 1, 2}
    );
    builder.SetNodeRotation({0.0, 0.0, 0.70710678118, 0.70710678118});
    REQUIRE(builder.Write(TEST_GLB));

    ECSScene scene;
    REQUIRE(LoadGltfForTest(scene, RepositoryRootForSceneLoaderTests(), TEST_GLB));
    auto meshView = scene.registry.view<MeshRef, Transform>();
    size_t count = std::distance(meshView.begin(), meshView.end());
    REQUIRE(count >= 1);
    auto entity = *meshView.begin();
    const auto& tf = meshView.get<Transform>(entity);

    glm::vec3 euler = glm::eulerAngles(tf.rotation);
    CHECK(euler.x == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(euler.y == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(euler.z == doctest::Approx(glm::radians(90.0f)).epsilon(0.01));

    cleanup();
}

TEST_CASE("Load glTF with parent-child node hierarchy applies parent transform")
{
    cleanup();
    GltfBuilder builder;
    builder.AddTriangle(
        {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {0, 1, 2}
    );
    builder.SetNodeTranslation({10.0f, 0.0f, 0.0f});
    builder.SetParentNodeTranslation({5.0f, 0.0f, 0.0f});
    REQUIRE(builder.Write(TEST_GLB));

    ECSScene scene;
    REQUIRE(LoadGltfForTest(scene, RepositoryRootForSceneLoaderTests(), TEST_GLB));
    auto meshView = scene.registry.view<MeshRef, Transform>();
    size_t count = std::distance(meshView.begin(), meshView.end());
    REQUIRE(count >= 1);
    auto entity = *meshView.begin();
    const auto& tf = meshView.get<Transform>(entity);
    glm::vec3 worldPos = glm::vec3(tf.worldMatrix[3]);
    CHECK(worldPos.x == doctest::Approx(15.0f).epsilon(0.001));
    CHECK(worldPos.y == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(worldPos.z == doctest::Approx(0.0f).epsilon(0.001));

    cleanup();
}

TEST_CASE("Load glTF with multiple primitives in one mesh creates separate SceneMeshes")
{
    cleanup();
    GltfBuilder builder;
    builder.AddTriangle(
        {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {0, 1, 2}
    );
    builder.AddTriangle(
        {{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f}},
        {0, 1, 2}
    );
    REQUIRE(builder.Write(TEST_GLB));

    ECSScene scene;
    REQUIRE(LoadGltfForTest(scene, RepositoryRootForSceneLoaderTests(), TEST_GLB));
    auto meshView = scene.registry.view<MeshRef>();
    CHECK(meshView.size() == 2);

    cleanup();
}

TEST_CASE("Load glTF with material on primitive assigns materialIndex")
{
    cleanup();
    GltfBuilder builder;
    builder.AddMaterial({1.0f, 0.0f, 0.0f, 1.0}, 0.0, 0.5);
    builder.AddTriangleWithMaterial(
        {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {0, 1, 2},
        0
    );
    REQUIRE(builder.Write(TEST_GLB));

    ECSScene scene;
    REQUIRE(LoadGltfForTest(scene, RepositoryRootForSceneLoaderTests(), TEST_GLB));
    auto meshView = scene.registry.view<MeshRef>();
    REQUIRE(meshView.size() >= 1);
    auto entity = *meshView.begin();
    const auto& ref = meshView.get<MeshRef>(entity);
    CHECK(ref.materialIndex == 0);
    REQUIRE(scene.materials.size() >= 1);
    CHECK(scene.materials[0].baseColor == glm::vec3(1.0f, 0.0f, 0.0f));

    cleanup();
}

TEST_CASE("Load glTF skips non-triangle primitives")
{
    cleanup();
    GltfBuilder builder;
    builder.AddPoints({{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}});
    REQUIRE(builder.Write(TEST_GLB));

    ECSScene scene;
    REQUIRE(LoadGltfForTest(scene, RepositoryRootForSceneLoaderTests(), TEST_GLB));
    auto meshView = scene.registry.view<MeshRef>();
    CHECK(meshView.empty());

    cleanup();
}

TEST_CASE("Load glTF with .gltf (ASCII) format also extracts geometry")
{
    cleanup();
    GltfBuilder builder;
    builder.AddTriangle(
        {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {0, 1, 2}
    );
    REQUIRE(builder.Write(TEST_GLTF));

    ECSScene scene;
    REQUIRE(LoadGltfForTest(scene, RepositoryRootForSceneLoaderTests(), TEST_GLTF));
    CHECK(scene.meshRegistry.GetCount() >= 1);
    REQUIRE(scene.meshRegistry.GetMesh(0).vertices.size() == 9);

    cleanup();
}
