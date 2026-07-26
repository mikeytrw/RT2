#include <doctest/doctest.h>

#include "SceneTypes.h"
#include "ECSScene.h"
#include "ECSComponents.h"
#include "SceneLoader.h"
#include "SceneLoaderTestSupport.h"
#include "json.hpp"
#include <glm/glm.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cmath>

namespace fs = std::filesystem;

// ============================================================================
// glTF Serialization Round-Trip Tests (RED phase)
// ============================================================================
// These tests verify that Scene data can be saved to a .gltf file and loaded
// back with all values preserved. We use temp files for isolation.
// ============================================================================

static const char* TEST_FILE = "test_scene.gltf";
static const char* TEST_FILE_GLB = "test_scene.glb";

static SceneTexture MakeTextureRef(const std::string& path)
{
    SceneTexture texture;
    texture.ref.kind = AssetKind::Texture;
    texture.ref.path = path;
    return texture;
}

static void cleanupObjTestFiles()
{
    fs::remove("test_scene.obj");
    fs::remove("test_scene.mtl");
	fs::remove("test_diffuse.ppm");
	fs::remove("test_normal.ppm");
	fs::remove("test_emissive.ppm");
	fs::remove("test_roughness.ppm");
}

static void writeObjTestTexture(const char* path, unsigned char value)
{
	std::ofstream image(path, std::ios::binary);
	image << "P6\n1 1\n255\n";
	const unsigned char pixel[3] = {value, value, value};
	image.write(reinterpret_cast<const char*>(pixel), sizeof(pixel));
}

TEST_CASE("OBJ import flips V coordinates and converts legacy MTL shininess")
{
    cleanupObjTestFiles();
    {
        std::ofstream mtl("test_scene.mtl");
        mtl << "newmtl concrete\n"
               "Kd 0.5 0.5 0.5\n"
               "Ns 16\n"
               "illum 2\n";
    }
    {
        std::ofstream obj("test_scene.obj");
        obj << "mtllib test_scene.mtl\n"
               "v 0 0 0\n"
               "v 1 0 0\n"
               "v 0 1 0\n"
               "vt 0.25 0.20\n"
               "vt 0.75 0.20\n"
               "vt 0.25 0.90\n"
               "usemtl concrete\n"
               "f 1/1 2/2 3/3\n";
    }

    ECSScene scene;
    REQUIRE(LoadObjForTest(scene, RepositoryRootForSceneLoaderTests(), "test_scene.obj"));
    REQUIRE(scene.meshRegistry.GetCount() == 1);
    const auto& mesh = scene.meshRegistry.GetMesh(0);
    REQUIRE(mesh.uvs.size() == 6);
    CHECK(mesh.uvs[0] == doctest::Approx(0.25f));
    CHECK(mesh.uvs[1] == doctest::Approx(0.80f));
    CHECK(mesh.uvs[5] == doctest::Approx(0.10f));

    REQUIRE(scene.materials.size() == 1);
    const float expectedRoughness = std::sqrt(std::sqrt(2.0f / 18.0f));
    CHECK(scene.materials[0].roughness == doctest::Approx(expectedRoughness));
    cleanupObjTestFiles();
}

TEST_CASE("OBJ import preserves shared indexed vertices")
{
    cleanupObjTestFiles();
    {
        std::ofstream obj("test_scene.obj");
        obj << "v 0 0 0\n"
               "v 1 0 0\n"
               "v 1 1 0\n"
               "v 0 1 0\n"
               "vt 0 0\n"
               "vt 1 0\n"
               "vt 1 1\n"
               "vt 0 1\n"
               "f 1/1 2/2 3/3\n"
               "f 1/1 3/3 4/4\n";
    }

    ECSScene scene;
    REQUIRE(LoadObjForTest(scene, RepositoryRootForSceneLoaderTests(), "test_scene.obj"));
    REQUIRE(scene.meshRegistry.GetCount() == 1);
    const auto& mesh = scene.meshRegistry.GetMesh(0);
    CHECK(mesh.vertices.size() / 3 == 4);
    REQUIRE(mesh.indices.size() == 6);
    CHECK(mesh.indices[0] == mesh.indices[3]);
    CHECK(mesh.indices[2] == mesh.indices[4]);
    CHECK(mesh.materialIndices.size() == 2);
    cleanupObjTestFiles();
}

TEST_CASE("OBJ import classifies color and data texture color spaces")
{
	cleanupObjTestFiles();
	writeObjTestTexture("test_diffuse.ppm", 128);
	writeObjTestTexture("test_normal.ppm", 128);
	writeObjTestTexture("test_emissive.ppm", 128);
	writeObjTestTexture("test_roughness.ppm", 128);

	{
		std::ofstream mtl("test_scene.mtl");
		mtl << "newmtl textured\n"
		       "Kd 1 1 1\n"
		       "Ke 1 1 1\n"
		       "map_Kd test_diffuse.ppm\n"
		       "norm test_normal.ppm\n"
		       "map_Ke test_emissive.ppm\n"
		       "map_Pr test_roughness.ppm\n";
	}
	{
		std::ofstream obj("test_scene.obj");
		obj << "mtllib test_scene.mtl\n"
		       "v 0 0 0\n"
		       "v 1 0 0\n"
		       "v 0 1 0\n"
		       "vt 0 0\n"
		       "vt 1 0\n"
		       "vt 0 1\n"
		       "usemtl textured\n"
		       "f 1/1 2/2 3/3\n";
	}

	ECSScene scene;
	REQUIRE(LoadObjForTest(scene, RepositoryRootForSceneLoaderTests(), "test_scene.obj"));
	REQUIRE(scene.materials.size() == 1);
	const SceneMaterial& material = scene.materials[0];
	REQUIRE(material.baseColorTextureIndex >= 0);
	REQUIRE(material.normalTextureIndex >= 0);
	REQUIRE(material.emissiveTextureIndex >= 0);
	REQUIRE(material.metallicRoughnessTextureIndex >= 0);
	CHECK(scene.textures[material.baseColorTextureIndex].isSRGB);
	CHECK_FALSE(scene.textures[material.normalTextureIndex].isSRGB);
	CHECK(scene.textures[material.emissiveTextureIndex].isSRGB);
	CHECK_FALSE(scene.textures[material.metallicRoughnessTextureIndex].isSRGB);
	cleanupObjTestFiles();
}

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
    ECSScene scene;
    REQUIRE(SceneLoader::Save(scene, TEST_FILE));
    ECSScene loaded;
    REQUIRE(LoadGltfForTest(loaded, RepositoryRootForSceneLoaderTests(), TEST_FILE));
    CHECK(loaded.meshRegistry.GetCount() == 0);
    CHECK(loaded.lights.empty());
    CHECK(loaded.textures.empty());
    cleanupTestFiles();
}

// --- Mesh round-trip ---

TEST_CASE("Mesh round-trips through glTF")
{
    cleanupTestFiles();
    ECSScene scene;

    MeshData meshData;
    meshData.vertices = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    meshData.indices = {0, 1, 2};
    uint32_t meshIdx = scene.meshRegistry.AddMesh(std::move(meshData));

    auto entity = scene.registry.create();
    auto& tf = scene.registry.emplace<Transform>(entity);
    tf.translation = {1.5f, 2.0f, -3.0f};
    tf.scale = {0.01f, 0.01f, 0.01f};
    auto& ref = scene.registry.emplace<MeshRef>(entity);
    ref.meshIndex = meshIdx;
    ref.materialIndex = 1;

    scene.materials.push_back(SceneMaterial{});
    scene.materials.push_back(SceneMaterial{});

    REQUIRE(SceneLoader::Save(scene, TEST_FILE));
    ECSScene loaded;
    REQUIRE(LoadGltfForTest(loaded, RepositoryRootForSceneLoaderTests(), TEST_FILE));

    auto meshView = loaded.registry.view<MeshRef, Transform>();
    size_t meshCount = std::distance(meshView.begin(), meshView.end());
    REQUIRE(meshCount == 1);
    auto loadedEntity = *meshView.begin();
    auto& loadedTf = meshView.get<Transform>(loadedEntity);
    auto& loadedRef = meshView.get<MeshRef>(loadedEntity);

    CHECK(loadedTf.translation == glm::vec3(1.5f, 2.0f, -3.0f));
    CHECK(loadedTf.scale.x == doctest::Approx(0.01f).epsilon(0.001));
    CHECK(loadedRef.materialIndex == 1);
    CHECK(loaded.meshRegistry.GetCount() >= 1);
    CHECK(loaded.meshRegistry.GetMesh(0).vertices.size() == 9);
    cleanupTestFiles();
}

// --- Material round-trip ---

TEST_CASE("Material round-trips through glTF")
{
    cleanupTestFiles();
    ECSScene scene;
    SceneMaterial mat;
    mat.type = MaterialType::Metal;
    mat.baseColor = {0.9f, 0.6f, 0.3f};
    mat.metallic = 0.8f;
    mat.roughness = 0.15f;
    mat.ior = 1.45f;
    mat.emissiveColor = {0.1f, 0.0f, 0.0f};
    mat.emissiveIntensity = 0.0f;
    mat.baseColorTextureIndex = 0;
    scene.materials.push_back(mat);

    scene.textures.push_back(MakeTextureRef("textures/albedo.png"));

    REQUIRE(SceneLoader::Save(scene, TEST_FILE));
    ECSScene loaded;
    REQUIRE(LoadGltfForTest(loaded, RepositoryRootForSceneLoaderTests(), TEST_FILE));
    REQUIRE(loaded.materials.size() >= 1);
    const auto& m = loaded.materials[0];
    CHECK(m.type == MaterialType::Metal);
    CHECK(m.baseColor == glm::vec3(0.9f, 0.6f, 0.3f));
    CHECK(m.metallic == doctest::Approx(0.8f).epsilon(0.001));
    CHECK(m.roughness == doctest::Approx(0.15f).epsilon(0.001));
    CHECK(m.ior == doctest::Approx(1.45f).epsilon(0.001));
    CHECK(m.baseColorTextureIndex == 0);
    cleanupTestFiles();
}

// --- Emissive material round-trip ---

TEST_CASE("Emissive material round-trips through glTF")
{
    cleanupTestFiles();
    ECSScene scene;
    SceneMaterial mat;
    mat.type = MaterialType::Emissive;
    mat.emissiveColor = {1.0f, 0.8f, 0.4f};
    mat.emissiveIntensity = 5.0f;
    scene.materials.push_back(mat);

    REQUIRE(SceneLoader::Save(scene, TEST_FILE));
    ECSScene loaded;
    REQUIRE(LoadGltfForTest(loaded, RepositoryRootForSceneLoaderTests(), TEST_FILE));
    REQUIRE(loaded.materials.size() >= 1);
    const auto& m = loaded.materials[0];
    CHECK(m.type == MaterialType::Emissive);
    CHECK(m.emissiveColor == glm::vec3(1.0f, 0.8f, 0.4f));
    CHECK(m.emissiveIntensity == doctest::Approx(5.0f).epsilon(0.001));
    cleanupTestFiles();
}

// --- Light round-trip ---

TEST_CASE("Point light round-trips through glTF")
{
    cleanupTestFiles();
    ECSScene scene;
    SceneLight light;
    light.type = LightType::Point;
    light.position = {5.0f, 10.0f, 0.0f};
    light.color = {1.0f, 0.5f, 0.2f};
    light.intensity = 50.0f;
    light.range = 30.0f;
    scene.lights.push_back(light);

    REQUIRE(SceneLoader::Save(scene, TEST_FILE));
    ECSScene loaded;
    REQUIRE(LoadGltfForTest(loaded, RepositoryRootForSceneLoaderTests(), TEST_FILE));
    REQUIRE(loaded.lights.size() >= 1);
    const auto& l = loaded.lights[0];
    CHECK(l.type == LightType::Point);
    CHECK(l.position == glm::vec3(5.0f, 10.0f, 0.0f));
    CHECK(l.color == glm::vec3(1.0f, 0.5f, 0.2f));
    CHECK(l.intensity == doctest::Approx(50.0f).epsilon(0.001));
    CHECK(l.range == doctest::Approx(30.0f).epsilon(0.001));
    cleanupTestFiles();
}

TEST_CASE("Spot light round-trips through glTF")
{
    cleanupTestFiles();
    ECSScene scene;
    SceneLight light;
    light.type = LightType::Spot;
    light.position = {0.0f, 5.0f, 0.0f};
    light.direction = {0.0f, -1.0f, 0.0f};
    light.color = {1.0f, 1.0f, 0.9f};
    light.intensity = 20.0f;
    light.innerConeAngle = 25.0f;
    light.outerConeAngle = 40.0f;
    scene.lights.push_back(light);

    REQUIRE(SceneLoader::Save(scene, TEST_FILE));
    ECSScene loaded;
    REQUIRE(LoadGltfForTest(loaded, RepositoryRootForSceneLoaderTests(), TEST_FILE));
    REQUIRE(loaded.lights.size() >= 1);
    const auto& l = loaded.lights[0];
    CHECK(l.type == LightType::Spot);
    CHECK(l.direction == glm::vec3(0.0f, -1.0f, 0.0f));
    CHECK(l.innerConeAngle == doctest::Approx(25.0f).epsilon(0.001));
    CHECK(l.outerConeAngle == doctest::Approx(40.0f).epsilon(0.001));
    cleanupTestFiles();
}

// --- Texture round-trip ---

TEST_CASE("Texture round-trips through glTF")
{
    cleanupTestFiles();
    ECSScene scene;
    scene.textures.push_back(MakeTextureRef("textures/albedo.png"));
    scene.textures.push_back(MakeTextureRef("textures/normal.png"));

    REQUIRE(SceneLoader::Save(scene, TEST_FILE));
    ECSScene loaded;
    REQUIRE(LoadGltfForTest(loaded, RepositoryRootForSceneLoaderTests(), TEST_FILE));
    REQUIRE(loaded.textures.size() == 2);
    CHECK(loaded.textures[0].ref.path == "textures/albedo.png");
    CHECK(loaded.textures[1].ref.path == "textures/normal.png");
    cleanupTestFiles();
}

// --- Camera round-trip ---

TEST_CASE("Camera round-trips through glTF")
{
    cleanupTestFiles();
    ECSScene scene;
    scene.camera.position = {3.0f, 4.0f, 5.0f};
    scene.camera.forwardDirection = {0.0f, -0.5f, -1.0f};
    scene.camera.verticalFOV = 60.0f;
    scene.camera.aperture = 0.1f;
    scene.camera.focusDistance = 5.0f;

    REQUIRE(SceneLoader::Save(scene, TEST_FILE));
    ECSScene loaded;
    REQUIRE(LoadGltfForTest(loaded, RepositoryRootForSceneLoaderTests(), TEST_FILE));
    const auto& cam = loaded.camera;
    CHECK(cam.position == glm::vec3(3.0f, 4.0f, 5.0f));
    CHECK(cam.forwardDirection == glm::vec3(0.0f, -0.5f, -1.0f));
    CHECK(cam.verticalFOV == doctest::Approx(60.0f).epsilon(0.001));
    CHECK(cam.aperture == doctest::Approx(0.1f).epsilon(0.001));
    CHECK(cam.focusDistance == doctest::Approx(5.0f).epsilon(0.001));
    cleanupTestFiles();
}

// --- Full scene round-trip ---

TEST_CASE("Full scene with multiple meshes, materials, lights round-trips")
{
    cleanupTestFiles();
    ECSScene scene;

    scene.textures.push_back(MakeTextureRef("textures/albedo.png"));
    scene.textures.push_back(MakeTextureRef("textures/normal.png"));

    SceneMaterial mat0;
    mat0.type = MaterialType::Lambertian;
    mat0.baseColor = {0.5f, 0.5f, 0.5f};
    scene.materials.push_back(mat0);

    SceneMaterial mat1;
    mat1.type = MaterialType::Metal;
    mat1.baseColor = {0.8f, 0.8f, 0.9f};
    mat1.metallic = 1.0f;
    mat1.roughness = 0.1f;
    mat1.baseColorTextureIndex = 0;
    scene.materials.push_back(mat1);

    SceneMaterial mat2;
    mat2.type = MaterialType::Emissive;
    mat2.emissiveColor = {1.0f, 1.0f, 0.9f};
    mat2.emissiveIntensity = 10.0f;
    scene.materials.push_back(mat2);

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
        auto& tf = scene.registry.emplace<Transform>(entity);
        tf.translation = {0.0f, 0.0f, 0.0f};
        auto& ref = scene.registry.emplace<MeshRef>(entity);
        ref.meshIndex = meshIdx0;
        ref.materialIndex = 0;
    }
    {
        auto entity = scene.registry.create();
        auto& tf = scene.registry.emplace<Transform>(entity);
        tf.translation = {0.0f, 0.0f, 0.0f};
        tf.scale = {0.01f, 0.01f, 0.01f};
        auto& ref = scene.registry.emplace<MeshRef>(entity);
        ref.meshIndex = meshIdx1;
        ref.materialIndex = 1;
    }

    SceneLight light;
    light.type = LightType::Point;
    light.position = {5.0f, 10.0f, 5.0f};
    light.intensity = 30.0f;
    scene.lights.push_back(light);

    scene.camera.position = {2.0f, 2.0f, 8.0f};
    scene.camera.verticalFOV = 50.0f;

    REQUIRE(SceneLoader::Save(scene, TEST_FILE));
    ECSScene loaded;
    REQUIRE(LoadGltfForTest(loaded, RepositoryRootForSceneLoaderTests(), TEST_FILE));

    REQUIRE(loaded.textures.size() == 2);
    CHECK(loaded.textures[0].ref.path == "textures/albedo.png");

    REQUIRE(loaded.materials.size() >= 3);
    CHECK(loaded.materials[0].type == MaterialType::Lambertian);
    CHECK(loaded.materials[1].type == MaterialType::Metal);
    CHECK(loaded.materials[1].baseColorTextureIndex == 0);
    CHECK(loaded.materials[2].type == MaterialType::Emissive);
    CHECK(loaded.materials[2].emissiveIntensity == doctest::Approx(10.0f).epsilon(0.001));

    auto meshView = loaded.registry.view<MeshRef, Transform>();
    size_t meshCount = std::distance(meshView.begin(), meshView.end());
    REQUIRE(meshCount == 2);
    bool foundScale001 = false;
    for (auto e : meshView)
    {
        const auto& ref = meshView.get<MeshRef>(e);
        const auto& tf = meshView.get<Transform>(e);
        if (ref.materialIndex == 1)
        {
            CHECK(tf.scale.x == doctest::Approx(0.01f).epsilon(0.001));
            foundScale001 = true;
        }
    }
    CHECK(foundScale001);

    REQUIRE(loaded.lights.size() >= 1);
    CHECK(loaded.lights[0].type == LightType::Point);
    CHECK(loaded.lights[0].position == glm::vec3(5.0f, 10.0f, 5.0f));

    CHECK(loaded.camera.position == glm::vec3(2.0f, 2.0f, 8.0f));
    CHECK(loaded.camera.verticalFOV == doctest::Approx(50.0f).epsilon(0.001));

    cleanupTestFiles();
}

// --- GLB format ---

TEST_CASE("Scene saves and loads as GLB (binary glTF)")
{
    cleanupTestFiles();
    ECSScene scene;
    scene.materials.push_back({});
    SceneMaterial mat;
    mat.baseColor = {0.2f, 0.4f, 0.6f};
    scene.materials.push_back(mat);

    REQUIRE(SceneLoader::Save(scene, TEST_FILE_GLB));
    ECSScene loaded;
    REQUIRE(LoadGltfForTest(loaded, RepositoryRootForSceneLoaderTests(), TEST_FILE_GLB));
    REQUIRE(loaded.materials.size() >= 2);
    CHECK(loaded.materials[1].baseColor == glm::vec3(0.2f, 0.4f, 0.6f));
    cleanupTestFiles();
}

// --- Non-existent file returns false ---

TEST_CASE("Load returns false for non-existent file")
{
    ECSScene loaded;
    CHECK_FALSE(LoadGltfForTest(loaded, RepositoryRootForSceneLoaderTests(), "does_not_exist.gltf"));
}

TEST_CASE("Phase7 W3 step 7.4: glTF export relativizes texture refs")
{
    const auto root = fs::temp_directory_path() /
        "rt2_step7_gltf_portable";
    const auto output = root / "scene.gltf";
    const auto source = root / "textures" / "albedo.png";
    fs::create_directories(source.parent_path());

    ECSScene scene;
    scene.textures.push_back(MakeTextureRef(source.generic_string()));
    REQUIRE(SceneLoader::Save(scene, output.string()));

    nlohmann::json saved;
    { std::ifstream in(output); in >> saved; }
    REQUIRE(saved["images"].size() == 1);
    CHECK(saved["images"][0]["uri"] == "textures/albedo.png");
    fs::remove_all(root);
}

#ifdef _WIN32
TEST_CASE("Phase7 W3 step 7.4: glTF export rejects cross-volume texture refs")
{
    const auto root = fs::temp_directory_path() /
        "rt2_step7_gltf_nonportable";
    const auto output = root / "scene.gltf";
    fs::create_directories(root);
    { std::ofstream out(output, std::ios::binary); out << "sentinel"; }

    ECSScene scene;
    scene.textures.push_back(MakeTextureRef("Z:/external/albedo.png"));
    CHECK_FALSE(SceneLoader::Save(scene, output.string()));
    std::ifstream in(output, std::ios::binary);
    CHECK(std::string((std::istreambuf_iterator<char>(in)), {}) ==
          "sentinel");
    in.close();
    fs::remove_all(root);
}
#endif

// --- Save returns false for invalid path ---

TEST_CASE("Save returns false for invalid path")
{
    ECSScene scene;
    CHECK_FALSE(SceneLoader::Save(scene, "Z:/nonexistent_dir/scene.gltf"));
}

// ============================================================================
// Phase 8 step 3 — KHR_lights_punctual reaches ImportIntoECS, not just Load
// ============================================================================

TEST_CASE("Phase8 step 3: Load and Import agree on KHR_lights_punctual")
{
    // Before this step, ImportIntoECS never parsed the extension, so importing
    // a glTF dropped every light without a diagnostic. This asserts the two
    // entry points agree, so the gap cannot silently reopen.
    const auto dir = fs::temp_directory_path() / "rt2_p8_step3";
    fs::create_directories(dir);
    const auto target = dir / "lights.gltf";

    // Author one light of each type, then round-trip through Save, which
    // emits KHR_lights_punctual from ecsScene.lights.
    ECSScene authored;
    {
        SceneLight point;
        point.type = LightType::Point;
        point.color = {1.0f, 0.5f, 0.25f};
        point.intensity = 7.5f;
        point.range = 12.0f;
        authored.lights.push_back(point);

        SceneLight spot;
        spot.type = LightType::Spot;
        spot.color = {0.25f, 0.5f, 1.0f};
        spot.intensity = 3.5f;
        spot.innerConeAngle = 14.0f;
        spot.outerConeAngle = 28.0f;
        authored.lights.push_back(spot);

        SceneLight directional;
        directional.type = LightType::Directional;
        directional.color = {1.0f, 1.0f, 0.9f};
        directional.intensity = 2.0f;
        authored.lights.push_back(directional);
    }
    REQUIRE(SceneLoader::Save(authored, target.string()));

    auto checkLights = [](const ECSScene& scene, const char* which)
    {
        INFO("entry point: " << which);
        REQUIRE(scene.lights.size() == 3);

        CHECK(scene.lights[0].type == LightType::Point);
        CHECK(scene.lights[0].intensity == doctest::Approx(7.5f));
        CHECK(scene.lights[0].range == doctest::Approx(12.0f));

        CHECK(scene.lights[1].type == LightType::Spot);
        CHECK(scene.lights[1].intensity == doctest::Approx(3.5f));
        CHECK(scene.lights[1].innerConeAngle == doctest::Approx(14.0f));
        CHECK(scene.lights[1].outerConeAngle == doctest::Approx(28.0f));

        // Directional is the value that silently became Point while the
        // parser mapped only "spot" and defaulted everything else.
        CHECK(scene.lights[2].type == LightType::Directional);
        CHECK(scene.lights[2].intensity == doctest::Approx(2.0f));
    };

    std::vector<rt2::core::AssetDiagnostic> diagnostics;

    ECSScene loaded;
    REQUIRE(LoadGltfForTest(loaded, dir, target, diagnostics));
    checkLights(loaded, "LoadIntoECS");

    ECSScene imported;
    const auto context = MakeSceneLoaderTestContext(dir, target);
    // Extra parens: doctest's expression decomposition makes `!= entt::null`
    // ambiguous (C2593) without them.
    REQUIRE((SceneLoader::ImportIntoECS(imported, context, diagnostics) != entt::null));
    checkLights(imported, "ImportIntoECS");

    // The two entry points must not disagree — that divergence is the bug.
    REQUIRE(loaded.lights.size() == imported.lights.size());
    for (size_t i = 0; i < loaded.lights.size(); ++i)
    {
        INFO("light index " << i);
        CHECK(loaded.lights[i].type == imported.lights[i].type);
        CHECK(loaded.lights[i].intensity == doctest::Approx(imported.lights[i].intensity));
        CHECK(loaded.lights[i].innerConeAngle == doctest::Approx(imported.lights[i].innerConeAngle));
        CHECK(loaded.lights[i].outerConeAngle == doctest::Approx(imported.lights[i].outerConeAngle));
    }

    fs::remove(target);
}
