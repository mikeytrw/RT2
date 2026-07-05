#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "Scene.h"
#include <glm/glm.hpp>

// ============================================================================
// Scene Data Structure Tests (RED phase)
// ============================================================================

TEST_CASE("Scene starts empty")
{
    Scene scene;
    CHECK(scene.GetMeshes().empty());
    CHECK(scene.GetMaterials().empty());
    CHECK(scene.GetLights().empty());
    CHECK(scene.GetTextures().empty());
}

TEST_CASE("Scene::Clear empties all collections")
{
    Scene scene;
    scene.AddMesh({});
    scene.AddMaterial({});
    scene.AddLight({});
    scene.AddTexture({});
    scene.Clear();
    CHECK(scene.GetMeshes().empty());
    CHECK(scene.GetMaterials().empty());
    CHECK(scene.GetLights().empty());
    CHECK(scene.GetTextures().empty());
}

// --- Meshes ---

TEST_CASE("AddMesh returns correct index and stores mesh")
{
    Scene scene;
    SceneMesh mesh;
    mesh.filepath = "models/car.obj";
    mesh.position = {1.0f, 2.0f, 3.0f};
    mesh.rotation = {0.1f, 0.2f, 0.3f};
    mesh.scale = 0.01f;
    mesh.materialIndex = 2;

    int idx = scene.AddMesh(mesh);
    CHECK(idx == 0);
    CHECK(scene.GetMeshes().size() == 1);
    CHECK(scene.GetMesh(idx).filepath == "models/car.obj");
    CHECK(scene.GetMesh(idx).position == glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(scene.GetMesh(idx).scale == 0.01f);
    CHECK(scene.GetMesh(idx).materialIndex == 2);
}

TEST_CASE("Multiple meshes get sequential indices")
{
    Scene scene;
    int i0 = scene.AddMesh({});
    int i1 = scene.AddMesh({});
    int i2 = scene.AddMesh({});
    CHECK(i0 == 0);
    CHECK(i1 == 1);
    CHECK(i2 == 2);
    CHECK(scene.GetMeshes().size() == 3);
}

TEST_CASE("GetMesh returns mutable reference")
{
    Scene scene;
    int idx = scene.AddMesh({});
    scene.GetMesh(idx).filepath = "changed.obj";
    scene.GetMesh(idx).scale = 5.0f;
    CHECK(scene.GetMesh(idx).filepath == "changed.obj");
    CHECK(scene.GetMesh(idx).scale == 5.0f);
}

// --- Materials ---

TEST_CASE("AddMaterial returns correct index and stores material")
{
    Scene scene;
    SceneMaterial mat;
    mat.type = MaterialType::Metal;
    mat.baseColor = {0.9f, 0.6f, 0.3f};
    mat.metallic = 1.0f;
    mat.roughness = 0.2f;

    int idx = scene.AddMaterial(mat);
    CHECK(idx == 0);
    CHECK(scene.GetMaterials().size() == 1);
    CHECK(scene.GetMaterial(idx).type == MaterialType::Metal);
    CHECK(scene.GetMaterial(idx).baseColor == glm::vec3(0.9f, 0.6f, 0.3f));
    CHECK(scene.GetMaterial(idx).metallic == 1.0f);
    CHECK(scene.GetMaterial(idx).roughness == 0.2f);
}

TEST_CASE("Default material is Lambertian with sensible defaults")
{
    SceneMaterial mat;
    CHECK(mat.type == MaterialType::Lambertian);
    CHECK(mat.metallic == 0.0f);
    CHECK(mat.roughness == 0.5f);
    CHECK(mat.ior == 1.5f);
    CHECK(mat.emissiveIntensity == 0.0f);
    CHECK(mat.baseColorTextureIndex == -1);
    CHECK(mat.normalTextureIndex == -1);
}

TEST_CASE("Material supports Emissive type")
{
    Scene scene;
    SceneMaterial mat;
    mat.type = MaterialType::Emissive;
    mat.emissiveColor = {1.0f, 0.8f, 0.4f};
    mat.emissiveIntensity = 5.0f;

    int idx = scene.AddMaterial(mat);
    CHECK(scene.GetMaterial(idx).type == MaterialType::Emissive);
    CHECK(scene.GetMaterial(idx).emissiveColor == glm::vec3(1.0f, 0.8f, 0.4f));
    CHECK(scene.GetMaterial(idx).emissiveIntensity == 5.0f);
}

// --- Lights ---

TEST_CASE("AddLight returns correct index and stores light")
{
    Scene scene;
    SceneLight light;
    light.type = LightType::Point;
    light.position = {5.0f, 10.0f, 0.0f};
    light.color = {1.0f, 0.5f, 0.2f};
    light.intensity = 50.0f;
    light.range = 30.0f;

    int idx = scene.AddLight(light);
    CHECK(idx == 0);
    CHECK(scene.GetLights().size() == 1);
    CHECK(scene.GetLight(idx).type == LightType::Point);
    CHECK(scene.GetLight(idx).position == glm::vec3(5.0f, 10.0f, 0.0f));
    CHECK(scene.GetLight(idx).intensity == 50.0f);
}

TEST_CASE("Spot light has cone angles")
{
    Scene scene;
    SceneLight light;
    light.type = LightType::Spot;
    light.direction = {0.0f, -1.0f, 0.0f};
    light.innerConeAngle = 25.0f;
    light.outerConeAngle = 40.0f;

    int idx = scene.AddLight(light);
    CHECK(scene.GetLight(idx).type == LightType::Spot);
    CHECK(scene.GetLight(idx).innerConeAngle == 25.0f);
    CHECK(scene.GetLight(idx).outerConeAngle == 40.0f);
}

// --- Textures ---

TEST_CASE("AddTexture returns correct index and stores texture")
{
    Scene scene;
    SceneTexture tex;
    tex.filepath = "textures/albedo.png";

    int idx = scene.AddTexture(tex);
    CHECK(idx == 0);
    CHECK(scene.GetTextures().size() == 1);
    CHECK(scene.GetTexture(idx).filepath == "textures/albedo.png");
}

TEST_CASE("Material references texture by index")
{
    Scene scene;
    int texIdx = scene.AddTexture({"textures/albedo.png"});
    SceneMaterial mat;
    mat.baseColorTextureIndex = texIdx;
    int matIdx = scene.AddMaterial(mat);
    CHECK(scene.GetMaterial(matIdx).baseColorTextureIndex == 0);
}

// --- Camera ---

TEST_CASE("Scene has camera with default values")
{
    Scene scene;
    const SceneCamera& cam = scene.GetCamera();
    CHECK(cam.position == glm::vec3(0.0f, 1.0f, 10.0f));
    CHECK(cam.forwardDirection == glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(cam.verticalFOV == 45.0f);
}

TEST_CASE("Scene camera is mutable")
{
    Scene scene;
    scene.GetCamera().position = {5.0f, 5.0f, 5.0f};
    scene.GetCamera().verticalFOV = 60.0f;
    CHECK(scene.GetCamera().position == glm::vec3(5.0f, 5.0f, 5.0f));
    CHECK(scene.GetCamera().verticalFOV == 60.0f);
}