#include <doctest/doctest.h>

#include "Scene.h"
#include "SceneLoader.h"
#include <glm/glm.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ============================================================================
// glTF Serialization Round-Trip Tests (RED phase)
// ============================================================================
// These tests verify that Scene data can be saved to a .gltf file and loaded
// back with all values preserved. We use temp files for isolation.
// ============================================================================

static const char* TEST_FILE = "test_scene.gltf";
static const char* TEST_FILE_GLB = "test_scene.glb";

static void cleanupTestFiles()
{
    fs::remove(TEST_FILE);
    fs::remove(TEST_FILE_GLB);
    fs::remove("test_scene.bin");
    // tinygltf may create additional files
    for (auto& entry : fs::directory_iterator("."))
    {
        if (entry.path().filename().string().find("test_scene") == 0)
            fs::remove(entry.path());
    }
}

// --- Minimal empty scene ---

TEST_CASE("Save and load empty scene")
{
    cleanupTestFiles();
    Scene scene;
    REQUIRE(SceneLoader::Save(scene, TEST_FILE));
    Scene loaded;
    REQUIRE(SceneLoader::Load(loaded, TEST_FILE));
    CHECK(loaded.GetMeshes().empty());
    CHECK(loaded.GetMaterials().empty());
    CHECK(loaded.GetLights().empty());
    CHECK(loaded.GetTextures().empty());
    cleanupTestFiles();
}

// --- Mesh round-trip ---

TEST_CASE("Mesh round-trips through glTF")
{
    cleanupTestFiles();
    Scene scene;
    SceneMesh mesh;
    mesh.filepath = "models/car.obj";
    mesh.position = {1.5f, 2.0f, -3.0f};
    mesh.rotation = {0.0f, 0.5f, 0.0f};
    mesh.scale = 0.01f;
    mesh.materialIndex = 1;
    scene.AddMesh(mesh);

    REQUIRE(SceneLoader::Save(scene, TEST_FILE));
    Scene loaded;
    REQUIRE(SceneLoader::Load(loaded, TEST_FILE));
    REQUIRE(loaded.GetMeshes().size() == 1);
    const auto& m = loaded.GetMesh(0);
    CHECK(m.filepath == "models/car.obj");
    CHECK(m.position == glm::vec3(1.5f, 2.0f, -3.0f));
    CHECK(m.rotation == glm::vec3(0.0f, 0.5f, 0.0f));
    CHECK(m.scale == 0.01f);
    CHECK(m.materialIndex == 1);
    cleanupTestFiles();
}

// --- Material round-trip ---

TEST_CASE("Material round-trips through glTF")
{
    cleanupTestFiles();
    Scene scene;
    SceneMaterial mat;
    mat.type = MaterialType::Metal;
    mat.baseColor = {0.9f, 0.6f, 0.3f};
    mat.metallic = 0.8f;
    mat.roughness = 0.15f;
    mat.ior = 1.45f;
    mat.emissiveColor = {0.1f, 0.0f, 0.0f};
    mat.emissiveIntensity = 0.0f;
    mat.baseColorTextureIndex = 0;
    scene.AddMaterial(mat);

    REQUIRE(SceneLoader::Save(scene, TEST_FILE));
    Scene loaded;
    REQUIRE(SceneLoader::Load(loaded, TEST_FILE));
    REQUIRE(loaded.GetMaterials().size() == 1);
    const auto& m = loaded.GetMaterial(0);
    CHECK(m.type == MaterialType::Metal);
    CHECK(m.baseColor == glm::vec3(0.9f, 0.6f, 0.3f));
    CHECK(m.metallic == 0.8f);
    CHECK(m.roughness == 0.15f);
    CHECK(m.ior == 1.45f);
    CHECK(m.baseColorTextureIndex == 0);
    cleanupTestFiles();
}

// --- Emissive material round-trip ---

TEST_CASE("Emissive material round-trips through glTF")
{
    cleanupTestFiles();
    Scene scene;
    SceneMaterial mat;
    mat.type = MaterialType::Emissive;
    mat.emissiveColor = {1.0f, 0.8f, 0.4f};
    mat.emissiveIntensity = 5.0f;
    scene.AddMaterial(mat);

    REQUIRE(SceneLoader::Save(scene, TEST_FILE));
    Scene loaded;
    REQUIRE(SceneLoader::Load(loaded, TEST_FILE));
    REQUIRE(loaded.GetMaterials().size() == 1);
    const auto& m = loaded.GetMaterial(0);
    CHECK(m.type == MaterialType::Emissive);
    CHECK(m.emissiveColor == glm::vec3(1.0f, 0.8f, 0.4f));
    CHECK(m.emissiveIntensity == 5.0f);
    cleanupTestFiles();
}

// --- Light round-trip ---

TEST_CASE("Point light round-trips through glTF")
{
    cleanupTestFiles();
    Scene scene;
    SceneLight light;
    light.type = LightType::Point;
    light.position = {5.0f, 10.0f, 0.0f};
    light.color = {1.0f, 0.5f, 0.2f};
    light.intensity = 50.0f;
    light.range = 30.0f;
    scene.AddLight(light);

    REQUIRE(SceneLoader::Save(scene, TEST_FILE));
    Scene loaded;
    REQUIRE(SceneLoader::Load(loaded, TEST_FILE));
    REQUIRE(loaded.GetLights().size() == 1);
    const auto& l = loaded.GetLight(0);
    CHECK(l.type == LightType::Point);
    CHECK(l.position == glm::vec3(5.0f, 10.0f, 0.0f));
    CHECK(l.color == glm::vec3(1.0f, 0.5f, 0.2f));
    CHECK(l.intensity == 50.0f);
    CHECK(l.range == 30.0f);
    cleanupTestFiles();
}

TEST_CASE("Spot light round-trips through glTF")
{
    cleanupTestFiles();
    Scene scene;
    SceneLight light;
    light.type = LightType::Spot;
    light.position = {0.0f, 5.0f, 0.0f};
    light.direction = {0.0f, -1.0f, 0.0f};
    light.color = {1.0f, 1.0f, 0.9f};
    light.intensity = 20.0f;
    light.innerConeAngle = 25.0f;
    light.outerConeAngle = 40.0f;
    scene.AddLight(light);

    REQUIRE(SceneLoader::Save(scene, TEST_FILE));
    Scene loaded;
    REQUIRE(SceneLoader::Load(loaded, TEST_FILE));
    REQUIRE(loaded.GetLights().size() == 1);
    const auto& l = loaded.GetLight(0);
    CHECK(l.type == LightType::Spot);
    CHECK(l.direction == glm::vec3(0.0f, -1.0f, 0.0f));
    CHECK(l.innerConeAngle == 25.0f);
    CHECK(l.outerConeAngle == 40.0f);
    cleanupTestFiles();
}

// --- Texture round-trip ---

TEST_CASE("Texture round-trips through glTF")
{
    cleanupTestFiles();
    Scene scene;
    scene.AddTexture({"textures/albedo.png"});
    scene.AddTexture({"textures/normal.png"});

    REQUIRE(SceneLoader::Save(scene, TEST_FILE));
    Scene loaded;
    REQUIRE(SceneLoader::Load(loaded, TEST_FILE));
    REQUIRE(loaded.GetTextures().size() == 2);
    CHECK(loaded.GetTexture(0).filepath == "textures/albedo.png");
    CHECK(loaded.GetTexture(1).filepath == "textures/normal.png");
    cleanupTestFiles();
}

// --- Camera round-trip ---

TEST_CASE("Camera round-trips through glTF")
{
    cleanupTestFiles();
    Scene scene;
    scene.GetCamera().position = {3.0f, 4.0f, 5.0f};
    scene.GetCamera().forwardDirection = {0.0f, -0.5f, -1.0f};
    scene.GetCamera().verticalFOV = 60.0f;
    scene.GetCamera().aperture = 0.1f;
    scene.GetCamera().focusDistance = 5.0f;

    REQUIRE(SceneLoader::Save(scene, TEST_FILE));
    Scene loaded;
    REQUIRE(SceneLoader::Load(loaded, TEST_FILE));
    const auto& cam = loaded.GetCamera();
    CHECK(cam.position == glm::vec3(3.0f, 4.0f, 5.0f));
    CHECK(cam.forwardDirection == glm::vec3(0.0f, -0.5f, -1.0f));
    CHECK(cam.verticalFOV == 60.0f);
    CHECK(cam.aperture == 0.1f);
    CHECK(cam.focusDistance == 5.0f);
    cleanupTestFiles();
}

// --- Full scene round-trip ---

TEST_CASE("Full scene with multiple meshes, materials, lights round-trips")
{
    cleanupTestFiles();
    Scene scene;

    // Textures
    scene.AddTexture({"textures/albedo.png"});
    scene.AddTexture({"textures/normal.png"});

    // Materials
    SceneMaterial mat0;
    mat0.type = MaterialType::Lambertian;
    mat0.baseColor = {0.5f, 0.5f, 0.5f};
    scene.AddMaterial(mat0);

    SceneMaterial mat1;
    mat1.type = MaterialType::Metal;
    mat1.baseColor = {0.8f, 0.8f, 0.9f};
    mat1.metallic = 1.0f;
    mat1.roughness = 0.1f;
    mat1.baseColorTextureIndex = 0;
    scene.AddMaterial(mat1);

    SceneMaterial mat2;
    mat2.type = MaterialType::Emissive;
    mat2.emissiveColor = {1.0f, 1.0f, 0.9f};
    mat2.emissiveIntensity = 10.0f;
    scene.AddMaterial(mat2);

    // Meshes
    SceneMesh mesh0;
    mesh0.filepath = "models/floor.obj";
    mesh0.position = {0.0f, 0.0f, 0.0f};
    mesh0.materialIndex = 0;
    scene.AddMesh(mesh0);

    SceneMesh mesh1;
    mesh1.filepath = "models/car.obj";
    mesh1.position = {0.0f, 0.0f, 0.0f};
    mesh1.scale = 0.01f;
    mesh1.materialIndex = 1;
    scene.AddMesh(mesh1);

    // Lights
    SceneLight light;
    light.type = LightType::Point;
    light.position = {5.0f, 10.0f, 5.0f};
    light.intensity = 30.0f;
    scene.AddLight(light);

    // Camera
    scene.GetCamera().position = {2.0f, 2.0f, 8.0f};
    scene.GetCamera().verticalFOV = 50.0f;

    REQUIRE(SceneLoader::Save(scene, TEST_FILE));
    Scene loaded;
    REQUIRE(SceneLoader::Load(loaded, TEST_FILE));

    // Verify textures
    REQUIRE(loaded.GetTextures().size() == 2);
    CHECK(loaded.GetTexture(0).filepath == "textures/albedo.png");

    // Verify materials
    REQUIRE(loaded.GetMaterials().size() == 3);
    CHECK(loaded.GetMaterial(0).type == MaterialType::Lambertian);
    CHECK(loaded.GetMaterial(1).type == MaterialType::Metal);
    CHECK(loaded.GetMaterial(1).baseColorTextureIndex == 0);
    CHECK(loaded.GetMaterial(2).type == MaterialType::Emissive);
    CHECK(loaded.GetMaterial(2).emissiveIntensity == 10.0f);

    // Verify meshes
    REQUIRE(loaded.GetMeshes().size() == 2);
    CHECK(loaded.GetMesh(0).filepath == "models/floor.obj");
    CHECK(loaded.GetMesh(0).materialIndex == 0);
    CHECK(loaded.GetMesh(1).filepath == "models/car.obj");
    CHECK(loaded.GetMesh(1).scale == 0.01f);
    CHECK(loaded.GetMesh(1).materialIndex == 1);

    // Verify lights
    REQUIRE(loaded.GetLights().size() == 1);
    CHECK(loaded.GetLight(0).type == LightType::Point);
    CHECK(loaded.GetLight(0).position == glm::vec3(5.0f, 10.0f, 5.0f));

    // Verify camera
    CHECK(loaded.GetCamera().position == glm::vec3(2.0f, 2.0f, 8.0f));
    CHECK(loaded.GetCamera().verticalFOV == 50.0f);

    cleanupTestFiles();
}

// --- GLB format ---

TEST_CASE("Scene saves and loads as GLB (binary glTF)")
{
    cleanupTestFiles();
    Scene scene;
    scene.AddMaterial({});
    SceneMaterial mat;
    mat.baseColor = {0.2f, 0.4f, 0.6f};
    scene.AddMaterial(mat);

    REQUIRE(SceneLoader::Save(scene, TEST_FILE_GLB));
    Scene loaded;
    REQUIRE(SceneLoader::Load(loaded, TEST_FILE_GLB));
    REQUIRE(loaded.GetMaterials().size() == 2);
    CHECK(loaded.GetMaterial(1).baseColor == glm::vec3(0.2f, 0.4f, 0.6f));
    cleanupTestFiles();
}

// --- Non-existent file returns false ---

TEST_CASE("Load returns false for non-existent file")
{
    Scene loaded;
    CHECK_FALSE(SceneLoader::Load(loaded, "does_not_exist.gltf"));
}

// --- Save returns false for invalid path ---

TEST_CASE("Save returns false for invalid path")
{
    Scene scene;
    CHECK_FALSE(SceneLoader::Save(scene, "Z:/nonexistent_dir/scene.gltf"));
}