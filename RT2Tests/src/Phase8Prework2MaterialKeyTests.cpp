#include <doctest/doctest.h>

#include "ECSComponents.h"
#include "ECSScene.h"
#include "Phase1AFixtureGenerator.h"
#include "SceneAssetResolver.h"
#include "SceneDocument.h"
#include "SceneManager.h"
#include "SceneSerializer.h"
#include "SceneSerializerTestSupport.h"
#include "core/Error.h"
#include "core/UUID.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace rt2::core;

// ============================================================================
// Phase 8 pre-work 2 — source-material identity and key-based override
// matching (implementation spec, docs/game-engine-development-plan.md).
//
// Loaders mint a durable identity on every loader-surfaced material
// (SceneMaterial::sourceKey, name-preferring: "gltf:material:name=<n>" /
// "obj:material:name=<n>", index form only when the name is absent or
// duplicated). RecordMaterialOverride freezes that identity into the
// override; the resolver matches the override by key against the rebuilt
// staged materials and only falls back to slot position when the key misses
// (D3: a Stale diagnostic, never fatal). Legacy "<meshKey>:material" keys
// are rebased to the new form at resolve time (D4).
//
// The five cases below each carry a discrimination proof (red-then-green)
// recorded in the verification report: C1 serializer round trip, C2 loader
// surface on all four paths (including the OBJ mutation seam), C3 cross-path
// resolver matching across a material-definition reorder (texture-copy
// observable), C4 D3 miss diagnostic + slot fallback, C5 shared-mesh
// entities matched by name when definitions reorder (index form cannot).
// ============================================================================

namespace
{

std::filesystem::path UniqueTempDir(const std::string& tag)
{
	auto dir = std::filesystem::temp_directory_path() / ("rt2_" + tag);
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
	std::filesystem::create_directories(dir, ec);
	return dir;
}

// A 1x1 PPM pixel payload as a data URI (same encoding scheme as
// GenerateTinyTexturedGlb). A = RGB(128,64,32), B = RGB(32,64,128) — distinct
// bytes so the loader never dedupes them, letting tests identify which
// texture a resolved override points at.
const char* kTexAUri =
	"data:application/octet-stream;base64,UDYKMSAxCjI1NQqAQCA=";
const char* kTexBUri =
	"data:application/octet-stream;base64,UDYKMSAxCjI1NQogQIA=";

// One mesh, two primitives, two NAMED textured materials.
//   primitive 0 -> material[0], primitive 1 -> material[1]
// `swapDefs` physically swaps the two material definitions (names travel with
// their values, so after the swap the loader surfaces Green at slot 0 and
// Red at slot 1 — a reorder of definitions, the case index-form keys cannot
// follow). `renameRedTo` renames the first definition (a rename the key
// match must miss, per D3).
bool GenerateTwoMaterialGlb(const std::filesystem::path& path, Error& err,
                            bool swapDefs = false,
                            const std::string& renameRedTo = "")
{
	err = Error{};

	tinygltf::Model model;
	model.asset.version = "2.0";
	model.asset.generator = "RT2 Phase 8 pre-work 2 fixture";

	tinygltf::Image imgA;
	imgA.uri = kTexAUri;
	imgA.mimeType = "image/x-portable-pixmap";
	model.images.push_back(imgA);
	tinygltf::Image imgB;
	imgB.uri = kTexBUri;
	imgB.mimeType = "image/x-portable-pixmap";
	model.images.push_back(imgB);

	tinygltf::Texture texA;
	texA.source = 0;
	model.textures.push_back(texA);
	tinygltf::Texture texB;
	texB.source = 1;
	model.textures.push_back(texB);

	auto makeMat = [](const std::string& name, const std::vector<double>& color,
	                  int texIdx) {
		tinygltf::Material m;
		m.name = name;
		m.pbrMetallicRoughness.baseColorFactor = color;
		m.pbrMetallicRoughness.metallicFactor = 0.0;
		m.pbrMetallicRoughness.roughnessFactor = 0.7;
		m.pbrMetallicRoughness.baseColorTexture.index = texIdx;
		m.pbrMetallicRoughness.baseColorTexture.texCoord = 0;
		return m;
	};

	tinygltf::Material red =
		makeMat(renameRedTo.empty() ? std::string("Red") : renameRedTo,
		        {0.9, 0.1, 0.1, 1.0}, 0);
	tinygltf::Material green = makeMat("Green", {0.1, 0.9, 0.1, 1.0}, 1);
	if (swapDefs)
	{
		model.materials.push_back(std::move(green));
		model.materials.push_back(std::move(red));
	}
	else
	{
		model.materials.push_back(std::move(red));
		model.materials.push_back(std::move(green));
	}

	auto appendBuffer = [&](const std::vector<unsigned char>& data) -> int {
		tinygltf::Buffer b;
		b.data = data;
		model.buffers.push_back(b);
		return (int)model.buffers.size() - 1;
	};
	auto addVec3Accessor = [&](const std::vector<float>& data) -> int {
		std::vector<unsigned char> bytes(data.size() * sizeof(float));
		std::memcpy(bytes.data(), data.data(), bytes.size());
		int buf = appendBuffer(bytes);
		tinygltf::BufferView bv;
		bv.buffer = buf; bv.byteOffset = 0; bv.byteLength = bytes.size();
		bv.byteStride = 0; bv.target = 34962;
		model.bufferViews.push_back(bv);
		int bvIdx = (int)model.bufferViews.size() - 1;
		tinygltf::Accessor acc;
		acc.bufferView = bvIdx; acc.byteOffset = 0;
		acc.componentType = 5126; acc.count = data.size() / 3; acc.type = 3;
		model.accessors.push_back(acc);
		return (int)model.accessors.size() - 1;
	};
	auto addIndexAccessor = [&](const std::vector<uint32_t>& data) -> int {
		std::vector<unsigned char> bytes(data.size() * sizeof(uint32_t));
		std::memcpy(bytes.data(), data.data(), bytes.size());
		int buf = appendBuffer(bytes);
		tinygltf::BufferView bv;
		bv.buffer = buf; bv.byteOffset = 0; bv.byteLength = bytes.size();
		bv.byteStride = 0; bv.target = 34963;
		model.bufferViews.push_back(bv);
		int bvIdx = (int)model.bufferViews.size() - 1;
		tinygltf::Accessor acc;
		acc.bufferView = bvIdx; acc.byteOffset = 0;
		acc.componentType = 5125; acc.count = data.size(); acc.type = 65;
		model.accessors.push_back(acc);
		return (int)model.accessors.size() - 1;
	};

	std::vector<float> positions = {
		0.0f, 0.0f, 0.0f,
		1.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f,
		1.0f, 1.0f, 0.0f,
	};
	int posAcc = addVec3Accessor(positions);

	tinygltf::Primitive prim0;
	prim0.attributes["POSITION"] = posAcc;
	prim0.indices = addIndexAccessor({0, 1, 2});
	prim0.material = 0;
	prim0.mode = 4;
	tinygltf::Primitive prim1;
	prim1.attributes["POSITION"] = posAcc;
	prim1.indices = addIndexAccessor({1, 3, 2});
	prim1.material = 1;
	prim1.mode = 4;
	tinygltf::Mesh mesh;
	mesh.primitives.push_back(prim0);
	mesh.primitives.push_back(prim1);
	model.meshes.push_back(mesh);

	tinygltf::Node node;
	node.mesh = 0;
	node.name = "TwoMatMesh";
	model.nodes.push_back(node);

	tinygltf::Scene scene;
	scene.nodes = {0};
	model.scenes.push_back(scene);
	model.defaultScene = 0;

	tinygltf::TinyGLTF loader;
	std::string gerr, gwarn;
	bool ok = loader.WriteGltfSceneToFile(&model, path.string(),
	                                      true,  // embedImages
	                                      true,  // embedBuffers
	                                      true,  // prettyPrint
	                                      true); // writeBinary (GLB)
	if (!ok)
	{
		err.code = Error::Io;
		err.path = path.string();
		err.detail = "WriteGltfSceneToFile failed for two-material GLB";
		return false;
	}
	return true;
}

// OBJ with two materials (mat_a / mat_b) and one triangle per material.
struct TwoMatObjFixture
{
	std::filesystem::path dir;
	std::filesystem::path objPath;
	std::filesystem::path mtlPath;
	std::string shapeName;

	TwoMatObjFixture() = default;
	TwoMatObjFixture(const TwoMatObjFixture&) = delete;
	TwoMatObjFixture& operator=(const TwoMatObjFixture&) = delete;
	TwoMatObjFixture(TwoMatObjFixture&& o) noexcept
		: dir(std::move(o.dir)), objPath(std::move(o.objPath)),
		  mtlPath(std::move(o.mtlPath)), shapeName(std::move(o.shapeName))
	{
		o.objPath.clear();
		o.mtlPath.clear();
	}
	TwoMatObjFixture& operator=(TwoMatObjFixture&& o) noexcept
	{
		if (this != &o)
		{
			Cleanup();
			dir = std::move(o.dir);
			objPath = std::move(o.objPath);
			mtlPath = std::move(o.mtlPath);
			shapeName = std::move(o.shapeName);
			o.objPath.clear();
			o.mtlPath.clear();
		}
		return *this;
	}
	~TwoMatObjFixture() { Cleanup(); }
	void Cleanup()
	{
		std::error_code ec;
		if (!objPath.empty()) std::filesystem::remove(objPath, ec);
		if (!mtlPath.empty()) std::filesystem::remove(mtlPath, ec);
	}
};

TwoMatObjFixture MakeTwoMatObj(const std::filesystem::path& dir,
                               const std::string& objName)
{
	TwoMatObjFixture f;
	f.dir = dir;
	f.shapeName = objName + "_shape";
	f.objPath = dir / (objName + ".obj");
	f.mtlPath = dir / (objName + ".mtl");
	{
		std::ofstream mtl(f.mtlPath);
		mtl << "newmtl mat_a\n"
		    << "Kd 0.9 0.1 0.1\n"
		    << "Ns 16\nillum 2\n";
		mtl << "newmtl mat_b\n"
		    << "Kd 0.1 0.9 0.1\n"
		    << "Ns 16\nillum 2\n";
	}
	{
		std::ofstream obj(f.objPath);
		obj << "mtllib " << (objName + ".mtl") << "\n"
		    << "v 0 0 0\n"
		    << "v 1 0 0\n"
		    << "v 0 1 0\n"
		    << "v 1 1 0\n"
		    << "v 2 0 0\n"
		    << "v 2 1 0\n"
		    << "vt 0 0\nvt 1 0\nvt 0 1\n"
		    << "g " << f.shapeName << "\n"
		    << "usemtl mat_a\n"
		    << "f 1/1 2/2 3/3\n"
		    << "usemtl mat_b\n"
		    << "f 2/2 5/2 6/3\n";
	}
	return f;
}

// First glTF entity whose imported source key is exactly `sourceKey`.
entt::entity FindGltfEntity(entt::registry& reg, const std::string& sourceKey)
{
	auto view = reg.view<ImportedMeshSourceComponent>();
	for (auto e : view)
	{
		if (view.get<ImportedMeshSourceComponent>(e).model.sourceKey == sourceKey)
			return e;
	}
	return entt::null;
}

// First OBJ entity whose imported source key starts with `prefix`.
entt::entity FindObjEntity(entt::registry& reg, const std::string& prefix)
{
	auto view = reg.view<ImportedMeshSourceComponent>();
	for (auto e : view)
	{
		const auto& k = view.get<ImportedMeshSourceComponent>(e).model.sourceKey;
		if (k.rfind(prefix, 0) == 0)
			return e;
	}
	return entt::null;
}

int CountStale(const std::vector<AssetDiagnostic>& diagnostics)
{
	int n = 0;
	for (const auto& d : diagnostics)
		if (d.severity == AssetDiagnostic::Stale)
			++n;
	return n;
}

MaterialOverrideComponent* FindOverride(entt::registry& reg,
                                        entt::entity e)
{
	return reg.try_get<MaterialOverrideComponent>(e);
}

} // namespace

// ---------------------------------------------------------------------------
// C1: the serializer round trips the minted key on both the material block
// and the override. Fault for red: drop the j["sourceKey"] write in
// SceneSerializer::MaterialToJson.
// ---------------------------------------------------------------------------
TEST_CASE("P8 pre-work 2: material key survives the scene round trip")
{
	auto dir = UniqueTempDir("p8p2_c1");
	auto glbPath = dir / "twomat.glb";
	Error genErr;
	REQUIRE(GenerateTwoMaterialGlb(glbPath, genErr));
	auto scenePath = dir / "c1.rt2scene";

	SceneManager mgr;
	REQUIRE(mgr.LoadScene(glbPath.string()));
	auto& reg = mgr.GetECS().registry;
	const entt::entity prim0 = FindGltfEntity(reg, "gltf:scene=0:node=0:mesh=0:primitive=0");
	{ const bool prim0Ok = (prim0 != entt::null); REQUIRE(prim0Ok); }
	REQUIRE(reg.get<MeshRef>(prim0).materialIndex == 0);
	const auto uuid = mgr.GetEntityUuid(SceneManager::EntityId{prim0});

	// Record the override: the key is minted from the material's own
	// loader-surfaced identity, name form (the glTF names are present and
	// unique).
	REQUIRE(mgr.SetMaterialIndexState(uuid, 0, nullptr, nullptr).success);
	const auto* ov = FindOverride(reg, prim0);
	REQUIRE(ov);
	CHECK(ov->sourceMaterialKey == "gltf:material:name=Red");
	CHECK_FALSE(ov->sourceMaterialKey.empty());

	Error saveErr;
	REQUIRE(SaveSceneForTest(mgr.AuthoringDoc(), scenePath, saveErr));
	REQUIRE(saveErr.IsOk());

	DeterministicUuidProvider provider2;
	SceneDocument loaded;
	loaded.SetUuidProvider(&provider2);
	Error loadErr;
	REQUIRE(SceneSerializer::Load(loaded, scenePath, loadErr));
	REQUIRE(loadErr.IsOk());

	// The material block carries its key through the serializer.
	REQUIRE(loaded.ecs.materials.size() >= 2);
	CHECK(loaded.ecs.materials[0].sourceKey == "gltf:material:name=Red");
	CHECK(loaded.ecs.materials[1].sourceKey == "gltf:material:name=Green");

	std::vector<AssetDiagnostic> diagnostics;
	Error resolveErr;
	REQUIRE(SceneAssetResolver::ResolveAll(loaded,
		AssetResolutionContext{ dir, nullptr }, diagnostics, resolveErr));
	REQUIRE(resolveErr.IsOk());
	CHECK(CountStale(diagnostics) == 0);

	// The override's durable key survives load + resolve verbatim.
	const entt::entity le = loaded.FindByUuid(uuid);
	{ const bool leOk = (le != entt::null); REQUIRE(leOk); }
	const auto* lov = FindOverride(loaded.ecs.registry, le);
	REQUIRE(lov);
	CHECK(lov->sourceMaterialKey == "gltf:material:name=Red");
	REQUIRE(lov->materialIndex >= 0);
	REQUIRE(lov->materialIndex < (int)loaded.ecs.materials.size());

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C2: all four loader surfaces mint name-preferring keys, and the mint reads
// the material's live identity (OBJ mutation seam) rather than deriving a
// key from slot position. Faults for red: disable the mint assignment in the
// corresponding loader loop (SceneLoader.cpp ~:726 glTF load, ~:1429 glTF
// import, ~:2061 OBJ load, ~:2369 OBJ import).
// ---------------------------------------------------------------------------
TEST_CASE("P8 pre-work 2: all four loader surfaces mint name-preferring keys")
{
	auto dir = UniqueTempDir("p8p2_c2");
	auto glbPath = dir / "twomat.glb";
	Error genErr;
	REQUIRE(GenerateTwoMaterialGlb(glbPath, genErr));
	auto objFixture = MakeTwoMatObj(dir, "twomat");

	// glTF load path (LoadIntoECS).
	{
		SceneManager mgr;
		REQUIRE(mgr.LoadScene(glbPath.string()));
		auto& mats = mgr.GetECS().materials;
		REQUIRE(mats.size() >= 2);
		CHECK(mats[0].sourceKey == "gltf:material:name=Red");
		CHECK(mats[1].sourceKey == "gltf:material:name=Green");
	}

	// glTF import path (ImportIntoECS).
	{
		SceneManager mgr;
		REQUIRE(mgr.ImportGltf(glbPath.string()).IsValid());
		auto& mats = mgr.GetECS().materials;
		REQUIRE(mats.size() >= 2);
		CHECK(mats[0].sourceKey == "gltf:material:name=Red");
		CHECK(mats[1].sourceKey == "gltf:material:name=Green");
	}

	// OBJ load path (LoadObjIntoECS).
	{
		SceneManager mgr;
		REQUIRE(mgr.LoadScene(objFixture.objPath.string()));
		auto& mats = mgr.GetECS().materials;
		REQUIRE(mats.size() >= 2);
		CHECK(mats[0].sourceKey == "obj:material:name=mat_a");
		CHECK(mats[1].sourceKey == "obj:material:name=mat_b");
	}

	// OBJ import path (ImportObjIntoECS, per-shape).
	{
		SceneManager mgr;
		ImportSettings settings;
		settings.mergeMegaMesh = false;
		REQUIRE(mgr.ImportObj(objFixture.objPath.string(), settings).IsValid());
		auto& mats = mgr.GetECS().materials;
		REQUIRE(mats.size() >= 2);
		CHECK(mats[0].sourceKey == "obj:material:name=mat_a");
		CHECK(mats[1].sourceKey == "obj:material:name=mat_b");

		// OBJ mutation seam: the mint reads the material's live identity at
		// record time. Mutating the surfaced sourceKey must change the minted
		// override key; a slot-derived key would not see the mutation.
		auto& reg = mgr.GetECS().registry;
		const entt::entity shape0 = FindObjEntity(reg, "obj:shape=0:");
		{ const bool shape0Ok = (shape0 != entt::null); REQUIRE(shape0Ok); }
		const auto uuid = mgr.GetEntityUuid(SceneManager::EntityId{shape0});
		mgr.GetECS().materials[0].sourceKey = "obj:material:name=mat_a_mutated";
		REQUIRE(mgr.SetMaterialIndexState(uuid, 0, nullptr, nullptr).success);
		const auto* ov = FindOverride(reg, shape0);
		REQUIRE(ov);
		CHECK(ov->sourceMaterialKey == "obj:material:name=mat_a_mutated");
	}

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C3: cross-path resolve (key minted by the load path, matched against the
// import path's staging) follows the material NAMED Red across a
// definition-swap reorder. The observable is the texture-index copy: the
// override adopts the staged material's texture at the KEY-matched slot.
// Fault for red: make IsNewFormMaterialKey always return false
// (slot-only matching) — the override then copies the texture of whatever
// definition now sits at slot 0 (Green), and a Stale diagnostic appears.
// ---------------------------------------------------------------------------
TEST_CASE("P8 pre-work 2: resolver follows the name across a definition reorder")
{
	auto dir = UniqueTempDir("p8p2_c3");
	auto glbPath = dir / "twomat.glb";
	Error genErr;
	REQUIRE(GenerateTwoMaterialGlb(glbPath, genErr));
	auto scenePath = dir / "c3.rt2scene";

	SceneManager mgr;
	REQUIRE(mgr.LoadScene(glbPath.string()));
	auto& reg = mgr.GetECS().registry;
	const entt::entity prim0 = FindGltfEntity(reg, "gltf:scene=0:node=0:mesh=0:primitive=0");
	{ const bool prim0Ok = (prim0 != entt::null); REQUIRE(prim0Ok); }
	REQUIRE(reg.get<MeshRef>(prim0).materialIndex == 0);
	const auto uuid = mgr.GetEntityUuid(SceneManager::EntityId{prim0});
	REQUIRE(mgr.SetMaterialIndexState(uuid, 0, nullptr, nullptr).success);
	REQUIRE(FindOverride(reg, prim0)->sourceMaterialKey == "gltf:material:name=Red");

	Error saveErr;
	REQUIRE(SaveSceneForTest(mgr.AuthoringDoc(), scenePath, saveErr));
	REQUIRE(saveErr.IsOk());

	// Rebuild the source with the two material definitions physically swapped
	// (names travel with their values): Red now surfaces at slot 1.
	Error genErr2;
	REQUIRE(GenerateTwoMaterialGlb(glbPath, genErr2, /*swapDefs=*/true));

	DeterministicUuidProvider provider2;
	SceneDocument loaded;
	loaded.SetUuidProvider(&provider2);
	Error loadErr;
	REQUIRE(SceneSerializer::Load(loaded, scenePath, loadErr));
	REQUIRE(loadErr.IsOk());

	const int texBase = (int)loaded.ecs.textures.size();
	std::vector<AssetDiagnostic> diagnostics;
	Error resolveErr;
	REQUIRE(SceneAssetResolver::ResolveAll(loaded,
		AssetResolutionContext{ dir, nullptr }, diagnostics, resolveErr));
	REQUIRE(resolveErr.IsOk());

	// Key matching must bind the material NAMED Red (staged slot 1, texture
	// A = texBase), not the definition that now sits at slot 0 (Green,
	// texture B = texBase + 1).
	CHECK(CountStale(diagnostics) == 0);
	const entt::entity le = loaded.FindByUuid(uuid);
	{ const bool leOk = (le != entt::null); REQUIRE(leOk); }
	const auto* lov = FindOverride(loaded.ecs.registry, le);
	REQUIRE(lov);
	CHECK(lov->material.baseColorTextureIndex == texBase);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C4: a key that no longer exists in the rebuilt source (renamed material)
// is a loud D3 miss — a Stale diagnostic — and still falls back to slot
// position so the override keeps working. Fault for red: remove the D3
// diagnostic block in the resolver's plan pass (the miss then stays silent).
// ---------------------------------------------------------------------------
TEST_CASE("P8 pre-work 2: missing key is loud (D3) and falls back to slot")
{
	auto dir = UniqueTempDir("p8p2_c4");
	auto glbPath = dir / "twomat.glb";
	Error genErr;
	REQUIRE(GenerateTwoMaterialGlb(glbPath, genErr));
	auto scenePath = dir / "c4.rt2scene";

	SceneManager mgr;
	REQUIRE(mgr.LoadScene(glbPath.string()));
	auto& reg = mgr.GetECS().registry;
	const entt::entity prim0 = FindGltfEntity(reg, "gltf:scene=0:node=0:mesh=0:primitive=0");
	{ const bool prim0Ok = (prim0 != entt::null); REQUIRE(prim0Ok); }
	const auto uuid = mgr.GetEntityUuid(SceneManager::EntityId{prim0});
	REQUIRE(mgr.SetMaterialIndexState(uuid, 0, nullptr, nullptr).success);
	REQUIRE(FindOverride(reg, prim0)->sourceMaterialKey == "gltf:material:name=Red");

	Error saveErr;
	REQUIRE(SaveSceneForTest(mgr.AuthoringDoc(), scenePath, saveErr));
	REQUIRE(saveErr.IsOk());

	// Rename the first definition: the override's key ("name=Red") now names
	// nothing in the rebuilt source. Slots are unchanged, so slot fallback
	// still binds position 0 — but the miss must be loud.
	Error genErr2;
	REQUIRE(GenerateTwoMaterialGlb(glbPath, genErr2, /*swapDefs=*/false,
	                               /*renameRedTo=*/"Scarlet"));

	DeterministicUuidProvider provider2;
	SceneDocument loaded;
	loaded.SetUuidProvider(&provider2);
	Error loadErr;
	REQUIRE(SceneSerializer::Load(loaded, scenePath, loadErr));
	REQUIRE(loadErr.IsOk());

	std::vector<AssetDiagnostic> diagnostics;
	Error resolveErr;
	REQUIRE(SceneAssetResolver::ResolveAll(loaded,
		AssetResolutionContext{ dir, nullptr }, diagnostics, resolveErr));
	REQUIRE(resolveErr.IsOk());

	// Exactly one Stale diagnostic, naming the missing key.
	CHECK(CountStale(diagnostics) == 1);
	for (const auto& d : diagnostics)
		if (d.severity == AssetDiagnostic::Stale)
			CHECK(d.sourceKey == "gltf:material:name=Red");

	// The override is still applied (slot fallback) with its authored
	// snapshot intact.
	const entt::entity le = loaded.FindByUuid(uuid);
	{ const bool leOk = (le != entt::null); REQUIRE(leOk); }
	auto& lreg = loaded.ecs.registry;
	const auto* lov = FindOverride(lreg, le);
	REQUIRE(lov);
	CHECK(lov->material.baseColor.x == doctest::Approx(0.9f).epsilon(1e-4f));
	const auto* lref = lreg.try_get<MeshRef>(le);
	REQUIRE(lref);
	REQUIRE(lov->materialIndex >= 0);
	CHECK(lref->materialIndex == lov->materialIndex);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C4 migration: legacy "<meshKey>:material" keys are rebased to the new form
// at resolve time (D4). glTF: the legacy key encodes the mesh's historical
// material slot, so it is rewritten to the identity of the material now at
// that slot — the file migrates in place on its next save. OBJ: no concrete
// slot (per-triangle materials), so the key stays legacy with its historical
// entity-wide semantics, and no diagnostic is raised (it is obsolete, not a
// miss). Fault for red: disable the rebase assignment in the commit pass.
// ---------------------------------------------------------------------------
TEST_CASE("P8 pre-work 2: legacy keys are rebased (glTF) or kept (OBJ)")
{
	auto dir = UniqueTempDir("p8p2_c4d4");
	auto glbPath = dir / "twomat.glb";
	Error genErr;
	REQUIRE(GenerateTwoMaterialGlb(glbPath, genErr));
	auto objFixture = MakeTwoMatObj(dir, "twomat");

	// glTF: legacy key rewritten to the material identity at its slot.
	{
		auto scenePath = dir / "legacy_gltf.rt2scene";
		SceneManager mgr;
		REQUIRE(mgr.LoadScene(glbPath.string()));
		auto& reg = mgr.GetECS().registry;
		const entt::entity prim0 = FindGltfEntity(reg, "gltf:scene=0:node=0:mesh=0:primitive=0");
		{ const bool prim0Ok = (prim0 != entt::null); REQUIRE(prim0Ok); }
		const auto uuid = mgr.GetEntityUuid(SceneManager::EntityId{prim0});
		REQUIRE(mgr.SetMaterialIndexState(uuid, 0, nullptr, nullptr).success);
		REQUIRE(FindOverride(reg, prim0)->sourceMaterialKey == "gltf:material:name=Red");

		Error saveErr;
		REQUIRE(SaveSceneForTest(mgr.AuthoringDoc(), scenePath, saveErr));
		REQUIRE(saveErr.IsOk());

		DeterministicUuidProvider provider2;
		SceneDocument loaded;
		loaded.SetUuidProvider(&provider2);
		Error loadErr;
		REQUIRE(SceneSerializer::Load(loaded, scenePath, loadErr));
		REQUIRE(loadErr.IsOk());

		// Hand-craft the pre-work-2 authored form: the mesh key plus the
		// ":material" suffix (what RecordMaterialOverride used to write).
		const entt::entity le = loaded.FindByUuid(uuid);
		{ const bool leOk = (le != entt::null); REQUIRE(leOk); }
		auto* lov = FindOverride(loaded.ecs.registry, le);
		REQUIRE(lov);
		lov->sourceMaterialKey = "gltf:scene=0:node=0:mesh=0:primitive=0:material";

		std::vector<AssetDiagnostic> diagnostics;
		Error resolveErr;
		REQUIRE(SceneAssetResolver::ResolveAll(loaded,
			AssetResolutionContext{ dir, nullptr }, diagnostics, resolveErr));
		REQUIRE(resolveErr.IsOk());

		// Rebases in place; the override still applies to the material at the
		// historical slot.
		CHECK(CountStale(diagnostics) == 0);
		const auto* lov2 = FindOverride(loaded.ecs.registry, le);
		REQUIRE(lov2);
		CHECK(lov2->sourceMaterialKey == "gltf:material:name=Red");
		const auto* lref = loaded.ecs.registry.try_get<MeshRef>(le);
		REQUIRE(lref);
		REQUIRE(lov2->materialIndex >= 0);
		CHECK(lref->materialIndex == lov2->materialIndex);
	}

	// OBJ: legacy key kept as-is, no diagnostic.
	{
		auto scenePath = dir / "legacy_obj.rt2scene";
		SceneManager mgr;
		REQUIRE(mgr.LoadScene(objFixture.objPath.string()));
		auto& reg = mgr.GetECS().registry;
		const entt::entity mega = FindObjEntity(reg, "obj:whole-model");
		{ const bool megaOk = (mega != entt::null); REQUIRE(megaOk); }
		const auto uuid = mgr.GetEntityUuid(SceneManager::EntityId{mega});
		REQUIRE(mgr.SetMaterialIndexState(uuid, 0, nullptr, nullptr).success);
		REQUIRE(FindOverride(reg, mega)->sourceMaterialKey == "obj:material:name=mat_a");

		Error saveErr;
		REQUIRE(SaveSceneForTest(mgr.AuthoringDoc(), scenePath, saveErr));
		REQUIRE(saveErr.IsOk());

		DeterministicUuidProvider provider2;
		SceneDocument loaded;
		loaded.SetUuidProvider(&provider2);
		Error loadErr;
		REQUIRE(SceneSerializer::Load(loaded, scenePath, loadErr));
		REQUIRE(loadErr.IsOk());

		const entt::entity le = loaded.FindByUuid(uuid);
		{ const bool leOk = (le != entt::null); REQUIRE(leOk); }
		auto* lov = FindOverride(loaded.ecs.registry, le);
		REQUIRE(lov);
		lov->sourceMaterialKey = "obj:whole-model:material";

		std::vector<AssetDiagnostic> diagnostics;
		Error resolveErr;
		REQUIRE(SceneAssetResolver::ResolveAll(loaded,
			AssetResolutionContext{ dir, nullptr }, diagnostics, resolveErr));
		REQUIRE(resolveErr.IsOk());

		CHECK(CountStale(diagnostics) == 0);
		const auto* lov2 = FindOverride(loaded.ecs.registry, le);
		REQUIRE(lov2);
		CHECK(lov2->sourceMaterialKey == "obj:whole-model:material");
	}

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C5: two materials on ONE mesh (shared-mesh primitives). Overrides recorded
// on both primitives; after a definition-swap reorder each override must
// follow ITS material by name. Index-form keys cannot: "index=0" re-binds to
// whatever definition sits at slot 0 after the swap, silently swapping which
// override owns which texture. Fault for red: mint index-form keys in the
// load path.
// ---------------------------------------------------------------------------
TEST_CASE("P8 pre-work 2: shared-mesh materials follow their names on reorder")
{
	auto dir = UniqueTempDir("p8p2_c5");
	auto glbPath = dir / "twomat.glb";
	Error genErr;
	REQUIRE(GenerateTwoMaterialGlb(glbPath, genErr));
	auto scenePath = dir / "c5.rt2scene";

	SceneManager mgr;
	REQUIRE(mgr.LoadScene(glbPath.string()));
	auto& reg = mgr.GetECS().registry;
	const entt::entity prim0 = FindGltfEntity(reg, "gltf:scene=0:node=0:mesh=0:primitive=0");
	const entt::entity prim1 = FindGltfEntity(reg, "gltf:scene=0:node=0:mesh=0:primitive=1");
	{ const bool prim0Ok = (prim0 != entt::null); REQUIRE(prim0Ok); }
	{ const bool prim1Ok = (prim1 != entt::null); REQUIRE(prim1Ok); }
	const auto uuid0 = mgr.GetEntityUuid(SceneManager::EntityId{prim0});
	const auto uuid1 = mgr.GetEntityUuid(SceneManager::EntityId{prim1});
	REQUIRE(mgr.SetMaterialIndexState(uuid0, 0, nullptr, nullptr).success);
	REQUIRE(mgr.SetMaterialIndexState(uuid1, 1, nullptr, nullptr).success);
	REQUIRE(FindOverride(reg, prim0)->sourceMaterialKey == "gltf:material:name=Red");
	REQUIRE(FindOverride(reg, prim1)->sourceMaterialKey == "gltf:material:name=Green");

	Error saveErr;
	REQUIRE(SaveSceneForTest(mgr.AuthoringDoc(), scenePath, saveErr));
	REQUIRE(saveErr.IsOk());

	Error genErr2;
	REQUIRE(GenerateTwoMaterialGlb(glbPath, genErr2, /*swapDefs=*/true));

	DeterministicUuidProvider provider2;
	SceneDocument loaded;
	loaded.SetUuidProvider(&provider2);
	Error loadErr;
	REQUIRE(SceneSerializer::Load(loaded, scenePath, loadErr));
	REQUIRE(loadErr.IsOk());

	const int texBase = (int)loaded.ecs.textures.size();
	std::vector<AssetDiagnostic> diagnostics;
	Error resolveErr;
	REQUIRE(SceneAssetResolver::ResolveAll(loaded,
		AssetResolutionContext{ dir, nullptr }, diagnostics, resolveErr));
	REQUIRE(resolveErr.IsOk());
	CHECK(CountStale(diagnostics) == 0);

	const entt::entity le0 = loaded.FindByUuid(uuid0);
	const entt::entity le1 = loaded.FindByUuid(uuid1);
	{ const bool le0Ok = (le0 != entt::null); REQUIRE(le0Ok); }
	{ const bool le1Ok = (le1 != entt::null); REQUIRE(le1Ok); }
	const auto* ov0 = FindOverride(loaded.ecs.registry, le0);
	const auto* ov1 = FindOverride(loaded.ecs.registry, le1);
	REQUIRE(ov0);
	REQUIRE(ov1);
	CHECK(ov0->material.baseColorTextureIndex == texBase);     // Red / texA
	CHECK(ov1->material.baseColorTextureIndex == texBase + 1); // Green / texB

	std::filesystem::remove_all(dir);
}
