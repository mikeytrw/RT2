#include <doctest/doctest.h>

#include "ECSComponents.h"
#include "SceneLoader.h"
#include "SceneLoaderTestSupport.h"
#include "SceneManager.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ============================================================================
// Reported repro: import model A, delete its entity, import model B, and B
// renders with A's textures.
//
// Everything here stays on the ECS side -- import, delete, compact, import --
// so if the texture indices come out wrong the bug is in the scene's own
// bookkeeping. If they come out right, the mis-binding is downstream in the
// GPU texture array, and this file has ruled out the half that is cheap to
// test.
// ============================================================================

namespace {

// Minimal uncompressed 24-bit TGA. stb_image reads these, and the test project
// links the reader but no writer, so this is the cheapest real image file to
// produce.
void WriteTga(const std::filesystem::path& path,
              unsigned char r, unsigned char g, unsigned char b)
{
	std::ofstream file(path, std::ios::binary);
	unsigned char header[18] = {};
	header[2] = 2;    // uncompressed true-colour
	header[12] = 2;   // width  = 2
	header[14] = 2;   // height = 2
	header[16] = 24;  // bits per pixel
	file.write(reinterpret_cast<const char*>(header), sizeof(header));
	for (int i = 0; i < 4; ++i)
	{
		file.put(static_cast<char>(b));
		file.put(static_cast<char>(g));
		file.put(static_cast<char>(r));
	}
}

// One triangle, one material, one base-colour texture.
std::filesystem::path MakeTexturedObj(const std::filesystem::path& dir,
                                      const std::string& name,
                                      unsigned char r, unsigned char g, unsigned char b)
{
	std::filesystem::create_directories(dir);
	const std::string texName = name + "_tex.tga";
	WriteTga(dir / texName, r, g, b);

	{
		std::ofstream mtl(dir / (name + ".mtl"));
		mtl << "newmtl " << name << "_mat\n"
		    << "Kd 1 1 1\n"
		    << "map_Kd " << texName << "\n";
	}

	const auto objPath = dir / (name + ".obj");
	{
		std::ofstream obj(objPath);
		obj << "mtllib " << name << ".mtl\n"
		    << "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
		    << "vt 0 0\nvt 1 0\nvt 0 1\n"
		    << "usemtl " << name << "_mat\n"
		    << "f 1/1 2/2 3/3\n";
	}
	return objPath;
}

std::filesystem::path MakeCopiedTexturedGltf(const std::filesystem::path& dir,
                                             const std::string& name)
{
	std::error_code error;
	std::filesystem::create_directories(dir, error);
	REQUIRE(error.value() == 0);
	const auto source = std::filesystem::current_path() /
		"RT2App" / "assets" / "phase1a-fixtures" / "tiny_textured.glb";
	const auto target = dir / (name + ".glb");
	error.clear();
	const bool copied = std::filesystem::copy_file(
		source, target, std::filesystem::copy_options::overwrite_existing, error);
	REQUIRE(copied);
	REQUIRE(error.value() == 0);
	return target;
}

entt::entity FindFirstMeshEntity(const ECSScene& scene)
{
	auto view = scene.registry.view<MeshRef>();
	for (const auto entity : view)
		return entity;
	return entt::null;
}

entt::entity FindMeshEntityInSubtree(const ECSScene& scene, entt::entity root)
{
	if (root == entt::null || !scene.registry.valid(root))
		return entt::null;

	std::vector<entt::entity> pending{root};
	while (!pending.empty())
	{
		const auto entity = pending.back();
		pending.pop_back();
		if (!scene.registry.valid(entity))
			continue;
		if (scene.registry.all_of<MeshRef>(entity))
			return entity;
		if (const auto* hierarchy = scene.registry.try_get<Hierarchy>(entity))
			for (const auto child : hierarchy->children)
				pending.push_back(child);
	}
	return entt::null;
}

void CheckImportedResources(const ECSScene& scene,
                           entt::entity root,
                           uint32_t meshBase,
                           int materialBase,
                           int textureBase,
                           const std::string& expectedTexturePath)
{
	const auto entity = FindMeshEntityInSubtree(scene, root);
	REQUIRE(entity != static_cast<entt::entity>(entt::null));
	const auto& ref = scene.registry.get<MeshRef>(entity);
	INFO("mesh=" << ref.meshIndex << " material=" << ref.materialIndex);
	CHECK(ref.meshIndex >= meshBase);
	REQUIRE(ref.meshIndex < scene.meshRegistry.GetCount());

	const auto& mesh = scene.meshRegistry.GetMesh(ref.meshIndex);
	std::vector<uint32_t> materialIndices;
	if (ref.materialIndex >= 0)
		materialIndices.push_back(static_cast<uint32_t>(ref.materialIndex));
	else
		materialIndices = mesh.materialIndices;
	REQUIRE_FALSE(materialIndices.empty());

	int checkedMaterials = 0;
	int checkedTextures = 0;
	for (const uint32_t materialIndex : materialIndices)
	{
		INFO("material=" << materialIndex);
		CHECK(materialIndex >= static_cast<uint32_t>(materialBase));
		REQUIRE(materialIndex < scene.materials.size());
		const auto& material = scene.materials[materialIndex];
		++checkedMaterials;

		const int textureIndices[] = {
			material.baseColorTextureIndex,
			material.normalTextureIndex,
			material.emissiveTextureIndex,
			material.metallicRoughnessTextureIndex,
		};
		for (const int textureIndex : textureIndices)
		{
			if (textureIndex < 0)
				continue;
			INFO("texture=" << textureIndex);
			CHECK(textureIndex >= textureBase);
			REQUIRE(textureIndex < static_cast<int>(scene.textures.size()));
			const auto& path = scene.textures[textureIndex].ref.path;
			CHECK(path.find(expectedTexturePath) != std::string::npos);
			++checkedTextures;
		}
	}
	CHECK(checkedMaterials > 0);
	CHECK(checkedTextures > 0);
}

// Addressing a material through an entity differs between merged and
// per-shape imports, so assert on the scene's tables instead: those are what
// the GPU scene is built from, and where a stale index would show.
std::string DescribeTables(SceneManager& mgr)
{
	const auto& ecs = mgr.GetECS();
	std::string out = "materials=" + std::to_string(ecs.materials.size()) +
	                  " textures=" + std::to_string(ecs.textures.size());
	for (size_t i = 0; i < ecs.materials.size(); ++i)
		out += " | mat[" + std::to_string(i) + "].baseColorTex=" +
		       std::to_string(ecs.materials[i].baseColorTextureIndex);
	for (size_t i = 0; i < ecs.textures.size(); ++i)
		out += " | tex[" + std::to_string(i) + "]=" + ecs.textures[i].ref.path;
	return out;
}

} // namespace

TEST_CASE("Import: a model imported after a delete keeps its own textures")
{
	const auto dir = std::filesystem::temp_directory_path() / "rt2_import_texture_rebind";
	std::filesystem::remove_all(dir);

	const auto modelA = MakeTexturedObj(dir, "modelA", 255, 0, 0);
	const auto modelB = MakeTexturedObj(dir, "modelB", 0, 0, 255);

	SceneManager mgr;
	ImportSettings settings;
	settings.mergeMegaMesh = false;

	const auto rootA = mgr.ImportObj(modelA.string(), settings);
	REQUIRE(rootA.IsValid());
	MESSAGE("after import A: " << DescribeTables(mgr));

	// The app compacts on scene change whenever the undo history is empty,
	// which is the state this reproduces.
	mgr.RemoveEntity(rootA);
	mgr.CompactMeshRegistry();
	MESSAGE("after delete+compact: " << DescribeTables(mgr));

	const auto rootB = mgr.ImportObj(modelB.string(), settings);
	REQUIRE(rootB.IsValid());
	MESSAGE("after import B: " << DescribeTables(mgr));

	// Every material that names a texture must name one of B's.
	const auto& ecs = mgr.GetECS();
	int checked = 0;
	for (const auto& mat : ecs.materials)
	{
		if (mat.baseColorTextureIndex < 0) continue;
		REQUIRE(mat.baseColorTextureIndex < (int)ecs.textures.size());
		const std::string path = ecs.textures[mat.baseColorTextureIndex].ref.path;
		INFO("material resolved to: " << path);
		CHECK(path.find("modelB") != std::string::npos);
		CHECK(path.find("modelA") == std::string::npos);
		++checked;
	}
	CHECK(checked > 0);

	std::filesystem::remove_all(dir);
}

// ============================================================================
// The actual defect behind the report.
//
// A merged mega-mesh OBJ stores per-triangle material indices and sets
// MeshRef::materialIndex to -1, so those per-triangle values are the only
// thing addressing a material. They are written straight from tinyobj, which
// numbers materials from 0 within the file being read -- with no matBase
// added for materials already in the scene.
//
// So the second OBJ imported into a scene indexes the FIRST import's
// materials, and renders with its textures. The glTF path gets this right
// (SceneLoader.cpp: "Offset material index by matBase"); only OBJ does not.
//
// Deleting the first model's entity is what makes it obvious rather than
// necessary: the orphaned material stays in the table, so the survivor on
// screen is wearing the deleted model's texture.
// ============================================================================
TEST_CASE("Import: a second OBJ's triangles reference its own materials")
{
	const auto dir = std::filesystem::temp_directory_path() / "rt2_import_obj_matbase";
	std::filesystem::remove_all(dir);

	const auto modelA = MakeTexturedObj(dir, "modelA", 255, 0, 0);
	const auto modelB = MakeTexturedObj(dir, "modelB", 0, 0, 255);

	SceneManager mgr;
	ImportSettings settings;
	settings.mergeMegaMesh = true;

	REQUIRE(mgr.ImportObj(modelA.string(), settings).IsValid());
	const auto rootB = mgr.ImportObj(modelB.string(), settings);
	REQUIRE(rootB.IsValid());

	const auto& ecs = mgr.GetECS();
	auto& reg = const_cast<entt::registry&>(ecs.registry);
	MESSAGE("tables: " << DescribeTables(mgr));

	// Find B's mesh via its subtree.
	uint32_t meshIdx = UINT32_MAX;
	std::vector<entt::entity> stack{ rootB.id };
	while (!stack.empty())
	{
		const auto e = stack.back();
		stack.pop_back();
		if (!reg.valid(e)) continue;
		if (auto* mr = reg.try_get<MeshRef>(e)) { meshIdx = mr->meshIndex; break; }
		if (auto* h = reg.try_get<Hierarchy>(e))
			for (auto c : h->children) stack.push_back(c);
	}
	REQUIRE(meshIdx != UINT32_MAX);

	const auto& mesh = ecs.meshRegistry.GetMesh(meshIdx);
	REQUIRE_FALSE(mesh.materialIndices.empty());

	for (uint32_t matIdx : mesh.materialIndices)
	{
		REQUIRE(matIdx < ecs.materials.size());
		const int texIdx = ecs.materials[matIdx].baseColorTextureIndex;
		REQUIRE(texIdx >= 0);
		REQUIRE(texIdx < (int)ecs.textures.size());
		const std::string path = ecs.textures[texIdx].ref.path;
		INFO("B triangle -> material " << matIdx << " -> " << path);
		CHECK(path.find("modelB") != std::string::npos);
	}

	std::filesystem::remove_all(dir);
}

// ============================================================================
// The defect actually behind the report.
//
// The import dialog loads a model into a *fresh* ECSScene -- where its
// material legitimately sits at index 0 -- and then merges that scene into the
// live one. MergeImportedECS rebases nearly everything on the way in: mesh
// indices by meshBase, per-triangle material indices by matBase, and each
// material's texture indices by texBase. It did not rebase
// MeshRef::materialIndex, which is how a mesh with no per-triangle material
// data selects its material.
//
// So the second model imported kept materialIndex 0 and rendered with the
// first model's material -- and therefore its textures. The first model looked
// correct only because its material really was index 0.
// ============================================================================
TEST_CASE("MergeImportedECS rebases MeshRef::materialIndex onto the merged table")
{
	SceneManager mgr;

	// An existing model already occupies material slot 0.
	mgr.AddMaterial(SceneMaterial{});
	{
		MeshData existing;
		existing.vertices = {0,0,0, 1,0,0, 0,1,0};
		existing.indices = {0, 1, 2};
		mgr.GetECS().meshRegistry.AddMesh(std::move(existing));
	}
	REQUIRE(mgr.GetECS().materials.size() == 1);

	// A freshly loaded import: one material at its own index 0, one mesh
	// whose MeshRef names that material.
	ECSScene src;
	SceneMaterial imported;
	imported.baseColor = {0.1f, 0.2f, 0.3f};
	src.materials.push_back(imported);

	MeshData mesh;
	mesh.vertices = {0,0,0, 1,0,0, 0,1,0};
	mesh.indices = {0, 1, 2};
	const uint32_t srcMesh = src.meshRegistry.AddMesh(std::move(mesh));

	const auto srcRoot = src.registry.create();
	src.registry.emplace<Transform>(srcRoot);
	src.registry.emplace<NameComponent>(srcRoot, std::string("ImportedRoot"));

	const auto srcChild = src.registry.create();
	src.registry.emplace<Transform>(srcChild);
	src.registry.emplace<MeshRef>(srcChild, srcMesh, 0);
	src.registry.emplace<VisibleComponent>(srcChild);
	src.registry.emplace<NameComponent>(srcChild, std::string("ImportedMesh"));
	src.registry.emplace<Hierarchy>(srcRoot).children.push_back(srcChild);
	src.registry.emplace<Hierarchy>(srcChild).parent = srcRoot;

	const auto merged = mgr.MergeImportedECS(std::move(src), srcRoot, "imported.glb");
	REQUIRE(merged.IsValid());

	const auto& ecs = mgr.GetECS();
	REQUIRE(ecs.materials.size() == 2);

	// The imported mesh must name the slot its material actually landed in.
	auto& reg = const_cast<entt::registry&>(ecs.registry);
	int found = 0;
	auto view = reg.view<MeshRef>();
	for (auto e : view)
	{
		const auto& ref = view.get<MeshRef>(e);
		if (ref.meshIndex == 0) continue;  // the pre-existing mesh
		INFO("imported MeshRef: mesh=" << ref.meshIndex << " material=" << ref.materialIndex);
		CHECK(ref.materialIndex == 1);
		++found;
	}
	CHECK(found == 1);
}

TEST_CASE("ImportObj: the second model keeps its own mesh material and textures")
{
	const auto dir = std::filesystem::temp_directory_path() /
		"rt2_import_rebase_scene_manager_obj";
	std::filesystem::remove_all(dir);

	const auto modelA = MakeTexturedObj(dir, "modelA", 255, 0, 0);
	const auto modelB = MakeTexturedObj(dir, "modelB", 0, 0, 255);

	SceneManager manager;
	ImportSettings settings;
	settings.mergeMegaMesh = false;

	const auto rootA = manager.ImportObj(modelA.string(), settings);
	REQUIRE(rootA.IsValid());
	const uint32_t meshBase = manager.GetECS().meshRegistry.GetCount();
	const int materialBase = static_cast<int>(manager.GetECS().materials.size());
	const int textureBase = static_cast<int>(manager.GetECS().textures.size());

	const auto rootB = manager.ImportObj(modelB.string(), settings);
	REQUIRE(rootB.IsValid());
	CheckImportedResources(manager.GetECS(), rootB.id, meshBase, materialBase,
		textureBase, "modelB_tex.tga");

	std::filesystem::remove_all(dir);
}

TEST_CASE("ImportGltf: the second model keeps its own mesh material and textures")
{
	const auto dir = std::filesystem::temp_directory_path() /
		"rt2_import_rebase_scene_manager_gltf";
	std::filesystem::remove_all(dir);

	const auto modelA = MakeCopiedTexturedGltf(dir, "modelA");
	const auto modelB = MakeCopiedTexturedGltf(dir, "modelB");

	SceneManager manager;
	const auto rootA = manager.ImportGltf(modelA.string());
	REQUIRE(rootA.IsValid());
	const uint32_t meshBase = manager.GetECS().meshRegistry.GetCount();
	const int materialBase = static_cast<int>(manager.GetECS().materials.size());
	const int textureBase = static_cast<int>(manager.GetECS().textures.size());

	const auto rootB = manager.ImportGltf(modelB.string());
	REQUIRE(rootB.IsValid());
	CheckImportedResources(manager.GetECS(), rootB.id, meshBase, materialBase,
		textureBase, "modelB.glb");

	std::filesystem::remove_all(dir);
}

TEST_CASE("LoadObjIntoECS plus MergeImportedECS rebases a second model")
{
	const auto dir = std::filesystem::temp_directory_path() /
		"rt2_import_rebase_loaded_obj";
	std::filesystem::remove_all(dir);

	const auto modelA = MakeTexturedObj(dir, "modelA", 255, 0, 0);
	const auto modelB = MakeTexturedObj(dir, "modelB", 0, 0, 255);
	std::vector<rt2::core::AssetDiagnostic> diagnostics;

	ECSScene sourceA;
	REQUIRE(SceneLoader::LoadObjIntoECS(
		sourceA, MakeSceneLoaderTestContext(dir, modelA), diagnostics));
	const auto sourceRootA = FindFirstMeshEntity(sourceA);
	REQUIRE(sourceRootA != static_cast<entt::entity>(entt::null));

	SceneManager manager;
	REQUIRE(manager.MergeImportedECS(
		std::move(sourceA), sourceRootA, modelA.string()).IsValid());
	const uint32_t meshBase = manager.GetECS().meshRegistry.GetCount();
	const int materialBase = static_cast<int>(manager.GetECS().materials.size());
	const int textureBase = static_cast<int>(manager.GetECS().textures.size());

	ECSScene sourceB;
	diagnostics.clear();
	REQUIRE(SceneLoader::LoadObjIntoECS(
		sourceB, MakeSceneLoaderTestContext(dir, modelB), diagnostics));
	const auto sourceRootB = FindFirstMeshEntity(sourceB);
	REQUIRE(sourceRootB != static_cast<entt::entity>(entt::null));
	const auto rootB = manager.MergeImportedECS(
		std::move(sourceB), sourceRootB, modelB.string());
	REQUIRE(rootB.IsValid());

	CheckImportedResources(manager.GetECS(), rootB.id, meshBase, materialBase,
		textureBase, "modelB_tex.tga");

	std::filesystem::remove_all(dir);
}

TEST_CASE("LoadIntoECS plus MergeImportedECS rebases a second model")
{
	const auto dir = std::filesystem::temp_directory_path() /
		"rt2_import_rebase_loaded_gltf";
	std::filesystem::remove_all(dir);

	const auto modelA = MakeCopiedTexturedGltf(dir, "modelA");
	const auto modelB = MakeCopiedTexturedGltf(dir, "modelB");
	std::vector<rt2::core::AssetDiagnostic> diagnostics;

	ECSScene sourceA;
	REQUIRE(SceneLoader::LoadIntoECS(
		sourceA, MakeSceneLoaderTestContext(dir, modelA), diagnostics));
	const auto sourceRootA = FindFirstMeshEntity(sourceA);
	REQUIRE(sourceRootA != static_cast<entt::entity>(entt::null));

	SceneManager manager;
	REQUIRE(manager.MergeImportedECS(
		std::move(sourceA), sourceRootA, modelA.string()).IsValid());
	const uint32_t meshBase = manager.GetECS().meshRegistry.GetCount();
	const int materialBase = static_cast<int>(manager.GetECS().materials.size());
	const int textureBase = static_cast<int>(manager.GetECS().textures.size());

	ECSScene sourceB;
	diagnostics.clear();
	REQUIRE(SceneLoader::LoadIntoECS(
		sourceB, MakeSceneLoaderTestContext(dir, modelB), diagnostics));
	const auto sourceRootB = FindFirstMeshEntity(sourceB);
	REQUIRE(sourceRootB != static_cast<entt::entity>(entt::null));
	const auto rootB = manager.MergeImportedECS(
		std::move(sourceB), sourceRootB, modelB.string());
	REQUIRE(rootB.IsValid());

	CheckImportedResources(manager.GetECS(), rootB.id, meshBase, materialBase,
		textureBase, "modelB.glb");

	std::filesystem::remove_all(dir);
}

TEST_CASE("CompactMeshRegistry remaps every scene resource index field")
{
	SceneManager manager;
	auto& scene = manager.GetECS();

	scene.textures.resize(6);
	for (size_t i = 0; i < scene.textures.size(); ++i)
		scene.textures[i].ref.path = "texture" + std::to_string(i);

	SceneMaterial unused;
	unused.baseColorTextureIndex = 0;
	scene.materials.push_back(unused);

	SceneMaterial materialA;
	materialA.baseColorTextureIndex = 1;
	materialA.normalTextureIndex = 2;
	materialA.emissiveTextureIndex = 3;
	materialA.metallicRoughnessTextureIndex = 4;
	scene.materials.push_back(materialA);

	SceneMaterial materialB;
	materialB.baseColorTextureIndex = 5;
	scene.materials.push_back(materialB);

	MeshData unusedMesh;
	unusedMesh.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
	unusedMesh.indices = {0, 1, 2};
	scene.meshRegistry.AddMesh(std::move(unusedMesh));

	MeshData usedMesh;
	usedMesh.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
	usedMesh.indices = {0, 1, 2};
	usedMesh.materialIndices = {2};
	scene.meshRegistry.AddMesh(std::move(usedMesh));

	const auto entity = scene.registry.create();
	scene.registry.emplace<Transform>(entity);
	scene.registry.emplace<MeshRef>(entity, 1u, 1);

	MaterialOverrideComponent overrideComponent;
	overrideComponent.authored = true;
	overrideComponent.materialIndex = 1;
	overrideComponent.material.baseColorTextureIndex = 1;
	overrideComponent.material.normalTextureIndex = 2;
	overrideComponent.material.emissiveTextureIndex = 3;
	overrideComponent.material.metallicRoughnessTextureIndex = 4;
	scene.registry.emplace<MaterialOverrideComponent>(entity, overrideComponent);

	REQUIRE(manager.CompactMeshRegistry());
	REQUIRE(scene.meshRegistry.GetCount() == 1);
	REQUIRE(scene.materials.size() == 2);
	REQUIRE(scene.textures.size() == 5);

	const auto& ref = scene.registry.get<MeshRef>(entity);
	CHECK(ref.meshIndex == 0);
	CHECK(ref.materialIndex == 0);
	CHECK(scene.meshRegistry.GetMesh(0).materialIndices ==
		std::vector<uint32_t>{1});

	const auto& compactedA = scene.materials[0];
	CHECK(compactedA.baseColorTextureIndex == 0);
	CHECK(compactedA.normalTextureIndex == 1);
	CHECK(compactedA.emissiveTextureIndex == 2);
	CHECK(compactedA.metallicRoughnessTextureIndex == 3);
	CHECK(scene.materials[1].baseColorTextureIndex == 4);

	const auto& compactedOverride =
		scene.registry.get<MaterialOverrideComponent>(entity);
	CHECK(compactedOverride.materialIndex == 0);
	CHECK(compactedOverride.material.baseColorTextureIndex == 0);
	CHECK(compactedOverride.material.normalTextureIndex == 1);
	CHECK(compactedOverride.material.emissiveTextureIndex == 2);
	CHECK(compactedOverride.material.metallicRoughnessTextureIndex == 3);
}
