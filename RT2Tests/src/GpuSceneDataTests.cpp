#include <doctest/doctest.h>

#include "GPUSceneData.h"
#include "Scene.h"
#include <glm/glm.hpp>

// ============================================================================
// GPUMaterial struct layout tests (std430 / 32-byte alignment)
// ============================================================================

TEST_CASE("GPUMaterial is 64 bytes (3 vec4 + ivec4, std430)")
{
    CHECK(sizeof(GPUMaterial) == 64);
}

TEST_CASE("GPUMaterial default values are sensible")
{
    GPUMaterial mat;
    CHECK(mat.baseColor_metallic == glm::vec4(0.8f, 0.8f, 0.8f, 0.0f));
    CHECK(mat.emissive_roughness == glm::vec4(0.0f, 0.0f, 0.0f, 0.5f));
    CHECK(mat.ior == 1.5f);
    CHECK(mat.textureIndices.x == -1); // no baseColor texture
    CHECK(mat.textureIndices.y == -1); // no normal texture
    CHECK(mat.textureIndices.z == -1); // no emissive texture
}

TEST_CASE("GPUMaterial from SceneMaterial preserves texture indices")
{
    SceneMaterial sm;
    sm.baseColorTextureIndex = 3;
    sm.normalTextureIndex = 7;
    sm.emissiveTextureIndex = 12;

    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);

    CHECK(gm.textureIndices.x == 3);
    CHECK(gm.textureIndices.y == 7);
    CHECK(gm.textureIndices.z == 12);
}

TEST_CASE("GPUMaterial from SceneMaterial preserves PBR fields")
{
    SceneMaterial sm;
    sm.type = MaterialType::Lambertian;
    sm.baseColor = {0.9f, 0.2f, 0.1f};
    sm.metallic = 0.8f;
    sm.roughness = 0.3f;
    sm.ior = 1.45f;
    sm.emissiveColor = {1.0f, 0.5f, 0.2f};
    sm.emissiveIntensity = 3.0f;

    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);

    CHECK(gm.baseColor_metallic.x == 0.9f);
    CHECK(gm.baseColor_metallic.y == 0.2f);
    CHECK(gm.baseColor_metallic.z == 0.1f);
    CHECK(gm.baseColor_metallic.w == 0.8f);
    CHECK(gm.emissive_roughness.x == 1.0f * 3.0f);
    CHECK(gm.emissive_roughness.y == 0.5f * 3.0f);
    CHECK(gm.emissive_roughness.z == 0.2f * 3.0f);
    CHECK(gm.emissive_roughness.w == 0.3f);
    CHECK(gm.ior == 1.45f);
}

TEST_CASE("GPUMaterial emissive is zero when intensity is zero")
{
    SceneMaterial sm;
    sm.emissiveColor = {1.0f, 0.5f, 0.2f};
    sm.emissiveIntensity = 0.0f;

    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);

    CHECK(gm.emissive_roughness.x == 0.0f);
    CHECK(gm.emissive_roughness.y == 0.0f);
    CHECK(gm.emissive_roughness.z == 0.0f);
}

// ============================================================================
// GPUMeshGeometry tests
// ============================================================================

TEST_CASE("GPUMeshGeometry stores vertices and indices")
{
    GPUMeshGeometry geo;
    geo.vertices = {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f};
    geo.indices = {0, 1, 2};
    geo.materialIndex = 3;

    CHECK(geo.vertices.size() == 9);
    CHECK(geo.indices.size() == 3);
    CHECK(geo.materialIndex == 3);
}

TEST_CASE("GPUMeshGeometry default materialIndex is 0")
{
    GPUMeshGeometry geo;
    CHECK(geo.materialIndex == 0);
}

// ============================================================================
// GPUSceneData tests
// ============================================================================

TEST_CASE("GPUSceneData starts empty")
{
    GPUSceneData scene;
    CHECK(scene.meshes.empty());
    CHECK(scene.materials.empty());
}

TEST_CASE("GPUSceneData holds multiple meshes and materials")
{
    GPUSceneData scene;

    GPUMeshGeometry geo1;
    geo1.materialIndex = 0;
    scene.meshes.push_back(geo1);

    GPUMeshGeometry geo2;
    geo2.materialIndex = 1;
    scene.meshes.push_back(geo2);

    scene.materials.push_back(GPUMaterial::fromSceneMaterial(SceneMaterial{}));
    scene.materials.push_back(GPUMaterial::fromSceneMaterial(SceneMaterial{}));

    CHECK(scene.meshes.size() == 2);
    CHECK(scene.materials.size() == 2);
    CHECK(scene.meshes[0].materialIndex == 0);
    CHECK(scene.meshes[1].materialIndex == 1);
}

// ============================================================================
// BuildGPUSceneData from Scene (integration of scene → GPU data)
// ============================================================================

TEST_CASE("BuildGPUSceneData converts Scene meshes and materials")
{
    Scene scene;

    // Add 2 materials
    SceneMaterial mat0;
    mat0.baseColor = {1.0f, 0.0f, 0.0f};
    mat0.metallic = 0.0f;
    scene.AddMaterial(mat0);

    SceneMaterial mat1;
    mat1.baseColor = {0.0f, 0.0f, 1.0f};
    mat1.metallic = 1.0f;
    scene.AddMaterial(mat1);

    // Add 2 meshes with different material indices
    SceneMesh mesh0;
    mesh0.hasGeometry = true;
    mesh0.vertices = {0, 0, 0,  1, 0, 0,  0, 1, 0};
    mesh0.indices = {0, 1, 2};
    mesh0.materialIndex = 0;
    scene.AddMesh(mesh0);

    SceneMesh mesh1;
    mesh1.hasGeometry = true;
    mesh1.vertices = {0, 0, 0,  1, 0, 0,  0, 1, 0};
    mesh1.indices = {0, 1, 2};
    mesh1.materialIndex = 1;
    scene.AddMesh(mesh1);

    GPUSceneData gpu = BuildGPUSceneData(scene);

    CHECK(gpu.meshes.size() == 2);
    CHECK(gpu.materials.size() == 2);
    CHECK(gpu.meshes[0].materialIndex == 0);
    CHECK(gpu.meshes[1].materialIndex == 1);

    // Material 0: red, non-metallic
    CHECK(gpu.materials[0].baseColor_metallic.x == 1.0f);
    CHECK(gpu.materials[0].baseColor_metallic.w == 0.0f);

    // Material 1: blue, metallic
    CHECK(gpu.materials[1].baseColor_metallic.z == 1.0f);
    CHECK(gpu.materials[1].baseColor_metallic.w == 1.0f);
}

TEST_CASE("BuildGPUSceneData handles mesh with invalid materialIndex")
{
    Scene scene;
    scene.AddMaterial(SceneMaterial{});

    SceneMesh mesh;
    mesh.hasGeometry = true;
    mesh.vertices = {0, 0, 0,  1, 0, 0,  0, 1, 0};
    mesh.indices = {0, 1, 2};
    mesh.materialIndex = 99; // out of range
    scene.AddMesh(mesh);

    GPUSceneData gpu = BuildGPUSceneData(scene);

    // Should clamp to material 0 (fallback)
    CHECK(gpu.meshes[0].materialIndex == 0);
}

TEST_CASE("BuildGPUSceneData skips meshes without geometry")
{
    Scene scene;
    scene.AddMaterial(SceneMaterial{});

    SceneMesh meshNoGeo;
    meshNoGeo.hasGeometry = false;
    meshNoGeo.materialIndex = 0;
    scene.AddMesh(meshNoGeo);

    SceneMesh meshWithGeo;
    meshWithGeo.hasGeometry = true;
    meshWithGeo.vertices = {0, 0, 0,  1, 0, 0,  0, 1, 0};
    meshWithGeo.indices = {0, 1, 2};
    meshWithGeo.materialIndex = 0;
    scene.AddMesh(meshWithGeo);

    GPUSceneData gpu = BuildGPUSceneData(scene);

    CHECK(gpu.meshes.size() == 1);
}