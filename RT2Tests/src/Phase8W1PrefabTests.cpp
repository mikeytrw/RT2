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
#include "Phase1AFixtureGenerator.h"
#include "core/Error.h"
#include "core/PathTransaction.h"
#include "core/UUID.h"
#include "SceneDocument.h"

#include <glm/glm.hpp>
#include "json.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define UUID RT2_WIN_UUID
#include <windows.h>
#undef UUID
#endif

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

TEST_CASE("Phase 8 W1 transaction fault seam: capture and sidecar install are loud and recover exact bytes")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("FaultSeam");
	const auto dir = UniqueTempDir("p8w1_fault_seam");
	const auto prefabPath = dir / "fault.rt2prefab";
	const auto sidecarPath = AssetSidecarPath(prefabPath);
	const std::string stale = "stale-bytes\n";
	WriteRaw(prefabPath, stale);

	SetPathTransactionFaultForTests(PathTransactionFaultPoint::CaptureRead);
	const auto captureFail = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	ClearPathTransactionFaultForTests();
	CHECK_FALSE(captureFail.ok);
	CHECK(captureFail.error.code == Error::Io);
	CHECK(ReadFileBinary(prefabPath) == stale);
	CHECK_FALSE(std::filesystem::exists(sidecarPath));

	WriteRaw(sidecarPath, "11111111-1111-4111-8111-111111111111\n");
	const std::string priorAsset = ReadFileBinary(prefabPath);
	const std::string priorSidecar = ReadFileBinary(sidecarPath);
	SetPathTransactionFaultForTests(PathTransactionFaultPoint::SidecarInstall);
	const auto installFail = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	ClearPathTransactionFaultForTests();
	CHECK_FALSE(installFail.ok);
	CHECK(ReadFileBinary(prefabPath) == priorAsset);
	CHECK(ReadFileBinary(sidecarPath) == priorSidecar);

	std::filesystem::remove_all(dir);
}

TEST_CASE("Phase 8 W1 transaction binds entries beyond the first directory enumeration batch")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("LargeDirectory");
	const auto dir = UniqueTempDir("p8w1_large_directory");
	for (int i = 0; i < 3200; ++i)
		WriteRaw(dir / ("f" + std::to_string(i) + ".tmp"), "x");
	const auto prefabPath = dir / "zzzz_target.rt2prefab";
	const std::string original = "original-prefab\n";
	WriteRaw(prefabPath, original);

	SetPathTransactionFaultForTests(PathTransactionFaultPoint::AssetInstall);
	const auto rollback = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	ClearPathTransactionFaultForTests();
	CHECK_FALSE(rollback.ok);
	CHECK(ReadFileBinary(prefabPath) == original);

	const auto committed = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	CHECK(committed.ok);
	CHECK(ReadFileBinary(prefabPath) != original);
	std::filesystem::remove_all(dir);
}

TEST_CASE("Phase 8 W1 native fault seam: every transition fails loudly and preserves ownership")
{
	const auto exercise = [](PathTransactionFaultPoint point, bool existing) {
		const auto dir = UniqueTempDir("p8w1_fault_" + std::to_string(static_cast<int>(point)));
		const auto path = dir / "fault.rt2prefab";
		if (existing) WriteRaw(path, "prior\n");
		if (point == PathTransactionFaultPoint::ManifestFlush)
			SetPathTransactionFaultForTests(point);
		auto tx = PrefabFileTransaction::Begin(path, {}, false);
		auto cleanup = [&] {
			if (tx.IsOk()) tx.value.reset();
			ClearPathTransactionFaultForTests();
			std::error_code cleanupEc;
			std::filesystem::remove_all(dir, cleanupEc);
		};
		if (point == PathTransactionFaultPoint::ManifestFlush)
		{
			ClearPathTransactionFaultForTests();
			CHECK_FALSE(tx.IsOk());
			bool manifestResidue = false;
			for (const auto& e : std::filesystem::directory_iterator(dir))
				manifestResidue = manifestResidue || e.path().extension() == ".manifest";
			CHECK_FALSE(manifestResidue);
			cleanup();
			return;
		}
		REQUIRE(tx.IsOk());
		if (point == PathTransactionFaultPoint::CaptureOpen ||
			point == PathTransactionFaultPoint::CaptureRead ||
			point == PathTransactionFaultPoint::QuarantineRename)
			SetPathTransactionFaultForTests(point);
		auto captured = tx.value->CapturePair();
		if (point == PathTransactionFaultPoint::CaptureOpen || point == PathTransactionFaultPoint::CaptureRead || point == PathTransactionFaultPoint::QuarantineRename)
		{
			ClearPathTransactionFaultForTests();
			CHECK_FALSE(captured.IsOk());
			(void)tx.value->Rollback();
			cleanup();
			return;
		}
		REQUIRE(captured.IsOk());
		if (point == PathTransactionFaultPoint::RollbackRestore)
		{
			SetPathTransactionFaultForTests(point);
			auto rb = tx.value->Rollback();
			ClearPathTransactionFaultForTests();
			CHECK_FALSE(rb.IsOk());
			cleanup();
			return;
		}
		if (point == PathTransactionFaultPoint::StageCreate ||
			point == PathTransactionFaultPoint::StageWrite)
			SetPathTransactionFaultForTests(point);
		auto staged = tx.value->Stage(std::nullopt, std::vector<uint8_t>{'n','e','w'});
		if (point == PathTransactionFaultPoint::StageCreate || point == PathTransactionFaultPoint::StageWrite)
		{
			ClearPathTransactionFaultForTests();
			CHECK_FALSE(staged.IsOk());
			(void)tx.value->Rollback();
			cleanup();
			return;
		}
		REQUIRE(staged.IsOk());
		if (point == PathTransactionFaultPoint::AssetInstall)
			SetPathTransactionFaultForTests(point);
		auto installed = tx.value->InstallSidecarThenAsset();
		if (point == PathTransactionFaultPoint::AssetInstall)
		{
			ClearPathTransactionFaultForTests();
			CHECK_FALSE(installed.IsOk());
			(void)tx.value->Rollback();
			cleanup();
			return;
		}
		REQUIRE(installed.IsOk());
		if (point == PathTransactionFaultPoint::ManifestCleanup)
			SetPathTransactionFaultForTests(point);
		auto finalized = tx.value->Finalize();
		if (point == PathTransactionFaultPoint::ManifestCleanup)
		{
			ClearPathTransactionFaultForTests();
			REQUIRE(finalized.IsOk());
			CHECK(finalized.value.recoveryWarning.has_value());
		}
		cleanup();
	};
	exercise(PathTransactionFaultPoint::CaptureOpen, true);
	exercise(PathTransactionFaultPoint::CaptureRead, true);
	exercise(PathTransactionFaultPoint::QuarantineRename, true);
	exercise(PathTransactionFaultPoint::StageCreate, true);
	exercise(PathTransactionFaultPoint::StageWrite, true);
	exercise(PathTransactionFaultPoint::AssetInstall, true);
	exercise(PathTransactionFaultPoint::ManifestFlush, false);
	exercise(PathTransactionFaultPoint::RollbackRestore, true);
	exercise(PathTransactionFaultPoint::ManifestCleanup, false);
}

TEST_CASE("Phase 8 W1: reparse, dangling-link, and unsupported-volume gates are loud")
{
	const auto dir = UniqueTempDir("p8w1_reparse_gate");
#ifdef _WIN32
	const auto real = dir / "real.rt2prefab";
	const auto link = dir / "link.rt2prefab";
	const auto dangling = dir / "dangling.rt2prefab";
	WriteRaw(real, "prior\n");
	if (CreateSymbolicLinkW(link.wstring().c_str(), real.wstring().c_str(), 0) == FALSE)
	{
		// Developer-mode/symlink privilege is an environment prerequisite; the
		// gate itself is exercised where the platform permits link creation.
		CHECK(true);
	}
	else
	{
		auto tx = PrefabFileTransaction::Begin(link, {}, false);
		REQUIRE(tx.IsOk());
		auto captured = tx.value->CapturePair();
		CHECK_FALSE(captured.IsOk());
		(void)tx.value->Rollback();
		CHECK(ReadFileBinary(real) == "prior\n");
	}
	if (CreateSymbolicLinkW(dangling.wstring().c_str(),
		(dir / "missing.rt2prefab").wstring().c_str(), 0) != FALSE)
	{
		auto tx = PrefabFileTransaction::Begin(dangling, {}, false);
		REQUIRE(tx.IsOk());
		CHECK_FALSE(tx.value->CapturePair().IsOk());
		(void)tx.value->Rollback();
	}
	auto remote = PrefabFileTransaction::Begin(
		std::filesystem::path(L"\\\\server\\share\\not-local.rt2prefab"), {}, false);
	CHECK_FALSE(remote.IsOk());
#else
	CHECK_FALSE(PrefabFileTransaction::Begin(dir / "x.rt2prefab", {}, false).IsOk());
#endif
	std::filesystem::remove_all(dir);
}

TEST_CASE("Phase 8 W1: logical-commit manifest residue is recovered idempotently")
{
	const auto dir = UniqueTempDir("p8w1_recover_logical_commit");
	const auto path = dir / "recover.rt2prefab";
	WriteRaw(path, "before\n");
	auto tx = PrefabFileTransaction::Begin(path, {}, false);
	REQUIRE(tx.IsOk());
	REQUIRE(tx.value->CapturePair().IsOk());
	REQUIRE(tx.value->Stage(std::nullopt,
		std::optional<std::vector<uint8_t>>({ 'a','f','t','e','r','\n' })).IsOk());
	REQUIRE(tx.value->InstallSidecarThenAsset().IsOk());
	SetPathTransactionFaultForTests(PathTransactionFaultPoint::ManifestCleanup);
	auto finalized = tx.value->Finalize();
	ClearPathTransactionFaultForTests();
	REQUIRE(finalized.IsOk());
	CHECK(finalized.value.recoveryWarning.has_value());
	tx.value.reset();
	bool manifestSeen = false;
	for (const auto& entry : std::filesystem::directory_iterator(dir))
		manifestSeen = manifestSeen || entry.path().extension() == ".manifest";
	CHECK(manifestSeen);
	REQUIRE(PrefabFileTransaction::RecoverDirectory(dir).IsOk());
	REQUIRE(PrefabFileTransaction::RecoverDirectory(dir).IsOk());
	CHECK(ReadFileBinary(path) == "after\n");
	for (const auto& entry : std::filesystem::directory_iterator(dir))
		CHECK(entry.path().extension() != ".manifest");
	std::filesystem::remove_all(dir);
}

TEST_CASE("Phase 8 W1 recovery quarantines truncated and relocated manifests")
{
	const auto dir = UniqueTempDir("p8w1_recovery_corrupt_manifest");
	const auto truncated = dir / ".rt2txn-truncated.manifest";
	WriteRaw(truncated, "123456789");
	REQUIRE(PrefabFileTransaction::RecoverDirectory(dir).IsOk());
	CHECK_FALSE(std::filesystem::exists(truncated));
	CHECK(std::filesystem::exists(dir / ".rt2txn-truncated.manifest.corrupt"));
	REQUIRE(PrefabFileTransaction::RecoverDirectory(dir).IsOk());
	auto unrelated = PrefabFileTransaction::Begin(dir / "unrelated.rt2prefab", {}, false);
	REQUIRE(unrelated.IsOk());
	unrelated.value.reset();

	const auto external = UniqueTempDir("p8w1_recovery_parent_mismatch_external");
	const auto externalAsset = external / "outside.rt2prefab";
	WriteRaw(externalAsset, "DO NOT TOUCH\n");
	const auto planted = dir / ".rt2txn-planted.manifest";
	const std::string payload = "Prepared\nasset=" + externalAsset.string() + "\nsidecar=\n";
	std::array<uint8_t, 8192> image{};
	struct Header { uint32_t magic; uint32_t version; uint64_t generation; uint32_t length; uint32_t crc; };
	Header header{0x32544A52u, 1u, 1u, static_cast<uint32_t>(payload.size()), 0u};
	uint32_t crc = 0xffffffffu;
	for (unsigned char byte : payload)
	{
		crc ^= byte;
		for (int bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	header.crc = ~crc;
	std::memcpy(image.data(), &header, sizeof(header));
	std::memcpy(image.data() + sizeof(header), payload.data(), payload.size());
	{
		std::ofstream out(planted, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(image.data()), image.size());
	}
	REQUIRE(PrefabFileTransaction::RecoverDirectory(dir).IsOk());
	CHECK_FALSE(std::filesystem::exists(planted));
	CHECK(std::filesystem::exists(dir / ".rt2txn-planted.manifest.corrupt"));
	CHECK(ReadFileBinary(externalAsset) == "DO NOT TOUCH\n");
	std::filesystem::remove_all(external);
	std::filesystem::remove_all(dir);
}

TEST_CASE("Phase 8 W1 recovery skips a busy live manifest")
{
	const auto dir = UniqueTempDir("p8w1_recovery_busy");
	const auto firstPath = dir / "first.rt2prefab";
	const auto secondPath = dir / "second.rt2prefab";
	WriteRaw(firstPath, "first\n");
	auto first = PrefabFileTransaction::Begin(firstPath, {}, false);
	REQUIRE(first.IsOk());
	REQUIRE(first.value->CapturePair().IsOk());
	auto second = PrefabFileTransaction::Begin(secondPath, {}, false);
	REQUIRE(second.IsOk());
	second.value.reset();
	first.value.reset();
	CHECK(ReadFileBinary(firstPath) == "first\n");
	std::filesystem::remove_all(dir);
}

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

// ============================================================================
// SECOND REPAIR (Sol merge assessment @c9dfea6) regression suite.
// Each test carries a fault-to-red discrimination proof.
// ============================================================================

// ---------------------------------------------------------------------------
// C1a (Sol P0): a sidecar failure at CREATE over a PRE-EXISTING prefab must
// restore the prior asset verbatim, never delete the user's work. The W1
// fix rolled back by removing the target unconditionally, which destroyed a
// pre-existing .rt2prefab on a failed sidecar commit.
//
// Fault for red: in CreatePrefabFromSubtree's rollback block, restore the
// W1 behavior — remove the target regardless of whether it existed before.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 C1a: failed create over a pre-existing prefab restores the prior asset")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("Keep");
	const auto dir = UniqueTempDir("p8w1_c1a_existing");
	const auto prefabPath = dir / "kept.rt2prefab";

	// Commit a valid prefab with a durable sidecar identity first.
	const auto first = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	REQUIRE(first.ok);
	const std::string prior = ReadFileBinary(prefabPath);
	REQUIRE(!prior.empty());
	REQUIRE(std::filesystem::exists(AssetSidecarPath(prefabPath)));

	// Sabotage the sidecar so the SECOND create's identity commit fails.
	{
		std::error_code ec;
		std::filesystem::remove(AssetSidecarPath(prefabPath), ec);
		std::filesystem::create_directory(AssetSidecarPath(prefabPath), ec);
		REQUIRE_FALSE(ec);
	}

	// A second create over the same path: Save replaces the asset, then the
	// sidecar cannot be durably committed -> rollback must restore the prior
	// bytes (not remove the file).
	const auto [root2, child2] = f.RootWithChild("Replacement");
	const auto second = f.manager.CreatePrefabFromSubtree({ root2 }, prefabPath);
	// Fault for red: W1 removed the target, so ok would be false AND the
	// prior asset would be gone.
	CHECK_FALSE(second.ok);
	CHECK(second.assetId.IsNull());
	CHECK(std::filesystem::exists(prefabPath));
	CHECK(ReadFileBinary(prefabPath) == prior);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C1b (Sol P0): a create that cannot durably commit its asset must fail LOUDLY
// (an Io error, never a silent ok=true) and must not leave recoverable state
// behind. Save and the rollback share the same atomic tmp+replace primitive
// on the same path, so an obstructed target (a directory parked at the asset
// path) fails the whole create as one loud Io — the guarantee that a user's
// pre-existing prefab is never silently replaced or left half-written.
//
// Fault for red: swallow the Io from create and report ok=true (a silent
// success), or truncate the occupied path — this asserts loud failure.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 C1b: a create that cannot commit its asset fails loudly")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("Loud");
	const auto dir = UniqueTempDir("p8w1_c1b_loud");
	const auto prefabPath = dir / "loud.rt2prefab";

	// A non-empty directory parked at the asset path blocks both the atomic
	// replace and any rollback, so the create must fail loudly with Io.
	{
		std::error_code ec;
		std::filesystem::create_directories(prefabPath / "occupied", ec);
		REQUIRE_FALSE(ec);
	}

	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	// Fault for red: a swallowed error would report success here.
	CHECK_FALSE(created.ok);
	CHECK(created.error.code == rt2::core::Error::Io);
	CHECK_FALSE(created.error.detail.empty());
	// The occupied directory is left untouched (nothing durable was silently
	// replaced/removed).
	CHECK(std::filesystem::is_directory(prefabPath));

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C1c (Sol follow-up P1): CreatePrefabFromSubtree must capture the EXACT prior
// bytes of a pre-existing target BEFORE any asset or sidecar mutation, and
// fail LOUDLY when that capture cannot succeed — it must never substitute an
// empty vector as a read-failure sentinel (an empty vector is the valid prior
// state of a zero-byte file). A non-regular, unreadable target (a directory)
// must be refused at the prior-state precondition, before Save.
//
// Fault for red: W1 shipped a silent capture — `std::filesystem::exists`
// (throwing), an unopened ifstream leaving priorBytes empty, and an unused
// readEc — with no up-front regular-file requirement. A directory target was
// only rejected downstream by Save's own report, so the precondition detail
// below never appeared and empty prior bytes remained substitutable on a
// later rollback.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 C1c: create over an unreadable/non-regular target fails before mutation")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("LoudCapture");
	const auto dir = UniqueTempDir("p8w1_c1c_unreadable");
	const auto prefabPath = dir / "loudcapture.rt2prefab";

	// A non-empty directory has no readable byte contents and is not a regular
	// file; a create over it must fail during prior-state capture, BEFORE any
	// asset replace or sidecar commit.
	{
		std::error_code ec;
		std::filesystem::create_directories(prefabPath / "occupied", ec);
		REQUIRE_FALSE(ec);
	}

	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	CHECK_FALSE(created.ok);
	CHECK(created.error.code == rt2::core::Error::Io);
	CHECK_FALSE(created.error.detail.empty());
	// Fault for red: W1 never checked the precondition, so the detail below
	// could not appear; the failure would have come from Save instead.
	CHECK(created.error.detail.find("not a regular file") != std::string::npos);

	// Neither the asset target nor its asset identity sidecar was mutated: the
	// directory still exists and no sibling sidecar/temp was committed.
	CHECK(std::filesystem::is_directory(prefabPath));
	CHECK_FALSE(std::filesystem::exists(AssetSidecarPath(prefabPath)));

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C2a (Sol P1): instantiate links PrefabInstanceComponent to the ACTUAL
// canonical root — a hand-authored [child, root] file (root at record 1)
// must get the link on the root, not on liveEntities[0].
//
// Fault for red: attach the link to liveEntities[0] (W1 shipped code) — the
// component lands on the child.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 C2a: [child, root] file links the root entity, not record 0")
{
	PrefabFixture f;
	const auto dir = UniqueTempDir("p8w1_c2a_childfirst");
	const auto prefabPath = dir / "childfirst.rt2prefab";

	// Build a two-record file ordered [child, root]: record 0 is the child
	// whose parent is the root's scene UUID; record 1 is the root (nil parent).
	PrefabDocument doc;
	PrefabEntityRecord childRec;
	childRec.templateId = f.ids.CreateV4();
	childRec.record.uuid = f.ids.CreateV4();
	childRec.record.name = "Child";
	childRec.record.visible = true;
	PrefabEntityRecord rootRec;
	rootRec.templateId = f.ids.CreateV4();
	rootRec.record.uuid = f.ids.CreateV4();
	rootRec.record.name = "Root";
	rootRec.record.visible = true;
	childRec.record.parentUuid = rootRec.record.uuid;
	doc.entities.push_back(std::move(childRec));
	doc.entities.push_back(std::move(rootRec));
	Error saveErr;
	REQUIRE(PrefabSerializer::Save(doc, prefabPath, saveErr));
	REQUIRE(saveErr.IsOk());

	std::vector<AssetDiagnostic> diags;
	const auto uuids = f.manager.ReserveKnownUuids(2);
	const auto inst = f.manager.InstantiatePrefabWithUuids(prefabPath, uuids, diags);
	// Fault for red: W1 succeeded but linked the child.
	REQUIRE(inst.mutation.success);
	REQUIRE(inst.instanceId.has_value());
	// createdRoots must name the root record's instance (uuids[1]).
	REQUIRE(inst.createdRoots.size() == 1);
	CHECK(inst.createdRoots[0] == uuids[1]);

	// The root entity (uuids[1]) carries the instance link; the child
	// (uuids[0]) must NOT.
	const auto rootHandle = f.manager.FindEntityByUuid(uuids[1]);
	const auto childHandle = f.manager.FindEntityByUuid(uuids[0]);
	REQUIRE(static_cast<uint32_t>(rootHandle) != static_cast<uint32_t>(entt::null));
	REQUIRE(static_cast<uint32_t>(childHandle) != static_cast<uint32_t>(entt::null));
	auto& reg = f.manager.GetECS().registry;
	CHECK(reg.all_of<PrefabInstanceComponent>(rootHandle));
	// Fault for red: W1 put the component on the child.
	CHECK_FALSE(reg.all_of<PrefabInstanceComponent>(childHandle));
	// Every member (including the root) carries the instance/template pair.
	CHECK(reg.all_of<PrefabMemberComponent>(rootHandle));
	CHECK(reg.all_of<PrefabMemberComponent>(childHandle));
	CHECK(reg.get<PrefabMemberComponent>(rootHandle).instanceId == *inst.instanceId);
	CHECK(reg.get<PrefabMemberComponent>(rootHandle).templateId == rootRec.templateId);
	CHECK(reg.get<PrefabMemberComponent>(childHandle).templateId == childRec.templateId);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C2b (Sol P1): duplicate templateId in a prefab file is rejected BEFORE any
// scene mutation — two members must never share one prefab-local identity.
//
// Fault for red: drop the templateId uniqueness check in the one-root
// validation block — a duplicate would proceed and give two entities the same
// templateId.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 C2b: duplicate templateId is rejected before any mutation")
{
	PrefabFixture f;
	const auto dir = UniqueTempDir("p8w1_c2b_dupetemplate");
	const auto prefabPath = dir / "dupe.rt2prefab";

	const auto sharedTemplateId = f.ids.CreateV4();
	PrefabDocument doc;
	for (int i = 0; i < 2; ++i)
	{
		PrefabEntityRecord rec;
		rec.templateId = sharedTemplateId;
		rec.record.uuid = f.ids.CreateV4();
		rec.record.name = "Twin";
		rec.record.visible = true;
		doc.entities.push_back(std::move(rec));
	}
	Error saveErr;
	REQUIRE(PrefabSerializer::Save(doc, prefabPath, saveErr));
	REQUIRE(saveErr.IsOk());

	const auto beforeEntities = f.manager.GetEntityCount();
	const auto beforeUuids = f.manager.AuthoringDoc().uuidIndex.Size();
	std::vector<AssetDiagnostic> diags;
	const auto uuids = f.manager.ReserveKnownUuids(2);
	const auto inst = f.manager.InstantiatePrefabWithUuids(prefabPath, uuids, diags);
	// Fault for red: without the check the instantiate would succeed.
	CHECK_FALSE(inst.mutation.success);
	CHECK_FALSE(f.EntityAlive(uuids[0]));
	CHECK_FALSE(f.EntityAlive(uuids[1]));
	CHECK(f.manager.GetEntityCount() == beforeEntities);
	CHECK(f.manager.AuthoringDoc().uuidIndex.Size() == beforeUuids);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C3a (Sol P1): the production scene save rejects a PrefabInstanceComponent
// whose reference is wrong-kind or invalid — a save the reader would reject
// must not succeed. This covers the empty-path and wrong-kind cases the
// generic Unknown-path check misses.
//
// Fault for red: drop the PrefabInstanceComponent pre-save validation in
// SceneSerializer (W1 shipped code) — the save then succeeds with a reference
// its own reader rejects.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 C3a: save rejects an invalid or wrong-kind prefab reference")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("BadRef");
	const auto dir = UniqueTempDir("p8w1_c3a_badref");
	const auto prefabPath = dir / "badref.rt2prefab";
	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	REQUIRE(created.ok);

	// Instantiate so the live scene carries a PrefabInstanceComponent.
	std::vector<AssetDiagnostic> instDiags;
	const auto uuids = f.manager.ReserveKnownUuids(2);
	const auto inst = f.manager.InstantiatePrefabWithUuids(prefabPath, uuids, instDiags);
	REQUIRE(inst.mutation.success);

	// Corrupt the reference on the root to a wrong kind (a known kind that is
	// NOT prefab) and to an empty path in two separate sub-cases.
	auto& doc = f.manager.AuthoringDoc();
	auto& reg = doc.ecs.registry;
	auto piView = reg.view<PrefabInstanceComponent>();
	REQUIRE(piView.size() == 1);
	const entt::entity rootHandle = *piView.begin();
	REQUIRE(static_cast<uint32_t>(rootHandle) != static_cast<uint32_t>(entt::null));

	// Sub-case 1: wrong kind (Script) with a non-empty path.
	{
		auto& pi = reg.get<PrefabInstanceComponent>(rootHandle);
		pi.prefab.kind = AssetKind::Script;
	}
	Error wrongKindErr;
	std::vector<AssetDiagnostic> d1;
	CHECK_FALSE(SceneSerializer::Save(doc, dir / "wrong.rt2scene", d1, wrongKindErr));
	CHECK(wrongKindErr.code == rt2::core::Error::InvalidArgument);

	// Sub-case 2: correct kind but empty path (invalid reference).
	{
		auto& pi = reg.get<PrefabInstanceComponent>(rootHandle);
		pi.prefab.kind = AssetKind::Prefab;
		pi.prefab.path.clear();
	}
	Error emptyPathErr;
	std::vector<AssetDiagnostic> d2;
	CHECK_FALSE(SceneSerializer::Save(doc, dir / "empty.rt2scene", d2, emptyPathErr));
	CHECK(emptyPathErr.code == rt2::core::Error::InvalidArgument);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C3b (Sol P1): a VALID prefab instance saves and reloads through the real
// serializer — pre-save validation must not reject what the reader accepts.
// Also proves the prefab block emits a portability diagnostic for an
// unrelativizable path like the other durable references do.
//
// Fault for red: a save-time validation that rejects valid prefab refs, or a
// missing AppendNonPortableDiagnostic in the prefab block (W1 shipped code).
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 C3b: valid prefab reference saves, reloads, and reports portability")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("GoodRef");
	const auto dir = UniqueTempDir("p8w1_c3b_goodref");
	const auto prefabPath = dir / "goodref.rt2prefab";
	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	REQUIRE(created.ok);

	std::vector<AssetDiagnostic> instDiags;
	const auto uuids = f.manager.ReserveKnownUuids(2);
	const auto inst = f.manager.InstantiatePrefabWithUuids(prefabPath, uuids, instDiags);
	REQUIRE(inst.mutation.success);

	// Save the live authoring scene; the prefab reference is rebased against
	// the scene's own directory (same dir here) so it stays relative/resolvable.
	const auto scenePath = dir / "good.rt2scene";
	auto& doc = f.manager.AuthoringDoc();
	std::vector<AssetDiagnostic> saveDiags;
	Error saveErr;
	REQUIRE(SceneSerializer::Save(doc, scenePath, saveDiags, saveErr));

	// Reload the scene and confirm the prefab link survives.
	rt2::core::SceneDocument loaded;
	Error loadErr;
	REQUIRE(SceneSerializer::Load(loaded, scenePath, loadErr));
	{
		auto view = loaded.ecs.registry.view<PrefabInstanceComponent>();
		REQUIRE(view.size() == 1);
		const auto& pi = view.get<PrefabInstanceComponent>(*view.begin());
		CHECK(pi.prefab.kind == AssetKind::Prefab);
		CHECK_FALSE(pi.prefab.path.empty());
		CHECK(pi.prefab.path.find("goodref.rt2prefab") != std::string::npos);
	}

	// The relative save above must NOT warn: a relocatable prefab ref stays
	// portable. Assert zero NonPortable advisories so the portability case
	// below is not confounded by unrelated warnings.
	{
		const auto nonPortable = std::count_if(saveDiags.begin(), saveDiags.end(),
			[](const AssetDiagnostic& d) { return d.severity == AssetDiagnostic::NonPortable; });
		CHECK(nonPortable == 0);
	}

	// Portability: repoint the SAME instance's prefab ref at a syntactically
	// cross-volume absolute path on a drive selected distinct from the temp
	// scene dir. RebasePath
	// cannot relativize across volumes (lexically_relative of two different
	// drives is empty), so production SceneSerializer::Save must emit exactly
	// one NonPortable advisory for the prefab kind carrying the original/
	// stored path and the entity identity, exactly like the other durable
	// reference kinds.
	{
		auto& reg = f.manager.AuthoringDoc().ecs.registry;
		auto view = reg.view<PrefabInstanceComponent>();
		REQUIRE(view.size() == 1);
		const entt::entity instEnt = *view.begin();
		auto& pi = view.get<PrefabInstanceComponent>(instEnt);

		rt2::core::UUID rootUuid;
		std::string rootName;
		if (auto* idc = reg.try_get<EntityIdComponent>(instEnt)) rootUuid = idc->id;
		if (auto* nc = reg.try_get<NameComponent>(instEnt))     rootName = nc->name;

		pi.prefab.kind = AssetKind::Prefab;
		const char sceneDrive = scenePath.root_name().string().empty() ? 'C' : scenePath.root_name().string()[0];
		char foreignDrive = sceneDrive == 'A' ? 'B' : 'A';
		if (foreignDrive == sceneDrive) ++foreignDrive;
		const std::string foreignPath = std::string(1, foreignDrive) + ":/synthetic/goodref.rt2prefab";
		pi.prefab.path = foreignPath;

		std::vector<AssetDiagnostic> crossDiags;
		Error crossErr;
		REQUIRE(SceneSerializer::Save(doc, dir / "crossvol.rt2scene", crossDiags, crossErr));
		REQUIRE(crossErr.IsOk());

		const auto nonPortable = std::count_if(crossDiags.begin(), crossDiags.end(),
			[](const AssetDiagnostic& d) { return d.severity == AssetDiagnostic::NonPortable; });
		// Fault for red (W1 shipped test): the diagnostic was never asserted
		// (saveDiags was discarded), so the prefab portability advisory was
		// unproven despite the verification report claiming it.
		REQUIRE(nonPortable == 1);
		const auto prefabDiag = std::find_if(crossDiags.begin(), crossDiags.end(),
			[](const AssetDiagnostic& d) { return d.severity == AssetDiagnostic::NonPortable; });
		REQUIRE(prefabDiag != crossDiags.end());
		CHECK(prefabDiag->kind == AssetKind::Prefab);
		CHECK(prefabDiag->refPath == foreignPath);
		CHECK(std::filesystem::path(prefabDiag->resolvedPath).lexically_normal().generic_string() ==
			std::filesystem::path(foreignPath).lexically_normal().generic_string());
		CHECK(prefabDiag->entityUuid == rootUuid);
		CHECK(prefabDiag->entityName == rootName);

		const nlohmann::json serialized = nlohmann::json::parse(ReadFileBinary(dir / "crossvol.rt2scene"));
		bool jsonPathFound = false;
		for (const auto& entity : serialized.at("entities"))
		{
			if (entity.value("uuid", std::string{}) == rootUuid.ToString() && entity.contains("prefabInstance"))
			{
				CHECK(entity.at("prefabInstance").at("asset").at("path").get<std::string>() == foreignPath);
				jsonPathFound = true;
			}
		}
		CHECK(jsonPathFound);
	}

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C4a (Sol P1): CreatePrefabCommand Undo must NOT clobber an out-of-band
// external edit — if the file changed since create, Undo fails loudly and
// leaves the external bytes untouched (never truncates them).
//
// Fault for red: W1 Undo overwrote the current bytes unconditionally.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 C4a: Undo refuses to clobber an out-of-band file edit")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("GuardUndo");
	const auto dir = UniqueTempDir("p8w1_c4a_undo_edit");
	const auto prefabPath = dir / "guardundo.rt2prefab";

	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	REQUIRE(created.ok);
	auto cmd = MakeCreatePrefabCommand(prefabPath, created, {}, false);
	REQUIRE(cmd);

	EditorCommandHistory history;
	REQUIRE(history.Execute(std::move(cmd), f.manager).success);

	// Out-of-band edit after create.
	const std::string external = "{\"external\":\"edit\"}\n";
	WriteRaw(prefabPath, external);

	const auto undoResult = history.Undo(f.manager);
	// Fault for red: W1 Undo truncated/overwrote the external edit silently.
	CHECK_FALSE(undoResult.success);
	CHECK(undoResult.error.code == rt2::core::Error::Io);
	CHECK(ReadFileBinary(prefabPath) == external);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C4b (Sol P1): CreatePrefabCommand Redo (Execute after Undo) must refuse to
// clobber an out-of-band edit made between Undo and Redo. Same loud-conflict
// contract, applied to the Execute/Redo direction.
//
// Fault for red: W1 Redo overwrote whatever was at the path.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 C4b: Redo refuses to clobber an out-of-band file edit")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("GuardRedo");
	const auto dir = UniqueTempDir("p8w1_c4b_redo_edit");
	const auto prefabPath = dir / "guardredo.rt2prefab";

	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	REQUIRE(created.ok);
	auto cmd = MakeCreatePrefabCommand(prefabPath, created, {}, false);
	REQUIRE(cmd);

	EditorCommandHistory history;
	REQUIRE(history.Execute(std::move(cmd), f.manager).success);
	REQUIRE(history.Undo(f.manager).success);
	// After Undo the file was removed (absent before create).
	CHECK_FALSE(std::filesystem::exists(prefabPath));

	// Out-of-band re-creation between Undo and Redo.
	const std::string external = "{\"external\":\"redo-edit\"}\n";
	WriteRaw(prefabPath, external);

	const auto redoResult = history.Redo(f.manager);
	// Fault for red: W1 Redo overwrote the external re-creation silently.
	CHECK_FALSE(redoResult.success);
	CHECK(redoResult.error.code == rt2::core::Error::Io);
	CHECK(ReadFileBinary(prefabPath) == external);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C4c (Sol P1): CreatePrefabCommand Undo's RESTORE (the existing-file branch)
// surfaces a failed write as a loud Io Failure, never a silent success. The
// overwrite branch is injected by setting FILE_ATTRIBUTE_READONLY; the
// transaction's explicit captured-handle attribute guard refuses before
// quarantine, while the file still reads back as the expected after state.
//
// Fault for red: W1 opened the target with trunc and wrote in place, reporting
// the failure only AFTER already destroying the prior target — the Undo must
// both fail loudly AND leave the recoverable after-state readable.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 C4c: Undo restore surfaces a failed overwrite and keeps prior state")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("RestoreFail");
	const auto dir = UniqueTempDir("p8w1_c4c_restore_fail");
	const auto prefabPath = dir / "restorefail.rt2prefab";

	// Pre-existing file whose prior bytes Undo must restore.
	const std::string stale = "{\"header\":\"rt2prefab\",\"version\":1,\"entities\":[]}\n";
	WriteRaw(prefabPath, stale);
	const std::vector<uint8_t> before(stale.begin(), stale.end());

	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	REQUIRE(created.ok);
	auto cmd = MakeCreatePrefabCommand(prefabPath, created, before, true);
	REQUIRE(cmd);

	EditorCommandHistory history;
	REQUIRE(history.Execute(std::move(cmd), f.manager).success);
	// The file is now the created (after) bytes.
	const std::string after = ReadFileBinary(prefabPath);
	REQUIRE(after != stale);

	// Make the target read-only. The explicit captured-handle attribute guard
	// must refuse before quarantine, while reads still succeed and the after
	// bytes remain intact.
	{
		std::error_code ec;
		std::filesystem::permissions(
			prefabPath,
			std::filesystem::perms::owner_write |
				std::filesystem::perms::group_write |
				std::filesystem::perms::others_write,
			std::filesystem::perm_options::remove, ec);
		REQUIRE_FALSE(ec);
	}

	const auto undoResult = history.Undo(f.manager);
	CHECK_FALSE(undoResult.success);
	CHECK(undoResult.error.code == rt2::core::Error::Io);

	// The recoverable state must remain intact for a subsequent retry — never
	// a destroyed target on a failed restore.
	std::error_code ec;
	std::filesystem::permissions(prefabPath,
		std::filesystem::perms::owner_write |
			std::filesystem::perms::group_write |
			std::filesystem::perms::others_write,
		std::filesystem::perm_options::add, ec);
	CHECK(ReadFileBinary(prefabPath) == after);

	std::filesystem::remove_all(dir);
}

TEST_CASE("Phase 8 W1: read-only sidecar refuses overwrite before quarantine")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("ReadonlySidecar");
	const auto dir = UniqueTempDir("p8w1_readonly_sidecar");
	const auto prefabPath = dir / "readonly.rt2prefab";
	const auto sidecarPath = rt2::core::AssetSidecarPath(prefabPath);
	const auto first = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	REQUIRE(first.ok);
	const auto beforeAsset = ReadFileBinary(prefabPath);
	const auto beforeSidecar = ReadFileBinary(sidecarPath);
#ifdef _WIN32
	REQUIRE(SetFileAttributesW(sidecarPath.wstring().c_str(), FILE_ATTRIBUTE_READONLY) != FALSE);
	const auto refused = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	CHECK_FALSE(refused.ok);
	CHECK(refused.error.code == rt2::core::Error::Io);
	CHECK(ReadFileBinary(prefabPath) == beforeAsset);
	CHECK(ReadFileBinary(sidecarPath) == beforeSidecar);
	REQUIRE(SetFileAttributesW(sidecarPath.wstring().c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE);
#else
	CHECK(true);
#endif
	std::filesystem::remove_all(dir);
}

TEST_CASE("Phase 8 W1: command surfaces post-commit recovery warning")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("RecoveryWarning");
	const auto dir = UniqueTempDir("p8w1_command_recovery_warning");
	const auto prefabPath = dir / "warning.rt2prefab";
	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	REQUIRE(created.ok);
	auto command = MakeCreatePrefabCommand(prefabPath, created, {}, false);
	REQUIRE(command);
	EditorCommandHistory history;
	SetPathTransactionFaultForTests(PathTransactionFaultPoint::ManifestCleanup);
	const auto result = history.Execute(std::move(command), f.manager);
	ClearPathTransactionFaultForTests();
	CHECK(result.success);
	CHECK(result.effective);
	CHECK(result.recoveryWarning.has_value());
	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C4d (Sol follow-up P1): CreatePrefabCommand must represent expected
// existence SEPARATELY from bytes. A pre-existing zero-byte file is restored
// to a zero-byte file on Undo (exists), so Redo's BEFORE-state check expects an
// EXISTING file with empty bytes. When another process DELETES that file
// between Undo and Redo, the file is now MISSING — a different state that must
// NOT satisfy the existing-empty expectation. Redo must fail loudly (Io) and
// leave the path absent, never silently recreate the prefab.
//
// Fault for red: W1 shipped FileMatches returning expected.empty() when the
// stream could not open, so a missing file whose expected bytes are empty was
// treated as "still matches" and Redo silently wrote the AFTER bytes over the
// out-of-band deletion.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 C4d: Redo refuses when a pre-existing zero-byte file is deleted out-of-band")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("GuardRedoZero");
	const auto dir = UniqueTempDir("p8w1_c4d_redo_delete_zerobyte");
	const auto prefabPath = dir / "guardredo_zero.rt2prefab";

	// Pre-existing zero-byte file: an empty bytes vector alone cannot
	// distinguish it from an absent file; fileExistedBefore=true carries the
	// distinction.
	WriteRaw(prefabPath, "");
	CHECK(std::filesystem::exists(prefabPath));
	CHECK(std::filesystem::file_size(prefabPath) == 0);
	const std::vector<uint8_t> beforeEmpty;

	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	REQUIRE(created.ok);
	auto cmd = MakeCreatePrefabCommand(prefabPath, created, beforeEmpty, true);
	REQUIRE(cmd);

	EditorCommandHistory history;
	REQUIRE(history.Execute(std::move(cmd), f.manager).success);
	REQUIRE(history.Undo(f.manager).success);
	// Undo restored the zero-byte file — it lives on as an EXISTING empty file.
	CHECK(std::filesystem::exists(prefabPath));
	CHECK(std::filesystem::file_size(prefabPath) == 0);

	// Out-of-band deletion between Undo and Redo.
	{
		std::error_code ec;
		std::filesystem::remove(prefabPath, ec);
		REQUIRE_FALSE(ec);
	}
	CHECK_FALSE(std::filesystem::exists(prefabPath));

	// Fault for red (W1 shipped code): an unopenable file with empty expected
	// bytes was treated as a match, so this Redo succeeded and recreated the
	// deleted prefab instead of surfacing the out-of-band deletion.
	const auto redoResult = history.Redo(f.manager);
	CHECK_FALSE(redoResult.success);
	CHECK(redoResult.error.code == rt2::core::Error::Io);
	// The out-of-band deletion stands — Redo never recreated the file.
	CHECK_FALSE(std::filesystem::exists(prefabPath));

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C4e (Sol follow-up P1): the inverse discrimination. When a create began from
// an ABSENT target, Undo removes the file and Redo expects ABSENCE. An
// out-of-band ZERO-BYTE file created after Undo is EXISTING state and must NOT
// satisfy the expected-absence expectation — Redo must fail and preserve the
// external zero-byte file. This guards existence vs bytes in the other
// direction (an existing zero-byte file is not an expected absence).
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 C4e: Redo refuses when an expected-absent path gains a zero-byte file")
{
	PrefabFixture f;
	const auto [root, child] = f.RootWithChild("GuardRedoAbsent");
	const auto dir = UniqueTempDir("p8w1_c4e_redo_zerobyte_absent");
	const auto prefabPath = dir / "guardredo_absent.rt2prefab";

	const auto created = f.manager.CreatePrefabFromSubtree({ root }, prefabPath);
	REQUIRE(created.ok);
	auto cmd = MakeCreatePrefabCommand(prefabPath, created, {}, false); // absent before
	REQUIRE(cmd);

	EditorCommandHistory history;
	REQUIRE(history.Execute(std::move(cmd), f.manager).success);
	REQUIRE(history.Undo(f.manager).success);
	CHECK_FALSE(std::filesystem::exists(prefabPath)); // Undo removed it

	// Out-of-band re-creation of a ZERO-BYTE file between Undo and Redo — the
	// inverse discrimination: an existing empty file is NOT an expected
	// absence and must not be treated as one.
	WriteRaw(prefabPath, "");
	CHECK(std::filesystem::exists(prefabPath));
	CHECK(std::filesystem::file_size(prefabPath) == 0);

	const auto redoResult = history.Redo(f.manager);
	CHECK_FALSE(redoResult.success);
	CHECK(redoResult.error.code == rt2::core::Error::Io);
	// The external zero-byte file is preserved (never clobbered, never treated
	// as the expected absent state).
	CHECK(std::filesystem::exists(prefabPath));
	CHECK(std::filesystem::file_size(prefabPath) == 0);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C5 (Sol P2): failed-instantiate rollback proves FULL state restoration for
// a post-merge failure that actually stages a mesh AND a material AND a
// texture. Asserts entity count, UUID-index consistency, all three resource
// tables, dirty/revision state, and that no surviving PrefabInstanceComponent
// link remains. The prior test only appended a mesh (primitive cube), so
// removing the materials/textures resize left it green.
//
// Fault for red: remove the materials.resize(matBase) / textures.resize(texBase)
// lines from rollbackCreated — materials/textures counts stay inflated.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W1 C5: rollback restores mesh+material+texture rows, UUID index, and state")
{
	PrefabFixture f;
	const auto dir = UniqueTempDir("p8w1_c5_discriminate");
	const auto glbPath = dir / "tiny_textured.glb";

	// A textured glTF (1 mesh, 1 material, 1 texture) so the imported prefab
	// stages all three tables on instantiate.
	Error genErr;
	REQUIRE(GenerateTinyTexturedGlb(glbPath, genErr));

	// Load it into the live scene so the entity is an imported mesh with a
	// durable source reference and its material/texture rows already present.
	REQUIRE(f.manager.LoadScene(glbPath.string()));
	auto& liveReg = f.manager.GetECS().registry;
	auto importedView = liveReg.view<ImportedMeshSourceComponent>();
	REQUIRE(importedView.size() > 0);
	const entt::entity importedRoot = *importedView.begin();

	// Create the prefab from the imported subtree (captures hasImportedSource).
	rt2::core::UUID importedUuid;
	{
		if (auto* idc = liveReg.try_get<EntityIdComponent>(importedRoot))
			importedUuid = idc->id;
		else
			importedUuid = f.manager.GetEntityUuid(SceneManager::EntityId{importedRoot});
	}
	REQUIRE(!importedUuid.IsNull());
	const auto prefabPath = dir / "imported.rt2prefab";
	const auto created = f.manager.CreatePrefabFromSubtree({ importedUuid }, prefabPath);
	REQUIRE(created.ok);

	// Inject a hierarchy cycle into the LIVE scene so the instance merge's
	// dst RebuildChildren fails AFTER all three resource tables were appended.
	{
		const auto a = f.manager.CreateEmpty("CycA").affectedEntities.front();
		const auto b = f.manager.CreateEmpty("CycB").affectedEntities.front();
		const auto ha = f.manager.FindEntityByUuid(a);
		const auto hb = f.manager.FindEntityByUuid(b);
		REQUIRE(static_cast<uint32_t>(ha) != static_cast<uint32_t>(entt::null));
		REQUIRE(static_cast<uint32_t>(hb) != static_cast<uint32_t>(entt::null));
		liveReg.emplace_or_replace<Hierarchy>(ha).parent = hb;
		liveReg.emplace_or_replace<Hierarchy>(hb).parent = ha;
	}

	const auto beforeEntities = f.manager.GetEntityCount();
	const auto beforeUuids = f.manager.AuthoringDoc().uuidIndex.Size();
	const auto beforeDocument = f.manager.DocumentGeneration();
	const auto beforeResource = f.manager.ResourceGeneration();
	const auto meshBase = f.manager.GetECS().meshRegistry.GetCount();
	const auto matBase = (int)f.manager.GetECS().materials.size();
	const auto texBase = (int)f.manager.GetECS().textures.size();
	const auto beforeRevision = f.manager.AuthoringRevision();
	const auto beforeDirty = f.manager.IsDirty();
	// The imported prefab must genuinely stage material + texture rows.
	REQUIRE(matBase >= 1);
	REQUIRE(texBase >= 1);

	std::vector<AssetDiagnostic> diags;
	const auto instUuid = f.manager.ReserveKnownUuid();
	const auto inst = f.manager.InstantiatePrefabWithUuids(
		prefabPath, { instUuid }, diags);
	CHECK_FALSE(inst.mutation.success);
	CHECK_FALSE(inst.instanceId.has_value());

	// Entity count and UUID index restored.
	CHECK(f.manager.GetEntityCount() == beforeEntities);
	CHECK(f.manager.AuthoringDoc().uuidIndex.Size() == beforeUuids);
	// All three resource tables restored.
	// Fault for red: dropping materials/textures resize keeps these inflated.
	CHECK(f.manager.GetECS().meshRegistry.GetCount() == meshBase);
	CHECK((int)f.manager.GetECS().materials.size() == matBase);
	CHECK((int)f.manager.GetECS().textures.size() == texBase);
	// No surviving instance link.
	{
		auto view = liveReg.view<PrefabInstanceComponent>();
		CHECK(view.size() == 0);
	}
	// Revision/dirty unchanged by a failed (rolled-back) operation.
	CHECK(f.manager.AuthoringRevision() == beforeRevision);
	CHECK(f.manager.IsDirty() == beforeDirty);
	// Generation state named by the rollback contract: a fully rolled-back,
	// failed instantiate returns before NotifyAuthoringChanged and the
	// rollback truncates the staged resource rows, so the document and
	// resource generations (advanced only by whole-scene Load/Adopt/Compact)
	// are unchanged. The attempted instance UUID was reserved but never
	// survives the rollback into the document UUID index.
	CHECK(f.manager.DocumentGeneration() == beforeDocument);
	CHECK(f.manager.ResourceGeneration() == beforeResource);
	CHECK_FALSE(f.manager.AuthoringDoc().uuidIndex.Contains(instUuid));

	std::filesystem::remove_all(dir);
}
