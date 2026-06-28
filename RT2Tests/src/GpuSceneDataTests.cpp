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
    CHECK(mat.alphaCutoff == 0.5f);
    CHECK(mat.alphaMode == 0.0f); // OPAQUE
    CHECK(mat.baseAlpha == 1.0f);
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

// ============================================================================
// GPUTriangleLight struct layout tests
// ============================================================================

TEST_CASE("GPUTriangleLight is 32 bytes (vec4 + uvec4, std430)")
{
    CHECK(sizeof(GPUTriangleLight) == 32);
}

TEST_CASE("GPUTriangleLight default values")
{
    GPUTriangleLight light;
    CHECK(light.emission_area == glm::vec4(0.0f));
    CHECK(light.ids == glm::uvec4(0, 0, 0, 0xFFFFFFFFu));
}

// ============================================================================
// Light list building tests (NEE Phase 1)
// ============================================================================

TEST_CASE("BuildGPUSceneData finds no lights when no emissive materials")
{
    Scene scene;

    SceneMaterial mat;
    mat.emissiveColor = {0.0f, 0.0f, 0.0f};
    mat.emissiveIntensity = 0.0f;
    scene.AddMaterial(mat);

    SceneMesh mesh;
    mesh.hasGeometry = true;
    mesh.vertices = {0, 0, 0,  1, 0, 0,  0, 1, 0};
    mesh.indices = {0, 1, 2};
    mesh.materialIndex = 0;
    scene.AddMesh(mesh);

    GPUSceneData gpu = BuildGPUSceneData(scene);

    CHECK(gpu.lights.empty());
    CHECK(gpu.totalLightArea == 0.0f);
}

TEST_CASE("BuildGPUSceneData finds one light per emissive triangle")
{
    Scene scene;

    SceneMaterial mat;
    mat.emissiveColor = {1.0f, 0.8f, 0.4f};
    mat.emissiveIntensity = 5.0f;
    scene.AddMaterial(mat);

    // 2 triangles (a quad split into 2 tris)
    SceneMesh mesh;
    mesh.hasGeometry = true;
    mesh.vertices = {0, 0, 0,  1, 0, 0,  1, 1, 0,  0, 1, 0};
    mesh.indices = {0, 1, 2,  0, 2, 3};
    mesh.materialIndex = 0;
    scene.AddMesh(mesh);

    GPUSceneData gpu = BuildGPUSceneData(scene);

    CHECK(gpu.lights.size() == 2);
    CHECK(gpu.totalLightArea == doctest::Approx(1.0f).epsilon(0.001f));  // unit square = area 1
}

TEST_CASE("GPUTriangleLight stores correct ids and emission")
{
    Scene scene;

    SceneMaterial mat;
    mat.emissiveColor = {1.0f, 0.5f, 0.2f};
    mat.emissiveIntensity = 3.0f;
    scene.AddMaterial(mat);

    SceneMesh mesh;
    mesh.hasGeometry = true;
    mesh.vertices = {0, 0, 0,  2, 0, 0,  0, 2, 0};
    mesh.indices = {0, 1, 2};
    mesh.materialIndex = 0;
    scene.AddMesh(mesh);

    GPUSceneData gpu = BuildGPUSceneData(scene);

    REQUIRE(gpu.lights.size() == 1);
    const auto& light = gpu.lights[0];

    // Emission = emissiveColor * intensity
    CHECK(light.emission_area.x == doctest::Approx(3.0f));
    CHECK(light.emission_area.y == doctest::Approx(1.5f));
    CHECK(light.emission_area.z == doctest::Approx(0.6f));

    // Area = 0.5 * |edge1 x edge2| = 0.5 * |(2,0,0) x (0,2,0)| = 0.5 * |(0,0,4)| = 2
    CHECK(light.emission_area.w == doctest::Approx(2.0f).epsilon(0.001f));

    // ids: instanceID=0, primitiveID=0, materialIndex=0, emissiveTexIdx=0xFFFFFFFF (no texture)
    CHECK(light.ids.x == 0);
    CHECK(light.ids.y == 0);
    CHECK(light.ids.z == 0);
    CHECK(light.ids.w == 0xFFFFFFFFu);
}

TEST_CASE("BuildGPUSceneData light ids map to correct instanceID and primitiveID")
{
    Scene scene;

    // Material 0: non-emissive, Material 1: emissive
    SceneMaterial mat0;
    scene.AddMaterial(mat0);

    SceneMaterial mat1;
    mat1.emissiveColor = {1.0f, 1.0f, 1.0f};
    mat1.emissiveIntensity = 2.0f;
    scene.AddMaterial(mat1);

    // Mesh 0: non-emissive (instanceID 0)
    SceneMesh mesh0;
    mesh0.hasGeometry = true;
    mesh0.vertices = {0, 0, 0,  1, 0, 0,  0, 1, 0};
    mesh0.indices = {0, 1, 2};
    mesh0.materialIndex = 0;
    scene.AddMesh(mesh0);

    // Mesh 1: emissive with 3 triangles (instanceID 1)
    SceneMesh mesh1;
    mesh1.hasGeometry = true;
    mesh1.vertices = {0, 0, 0,  1, 0, 0,  0, 1, 0,
                      1, 0, 0,  1, 1, 0,  0, 1, 0,
                      0, 0, 0,  1, 0, 0,  1, 1, 0};
    mesh1.indices = {0, 1, 2,  3, 4, 5,  6, 7, 8};
    mesh1.materialIndex = 1;
    scene.AddMesh(mesh1);

    GPUSceneData gpu = BuildGPUSceneData(scene);

    CHECK(gpu.lights.size() == 3);
    CHECK(gpu.meshes.size() == 2);

    // All lights should have instanceID=1 (second mesh), materialIndex=1
    for (uint32_t i = 0; i < gpu.lights.size(); i++)
    {
        CHECK(gpu.lights[i].ids.x == 1);  // instanceID
        CHECK(gpu.lights[i].ids.z == 1);  // materialIndex
        CHECK(gpu.lights[i].ids.y == i);  // primitiveID = triangle index within BLAS
    }
}

TEST_CASE("BuildGPUSceneData totalLightArea sums all light areas")
{
    Scene scene;

    SceneMaterial mat;
    mat.emissiveColor = {1.0f, 1.0f, 1.0f};
    mat.emissiveIntensity = 1.0f;
    scene.AddMaterial(mat);

    // Two emissive meshes with different areas
    // Mesh 0: area 2 (triangle (0,0,0),(2,0,0),(0,2,0))
    SceneMesh mesh0;
    mesh0.hasGeometry = true;
    mesh0.vertices = {0, 0, 0,  2, 0, 0,  0, 2, 0};
    mesh0.indices = {0, 1, 2};
    mesh0.materialIndex = 0;
    scene.AddMesh(mesh0);

    // Mesh 1: area 0.5 (triangle (0,0,0),(1,0,0),(0,1,0))
    SceneMesh mesh1;
    mesh1.hasGeometry = true;
    mesh1.vertices = {0, 0, 0,  1, 0, 0,  0, 1, 0};
    mesh1.indices = {0, 1, 2};
    mesh1.materialIndex = 0;
    scene.AddMesh(mesh1);

    GPUSceneData gpu = BuildGPUSceneData(scene);

    CHECK(gpu.lights.size() == 2);
    CHECK(gpu.totalLightArea == doctest::Approx(2.5f).epsilon(0.001f));
}

TEST_CASE("BuildGPUSceneData light stores emissiveTexIdx when material has emissive texture")
{
    Scene scene;

    SceneMaterial mat;
    mat.emissiveColor = {1.0f, 1.0f, 1.0f};
    mat.emissiveIntensity = 2.0f;
    mat.emissiveTextureIndex = 5;
    scene.AddMaterial(mat);

    SceneMesh mesh;
    mesh.hasGeometry = true;
    mesh.vertices = {0, 0, 0,  1, 0, 0,  0, 1, 0};
    mesh.indices = {0, 1, 2};
    mesh.materialIndex = 0;
    scene.AddMesh(mesh);

    GPUSceneData gpu = BuildGPUSceneData(scene);

    REQUIRE(gpu.lights.size() == 1);
    CHECK(gpu.lights[0].ids.w == 5u);  // emissiveTexIdx
}

// ============================================================================
// Alpha mode / translucency tests (M6 Phase 1)
// ============================================================================

TEST_CASE("GPUMaterial alphaMode defaults to OPAQUE (0.0)")
{
    GPUMaterial mat;
    CHECK(mat.alphaMode == 0.0f);
    CHECK(mat.alphaCutoff == 0.5f);
}

TEST_CASE("GPUMaterial fromSceneMaterial converts alphaMode MASK")
{
    SceneMaterial sm;
    sm.alphaMode = "MASK";
    sm.alphaCutoff = 0.3f;

    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);

    CHECK(gm.alphaMode == 1.0f);
    CHECK(gm.alphaCutoff == 0.3f);
}

TEST_CASE("GPUMaterial fromSceneMaterial converts alphaMode BLEND")
{
    SceneMaterial sm;
    sm.alphaMode = "BLEND";

    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);

    CHECK(gm.alphaMode == 2.0f);
}

TEST_CASE("GPUMaterial fromSceneMaterial converts alphaMode OPAQUE")
{
    SceneMaterial sm;
    sm.alphaMode = "OPAQUE";

    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);

    CHECK(gm.alphaMode == 0.0f);
}

TEST_CASE("GPUMaterial is still 64 bytes with alpha fields")
{
    CHECK(sizeof(GPUMaterial) == 64);
}

// ============================================================================
// M7: Dielectric transmission — KHR_materials_transmission import tests
// ============================================================================

TEST_CASE("Transmission material stores transmissionFactor (not in baseAlpha)")
{
    // M7 fix: transmissionFactor is separate from baseAlpha (opacity).
    SceneMaterial sm;
    sm.alphaMode = "OPAQUE";
    sm.transmissionFactor = 0.85f;
    sm.baseAlpha = 1.0f;  // fully opaque (no alpha transparency)
    sm.ior = 1.5f;

    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);

    CHECK(gm.alphaMode == 0.0f);       // OPAQUE
    CHECK(gm.baseAlpha == 1.0f);      // opacity unchanged
    CHECK(gm.ior == 1.5f);
    // transmissionFactor packed into textureIndices.w
    CHECK(glm::intBitsToFloat(gm.textureIndices.w) == 0.85f);
}

TEST_CASE("Transmission material with transmissionFactor=1 is fully transmissive")
{
    SceneMaterial sm;
    sm.transmissionFactor = 1.0f;  // 100% transmission
    sm.baseAlpha = 1.0f;           // still opaque for alpha purposes
    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);
    CHECK(glm::intBitsToFloat(gm.textureIndices.w) == 1.0f);
    CHECK(gm.baseAlpha == 1.0f);  // opacity unchanged
    CHECK(gm.alphaMode == 0.0f);  // OPAQUE
}

TEST_CASE("Opaque material with transmissionFactor=0 is not transmissive")
{
    SceneMaterial sm;
    sm.transmissionFactor = 0.0f;
    sm.baseAlpha = 1.0f;
    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);
    CHECK(glm::intBitsToFloat(gm.textureIndices.w) == 0.0f);
    CHECK(gm.baseAlpha == 1.0f);
    CHECK(gm.alphaMode == 0.0f);
}

TEST_CASE("Alpha-blended material does NOT trigger transmission")
{
    // A BLEND alpha material (e.g. bottle) should NOT enter the refraction branch.
    // baseAlpha < 1 here means partial opacity, NOT physical transmission.
    SceneMaterial sm;
    sm.alphaMode = "BLEND";
    sm.baseAlpha = 0.5f;        // 50% transparent (alpha opacity)
    sm.transmissionFactor = 0.0f;  // NOT physical glass
    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);
    CHECK(gm.alphaMode == 2.0f);  // BLEND
    CHECK(gm.baseAlpha == 0.5f); // opacity
    CHECK(glm::intBitsToFloat(gm.textureIndices.w) == 0.0f);  // no transmission
}