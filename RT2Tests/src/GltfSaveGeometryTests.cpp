#include <doctest/doctest.h>

#include "Scene.h"
#include "SceneLoader.h"

#include <glm/glm.hpp>
#include <filesystem>

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
    Scene scene;

    SceneMesh mesh;
    mesh.hasGeometry = true;
    mesh.vertices = {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f};
    mesh.indices = {0, 1, 2};
    mesh.position = {1.0f, 2.0f, 3.0f};
    mesh.scale = 2.0f;
    scene.AddMesh(mesh);

    REQUIRE(SceneLoader::Save(scene, ROUNDTRIP_GLB));
    Scene loaded;
    REQUIRE(SceneLoader::Load(loaded, ROUNDTRIP_GLB));

    REQUIRE(loaded.GetMeshes().size() == 1);
    const SceneMesh& m = loaded.GetMesh(0);
    REQUIRE(m.HasGeometry());

    REQUIRE(m.vertices.size() == 9);
    CHECK(m.vertices[0] == doctest::Approx(0.0f));
    CHECK(m.vertices[3] == doctest::Approx(1.0f));
    CHECK(m.vertices[6] == doctest::Approx(0.0f));
    CHECK(m.vertices[7] == doctest::Approx(1.0f));

    REQUIRE(m.indices.size() == 3);
    CHECK(m.indices[0] == 0);
    CHECK(m.indices[1] == 1);
    CHECK(m.indices[2] == 2);

    CHECK(m.position.x == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(m.position.y == doctest::Approx(2.0f).epsilon(0.001));
    CHECK(m.position.z == doctest::Approx(3.0f).epsilon(0.001));
    CHECK(m.scale == doctest::Approx(2.0f).epsilon(0.001));

    cleanup();
}

TEST_CASE("Geometry with normals round-trips through glTF save/load")
{
    cleanup();
    Scene scene;

    SceneMesh mesh;
    mesh.hasGeometry = true;
    mesh.vertices = {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f};
    mesh.normals = {0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f};
    mesh.indices = {0, 1, 2};
    scene.AddMesh(mesh);

    REQUIRE(SceneLoader::Save(scene, ROUNDTRIP_GLB));
    Scene loaded;
    REQUIRE(SceneLoader::Load(loaded, ROUNDTRIP_GLB));

    REQUIRE(loaded.GetMeshes().size() == 1);
    const SceneMesh& m = loaded.GetMesh(0);
    REQUIRE(m.HasGeometry());
    REQUIRE(m.normals.size() == 9);
    CHECK(m.normals[2] == doctest::Approx(1.0f));
    CHECK(m.normals[5] == doctest::Approx(1.0f));
    CHECK(m.normals[8] == doctest::Approx(1.0f));

    cleanup();
}

TEST_CASE("Geometry with UVs round-trips through glTF save/load")
{
    cleanup();
    Scene scene;

    SceneMesh mesh;
    mesh.hasGeometry = true;
    mesh.vertices = {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f};
    mesh.uvs = {0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f};
    mesh.indices = {0, 1, 2};
    scene.AddMesh(mesh);

    REQUIRE(SceneLoader::Save(scene, ROUNDTRIP_GLB));
    Scene loaded;
    REQUIRE(SceneLoader::Load(loaded, ROUNDTRIP_GLB));

    const SceneMesh& m = loaded.GetMesh(0);
    REQUIRE(m.HasGeometry());
    REQUIRE(m.uvs.size() == 6);
    CHECK(m.uvs[0] == doctest::Approx(0.0f));
    CHECK(m.uvs[2] == doctest::Approx(1.0f));
    CHECK(m.uvs[5] == doctest::Approx(1.0f));

    cleanup();
}

TEST_CASE("Multiple geometry meshes round-trip through glTF save/load")
{
    cleanup();
    Scene scene;

    SceneMesh mesh0;
    mesh0.hasGeometry = true;
    mesh0.vertices = {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f};
    mesh0.indices = {0, 1, 2};
    mesh0.position = {0.0f, 0.0f, 0.0f};
    scene.AddMesh(mesh0);

    SceneMesh mesh1;
    mesh1.hasGeometry = true;
    mesh1.vertices = {5.0f, 0.0f, 0.0f,  6.0f, 0.0f, 0.0f,  5.0f, 1.0f, 0.0f};
    mesh1.indices = {0, 1, 2};
    mesh1.position = {10.0f, 0.0f, 0.0f};
    scene.AddMesh(mesh1);

    REQUIRE(SceneLoader::Save(scene, ROUNDTRIP_GLB));
    Scene loaded;
    REQUIRE(SceneLoader::Load(loaded, ROUNDTRIP_GLB));

    REQUIRE(loaded.GetMeshes().size() == 2);
    CHECK(loaded.GetMesh(0).vertices[0] == doctest::Approx(0.0f));
    CHECK(loaded.GetMesh(1).vertices[0] == doctest::Approx(5.0f));
    CHECK(loaded.GetMesh(1).position.x == doctest::Approx(10.0f).epsilon(0.001));

    cleanup();
}

TEST_CASE("Geometry with material assignment round-trips")
{
    cleanup();
    Scene scene;

    SceneMaterial mat;
    mat.baseColor = {0.2f, 0.4f, 0.6f};
    scene.AddMaterial(mat);

    SceneMesh mesh;
    mesh.hasGeometry = true;
    mesh.vertices = {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f};
    mesh.indices = {0, 1, 2};
    mesh.materialIndex = 0;
    scene.AddMesh(mesh);

    REQUIRE(SceneLoader::Save(scene, ROUNDTRIP_GLB));
    Scene loaded;
    REQUIRE(SceneLoader::Load(loaded, ROUNDTRIP_GLB));

    REQUIRE(loaded.GetMaterials().size() == 1);
    CHECK(loaded.GetMaterial(0).baseColor == glm::vec3(0.2f, 0.4f, 0.6f));
    REQUIRE(loaded.GetMeshes().size() == 1);
    CHECK(loaded.GetMesh(0).materialIndex == 0);

    cleanup();
}

TEST_CASE("Mixed inline geometry and external filepath meshes round-trip")
{
    cleanup();
    Scene scene;

    // Mesh 0: inline geometry
    SceneMesh mesh0;
    mesh0.hasGeometry = true;
    mesh0.vertices = {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f};
    mesh0.indices = {0, 1, 2};
    scene.AddMesh(mesh0);

    // Mesh 1: external OBJ filepath (no geometry)
    SceneMesh mesh1;
    mesh1.filepath = "models/car.obj";
    mesh1.position = {5.0f, 0.0f, 0.0f};
    mesh1.scale = 0.01f;
    scene.AddMesh(mesh1);

    REQUIRE(SceneLoader::Save(scene, ROUNDTRIP_GLB));
    Scene loaded;
    REQUIRE(SceneLoader::Load(loaded, ROUNDTRIP_GLB));

    REQUIRE(loaded.GetMeshes().size() == 2);

    // First mesh should have geometry
    CHECK(loaded.GetMesh(0).HasGeometry());
    REQUIRE(loaded.GetMesh(0).vertices.size() == 9);

    // Second mesh should be an external filepath reference
    CHECK_FALSE(loaded.GetMesh(1).HasGeometry());
    CHECK(loaded.GetMesh(1).filepath == "models/car.obj");
    CHECK(loaded.GetMesh(1).position.x == doctest::Approx(5.0f).epsilon(0.001));
    CHECK(loaded.GetMesh(1).scale == doctest::Approx(0.01f).epsilon(0.001));

    cleanup();
}