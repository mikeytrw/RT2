#include <doctest/doctest.h>

#include "Scene.h"
#include "SceneLoader.h"
#include "GltfBuilder.h"  // helper to construct test glTF files

#include <glm/glm.hpp>
#include <filesystem>

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

    Scene scene;
    REQUIRE(SceneLoader::Load(scene, TEST_GLB));
    REQUIRE(scene.GetMeshes().size() == 1);

    const SceneMesh& mesh = scene.GetMesh(0);
    REQUIRE(mesh.HasGeometry());
    REQUIRE(mesh.vertices.size() == 9);  // 3 verts * 3 floats
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

    Scene scene;
    REQUIRE(SceneLoader::Load(scene, TEST_GLB));
    REQUIRE(scene.GetMeshes().size() == 1);

    const SceneMesh& mesh = scene.GetMesh(0);
    REQUIRE(mesh.HasGeometry());
    REQUIRE(mesh.normals.size() == 9);  // 3 verts * 3 floats
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

    Scene scene;
    REQUIRE(SceneLoader::Load(scene, TEST_GLB));
    REQUIRE(scene.GetMeshes().size() == 1);

    const SceneMesh& mesh = scene.GetMesh(0);
    REQUIRE(mesh.HasGeometry());
    REQUIRE(mesh.uvs.size() == 6);  // 3 verts * 2 floats
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

    Scene scene;
    REQUIRE(SceneLoader::Load(scene, TEST_GLB));
    const SceneMesh& mesh = scene.GetMesh(0);
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

    Scene scene;
    REQUIRE(SceneLoader::Load(scene, TEST_GLB));
    const SceneMesh& mesh = scene.GetMesh(0);
    REQUIRE(mesh.HasGeometry());
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

    Scene scene;
    REQUIRE(SceneLoader::Load(scene, TEST_GLB));
    REQUIRE(scene.GetMeshes().size() == 1);
    const SceneMesh& mesh = scene.GetMesh(0);
    CHECK(mesh.position == glm::vec3(5.0f, 10.0f, -3.0f));

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

    Scene scene;
    REQUIRE(SceneLoader::Load(scene, TEST_GLB));
    const SceneMesh& mesh = scene.GetMesh(0);
    // We store the transform in the SceneMesh; geometry is not pre-baked
    CHECK(mesh.scale == 2.0f);  // uniform scale uses first component

    cleanup();
}

TEST_CASE("Load glTF with node rotation quaternion converts to Euler")
{
    cleanup();
    GltfBuilder builder;
    builder.AddTriangle(
        {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {0, 1, 2}
    );
    // 90 degree rotation around Z axis: quaternion [0, 0, sin(45), cos(45)]
    // = [0, 0, 0.7071, 0.7071]
    builder.SetNodeRotation({0.0, 0.0, 0.70710678118, 0.70710678118});
    REQUIRE(builder.Write(TEST_GLB));

    Scene scene;
    REQUIRE(SceneLoader::Load(scene, TEST_GLB));
    const SceneMesh& mesh = scene.GetMesh(0);
    // Should convert to ~90 degrees on Z, ~0 on X and Y
    CHECK(mesh.rotation.x == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(mesh.rotation.y == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(mesh.rotation.z == doctest::Approx(glm::radians(90.0f)).epsilon(0.01));

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
    builder.SetParentNodeTranslation({5.0f, 0.0f, 0.0f}); // parent at (5,0,0)
    REQUIRE(builder.Write(TEST_GLB));

    Scene scene;
    REQUIRE(SceneLoader::Load(scene, TEST_GLB));
    REQUIRE(scene.GetMeshes().size() == 1);
    const SceneMesh& mesh = scene.GetMesh(0);
    // Combined translation: parent (5,0,0) + child (10,0,0) = (15,0,0)
    CHECK(mesh.position.x == doctest::Approx(15.0f).epsilon(0.001));
    CHECK(mesh.position.y == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(mesh.position.z == doctest::Approx(0.0f).epsilon(0.001));

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
    builder.AddTriangle(  // second primitive in same mesh
        {{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f}},
        {0, 1, 2}
    );
    REQUIRE(builder.Write(TEST_GLB));

    Scene scene;
    REQUIRE(SceneLoader::Load(scene, TEST_GLB));
    // Multiple primitives should produce multiple SceneMesh entries
    REQUIRE(scene.GetMeshes().size() == 2);

    cleanup();
}

TEST_CASE("Load glTF with material on primitive assigns materialIndex")
{
    cleanup();
    GltfBuilder builder;
    builder.AddMaterial({1.0f, 0.0f, 0.0f, 1.0}, 0.0, 0.5);  // red, non-metal, rough=0.5
    builder.AddTriangleWithMaterial(
        {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {0, 1, 2},
        0  // material index 0
    );
    REQUIRE(builder.Write(TEST_GLB));

    Scene scene;
    REQUIRE(SceneLoader::Load(scene, TEST_GLB));
    REQUIRE(scene.GetMeshes().size() == 1);
    CHECK(scene.GetMesh(0).materialIndex == 0);
    REQUIRE(scene.GetMaterials().size() == 1);
    CHECK(scene.GetMaterial(0).baseColor == glm::vec3(1.0f, 0.0f, 0.0f));

    cleanup();
}

TEST_CASE("Load glTF skips non-triangle primitives")
{
    cleanup();
    GltfBuilder builder;
    builder.AddPoints({{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}});
    REQUIRE(builder.Write(TEST_GLB));

    Scene scene;
    REQUIRE(SceneLoader::Load(scene, TEST_GLB));
    // Points (mode 0) should be skipped — no meshes
    CHECK(scene.GetMeshes().empty());

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
    REQUIRE(builder.Write(TEST_GLTF));  // .gltf not .glb

    Scene scene;
    REQUIRE(SceneLoader::Load(scene, TEST_GLTF));
    REQUIRE(scene.GetMeshes().size() == 1);
    REQUIRE(scene.GetMesh(0).HasGeometry());
    REQUIRE(scene.GetMesh(0).vertices.size() == 9);

    cleanup();
}