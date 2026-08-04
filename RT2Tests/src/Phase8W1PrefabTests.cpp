#include <doctest/doctest.h>

#include "AssetIdentity.h"
#include "EditorCommandHistory.h"
#include "EditorStructuralCommands.h"
#include "MeshRegistry.h"
#include "PrefabSerializer.h"
#include "SceneAssetReferenceVisitor.h"
#include "SceneAssetResolver.h"
#include "SceneManager.h"
#include "SceneSerializer.h"
#include "SceneSerializerTestSupport.h"
#include "SubtreeSnapshot.h"
#include "core/Error.h"
#include "core/UUID.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace rt2::core;

// ============================================================================
// Phase 8 W1 — create prefab from subtree, and instantiate (implementation
// spec, docs/game-engine-development-plan.md "Phase 8 — Prefabs", W1).
//
// W1 delivers the three SceneManager methods (CreatePrefabFromSubtree,
// InstantiatePrefabWithUuids, CountCanonicalPrefabEntities) plus the two undo
// commands (CreatePrefabCommand, asset-side file rewrite; InstantiatePrefab
// Command, scene-side RestoreSubtrees/RemoveSubtreesExact). An instance is a
// faithful copy plus a link (PrefabInstanceComponent on the root,
// PrefabMemberComponent on every member); nothing distinguishes overridden vs
// inherited yet (that is W3).
//
// Each test carries a discrimination proof (fault, confirm red, revert,
// confirm green) recorded in the verification report. Faults are described in
// the header comment of each test.
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

std::string ReadFileBinary(const std::filesystem::path& p)
{
	std::ifstream in(p, std::ios::binary);
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

void WriteRaw(const std::filesystem::path& p, const std::string& content)
{
	std::ofstream out(p, std::ios::binary | std::ios::trunc);
	out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

struct PrefabFixture
{
	DeterministicUuidProvider ids;
	SceneManager manager;

	PrefabFixture()
	{
		manager.SetUuidProvider(&ids);
		manager.AddMaterial(SceneMaterial{});
	}

	bool EntityAlive(const UUID& uuid) const
	{
		return manager.FindEntityByUuid(uuid) != entt::null;
	}

	UUID CreateEmpty(const char* name)
	{
		return manager.CreateEmpty(name).affectedEntities.front();
	}

	UUID CreateChild(const char* name, const UUID& parent)
	{
		return manager.CreateEmpty(name, parent).affectedEntities.front();
	}

	UUID CreateCube()
	{
		EditableTRS trs;
		const auto uuid = manager.ReserveKnownUuid();
		REQUIRE(manager.CreatePrimitiveEntity(
			uuid, "Cube", PrimitiveComponent::Cube, 1.0f, trs, 0).success);
		return uuid;
	}

	// Build a root + one child under it. Returns (root, child).
	std::pair<UUID, UUID> RootWithChild(const char* rootName)
	{
		const auto root = CreateEmpty(rootName);
		const auto child = CreateChild("Child", root);
		return { root, child };
	}
};

// Re-run of the W2 script-field helpers so a prefab instance can carry a
// Uuid-typed script field pointing at a sibling and we can read it back on
// the copied instance.
ScriptComponent& ScriptFor(SceneManager& manager, const UUID& entity)
{
	const auto handle = manager.FindEntityByUuid(entity);
	REQUIRE(static_cast<uint32_t>(handle) != static_cast<uint32_t>(entt::null));
	auto& registry = manager.GetECS().registry;
	auto* existing = registry.try_get<ScriptComponent>(handle);
	auto& script = existing
		? *existing
		: registry.emplace<ScriptComponent>(handle);
	script.asset.kind = AssetKind::Script;
	script.asset.path = "shared.lua";
	script.asset.sourceKey = "lua:asset=shared.lua";
	return script;
}

void SetUuidField(SceneManager& manager, const UUID& entity,
                  const char* name, const UUID& value)
{
	ScriptFor(manager, entity)
		.fieldValues[name] = ScriptFieldEntry{ ScriptFieldType::Uuid, value };
}

UUID FieldUuid(const ScriptComponent& script, const char* name)
{
	const auto it = script.fieldValues.find(name);
	REQUIRE(it != script.fieldValues.end());
	const auto* value = std::get_if<UUID>(&it->second.value);
	REQUIRE(value != nullptr);
	return *value;
}

} // namespace

// ---------------------------------------------------------------------------
// W1-A: instantiating a prefab multiple times produces unique scene UUIDs, and
// a script Uuid field pointing at a prefab sibling resolves to THAT instance's
// copy — never the template sibling and never another instance's sibling.
//
// Fault for red: remove the RemapCopiedScriptFields call from
// InstantiatePrefabWithUuids (W2 remapper). The first instance's field then
// keeps pointing at the template child (record.uuid), not the instance child.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1: multiple instances get unique UUIDs and remapped internal references")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("Robot");
	SetUuidField(f.manager, root, "sibling", child);

	const auto dir = UniqueTempDir("p8w1_dupe_refs");
	const auto prefabPath = dir / "robot.rt2prefab";
	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	REQUIRE(created.ok);
	REQUIRE(created.templateIds.size() == 2);

	// First instantiation.
	std::vector<AssetDiagnostic> diags1;
	const auto uuids1 = f.manager.ReserveKnownUuids(2);
	const auto inst1 = f.manager.InstantiatePrefabWithUuids(
		prefabPath, uuids1, diags1);
	REQUIRE(inst1.mutation.success);
	REQUIRE(inst1.createdRoots.size() == 1);
	REQUIRE(inst1.instanceId.has_value());
	// Roots are pre-order; root comes first.
	const auto inst1Root = uuids1[0];
	const auto inst1Child = uuids1[1];
	REQUIRE(inst1.createdRoots.front() == inst1Root);
	REQUIRE(f.EntityAlive(inst1Root));
	REQUIRE(f.EntityAlive(inst1Child));

	const auto s1 = f.manager.GetScriptState(inst1Root);
	REQUIRE(s1.has_value());
	CHECK(FieldUuid(*s1, "sibling") == inst1Child);   // instance's own child
	CHECK(FieldUuid(*s1, "sibling") != child);        // not the template child

	// Second instantiation: distinct UUIDs, its own sibling.
	std::vector<AssetDiagnostic> diags2;
	const auto uuids2 = f.manager.ReserveKnownUuids(2);
	const auto inst2 = f.manager.InstantiatePrefabWithUuids(
		prefabPath, uuids2, diags2);
	REQUIRE(inst2.mutation.success);
	const auto inst2Root = uuids2[0];
	const auto inst2Child = uuids2[1];

	// All four instance UUIDs unique and distinct from the template scene UUIDs.
	const UUID all[] = { inst1Root, inst1Child, inst2Root, inst2Child };
	for (auto a : all)
		CHECK(a != root);
	for (std::size_t i = 0; i < 4; ++i)
		for (std::size_t j = i + 1; j < 4; ++j)
			CHECK(all[i] != all[j]);

	const auto s2 = f.manager.GetScriptState(inst2Root);
	REQUIRE(s2.has_value());
	CHECK(FieldUuid(*s2, "sibling") == inst2Child);   // its own child
	CHECK(FieldUuid(*s2, "sibling") != inst1Child);   // not the other instance
	CHECK(FieldUuid(*s2, "sibling") != child);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// W1-B: a .rt2prefab file NEVER contains a resource-table index, by HARD RULE
// (PrefabSerializer.h). MeshRef::meshIndex / MeshRef::materialIndex /
// MaterialOverrideComponent::materialIndex are transient and stripped. The
// record reloads with the stripped fields absent (meshIndex=0, materialIndex
// = -1 defaults). B1 is VERIFIED INTENDED: no test floor asserts an instance
// material value; the rule is documented, not a defect.
//
// Fault for red: skip the StripTransientIndices pass in
// PrefabRecordToJson — the file then contains "materialIndex".
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1: prefab file never contains a resource-table index")
{
	PrefabFixture f;
	const auto cube = f.CreateCube(); // has PrimitiveComponent + MeshRef

	const auto dir = UniqueTempDir("p8w1_no_indices");
	const auto prefabPath = dir / "cube.rt2prefab";
	const auto created = f.manager.CreatePrefabFromSubtree({ cube }, prefabPath);
	REQUIRE(created.ok);

	// Raw bytes: the file must contain no resource-table index anywhere.
	const std::string raw = ReadFileBinary(prefabPath);
	CHECK(raw.find("materialIndex") == std::string::npos);
	CHECK(raw.find("meshIndex") == std::string::npos);
	CHECK(raw.find("TextureIndex") == std::string::npos);
	CHECK(raw.find("\"templateId\"") != std::string::npos);
	CHECK(raw.find("\"primitive\"") != std::string::npos);

	// Reload: stripped fields are absent so they take their defaults.
	PrefabDocument doc;
	Error loadErr;
	REQUIRE(PrefabSerializer::Load(doc, prefabPath, loadErr));
	REQUIRE(loadErr.IsOk());
	REQUIRE(doc.entities.size() == 1);
	const auto& rec = doc.entities[0].record;
	CHECK(rec.hasPrimitive);
	CHECK(rec.primitive.kind == PrimitiveComponent::Cube);
	CHECK(rec.meshIndex == 0);       // stripped -> default
	CHECK(rec.materialIndex == -1);  // stripped -> default (HARD RULE default)

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// W1-C: CreatePrefabFromSubtree writes a valid, loadable .rt2prefab; templateId
// is stable identity, not derived from scene UUIDs (amendment A1). The scene
// is NOT mutated by creation (asset-side). Count/instanced templateIds match
// the file.
//
// Fault for red: mint templateId from the entity's scene UUID (derive from
// record.uuid) instead of a fresh ID — templateId then equals the scene UUID.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1: prefab file valid, templateIds stable and not derived from scene UUIDs")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("Rig");
	const auto beforeCount = f.manager.GetEntityCount();

	const auto dir = UniqueTempDir("p8w1_template_ids");
	const auto prefabPath = dir / "rig.rt2prefab";
	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	REQUIRE(created.ok);
	REQUIRE(created.prefabPath == prefabPath);
	REQUIRE_FALSE(created.assetId.IsNull());
	REQUIRE(created.sourceSnapshot.entities.size() == 2);
	REQUIRE(created.templateIds.size() == 2);

	// Creation must not mutate the scene.
	CHECK(f.manager.GetEntityCount() == beforeCount);

	// templateIds are fresh, distinct, and NOT the template scene UUIDs.
	CHECK_FALSE(created.templateIds[0].IsNull());
	CHECK_FALSE(created.templateIds[1].IsNull());
	CHECK(created.templateIds[0] != created.templateIds[1]);
	CHECK(created.templateIds[0] != root);
	CHECK(created.templateIds[0] != child);
	CHECK(created.templateIds[1] != root);
	CHECK(created.templateIds[1] != child);

	// The file is loadable with the same record count and same templateIds.
	REQUIRE(f.manager.CountCanonicalPrefabEntities(prefabPath).IsOk());
	REQUIRE(f.manager.CountCanonicalPrefabEntities(prefabPath).value == 2);

	PrefabDocument doc;
	Error loadErr;
	REQUIRE(PrefabSerializer::Load(doc, prefabPath, loadErr));
	REQUIRE(loadErr.IsOk());
	REQUIRE(doc.entities.size() == 2);
	for (std::size_t i = 0; i < 2; ++i)
		CHECK(doc.entities[i].templateId == created.templateIds[i]);

	// Instanced members carry exactly the file's templateIds.
	// (Instantiation path is covered fully in W1-A; here we pin templateId.)
	std::vector<AssetDiagnostic> diags;
	const auto instUuids = f.manager.ReserveKnownUuids(2);
	const auto inst = f.manager.InstantiatePrefabWithUuids(
		prefabPath, instUuids, diags);
	REQUIRE(inst.mutation.success);
	{
		auto& reg = f.manager.GetECS().registry;
		const auto rootHandle = f.manager.FindEntityByUuid(instUuids[0]);
		const auto childHandle = f.manager.FindEntityByUuid(instUuids[1]);
		const auto* m0 = reg.try_get<PrefabMemberComponent>(rootHandle);
		const auto* m1 = reg.try_get<PrefabMemberComponent>(childHandle);
		REQUIRE(m0);
		REQUIRE(m1);
		CHECK(m0->templateId == created.templateIds[0]);
		CHECK(m1->templateId == created.templateIds[1]);
	}

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// W1-D: round-trip. Create prefab, instantiate, save scene, reload scene — the
// instance link components (PrefabInstanceComponent on root, PrefabMember on
// every member) survive EXACTLY (same instanceId, same templateIds).
//
// Fault for red: omit hasPrefabInstance/hasPrefabMember from
// BuildDocumentFromRecords — the reloaded scene then has no link components.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1: instance link components survive scene round-trip")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("Asset");
	const auto dir = UniqueTempDir("p8w1_roundtrip");
	const auto prefabPath = dir / "asset.rt2prefab";
	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	REQUIRE(created.ok);

	std::vector<AssetDiagnostic> instDiags;
	const auto instUuids = f.manager.ReserveKnownUuids(2);
	const auto inst = f.manager.InstantiatePrefabWithUuids(
		prefabPath, instUuids, instDiags);
	REQUIRE(inst.mutation.success);
	const UUID instanceId = *inst.instanceId;

	// Save the live scene and reload into a fresh document.
	const auto scenePath = dir / "roundtrip.rt2scene";
	Error saveErr;
	REQUIRE(SaveSceneForTest(f.manager.AuthoringDoc(), scenePath, saveErr));
	REQUIRE(saveErr.IsOk());

	DeterministicUuidProvider provider2;
	SceneDocument loaded;
	loaded.SetUuidProvider(&provider2);
	Error loadErr;
	REQUIRE(SceneSerializer::Load(loaded, scenePath, loadErr));
	REQUIRE(loadErr.IsOk());

	std::vector<AssetDiagnostic> resolveDiags;
	Error resolveErr;
	REQUIRE(SceneAssetResolver::ResolveAll(loaded,
		AssetResolutionContext{ dir, nullptr }, resolveDiags, resolveErr));

	// Find the reloaded instance root and child by their (unchanged) UUIDs.
	const auto rootHandle = loaded.FindByUuid(instUuids[0]);
	const auto childHandle = loaded.FindByUuid(instUuids[1]);
	REQUIRE(static_cast<uint32_t>(rootHandle) != static_cast<uint32_t>(entt::null));
	REQUIRE(static_cast<uint32_t>(childHandle) != static_cast<uint32_t>(entt::null));

	const auto* instComp = loaded.ecs.registry.try_get<PrefabInstanceComponent>(rootHandle);
	const auto* msg0 = loaded.ecs.registry.try_get<PrefabMemberComponent>(rootHandle);
	const auto* msg1 = loaded.ecs.registry.try_get<PrefabMemberComponent>(childHandle);
	// Fault for red: the link components were dropped from the load path.
	REQUIRE(instComp);
	REQUIRE(msg0);
	REQUIRE(msg1);
	CHECK(instComp->instanceId == instanceId);
	CHECK(instComp->prefab.kind == AssetKind::Prefab);
	CHECK(msg0->instanceId == instanceId);
	CHECK(msg0->templateId == created.templateIds[0]);
	CHECK(msg1->instanceId == instanceId);
	CHECK(msg1->templateId == created.templateIds[1]);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// W1-E: InstantiatePrefabCommand Undo/Redo. RecordApplied after the host
// instantiates; Undo removes the instance exactly (RemoveSubtreesExact), Redo
// restores it with the SAME UUIDs AND the link components verbatim.
//
// Fault for red: drop hasPrefabInstance/hasPrefabMember from
// ApplySubtreeRecord (used by RestoreSubtrees) — Redo then restores the
// entities but without the PrefabInstance/PrefabMember link.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1: InstantiatePrefabCommand Undo/Redo restores link")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("Inst");
	const auto dir = UniqueTempDir("p8w1_inst_cmd");
	const auto prefabPath = dir / "inst.rt2prefab";
	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	REQUIRE(created.ok);

	// Host reserves UUIDs and applies the instantiation.
	const auto uuids = f.manager.ReserveKnownUuids(2);
	std::vector<AssetDiagnostic> diags;
	const auto inst = f.manager.InstantiatePrefabWithUuids(
		prefabPath, uuids, diags);
	REQUIRE(inst.mutation.success);
	const UUID instanceId = *inst.instanceId;
	const auto instRoot = uuids[0];
	const auto instChild = uuids[1];
	REQUIRE(f.EntityAlive(instRoot));

	// Capture the instance snapshot for the command and record it.
	auto snapshot = f.manager.CaptureSubtreeSnapshot(inst.createdRoots);
	REQUIRE(snapshot.entities.size() == 2);
	auto cmd = MakeInstantiatePrefabCommand(std::move(snapshot), inst.createdRoots);
	REQUIRE(cmd);

	EditorCommandHistory history;
	EditorMutationResult applied;
	applied.success = true;
	applied.syncImpact = inst.mutation.syncImpact;
	applied.affectedEntities = inst.mutation.affectedEntities;
	REQUIRE(history.RecordApplied(std::move(cmd), f.manager, applied).success);
	REQUIRE(history.CanUndo());

	// Undo removes the instance (Exactly).
	REQUIRE(history.Undo(f.manager).success);
	REQUIRE_FALSE(f.EntityAlive(instRoot));
	REQUIRE_FALSE(f.EntityAlive(instChild));

	// Redo restores SAME UUIDs and the link.
	REQUIRE(history.Redo(f.manager).success);
	REQUIRE(f.EntityAlive(instRoot));
	REQUIRE(f.EntityAlive(instChild));
	{
		auto& reg = f.manager.GetECS().registry;
		const auto rootHandle = f.manager.FindEntityByUuid(instRoot);
		const auto childHandle = f.manager.FindEntityByUuid(instChild);
		const auto* instComp = reg.try_get<PrefabInstanceComponent>(rootHandle);
		const auto* msg0 = reg.try_get<PrefabMemberComponent>(rootHandle);
		const auto* msg1 = reg.try_get<PrefabMemberComponent>(childHandle);
		REQUIRE(instComp);
		REQUIRE(msg0);
		REQUIRE(msg1);
		CHECK(instComp->instanceId == instanceId);
		CHECK(msg0->instanceId == instanceId);
		CHECK(msg0->templateId == created.templateIds[0]);
		CHECK(msg1->instanceId == instanceId);
		CHECK(msg1->templateId == created.templateIds[1]);
	}

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// W1-F: CreatePrefabCommand Undo/Redo (asset-side, file rewrite). Captured
// prior contents are restored on Undo (and the file removed when it did not
// exist); Redo deterministically regenerates the .rt2prefab.
//
// Fault for red: remove the before-contents restore in CreatePrefabCommand::
// Undo (write empty / always remove) — a pre-existing file's bytes are then
// lost on Undo.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1: CreatePrefabCommand Undo restores prior contents, Redo regenerates")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("PrefabAsset");
	const auto dir = UniqueTempDir("p8w1_create_cmd");
	const auto prefabPath = dir / "restored.rt2prefab";

	// Pre-existing stale file whose bytes Undo must restore verbatim.
	const std::string stale = "{\"header\":\"rt2prefab\",\"version\":1,\"entities\":[]}\n";
	WriteRaw(prefabPath, stale);
	const std::vector<uint8_t> before(stale.begin(), stale.end());

	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	REQUIRE(created.ok);
	REQUIRE(ReadFileBinary(prefabPath) != stale); // overwritten by the create

	auto cmd = MakeCreatePrefabCommand(prefabPath, created, before, true);
	REQUIRE(cmd);

	EditorCommandHistory history;
	REQUIRE(history.Execute(std::move(cmd), f.manager).success);
	// Redo is a no-op re-write; after Execute the file is the created bytes.
	const std::string postExecute = ReadFileBinary(prefabPath);
	CHECK(postExecute != stale);

	// Undo restores the exact prior bytes.
	REQUIRE(history.Undo(f.manager).success);
	CHECK(ReadFileBinary(prefabPath) == stale);

	// Redo regenerates the prefab deterministically (loadable, templateIds).
	REQUIRE(history.Redo(f.manager).success);
	PrefabDocument doc;
	Error loadErr;
	REQUIRE(PrefabSerializer::Load(doc, prefabPath, loadErr));
	REQUIRE(loadErr.IsOk());
	REQUIRE(doc.entities.size() == 2);
	CHECK(doc.entities[0].templateId == created.templateIds[0]);
	CHECK(doc.entities[1].templateId == created.templateIds[1]);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// W1-F2: CreatePrefabCommand Undo removes the file when it did not exist
// before the create (empty prior contents branch).
//
// Fault for red: in the empty-prior branch, CreatePrefabCommand::Undo leaves
// the generated file in place instead of removing it.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1: CreatePrefabCommand Undo removes a never-present file")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("Fresh");
	const auto dir = UniqueTempDir("p8w1_create_absent");
	const auto prefabPath = dir / "fresh.rt2prefab";
	REQUIRE_FALSE(std::filesystem::exists(prefabPath));

	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	REQUIRE(created.ok);
	REQUIRE(std::filesystem::exists(prefabPath));

	auto cmd = MakeCreatePrefabCommand(prefabPath, created, {}, false);
	REQUIRE(cmd);

	EditorCommandHistory history;
	REQUIRE(history.Execute(std::move(cmd), f.manager).success);
	REQUIRE(history.Undo(f.manager).success);
	// Fault for red: if Undo does not remove, this fails.
	CHECK_FALSE(std::filesystem::exists(prefabPath));

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// B2 REGRESSION: instantiating a primitive prefab must REGISTER its geometry
// (via RegisterPrimitiveMesh) so the instance MeshRef points at a real mesh.
// Without the fix the registry is never appended and the instance's
// meshIndex (rebased) is out of range.
//
// Fault for red: remove the RegisterPrimitiveMesh block in
// InstantiatePrefabWithUuids — mesh count does not grow and the instance's
// meshIndex >= GetCount().
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 B2: instantiating a primitive registers its mesh")
{
	PrefabFixture f;
	const auto cube = f.CreateCube();
	const auto meshBase = f.manager.GetECS().meshRegistry.GetCount();
	REQUIRE(meshBase == 1); // the cube from CreatePrimitiveEntity

	const auto dir = UniqueTempDir("p8w1_b2_primitive");
	const auto prefabPath = dir / "cube.rt2prefab";
	const auto created = f.manager.CreatePrefabFromSubtree({ cube }, prefabPath);
	REQUIRE(created.ok);

	std::vector<AssetDiagnostic> instDiags;
	const auto instUuid = f.manager.ReserveKnownUuid();
	const auto inst = f.manager.InstantiatePrefabWithUuids(
		prefabPath, { instUuid }, instDiags);
	REQUIRE(inst.mutation.success);
	REQUIRE(f.EntityAlive(instUuid));

	const auto& registry = f.manager.GetECS().meshRegistry;
	const auto countAfter = registry.GetCount();
	const auto handle = f.manager.FindEntityByUuid(instUuid);
	const auto* ref = f.manager.GetECS().registry.try_get<MeshRef>(handle);
	REQUIRE(ref);
	// Fault for red: without registration the registry was not appended, so
	// the instance's meshIndex (rebased past base) is out of range.
	REQUIRE(ref->meshIndex < countAfter);
	REQUIRE(countAfter == meshBase + 1);
	const auto& mesh = registry.GetMesh(ref->meshIndex);
	CHECK_FALSE(mesh.vertices.empty());
	// B1 documented intended: the instance material index is the stripped
	// default (-1), by HARD RULE. No floor asserts an instance material value.
	CHECK(ref->materialIndex == -1);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// P1 REGRESSION: a sidecar WRITE failure must fail CreatePrefabFromSubtree —
// never report success with a session-only identity. Because create already
// wrote the .rt2prefab before the identity step, the fix ROLLS BACK the asset
// file so create is atomic (asset+sidecar both commit, or neither).
//
// Fault for red: ignore idErr from ResolveOrAssign in CreatePrefabFromSubtree
// (the W1 shipped code) — create then reports ok=true with the prefab file
// present and no committed sidecar.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 P1: create fails atomically when the asset sidecar cannot be committed")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("NoSidecar");
	const auto dir = UniqueTempDir("p8w1_p1_create_fail");
	const auto prefabPath = dir / "blocked.rt2prefab";

	// Inject the failure: occupy the sidecar path with a directory so both
	// the read and the atomic replace fail inside AssetIdentity.
	const auto sidecarPath = AssetSidecarPath(prefabPath);
	{
		std::error_code ec;
		std::filesystem::create_directory(sidecarPath, ec);
		REQUIRE_FALSE(ec);
	}

	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	// Fault for red: W1 reported ok=true here.
	CHECK_FALSE(created.ok);
	CHECK(created.assetId.IsNull());
	// The asset file was rolled back (atomic create).
	CHECK_FALSE(std::filesystem::exists(prefabPath));
	// The scene is untouched.
	CHECK(f.manager.GetEntityCount() == 2);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// P1 REGRESSION: a sidecar failure at INSTANTIATE must fail before any scene
// mutation. The instance carries the prefab's durable identity, so an
// unresolvable identity is a hard pre-mutation failure.
//
// Fault for red: resolve the sidecar identity inline at the link step (W1
// shipped code) — the failure is detected AFTER entities/resources were
// merged, leaving a successful-looking mutation with a session-only ID.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 P1: instantiate fails before mutation when the sidecar is unreadable")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("InstBlocked");
	const auto dir = UniqueTempDir("p8w1_p1_inst_fail");
	const auto prefabPath = dir / "blocked2.rt2prefab";

	// Create a valid prefab (commits a sidecar), then sabotage the sidecar.
	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	REQUIRE(created.ok);
	const auto sidecarPath = AssetSidecarPath(prefabPath);
	{
		std::error_code ec;
		std::filesystem::remove(sidecarPath, ec);
		std::filesystem::create_directory(sidecarPath, ec);
		REQUIRE_FALSE(ec);
	}

	const auto beforeEntities = f.manager.GetEntityCount();
	const auto beforeMeshes = f.manager.GetECS().meshRegistry.GetCount();
	std::vector<AssetDiagnostic> diags;
	const auto uuids = f.manager.ReserveKnownUuids(2);
	const auto inst = f.manager.InstantiatePrefabWithUuids(prefabPath, uuids, diags);
	// Fault for red: W1 reported success (identity swallowed at the link step).
	CHECK_FALSE(inst.mutation.success);
	CHECK_FALSE(f.EntityAlive(uuids[0]));
	CHECK(f.manager.GetEntityCount() == beforeEntities);
	CHECK(f.manager.GetECS().meshRegistry.GetCount() == beforeMeshes);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// P1 REGRESSION: transactional rollback of a failed instantiate restores the
// appended mesh/material/texture rows to their pre-merge bases. The shipped
// rollback only destroyed entities + UUID index, leaking the merged resource
// rows. A post-merge failure is injected by a hierarchy cycle in the live
// scene that the instance merge then trips on at dst RebuildChildren.
//
// Fault for red: keep rollbackCreated limited to entity destruction (W1
// shipped code) — after a failed instantiate the mesh/material/texture counts
// stay inflated by the aborted merge.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 P1: failed instantiate rolls back merged resource rows")
{
	PrefabFixture f;
	const auto cube = f.CreateCube(); // primitive -> registers a mesh row
	const auto dir = UniqueTempDir("p8w1_p1_rollback");
	const auto prefabPath = dir / "res.rt2prefab";
	const auto created = f.manager.CreatePrefabFromSubtree({ cube }, prefabPath);
	REQUIRE(created.ok);

	// Inject a hierarchy cycle into the LIVE scene so the instance merge's
	// dst RebuildChildren fails AFTER resources were appended.
	{
		const auto a = f.manager.CreateEmpty("CycleA").affectedEntities.front();
		const auto b = f.manager.CreateEmpty("CycleB").affectedEntities.front();
		const auto ha = f.manager.FindEntityByUuid(a);
		const auto hb = f.manager.FindEntityByUuid(b);
		REQUIRE(static_cast<uint32_t>(ha) != static_cast<uint32_t>(entt::null));
		REQUIRE(static_cast<uint32_t>(hb) != static_cast<uint32_t>(entt::null));
		auto& reg = f.manager.GetECS().registry;
		reg.emplace_or_replace<Hierarchy>(ha).parent = hb;
		reg.emplace_or_replace<Hierarchy>(hb).parent = ha;
	}

	const auto beforeEntities = f.manager.GetEntityCount();
	const auto meshBase = f.manager.GetECS().meshRegistry.GetCount();
	const auto matBase = (int)f.manager.GetECS().materials.size();
	const auto texBase = (int)f.manager.GetECS().textures.size();
	// The cube must actually append a row on the (aborted) merge.
	REQUIRE(meshBase >= 1);

	std::vector<AssetDiagnostic> diags;
	const auto instUuid = f.manager.ReserveKnownUuid();
	const auto inst = f.manager.InstantiatePrefabWithUuids(
		prefabPath, { instUuid }, diags);
	CHECK_FALSE(inst.mutation.success);

	// Fault for red: shipped rollback left the merged rows behind.
	CHECK(f.manager.GetEntityCount() == beforeEntities);
	CHECK(f.manager.GetECS().meshRegistry.GetCount() == meshBase);
	CHECK((int)f.manager.GetECS().materials.size() == matBase);
	CHECK((int)f.manager.GetECS().textures.size() == texBase);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// P2 REGRESSION: a prefab must have EXACTLY ONE top-level root. Multi-root
// input is rejected with a structured diagnostic BEFORE any file write or
// sidecar commit. Children beneath a single root remain fully supported
// (covered by every W1 test above, which use root+child).
//
// Fault for red: skip the one-root check in CreatePrefabFromSubtree (W1
// shipped code) — a two-sibling input creates a prefab whose instantiation
// root/link placement is ill-defined.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 P2: multi-root create is rejected before any write")
{
	PrefabFixture f;
	const auto a = f.manager.CreateEmpty("RootA").affectedEntities.front();
	const auto b = f.manager.CreateEmpty("RootB").affectedEntities.front();
	const auto dir = UniqueTempDir("p8w1_p2_multiroot");
	const auto prefabPath = dir / "multi.rt2prefab";
	REQUIRE_FALSE(std::filesystem::exists(prefabPath));

	const auto created = f.manager.CreatePrefabFromSubtree({ a, b }, prefabPath);
	// Fault for red: W1 wrote the prefab with two roots and reported ok.
	CHECK_FALSE(created.ok);
	CHECK(created.error.code == rt2::core::Error::InvalidEntity);
	CHECK_FALSE(created.error.detail.empty());
	CHECK_FALSE(std::filesystem::exists(prefabPath));
	CHECK(f.manager.GetEntityCount() == 2);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// P2 REGRESSION: instantiate rejects a file with multiple top-level roots
// before any mutation. Hand-authored or corrupted prefab files are covered.
//
// Fault for red: assume the first record in the file is the root and link the
// instance there (W1 shipped code) — a two-root file instantiates "fine" but
// the PrefabInstanceComponent sits on the wrong entity.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 P2: instantiate rejects a multi-root prefab file")
{
	PrefabFixture f;
	const auto dir = UniqueTempDir("p8w1_p2_multiroot_file");
	const auto prefabPath = dir / "tworoots.rt2prefab";

	// Build a two-root prefab document directly (both records nil-parent).
	PrefabDocument doc;
	for (int i = 0; i < 2; ++i)
	{
		PrefabEntityRecord rec;
		rec.templateId = f.ids.CreateV4();
		rec.record.uuid = f.ids.CreateV4();
		rec.record.name = "Root";
		rec.record.visible = true;
		doc.entities.push_back(std::move(rec));
	}
	Error saveErr;
	REQUIRE(PrefabSerializer::Save(doc, prefabPath, saveErr));
	REQUIRE(saveErr.IsOk());

	const auto beforeEntities = f.manager.GetEntityCount();
	std::vector<AssetDiagnostic> diags;
	const auto uuids = f.manager.ReserveKnownUuids(2);
	const auto inst = f.manager.InstantiatePrefabWithUuids(prefabPath, uuids, diags);
	CHECK_FALSE(inst.mutation.success);
	CHECK_FALSE(f.EntityAlive(uuids[0]));
	CHECK(f.manager.GetEntityCount() == beforeEntities);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// P3 REGRESSION: CreatePrefabCommand must distinguish a pre-existing zero-byte
// file from an absent file. An empty before-contents vector is ambiguous; the
// fileExistedBefore flag disambiguates. Undo of a create over a pre-existing
// zero-byte file restores that file (still zero bytes) — it must NOT be
// removed.
//
// Fault for red: gate Undo on m_BeforeContents.empty() (W1 shipped code) —
// Undo removes the file, destroying the pre-existing zero-byte asset.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 P3: Undo restores a pre-existing zero-byte file rather than removing it")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("ZeroByte");
	const auto dir = UniqueTempDir("p8w1_p3_zerobyte");
	const auto prefabPath = dir / "zero.rt2prefab";

	// Pre-existing zero-byte file: contents are empty, but the file EXISTS.
	WriteRaw(prefabPath, "");
	REQUIRE(std::filesystem::exists(prefabPath));
	REQUIRE(std::filesystem::file_size(prefabPath) == 0);
	const std::vector<uint8_t> beforeEmpty; // captured before-contents: empty

	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	REQUIRE(created.ok);
	REQUIRE(std::filesystem::file_size(prefabPath) > 0); // overwritten by create

	auto cmd = MakeCreatePrefabCommand(prefabPath, created, beforeEmpty, true);
	REQUIRE(cmd);

	EditorCommandHistory history;
	REQUIRE(history.Execute(std::move(cmd), f.manager).success);
	REQUIRE(history.Undo(f.manager).success);
	// Fault for red: W1 removed the file here because before-contents was empty.
	CHECK(std::filesystem::exists(prefabPath));
	CHECK(std::filesystem::file_size(prefabPath) == 0);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// P3 REGRESSION: CreatePrefabCommand::Undo surfaces a failed file REMOVAL as a
// loud Failure (never a silent success). The remove branch is injected by
// turning the prefab path into a non-empty directory so remove() fails.
//
// Fault for red: W1 shipped Undo ignored the remove error code and returned
// success even when the file could not be removed.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 P3: Undo surfaces a failed file removal")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("Removal");
	const auto dir = UniqueTempDir("p8w1_p3_remove_fail");
	const auto prefabPath = dir / "removefail.rt2prefab";

	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	REQUIRE(created.ok);
	auto cmd = MakeCreatePrefabCommand(prefabPath, created, {}, false);
	REQUIRE(cmd);

	EditorCommandHistory history;
	REQUIRE(history.Execute(std::move(cmd), f.manager).success);

	// Sabotage removal: replace the file with a non-empty directory.
	{
		std::error_code ec;
		std::filesystem::remove(prefabPath, ec);
		std::filesystem::create_directories(prefabPath / "junk", ec);
		REQUIRE_FALSE(ec);
	}

	const auto undoResult = history.Undo(f.manager);
	// Fault for red: W1 returned success while the file was left behind.
	CHECK_FALSE(undoResult.success);
	CHECK(undoResult.error.code == rt2::core::Error::Io);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// P4 REGRESSION: SceneAssetReferenceVisitor must collect the prefab reference
// carried by PrefabInstanceComponent.prefab, alongside imported/script slots.
//
// Fault for red: W1 shipped visitor only handled ImportedMeshSourceComponent
// and ScriptComponent — a scene whose only reference is a prefab instance
// came back empty.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 P4: visitor collects the PrefabInstanceComponent.prefab reference")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("PrefabRef");
	const auto dir = UniqueTempDir("p8w1_p4_visitor");
	const auto prefabPath = dir / "visitor.rt2prefab";
	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	REQUIRE(created.ok);

	// Instantiate so the live authoring scene carries a PrefabInstanceComponent.
	std::vector<AssetDiagnostic> diags;
	const auto uuids = f.manager.ReserveKnownUuids(2);
	const auto inst = f.manager.InstantiatePrefabWithUuids(prefabPath, uuids, diags);
	REQUIRE(inst.mutation.success);

	const auto& doc = f.manager.AuthoringDoc();
	const auto slots = CollectSceneAssetReferences(doc);

	// The prefab reference must be collected exactly once (on the root).
	std::size_t prefabSlots = 0;
	const AssetReference* found = nullptr;
	for (const auto& slot : slots)
	{
		if (slot.reference && slot.reference->kind == AssetKind::Prefab)
		{
			++prefabSlots;
			found = slot.reference;
		}
	}
	CHECK(prefabSlots == 1);
	REQUIRE(found);
	CHECK(found->path == prefabPath.string());
	CHECK_FALSE(found->assetId.IsNull());

	std::filesystem::remove_all(dir);
}
// A UUID-count mismatch, or duplicate/nil/already-present UUIDs, fails
// atomically with zero mutation.
//
// Fault for red: remove the count check (or the seen-set duplicate check) in
// InstantiatePrefabWithUuids — a wrong count would silently proceed / corrupt.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1: instantiate validates UUID count and uniqueness atomically")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("Guard");
	const auto dir = UniqueTempDir("p8w1_guard");
	const auto prefabPath = dir / "guard.rt2prefab";
	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	REQUIRE(created.ok);

	const auto beforeCount = f.manager.GetEntityCount();

	// Too few UUIDs.
	std::vector<AssetDiagnostic> d1;
	const auto few = f.manager.ReserveKnownUuids(1);
	const auto r1 = f.manager.InstantiatePrefabWithUuids(prefabPath, few, d1);
	CHECK_FALSE(r1.mutation.success);
	CHECK(f.manager.GetEntityCount() == beforeCount);

	// Too many UUIDs.
	std::vector<AssetDiagnostic> d2;
	const auto many = f.manager.ReserveKnownUuids(5);
	const auto r2 = f.manager.InstantiatePrefabWithUuids(prefabPath, many, d2);
	CHECK_FALSE(r2.mutation.success);
	CHECK(f.manager.GetEntityCount() == beforeCount);

	// A nil UUID.
	std::vector<AssetDiagnostic> d3;
	auto wNil = f.manager.ReserveKnownUuids(2);
	wNil[0] = UUID::Nil();
	const auto r3 = f.manager.InstantiatePrefabWithUuids(prefabPath, wNil, d3);
	CHECK_FALSE(r3.mutation.success);
	CHECK(f.manager.GetEntityCount() == beforeCount);

	// A UUID already present in the scene.
	std::vector<AssetDiagnostic> d4;
	auto wDup = f.manager.ReserveKnownUuids(2);
	wDup[0] = root; // root already exists in the live scene
	const auto r4 = f.manager.InstantiatePrefabWithUuids(prefabPath, wDup, d4);
	CHECK_FALSE(r4.mutation.success);
	CHECK(f.manager.GetEntityCount() == beforeCount);

	std::filesystem::remove_all(dir);
}