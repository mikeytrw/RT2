#include <doctest/doctest.h>

#include "ECSComponents.h"
#include "ECSScene.h"
#include "EditorCommand.h"
#include "EditorCommandHistory.h"
#include "EditorPropertyCommands.h"
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
#include <optional>
#include <vector>

using namespace rt2::core;

// ============================================================================
// Material-index undo restores the wrong override (implementation spec,
// docs/game-engine-development-plan.md, 2026-08-03).
//
// The defect: SceneEditorUI::RecordMaterialIndexEdit captured the command's
// before- and after-override with the same GetMaterialOverride call made
// twice, both AFTER SetMaterial had already replaced the override. The two
// snapshots were identical by construction and Undo wrote the post-edit
// durable record back over the pre-edit one — invisible in-session, and only
// user-visible as a reverted undo after save/reopen (the resolver reapplies
// the authored override).
//
// The fix (D1(b), decided in the verification report): the capture moved into
// SceneManager::SetMaterialIndexState, which returns the displaced (before)
// and freshly recorded (after) override as out-params of the mutation itself.
// The UI no longer reads MaterialOverrideComponent state at all, so the
// ordering cannot be inverted at the call site.
//
// These are the three tests from the spec, each with its discrimination
// proof recorded red-then-green in the verification report.
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

bool MaterialEq(const SceneMaterial& a, const SceneMaterial& b)
{
	constexpr float eps = 1e-5f;
	auto vEq = [eps](const glm::vec3& x, const glm::vec3& y) {
		return std::fabs(x.x - y.x) <= eps && std::fabs(x.y - y.y) <= eps &&
			std::fabs(x.z - y.z) <= eps;
	};
	return a.type == b.type &&
		vEq(a.baseColor, b.baseColor) &&
		std::fabs(a.metallic - b.metallic) <= eps &&
		std::fabs(a.roughness - b.roughness) <= eps &&
		std::fabs(a.ior - b.ior) <= eps &&
		vEq(a.emissiveColor, b.emissiveColor) &&
		std::fabs(a.emissiveIntensity - b.emissiveIntensity) <= eps &&
		a.baseColorTextureIndex == b.baseColorTextureIndex &&
		a.normalTextureIndex == b.normalTextureIndex &&
		a.emissiveTextureIndex == b.emissiveTextureIndex &&
		a.metallicRoughnessTextureIndex == b.metallicRoughnessTextureIndex;
}

// The resolver rewrites the override's texture indices from the re-imported
// staged material (SceneAssetResolver.cpp "Copy texture indices from the
// re-imported staged material"), so across the round-trip only the authored
// scalar edits identify which edit produced the durable record.
bool AuthoredScalarEq(const SceneMaterial& a, const SceneMaterial& b)
{
	constexpr float eps = 1e-5f;
	auto vEq = [eps](const glm::vec3& x, const glm::vec3& y) {
		return std::fabs(x.x - y.x) <= eps && std::fabs(x.y - y.y) <= eps &&
			std::fabs(x.z - y.z) <= eps;
	};
	return a.type == b.type &&
		vEq(a.baseColor, b.baseColor) &&
		std::fabs(a.metallic - b.metallic) <= eps &&
		std::fabs(a.roughness - b.roughness) <= eps &&
		std::fabs(a.ior - b.ior) <= eps &&
		vEq(a.emissiveColor, b.emissiveColor) &&
		std::fabs(a.emissiveIntensity - b.emissiveIntensity) <= eps;
}

// In-memory imported entity with an authored override mirroring slot 0.
// Materials are distinctive (red slot 0, green slot 1) so the durable
// override's material identifies which edit produced it.
struct IndexUndoFixture
{
	rt2::core::DeterministicUuidProvider ids;
	SceneManager mgr;
	rt2::core::UUID uuid;
	entt::entity entity = entt::null;
	SceneMaterial red;
	SceneMaterial green;

	IndexUndoFixture()
	{
		mgr.SetUuidProvider(&ids);
		red.baseColor = {0.9f, 0.1f, 0.1f};
		red.roughness = 0.11f;
		green.baseColor = {0.1f, 0.9f, 0.1f};
		green.roughness = 0.22f;
		mgr.AddMaterial(red);    // slot 0
		mgr.AddMaterial(green);  // slot 1
		const auto e = mgr.AddObject("E");  // MeshRef materialIndex 0
		REQUIRE(e.IsValid());
		entity = e.id;
		uuid = mgr.GetEntityUuid(e);

		ImportedMeshSourceComponent src;
		src.model.kind = AssetKind::Model;
		src.model.path = "model.glb";
		src.model.sourceKey = "gltf:scene=0:node=0:mesh=0:primitive=0";
		mgr.GetECS().registry.emplace<ImportedMeshSourceComponent>(entity, src);

		// Authored override mirroring slot 0 — the durable pre-edit record
		// that a correct undo must restore.
		MaterialOverrideComponent ov;
		ov.material = red;
		ov.authored = true;
		ov.materialIndex = 0;
		mgr.GetECS().registry.emplace<MaterialOverrideComponent>(entity, ov);
	}
};

} // namespace

// ---------------------------------------------------------------------------
// Test 1: undo restores the pre-edit override. Asserts the durable
// MaterialOverrideComponent, not merely the restored MeshRef index.
// ---------------------------------------------------------------------------
TEST_CASE("Material-index undo restores the pre-edit override")
{
	IndexUndoFixture f;
	auto& reg = f.mgr.GetECS().registry;

	// Phase 8 W3 S6-B: before is read live BEFORE any mutation and the after
	// override is the manager-staged canonical target (validate-only). The
	// command's composite then performs the first and only write.
	const auto before = f.mgr.GetMaterialOverride(f.uuid);
	REQUIRE(before.has_value());
	CHECK(MaterialEq(before->material, f.red));
	const auto staged = f.mgr.StageMaterialIndex(f.uuid, 1);
	REQUIRE(staged.IsOk());
	const auto& after = staged.value.override;
	REQUIRE(after.has_value());
	CHECK(MaterialEq(after->material, f.green));
	CHECK(after->authored);
	CHECK(reg.get<MeshRef>(f.entity).materialIndex == 0);

	auto cmd = MakeSetMaterialIndexCommandIfEffective(f.uuid, 0, 1, before, after);
	REQUIRE(cmd);

	EditorCommandHistory history;
	const auto re = history.Execute(std::move(cmd), f.mgr);
	REQUIRE(re.success);
	REQUIRE(reg.get<MeshRef>(f.entity).materialIndex == 1);

	const auto ru = history.Undo(f.mgr);
	REQUIRE(ru.success);
	REQUIRE(reg.get<MeshRef>(f.entity).materialIndex == 0);
	REQUIRE(reg.all_of<MaterialOverrideComponent>(f.entity));
	const auto& restored = reg.get<MaterialOverrideComponent>(f.entity);
	// The durable record must come back as the pre-edit value — not the
	// post-edit one with a corrected index (the present defect's shape).
	CHECK(MaterialEq(restored.material, f.red));
	CHECK(restored.authored);
}

// ---------------------------------------------------------------------------
// Test 2: redo still reaches the post-edit override. Stays green under the
// present defect (the after-capture is genuinely the after state); its
// sensitivity proof is the mirrored corruption, recorded separately.
// ---------------------------------------------------------------------------
TEST_CASE("Material-index undo+redo reaches the post-edit override")
{
	IndexUndoFixture f;
	auto& reg = f.mgr.GetECS().registry;

	const auto before = f.mgr.GetMaterialOverride(f.uuid);
	REQUIRE(before.has_value());
	const auto staged = f.mgr.StageMaterialIndex(f.uuid, 1);
	REQUIRE(staged.IsOk());
	REQUIRE(staged.value.override.has_value());

	auto cmd = MakeSetMaterialIndexCommandIfEffective(f.uuid, 0, 1,
		before, staged.value.override);
	REQUIRE(cmd);

	EditorCommandHistory history;
	REQUIRE(history.Execute(std::move(cmd), f.mgr).success);
	REQUIRE(history.Undo(f.mgr).success);
	CHECK(MaterialEq(reg.get<MaterialOverrideComponent>(f.entity).material, f.red));

	const auto rr = history.Redo(f.mgr);
	REQUIRE(rr.success);
	REQUIRE(reg.get<MeshRef>(f.entity).materialIndex == 1);
	REQUIRE(reg.all_of<MaterialOverrideComponent>(f.entity));
	CHECK(MaterialEq(reg.get<MaterialOverrideComponent>(f.entity).material, f.green));
}

// ---------------------------------------------------------------------------
// Test 3: the round-trip. After undo, serialize and reload through the
// resolver and assert the entity resolves to the pre-edit material. This is
// the assertion that would have caught the present defect — the in-session
// state looks correct without it.
// ---------------------------------------------------------------------------
TEST_CASE("Material-index undo: save/reopen resolves to the pre-edit material")
{
	auto dir = UniqueTempDir("miu_roundtrip");
	auto glbPath = dir / "tiny_textured.glb";
	Error genErr;
	REQUIRE(GenerateTinyTexturedGlb(glbPath, genErr));
	auto scenePath = dir / "miu_undo.rt2scene";

	SceneManager mgr;
	REQUIRE(mgr.LoadScene(glbPath.string()));
	auto& reg = mgr.GetECS().registry;
	auto view = reg.view<ImportedMeshSourceComponent>();
	REQUIRE(view.size() > 0);
	const entt::entity e = *view.begin();
	const auto uuid = mgr.GetEntityUuid(SceneManager::EntityId{e});
	const int origIdx = reg.get<MeshRef>(e).materialIndex;
	REQUIRE(origIdx >= 0);

	SceneMaterial red;
	red.baseColor = {0.9f, 0.1f, 0.1f};
	red.roughness = 0.11f;
	SceneMaterial green;
	green.baseColor = {0.1f, 0.9f, 0.1f};
	green.roughness = 0.22f;
	const int redSlot = mgr.AddMaterial(red);
	const int greenSlot = mgr.AddMaterial(green);

	// Pre-edit state: authored override mirroring the red slot.
	REQUIRE(mgr.SetMaterialIndexState(uuid, redSlot, nullptr, nullptr).success);
	REQUIRE(reg.all_of<MaterialOverrideComponent>(e));
	CHECK(MaterialEq(reg.get<MaterialOverrideComponent>(e).material, red));

	// Phase 8 W3 S6-B: before is read live and the after override is the
	// manager-staged canonical target; the command's composite performs the
	// first write.
	const auto before = mgr.GetMaterialOverride(uuid);
	REQUIRE(before.has_value());
	CHECK(MaterialEq(before->material, red));
	const auto staged = mgr.StageMaterialIndex(uuid, greenSlot);
	REQUIRE(staged.IsOk());
	REQUIRE(staged.value.override.has_value());

	auto cmd = MakeSetMaterialIndexCommandIfEffective(uuid, redSlot, greenSlot,
		before, staged.value.override);
	REQUIRE(cmd);
	EditorCommandHistory history;
	REQUIRE(history.Execute(std::move(cmd), mgr).success);
	REQUIRE(mgr.GetECS().registry.get<MeshRef>(e).materialIndex == greenSlot);

	// Undo: index and durable override return to the pre-edit state.
	REQUIRE(history.Undo(mgr).success);
	REQUIRE(mgr.GetECS().registry.get<MeshRef>(e).materialIndex == redSlot);
	CHECK(MaterialEq(mgr.GetECS().registry.get<MaterialOverrideComponent>(e).material, red));

	// Save and reopen through the resolver — the path the user actually walks.
	Error saveErr;
	REQUIRE(SaveSceneForTest(mgr.AuthoringDoc(), scenePath, saveErr));
	REQUIRE(saveErr.IsOk());

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

	const entt::entity le = loaded.FindByUuid(uuid);
	const bool found = (le != entt::null);
	REQUIRE(found);
	auto& lreg = loaded.ecs.registry;
	const auto* lref = lreg.try_get<MeshRef>(le);
	REQUIRE(lref);
	REQUIRE(lref->materialIndex >= 0);
	REQUIRE(lref->materialIndex < (int)loaded.ecs.materials.size());
	// The entity must come back with the pre-edit material, not the
	// post-edit one the broken undo would have written. Scalar comparison:
	// the resolver rewrote the override's texture indices from the
	// re-imported staged material, but keeps the authored scalars verbatim.
	CHECK(AuthoredScalarEq(loaded.ecs.materials[lref->materialIndex], red));
	CHECK_FALSE(AuthoredScalarEq(loaded.ecs.materials[lref->materialIndex], green));

	std::filesystem::remove_all(dir);
}
