#include "SceneManager.h"
#include "SceneLoader.h"
#include "SceneGraph.h"
#include "SceneHierarchy.h"
#include "EditorCameraWorkflow.h"
#include "EntityReferenceRemapper.h"
#include "PersistedComponents.h"
#include "PrimitiveGeometry.h"
#include "RTLog.h"
#include "ScriptComponentValidation.h"
#include "ScriptAssetPath.h"
#include "AssetIdentity.h"
#include "PrefabSerializer.h"
#include "PrefabComponentKey.h"
#include "SceneSerializer.h"
#include "SceneAssetResolver.h"
#include "stb_image.h"
#include <tinyexr.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <set>
#include <map>
#include <string>
#include <sstream>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <algorithm>

namespace
{
std::vector<entt::entity> ResolveCanonicalRoots(
	const rt2::core::SceneDocument& document,
	const std::vector<rt2::core::UUID>& uuids,
	rt2::core::Error& error)
{
	std::vector<entt::entity> resolved;
	std::unordered_set<entt::entity> unique;
	for (const auto& uuid : uuids)
	{
		const auto entity = document.FindByUuid(uuid);
		if (entity == entt::null || !document.ecs.registry.valid(entity))
		{
			error.code = rt2::core::Error::InvalidEntity;
			error.path = uuid.ToString();
			error.detail = "entity UUID is not present in the authoring scene";
			return {};
		}
		if (unique.insert(entity).second)
			resolved.push_back(entity);
	}

	std::vector<entt::entity> canonical;
	for (const auto candidate : resolved)
	{
		bool covered = false;
		for (const auto possibleAncestor : resolved)
			if (candidate != possibleAncestor &&
				SceneHierarchy::IsDescendant(document.ecs.registry,
				                             possibleAncestor, candidate))
			{
				covered = true;
				break;
			}
		if (!covered)
			canonical.push_back(candidate);
	}
	return canonical;
}

void RemoveChild(entt::registry& registry, entt::entity parent, entt::entity child)
{
	if (parent == entt::null)
		return;
	if (auto* hierarchy = registry.try_get<Hierarchy>(parent))
		hierarchy->children.erase(
			std::remove(hierarchy->children.begin(), hierarchy->children.end(), child),
			hierarchy->children.end());
}

void CopyAuthoredComponents(const entt::registry& sourceRegistry,
	                       entt::entity source,
	                       entt::registry& destinationRegistry,
	                       entt::entity destination)
{
	PersistedComponents::ForEach([&](auto tag)
	{
		using Component = typename decltype(tag)::Type;
		if (const auto* component = sourceRegistry.try_get<Component>(source))
			destinationRegistry.emplace<Component>(destination, *component);
	});
	if (auto* transform = destinationRegistry.try_get<Transform>(destination))
	{
		transform->worldMatrix = glm::mat4(1.0f);
		transform->prevWorldMatrix = glm::mat4(1.0f);
		transform->dirty = true;
	}
}

void RemapCopiedScriptFields(
	const std::vector<std::pair<rt2::core::UUID, rt2::core::UUID>>& sourceToDestination,
	const std::vector<entt::entity>& destinations,
	entt::registry& registry)
{
	// Convert the transient registry copy plan to the durable UUID-keyed
	// contract at this boundary. The remapper itself never sees entt handles.
	rt2::core::EntityUuidRemap uuidRemap;
	uuidRemap.reserve(sourceToDestination.size());
	for (const auto& [source, destination] : sourceToDestination)
	{
		if (!source.IsNull() && !destination.IsNull())
			uuidRemap.emplace(source, destination);
	}

	std::vector<ScriptComponent*> scripts;
	scripts.reserve(destinations.size());
	for (const auto destination : destinations)
	{
		if (auto* script = registry.try_get<ScriptComponent>(destination))
			scripts.push_back(script);
	}
	rt2::core::RemapEntityReferences(uuidRemap, scripts);
}

// Phase 8 W3, S4 / W3-D8 + review fix 2 — a copy/paste of a prefab instance is
// a NEW instance: every copied instance GROUP gets ONE fresh instanceId,
// reserved BEFORE any destination mutation and applied only after the create
// loop. The selection may cut a prefab instance at an arbitrary point (an
// ordinary folder above it, a member fragment left below, or both), so the
// unit of classification is the FOREST of copied entities, not each selected
// root in isolation.
//
// CopyAuthoredComponents copies all 13 persisted components verbatim
// (SceneManager.cpp:89-93), so a copy of a prefab instance would share the
// SOURCE's instanceId: duplicating an instance produces two entities (and two
// member groups) claiming the same instance identity. W3 groups overrides by
// instanceId, so that sharing would merge two instances into one group
// (W3-D8). Fresh instanceIds are drawn from the SAME UUID provider the manager
// uses everywhere.
//
// The classification is SPLIT so the provider is consumed before mutation and
// no provider draw can happen after the destination starts changing:
//
//   1. PLAN — PlanCopiedPrefabLinks(sourceRegistry, roots) inspects the SOURCE
//      forest only (never the destination):
//        * a group with at least one source root carrying
//          PrefabInstanceComponent is a COMPLETE instance: it gets ONE fresh
//          instanceId, shared by that group's copied root(s) AND every copied
//          PrefabMemberComponent carrying the same original instanceId.
//          templateIds and override vectors are copied verbatim by
//          CopyAuthoredComponents and must NOT be touched: a copy of a
//          diverged instance stays diverged (W3-D4). A nested full instance is
//          handled per-component: the inner instance's root keeps its own
//          group's fresh id on its PrefabInstanceComponent and the enclosing
//          group's fresh id on its PrefabMemberComponent — nesting is never
//          merged into the outer group.
//        * a member whose original instanceId has NO source root in the forest
//          is an ORPHAN member fragment: this is NOT an instance. Fabricating
//          an instance here would invent a link the user never made, so the
//          PrefabMemberComponent is stripped at apply time (and its
//          PrefabInstanceComponent when that id also has no fresh mapping) and
//          the entity becomes ordinary. The plan counts the stripped entities
//          so the caller raises a recovery warning (never a hard failure;
//          duplicate/paste of a partial selection stays a valid operation).
//        * two source roots sharing one original instanceId is malformed
//          ambiguous input; it is diagnosed via ambiguousGroups so the caller
//          can surface it, and the group is kept as ONE reminted group (never
//          split or merged).
//        * everything else (no prefab components): untouched, reserves nothing.
//
//   2. RESERVE — BEFORE any destination mutation, the destination entity UUIDs
//      (sources.size() draws, all first — validated non-nil, absent from the
//      authoring document, absent from the source entities' ids when the
//      source is a DISTINCT document, and distinct within the operation) and
//      then ONE instanceId per complete group are drawn from the provider.
//      Each instanceId draw must be non-nil and absent from every entity UUID
//      in the authoring document, every live
//      PrefabInstanceComponent/PrefabMemberComponent instanceId in the
//      destination registry, every live instanceId in the SOURCE registry when
//      the source is a DISTINCT document (paste from a snapshot/clipboard),
//      and every earlier reservation in the same operation. A hostile provider
//      that yields nil or colliding ids retries up to
//      kUuidReservationMaxAttempts; exhaustion fails the operation with a
//      stage-specific DuplicateUuid diagnostic and ZERO destination change.
//
//   3. APPLY — ApplyCopiedPrefabLinks runs after the create loop and consumes
//      ONLY the pre-reserved freshIdByOriginalId plan: it assigns each
//      group's fresh id to its copied roots and members and strips orphan
//      fragments. It never calls the UUID provider.
//
// All four copy paths (DuplicateSubtrees, PasteSubtreesFrom,
// DuplicateSubtreesWithUuids, PasteSubtreesWithUuids) chain
// plan -> reserve -> create -> apply. Structural restore (ApplySubtreeRecord,
// SceneManager.cpp:1800-1808) must NOT call these — undo/redo reinstates the
// recorded instanceId verbatim (W3-D4). InstantiatePrefabWithUuids reserves
// its single fresh instanceId through the same helper BEFORE it mutates
// resource tables or the registry.
struct CopiedPrefabPlan
{
	// Source entities carrying PrefabInstanceComponent, keyed by their
	// original instanceId. Each key names a COMPLETE instance group that gets
	// one fresh instanceId.
	std::unordered_map<rt2::core::UUID, std::vector<entt::entity>> rootsByGroup;
	// Source entities carrying PrefabMemberComponent, keyed by their original
	// instanceId. Member groups whose id is absent from rootsByGroup are
	// ORPHAN fragments (stripped at apply; recovery warning).
	std::unordered_map<rt2::core::UUID, std::vector<entt::entity>> membersByGroup;
	// Member fragments whose original instanceId had no source root in the
	// forest (recovery warning).
	std::size_t orphanFragments = 0;
	// Original instance groups with more than one source root (diagnosed).
	std::size_t ambiguousGroups = 0;
};

CopiedPrefabPlan PlanCopiedPrefabLinks(
	const entt::registry& sourceRegistry,
	const std::vector<entt::entity>& roots)
{
	CopiedPrefabPlan plan;

	// Collect the whole source forest: every entity reachable from any
	// selected root's subtree. No destination state is touched.
	std::vector<entt::entity> forest;
	{
		std::unordered_set<entt::entity> seen;
		for (const auto root : roots)
		{
			std::vector<entt::entity> subtree;
			SceneHierarchy::CollectSubtreePreOrder(sourceRegistry, root, subtree);
			for (const auto source : subtree)
				if (seen.insert(source).second)
					forest.push_back(source);
		}
	}

	// Group the SOURCE entities by their ORIGINAL instanceId. Roots (entities
	// carrying PrefabInstanceComponent) anchor a group; members (entities
	// carrying PrefabMemberComponent) belong to whatever group their id names.
	for (const auto entity : forest)
	{
		if (const auto* pic = sourceRegistry.try_get<PrefabInstanceComponent>(entity))
			plan.rootsByGroup[pic->instanceId].push_back(entity);
		if (const auto* member = sourceRegistry.try_get<PrefabMemberComponent>(entity))
			plan.membersByGroup[member->instanceId].push_back(entity);
	}

	for (const auto& group : plan.membersByGroup)
		if (plan.rootsByGroup.find(group.first) == plan.rootsByGroup.end())
			plan.orphanFragments += group.second.size();
	for (const auto& group : plan.rootsByGroup)
		if (group.second.size() > 1)
			++plan.ambiguousGroups;
	return plan;
}

// Finite attempt budget for BOTH pre-mutation UUID reservations (entity UUIDs
// and fresh instance-IDs). A degraded/hostile UUID provider — nil draws, draws
// colliding with a live entity/instance/member, or draws repeated within one
// operation — exhausts this budget and the operation FAILS loudly
// (DuplicateUuid) BEFORE any destination mutation.
constexpr int kUuidReservationMaxAttempts = 16;

// Every UUID a freshly reserved instanceId must NOT equal: every entity UUID
// in the authoring document, every live destination PrefabInstanceComponent/
// PrefabMemberComponent instanceId, and — when the source is a DISTINCT
// document (paste from a snapshot/clipboard) — every live source instanceId,
// so a pasted instance never claims an identity still held by the fragment it
// was copied from. Duplicate copies pass source == &destination (the copy
// source is the same registry), which adds nothing twice.
std::unordered_set<rt2::core::UUID> FreshInstanceIdForbiddenSet(
	const rt2::core::SceneDocument& authoring,
	const entt::registry& destination,
	const entt::registry* source)
{
	std::unordered_set<rt2::core::UUID> forbidden;
	for (const auto& entry : authoring.uuidIndex.All())
		forbidden.insert(entry.first);
	const auto collectLiveInstanceIds = [&forbidden](const entt::registry& reg) {
		auto picView = reg.view<PrefabInstanceComponent>();
		for (const auto e : picView)
		{
			const auto& id = picView.get<PrefabInstanceComponent>(e).instanceId;
			if (!id.IsNull())
				forbidden.insert(id);
		}
		auto memberView = reg.view<PrefabMemberComponent>();
		for (const auto e : memberView)
		{
			const auto& id = memberView.get<PrefabMemberComponent>(e).instanceId;
			if (!id.IsNull())
				forbidden.insert(id);
		}
	};
	collectLiveInstanceIds(destination);
	if (source && source != &destination)
		collectLiveInstanceIds(*source);
	return forbidden;
}

// Every entity UUID a freshly reserved entity id must NOT equal: every entity
// UUID indexed in the authoring document, and — when the source is a DISTINCT
// document (a paste from a snapshot/clipboard) — every source entity's id, so
// a pasted entity never adopts the identity of the entity it was copied from.
// A duplicate passes source == &destination (the copied forest lives in the
// same registry), whose entity ids the authoring index already covers.
std::unordered_set<rt2::core::UUID> EntityUuidForbiddenSet(
	const rt2::core::SceneDocument& authoring,
	const entt::registry& destination,
	const entt::registry* source)
{
	std::unordered_set<rt2::core::UUID> forbidden;
	for (const auto& entry : authoring.uuidIndex.All())
		forbidden.insert(entry.first);
	if (source && source != &destination)
	{
		auto view = source->view<EntityIdComponent>();
		for (const auto e : view)
		{
			const auto& id = view.get<EntityIdComponent>(e).id;
			if (!id.IsNull())
				forbidden.insert(id);
		}
	}
	return forbidden;
}

// Reserve `count` entity UUIDs destined for freshly created entities, drawing
// from `produce`. Each draw survives three rejection rules before it is staged:
// a nil draw is a broken provider; a draw equal to any id in `forbidden` would
// collide with a live entity (the authoring index) or — for a paste from a
// distinct document — with the source entity it was copied from; a draw already
// in `operationLocal` would make two destinations in this operation share one
// id. Retries up to kUuidReservationMaxAttempts per draw (the same finite
// budget as fresh instance-ID reservation); on exhaustion fills `err` with a
// stage-specific DuplicateUuid naming the offending (last) draw, stages
// NOTHING, and returns false. The caller MUST run this before any destination
// mutation, so failure here is transactional.
bool ReserveValidEntityUuids(
	const std::function<rt2::core::UUID()>& produce,
	const std::unordered_set<rt2::core::UUID>& forbidden,
	std::unordered_set<rt2::core::UUID>& operationLocal,
	size_t count,
	const std::string& stage,
	std::vector<rt2::core::UUID>& out,
	rt2::core::Error& err)
{
	out.reserve(count);
	rt2::core::UUID lastDraw;
	for (size_t i = 0; i < count; ++i)
	{
		bool staged = false;
		for (int attempt = 0; attempt < kUuidReservationMaxAttempts; ++attempt)
		{
			lastDraw = produce();
			if (lastDraw.IsNull())
				continue;                    // nil draw: retry (broken provider)
			if (forbidden.count(lastDraw) != 0)
				continue;                    // live entity / source-entity id
			if (!operationLocal.insert(lastDraw).second)
				continue;                    // same id drawn twice this operation
			out.push_back(lastDraw);
			staged = true;
			break;
		}
		if (!staged)
		{
			err.code = rt2::core::Error::DuplicateUuid;
			err.path = lastDraw.ToString();
			err.detail = stage + ": exceeded " +
				std::to_string(kUuidReservationMaxAttempts) +
				" entity-UUID reservation attempts before mutation (the UUID "
				"provider yielded nil, an id already indexed in the authoring "
				"document, a source entity's id, or a repeat)";
			return false;
		}
	}
	return true;
}

// Draw ONE fresh instanceId from `produce` that is collision-free against
// `forbidden` and distinct from every id already reserved in this operation
// (`operationLocal`). Retries up to kUuidReservationMaxAttempts; on exhaustion
// fills `err` with a DuplicateUuid diagnostic naming the offending (last)
// draw and returns nullopt. The caller MUST have no destination mutation in
// flight when this fails — the whole point of reserving before creating.
std::optional<rt2::core::UUID> ReserveFreshInstanceId(
	const std::function<rt2::core::UUID()>& produce,
	const std::unordered_set<rt2::core::UUID>& forbidden,
	std::unordered_set<rt2::core::UUID>& operationLocal,
	rt2::core::Error& err)
{
	rt2::core::UUID lastAttempt;
	for (int attempt = 0; attempt < kUuidReservationMaxAttempts; ++attempt)
	{
		lastAttempt = produce();
		if (lastAttempt.IsNull())
			continue;                    // nil draw: retry (broken provider)
		if (forbidden.count(lastAttempt) != 0)
			continue;                    // live entity/instance/member id
		if (!operationLocal.insert(lastAttempt).second)
			continue;                    // same id drawn twice this operation
		return lastAttempt;
	}
	err.code = rt2::core::Error::DuplicateUuid;
	err.path = lastAttempt.ToString();
	err.detail = "exceeded " + std::to_string(kUuidReservationMaxAttempts) +
		" fresh instance-ID reservation attempts before mutation (the UUID "
		"provider yielded nil, a live instance/member id, or a repeat)";
	return std::nullopt;
}

// Apply the PRE-RESERVED plan to the freshly created copies. Consumes only
// freshIdByOriginalId (built entirely by ReserveFreshInstanceId BEFORE the
// create loop) — never calls the UUID provider. Orphan member fragments are
// stripped here (their original id has no fresh mapping).
void ApplyCopiedPrefabLinks(
	entt::registry& destinationRegistry,
	const CopiedPrefabPlan& plan,
	const std::unordered_map<entt::entity, entt::entity>& remap,
	const std::unordered_map<rt2::core::UUID, rt2::core::UUID>& freshIdByOriginalId)
{
	for (const auto& group : plan.membersByGroup)
	{
		const auto freshIt = freshIdByOriginalId.find(group.first);
		if (freshIt == freshIdByOriginalId.end())
		{
			// Orphan member fragment: no copied root carries this original id.
			// Strip the member link; if the entity's own
			// PrefabInstanceComponent names an id that also has no fresh
			// mapping, strip that too.
			for (const auto source : group.second)
			{
				const auto entity = remap.at(source);
				if (destinationRegistry.try_get<PrefabMemberComponent>(entity))
					destinationRegistry.remove<PrefabMemberComponent>(entity);
				const auto* pic = destinationRegistry.try_get<PrefabInstanceComponent>(entity);
				if (pic && freshIdByOriginalId.find(pic->instanceId) == freshIdByOriginalId.end())
					destinationRegistry.remove<PrefabInstanceComponent>(entity);
			}
			continue;
		}
		// Full-instance members: propagate the group's single fresh id. A root
		// entity may appear here too (a nested instance is also a member of the
		// enclosing group); its member record gets the ENCLOSING group's id,
		// which is correct — its own PrefabInstanceComponent is assigned its own
		// group's id below.
		for (const auto source : group.second)
		{
			const auto entity = remap.at(source);
			if (auto* member = destinationRegistry.try_get<PrefabMemberComponent>(entity))
				member->instanceId = freshIt->second;
		}
	}

	for (const auto& group : plan.rootsByGroup)
	{
		const rt2::core::UUID& fresh = freshIdByOriginalId.at(group.first);
		for (const auto source : group.second)
		{
			const auto entity = remap.at(source);
			if (auto* pic = destinationRegistry.try_get<PrefabInstanceComponent>(entity))
				pic->instanceId = fresh;
		}
	}
}

// Build the recovery-warning text for a CopiedPrefabPlan, or std::nullopt when
// nothing needed to be repaired. Never a hard failure.
// operation is a short lowercase verb ("duplicate", "paste") used in the
// detail, matching the pre-existing warning wording.
std::optional<rt2::core::Error> MakeCopiedPrefabRecoveryWarning(
	const char* operation,
	const CopiedPrefabPlan& plan)
{
	if (plan.orphanFragments == 0 && plan.ambiguousGroups == 0)
		return std::nullopt;

	std::string detail = std::string(operation) + ": ";
	if (plan.orphanFragments > 0)
	{
		detail += std::to_string(plan.orphanFragments) +
			" copied member(s) had no instance root in the copied set; "
			"prefab links were stripped (ordinary entities)";
	}
	if (plan.ambiguousGroups > 0)
	{
		if (plan.orphanFragments > 0)
			detail += " ";
		detail += std::to_string(plan.ambiguousGroups) +
			" instance group(s) had multiple copied roots sharing one "
			"instanceId; kept as one reminted group";
	}

	rt2::core::Error warning;
	warning.code = rt2::core::Error::InvalidHierarchy;
	warning.detail = std::move(detail);
	return warning;
}

void LogAssetDiagnostics(
	const std::vector<rt2::core::AssetDiagnostic>& diagnostics,
	size_t base,
	const char* context)
{
	for (size_t i = base; i < diagnostics.size(); ++i)
	{
		const auto& diagnostic = diagnostics[i];
		printf("[Asset] %s %s: path=%s source=%s detail=%s\n",
		       context,
		       rt2::core::AssetDiagnosticSeverityName(
			       diagnostic.severity),
		       diagnostic.refPath.c_str(),
		       diagnostic.sourceKey.c_str(),
		       diagnostic.detail.c_str());
	}
}

// All resource indices stored in ECSScene pass through this walk. Import
// merging uses bases; compaction supplies old-to-new maps. Keeping both
// operations on the same field list makes a new index-bearing field visible
// at the one place that must be updated for both operations.
enum class IndexRebaseMode
{
	None,
	Base,
	Remap,
};

template <typename Index>
class IndexRebaseAxis
{
public:
	void SetBase(Index base)
	{
		m_mode = IndexRebaseMode::Base;
		m_base = base;
		m_remap = nullptr;
	}

	void SetRemap(const std::map<Index, Index>& remap)
	{
		m_mode = IndexRebaseMode::Remap;
		m_remap = &remap;
	}

	bool IsActive() const { return m_mode != IndexRebaseMode::None; }

	Index Apply(Index index, Index unmapped) const
	{
		switch (m_mode)
		{
			case IndexRebaseMode::None:
				return index;
			case IndexRebaseMode::Base:
				return index + m_base;
			case IndexRebaseMode::Remap:
			{
				const auto it = m_remap->find(index);
				return it != m_remap->end() ? it->second : unmapped;
			}
		}
		return index;
	}

private:
	IndexRebaseMode m_mode = IndexRebaseMode::None;
	Index m_base = 0;
	const std::map<Index, Index>* m_remap = nullptr;
};

struct IndexRebase
{
	IndexRebaseAxis<uint32_t> mesh;
	IndexRebaseAxis<int> material;
	IndexRebaseAxis<int> texture;

	uint32_t Mesh(uint32_t index) const
	{
		return mesh.Apply(index, index);
	}

	int Material(int index) const
	{
		if (index < 0)
			return index;
		// Ordinary material references historically remain unchanged when a
		// compaction map has no entry for them; preserve that behavior.
		return material.Apply(index, index);
	}

	int MaterialOverride(int index) const
	{
		if (index < 0)
			return index;
		// The old compaction pass invalidated a transient override slot when
		// its material was removed; this deliberate asymmetry must remain.
		return material.Apply(index, -1);
	}

	int Texture(int index) const
	{
		if (index < 0)
			return index;
		// Compaction historically invalidated orphaned texture references.
		return texture.Apply(index, -1);
	}
};

// The four texture indices on a SceneMaterial are the complete field list
// for texture marking and rebasing. RebaseIndices and the texture pass of
// CompactMeshRegistry (including MaterialOverrideComponent snapshots) must
// enumerate exactly these; visiting them through this helper keeps the
// field list in one place so the passes cannot silently diverge.
template <typename MaterialT, typename Fn>
void ForEachMaterialTextureIndex(MaterialT& material, Fn&& fn)
{
	fn(material.baseColorTextureIndex);
	fn(material.normalTextureIndex);
	fn(material.emissiveTextureIndex);
	fn(material.metallicRoughnessTextureIndex);
}

void RebaseIndices(ECSScene& scene, const IndexRebase& rebase)
{
	// This is the complete list of scene-resource index fields. Keep all
	// additions here so merge and compaction cannot silently diverge.
	auto rebaseMaterialTextures = [&](SceneMaterial& material)
	{
		ForEachMaterialTextureIndex(material, [&](int& index) {
			index = rebase.Texture(index);
		});
	};

	if (rebase.material.IsActive())
	{
		for (uint32_t meshIndex = 0;
		     meshIndex < scene.meshRegistry.GetCount();
		     ++meshIndex)
		{
			auto& mesh = scene.meshRegistry.GetMesh(meshIndex);
			for (auto& materialIndex : mesh.materialIndices)
			{
				const int remapped = rebase.Material(static_cast<int>(materialIndex));
				if (remapped >= 0)
					materialIndex = static_cast<uint32_t>(remapped);
			}
		}
	}

	if (rebase.texture.IsActive())
		for (auto& material : scene.materials)
			rebaseMaterialTextures(material);

	if (rebase.mesh.IsActive() || rebase.material.IsActive())
	{
		auto meshRefView = scene.registry.view<MeshRef>();
		for (const auto entity : meshRefView)
		{
			auto& ref = meshRefView.get<MeshRef>(entity);
			if (rebase.mesh.IsActive())
				ref.meshIndex = rebase.Mesh(ref.meshIndex);
			if (rebase.material.IsActive())
				ref.materialIndex = rebase.Material(ref.materialIndex);
		}
	}

	if (rebase.material.IsActive() || rebase.texture.IsActive())
	{
		auto overrideView = scene.registry.view<MaterialOverrideComponent>();
		for (const auto entity : overrideView)
		{
			auto& materialOverride = overrideView.get<MaterialOverrideComponent>(entity);
			if (rebase.texture.IsActive())
				rebaseMaterialTextures(materialOverride.material);
			if (rebase.material.IsActive())
				materialOverride.materialIndex =
					rebase.MaterialOverride(materialOverride.materialIndex);
		}
	}
}
}

// Fill an imported entity's source path (if empty) and assign it a stable
// asset ID from the per-asset sidecar (.rt2meta), minting+writing the sidecar
// when absent (Phase 7 W1, per D8). Resolution by path is unchanged; the ID
// is plumbed but not yet authoritative. A minted ID is logged so a missing or
// malformed sidecar is observable, not silent. Errors do not block import:
// the scene still gets an ID for this session; the next save retries.
void FillImportedSourcePathAndId(entt::registry& reg,
                                 const std::string& filepath,
                                 rt2::core::IUuidProvider& provider)
{
	auto mv = reg.view<ImportedMeshSourceComponent>();
	for (auto e : mv)
	{
		auto& src = reg.get<ImportedMeshSourceComponent>(e);
		if (src.model.path.empty())
			src.model.path = filepath;
		if (!src.model.assetId.IsNull())
			continue; // already has a stable ID (e.g. from a loaded .rt2scene)

		bool minted = false;
		rt2::core::Error idErr;
		const rt2::core::UUID id =
			rt2::core::ResolveOrAssign(filepath, provider, minted, idErr);
		src.model.assetId = id;
		if (minted)
		{
			if (!idErr.IsOk())
			{
				printf("[Asset] %s: sidecar issue, assigned new id %s: %s\n",
				       filepath.c_str(), id.ToString().c_str(),
				       idErr.Format().c_str());
			}
			else
			{
				printf("[Asset] %s: assigned new id %s\n",
				       filepath.c_str(), id.ToString().c_str());
			}
			fflush(stdout);
		}
	}
}

SceneManager::SceneManager()
	: m_EcsScene(m_Authoring.ecs)
	, m_CurrentGpuScene(m_Authoring.gpuCache)
{
	m_Authoring.SetUuidProvider(&m_DefaultProvider);
}

void SceneManager::SetUuidProvider(rt2::core::IUuidProvider* provider)
{
	m_UuidProvider = provider ? provider : &m_DefaultProvider;
	m_Authoring.SetUuidProvider(m_UuidProvider);
}

entt::entity SceneManager::FindEntityByUuid(const rt2::core::UUID& uuid) const
{
	return m_Authoring.FindByUuid(uuid);
}

rt2::core::UUID SceneManager::GetEntityUuid(EntityId entity) const
{
	if (!entity.IsValid() || !m_EcsScene.registry.valid(entity.id))
		return rt2::core::UUID::Nil();
	const auto* identity = m_EcsScene.registry.try_get<EntityIdComponent>(entity.id);
	return identity ? identity->id : rt2::core::UUID::Nil();
}

void SceneManager::ReplaceAuthoringDocument(rt2::core::SceneDocument&& document,
	                                         uint64_t authoringRevision)
{
	// SceneDocument does not own its provider. Rebind it to the manager's
	// provider before and after assignment so future entity creation remains
	// valid even when the temporary document used a short-lived provider.
	document.SetUuidProvider(m_UuidProvider);
	m_Authoring = std::move(document);
	m_Authoring.SetUuidProvider(m_UuidProvider);
	rt2::core::Error hierarchyError;
	if (!SceneHierarchy::RebuildChildren(m_EcsScene.registry, hierarchyError))
		printf("[Scene] Adopted document hierarchy is invalid: %s\n",
		       hierarchyError.Format().c_str());

	m_EntityCache.clear();
	m_EntityCacheDirty = true;
	m_AuthoringRevision = authoringRevision;
	ReconcileStoredCameraDirections();
	++m_DocumentGeneration;
	++m_ResourceGeneration;
}

bool SceneManager::LoadScene(
	const std::string& filepath,
	std::vector<rt2::core::AssetDiagnostic>* diagnostics)
{
	printf("[Scene] LoadScene: '%s'\n", filepath.c_str());
	fflush(stdout);

	std::string ext = filepath.substr(filepath.find_last_of('.') + 1);
	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
	bool isObj = (ext == "obj");
	std::vector<rt2::core::AssetDiagnostic> localDiagnostics;
	auto& diagnosticSink =
		diagnostics ? *diagnostics : localDiagnostics;
	const size_t diagnosticBase = diagnosticSink.size();
	rt2::core::AssetResolutionContext importContext = m_AssetResolutionContext;
	if (importContext.assetRoot.empty())
		importContext.assetRoot = std::filesystem::u8path(filepath).parent_path();

	if (isObj)
	{
		rt2::core::TextureAssetLoadContext textureContext;
		if (!rt2::core::BuildExplicitImportTextureContext(
			    std::filesystem::u8path(filepath), m_UuidProvider,
			    importContext,
			    textureContext, diagnosticSink))
		{
			if (!diagnostics)
				LogAssetDiagnostics(
					diagnosticSink, diagnosticBase, "LoadScene");
			return false;
		}
		if (!SceneLoader::LoadObjIntoECS(
			    m_EcsScene, textureContext, diagnosticSink))
		{
			if (!diagnostics)
				LogAssetDiagnostics(
					diagnosticSink, diagnosticBase, "LoadScene");
			printf("[Scene] LoadObjIntoECS failed!\n");
			return false;
		}
		printf("[Scene] LoadObjIntoECS succeeded\n");
		fflush(stdout);
		// Fall through to UUID assignment + wrapper-root + validation below.
	}
	else
	{
		rt2::core::TextureAssetLoadContext textureContext;
		if (!rt2::core::BuildExplicitImportTextureContext(
			    std::filesystem::u8path(filepath), m_UuidProvider,
			    importContext,
			    textureContext, diagnosticSink))
		{
			if (!diagnostics)
				LogAssetDiagnostics(
					diagnosticSink, diagnosticBase, "LoadScene");
			return false;
		}
		if (!SceneLoader::LoadIntoECS(
			    m_EcsScene, textureContext, diagnosticSink))
		{
			if (!diagnostics)
				LogAssetDiagnostics(
					diagnosticSink, diagnosticBase, "LoadScene");
			printf("[Scene] SceneLoader::LoadIntoECS failed!\n");
			return false;
		}
		printf("[Scene] SceneLoader::LoadIntoECS succeeded\n");

		const auto& cam = m_EcsScene.camera;
		printf("[Scene] Camera: pos=(%.1f,%.1f,%.1f), forward=(%.1f,%.1f,%.1f), fov=%.1f\n",
		       cam.position.x, cam.position.y, cam.position.z,
		       cam.forwardDirection.x, cam.forwardDirection.y, cam.forwardDirection.z,
		       cam.verticalFOV);
	}

	{
		auto& reg = m_EcsScene.registry;

		std::vector<entt::entity> roots;
		auto view = reg.view<Transform>();
		for (auto entity : view)
		{
			auto* h = reg.try_get<Hierarchy>(entity);
			if (!h || h->parent == entt::null)
				roots.push_back(entity);
		}

		if (!roots.empty())
		{
			std::string name = filepath;
			size_t lastSlash = name.find_last_of("/\\");
			if (lastSlash != std::string::npos)
				name = name.substr(lastSlash + 1);
			size_t lastDot = name.find_last_of('.');
			if (lastDot != std::string::npos)
				name = name.substr(0, lastDot);

			auto rootEntity = reg.create();
			Transform& tf = reg.emplace<Transform>(rootEntity);
			tf.dirty = true;
			reg.emplace<NameComponent>(rootEntity, name);
			reg.emplace<VisibleComponent>(rootEntity);
			Hierarchy& rootHier = reg.emplace<Hierarchy>(rootEntity);
			rootHier.parent = entt::null;

			for (auto child : roots)
			{
				auto* childHier = reg.try_get<Hierarchy>(child);
				if (!childHier)
					childHier = &reg.emplace<Hierarchy>(child);
				childHier->parent = rootEntity;
				rootHier.children.push_back(child);
				SceneGraph::SetLocalDirty(reg, child);
			}

			SceneGraph::SetLocalDirty(reg, rootEntity);
			SceneGraph::UpdateWorldTransforms(reg);

			m_Authoring.AssignNewUuid(rootEntity);
		}
	}

	// Rebuild the UUID index for all entities loaded by the scene loader.
	// SceneLoader creates entities directly on the registry without going
	// through SceneManager::Add*, so they do not yet have EntityIdComponent.
	// Assign UUIDs to any entity that lacks one, then validate.
	{
		auto& reg = m_EcsScene.registry;
		auto view = reg.view<Transform>();
		for (auto entity : view)
		{
			if (!reg.all_of<EntityIdComponent>(entity))
				m_Authoring.AssignNewUuid(entity);
		}
	}

	// Record the source model path on imported mesh entities so the native
	// .rt2scene serializer can persist a durable reference, and assign each
	// a stable asset ID from its sidecar (Phase 7 W1).
	FillImportedSourcePathAndId(m_EcsScene.registry, filepath, *m_UuidProvider);

	rt2::core::Error hierarchyError;
	if (!SceneHierarchy::RebuildChildren(m_EcsScene.registry, hierarchyError))
	{
		printf("[Scene] Hierarchy validation failed after load: %s\n",
		       hierarchyError.Format().c_str());
		return false;
	}

	rt2::core::Error uuidErr;
	if (!m_Authoring.ValidateUniqueUuids(uuidErr))
	{
		printf("[Scene] UUID validation failed after load: %s\n", uuidErr.Format().c_str());
		fflush(stdout);
	}

	printf("[Scene] Loaded %d meshes, %d materials, %d lights, %d textures\n",
	       (int)m_EcsScene.meshRegistry.GetCount(), (int)m_EcsScene.materials.size(),
	       (int)m_EcsScene.registry.view<const LightComponent>().size(),
	       (int)m_EcsScene.textures.size());

	m_EntityCacheDirty = true;
	++m_DocumentGeneration;
	++m_ResourceGeneration;
	if (!diagnostics)
		LogAssetDiagnostics(diagnosticSink, diagnosticBase, "LoadScene");
	return true;
}

bool SceneManager::LoadEnvMap(const std::string& filepath,
                                rt2::core::Error* envImportErr)
{
	printf("[EnvMap] Loading '%s'\n", filepath.c_str());

	bool isEXR = filepath.size() >= 4 &&
	             (filepath.compare(filepath.size() - 4, 4, ".exr") == 0 ||
	              filepath.compare(filepath.size() - 4, 4, ".EXR") == 0);

	int w = 0, h = 0;
	std::vector<float> pixels;

	if (isEXR)
	{
		float* outRGBA = nullptr;
		const char* err = nullptr;
		int ret = LoadEXR(&outRGBA, &w, &h, filepath.c_str(), &err);
		if (ret != TINYEXR_SUCCESS || !outRGBA)
		{
			printf("[EnvMap] Failed to load EXR: %s\n", err ? err : "unknown");
			if (err) free((void*)err);
			return false;
		}
		pixels.assign(outRGBA, outRGBA + (size_t)w * h * 4);
		free(outRGBA);
		if (err) free((void*)err);
		printf("[EnvMap] Loaded %dx%d EXR\n", w, h);
	}
	else
	{
		int channels;
		float* data = stbi_loadf(filepath.c_str(), &w, &h, &channels, 4);
		if (!data)
		{
			printf("[EnvMap] Failed to load HDR file!\n");
			return false;
		}
		pixels.assign(data, data + (size_t)w * h * 4);
		stbi_image_free(data);
		printf("[EnvMap] Loaded %dx%d HDR\n", w, h);
	}

	m_Authoring.environment.ref.kind = AssetKind::Environment;
	m_Authoring.environment.ref.path = filepath;
	m_Authoring.environment.width = w;
	m_Authoring.environment.height = h;
	m_Authoring.environment.floatPixels = std::move(pixels);

	// Phase 7 W3 step 4: assign a stable env-asset ID via the per-asset
	// sidecar, paralleling model import. The sidecar is the durable source
	// of truth; env.ref.assetId is a cache of it. ResolveEnvironment reads
	// the sidecar through the shared locator and never mints.
	if (m_UuidProvider)
	{
		bool minted = false;
		rt2::core::Error idErr;
		const rt2::core::UUID id = rt2::core::ResolveOrAssign(
			filepath, *m_UuidProvider, minted, idErr);
		m_Authoring.environment.ref.assetId = id;
		if (minted)
		{
			printf("[Asset] %s: assigned new id %s%s%s\n",
			       filepath.c_str(), id.ToString().c_str(),
			       idErr.IsOk() ? "" : ": ",
			       idErr.IsOk() ? "" : idErr.Format().c_str());
			fflush(stdout);
		}
		// Retain a structured diagnostic for sidecar read/write errors
		// (item 4): the load still succeeds, but the caller can surface
		// the error instead of relying on console output.
		if (envImportErr)
			*envImportErr = idErr;
	}
	return true;
}

void SceneManager::ClearEnvMap()
{
	m_Authoring.environment.Clear();
}

void SceneManager::SetEnvMapData(const std::string& filepath, int w, int h,
                                 std::vector<float> pixels,
                                 rt2::core::Error* envImportErr)
{
	m_Authoring.environment.ref.kind = AssetKind::Environment;
	m_Authoring.environment.ref.path = filepath;
	m_Authoring.environment.width = w;
	m_Authoring.environment.height = h;
	m_Authoring.environment.floatPixels = std::move(pixels);

	// Phase 7 W3 step 4: assign a stable env-asset ID via the sidecar. This
	// is the async-load completion path (WalnutApp background decode); it
	// must keep env.ref.assetId in lockstep with LoadEnvMap so a save/reopen
	// round-trip resolves by the same identity.
	if (m_UuidProvider)
	{
		bool minted = false;
		rt2::core::Error idErr;
		const rt2::core::UUID id = rt2::core::ResolveOrAssign(
			filepath, *m_UuidProvider, minted, idErr);
		m_Authoring.environment.ref.assetId = id;
		if (minted)
		{
			printf("[Asset] %s: assigned new id %s%s%s\n",
			       filepath.c_str(), id.ToString().c_str(),
			       idErr.IsOk() ? "" : ": ",
			       idErr.IsOk() ? "" : idErr.Format().c_str());
			fflush(stdout);
		}
		// Retain a structured diagnostic for sidecar read/write errors
		// (item 4).
		if (envImportErr)
			*envImportErr = idErr;
	}
}

SceneManager::EntityId SceneManager::ImportGltf(
	const std::string& filepath,
	std::vector<rt2::core::AssetDiagnostic>* diagnostics)
{
	std::vector<rt2::core::AssetDiagnostic> localDiagnostics;
	auto& diagnosticSink =
		diagnostics ? *diagnostics : localDiagnostics;
	const size_t diagnosticBase = diagnosticSink.size();
	rt2::core::AssetResolutionContext importContext = m_AssetResolutionContext;
	if (importContext.assetRoot.empty())
		importContext.assetRoot = std::filesystem::u8path(filepath).parent_path();
	rt2::core::TextureAssetLoadContext textureContext;
	if (!rt2::core::BuildExplicitImportTextureContext(
		    std::filesystem::u8path(filepath), m_UuidProvider,
		    importContext,
		    textureContext, diagnosticSink))
	{
		if (!diagnostics)
			LogAssetDiagnostics(
				diagnosticSink, diagnosticBase, "ImportGltf");
		return EntityId{};
	}
	entt::entity root = SceneLoader::ImportIntoECS(
		m_EcsScene, textureContext, diagnosticSink);
	if (root == entt::null)
	{
		if (!diagnostics)
			LogAssetDiagnostics(
				diagnosticSink, diagnosticBase, "ImportGltf");
		return EntityId{};
	}

	// Assign UUIDs to any imported entity that lacks one.
	auto& reg = m_EcsScene.registry;
	auto view = reg.view<Transform>();
	for (auto entity : view)
	{
		if (!reg.all_of<EntityIdComponent>(entity))
			m_Authoring.AssignNewUuid(entity);
	}

	// Record the source model path on every imported mesh entity so the
	// native .rt2scene serializer can persist a durable reference, and
	// assign each a stable asset ID from its sidecar (Phase 7 W1).
	{
		auto& reg = m_EcsScene.registry;
		FillImportedSourcePathAndId(reg, filepath, *m_UuidProvider);
	}

	m_EntityCacheDirty = true;
	if (!diagnostics)
		LogAssetDiagnostics(
			diagnosticSink, diagnosticBase, "ImportGltf");
	return EntityId{ root };
}

SceneManager::EntityId SceneManager::ImportObj(
	const std::string& filepath,
	const ImportSettings& settings,
	std::vector<rt2::core::AssetDiagnostic>* diagnostics)
{
	std::vector<rt2::core::AssetDiagnostic> localDiagnostics;
	auto& diagnosticSink =
		diagnostics ? *diagnostics : localDiagnostics;
	const size_t diagnosticBase = diagnosticSink.size();
	rt2::core::AssetResolutionContext importContext = m_AssetResolutionContext;
	if (importContext.assetRoot.empty())
		importContext.assetRoot = std::filesystem::u8path(filepath).parent_path();
	rt2::core::TextureAssetLoadContext textureContext;
	if (!rt2::core::BuildExplicitImportTextureContext(
		    std::filesystem::u8path(filepath), m_UuidProvider,
		    importContext,
		    textureContext, diagnosticSink))
	{
		if (!diagnostics)
			LogAssetDiagnostics(
				diagnosticSink, diagnosticBase, "ImportObj");
		return EntityId{};
	}
	entt::entity root = SceneLoader::ImportObjIntoECS(
		m_EcsScene, settings, textureContext, diagnosticSink);
	if (root == entt::null)
	{
		if (!diagnostics)
			LogAssetDiagnostics(
				diagnosticSink, diagnosticBase, "ImportObj");
		return EntityId{};
	}

	// Assign UUIDs to any imported entity that lacks one.
	auto& reg = m_EcsScene.registry;
	auto view = reg.view<Transform>();
	for (auto entity : view)
	{
		if (!reg.all_of<EntityIdComponent>(entity))
			m_Authoring.AssignNewUuid(entity);
	}

	// Record the source model path on every imported mesh entity so the
	// native .rt2scene serializer can persist a durable reference, and
	// assign each a stable asset ID from its sidecar (Phase 7 W1).
	{
		FillImportedSourcePathAndId(reg, filepath, *m_UuidProvider);
	}

	m_EntityCacheDirty = true;
	if (!diagnostics)
		LogAssetDiagnostics(
			diagnosticSink, diagnosticBase, "ImportObj");
	return EntityId{ root };
}

SceneManager::EntityId SceneManager::MergeImportedECS(ECSScene&& src,
                                                       entt::entity srcRoot,
                                                       const std::string& sourcePath)
{
	if (srcRoot == entt::null || !src.registry.valid(srcRoot))
		return EntityId{};

	auto& dst = m_EcsScene;
	auto& dstReg = dst.registry;
	auto& srcReg = src.registry;

	// Record base offsets in the destination scene.
	const uint32_t meshBase = dst.meshRegistry.GetCount();
	const int matBase = (int)dst.materials.size();
	const int texBase = (int)dst.textures.size();
	IndexRebase rebase;
	rebase.mesh.SetBase(meshBase);
	rebase.material.SetBase(matBase);
	rebase.texture.SetBase(texBase);
	RebaseIndices(src, rebase);

	// Append meshes after the complete source scene has been rebased.
	for (uint32_t i = 0; i < src.meshRegistry.GetCount(); ++i)
		dst.meshRegistry.AddMesh(src.meshRegistry.GetMesh(i));

	// Append materials after their texture indices have been rebased.
	for (const auto& sm : src.materials)
		dst.materials.push_back(sm);

	// Append textures.
	for (auto& st : src.textures)
		dst.textures.push_back(std::move(st));

	// Map src entities to dst entities.
	std::unordered_map<entt::entity, entt::entity> entityMap;

	// First pass: create all dst entities and copy simple components.
	{
		auto view = srcReg.view<Transform>();
		for (auto e : view)
		{
			entt::entity dstE = dstReg.create();
			entityMap[e] = dstE;

			const auto& srcTf = view.get<Transform>(e);
			Transform& dstTf = dstReg.emplace<Transform>(dstE);
			dstTf.translation = srcTf.translation;
			dstTf.rotation = srcTf.rotation;
			dstTf.scale = srcTf.scale;
			dstTf.dirty = true;
		}
	}

	// Copy MeshRef after the complete source index walk. -1 remains the
	// "use per-triangle indices" sentinel.
	{
		auto view = srcReg.view<MeshRef>();
		for (auto e : view)
		{
			auto it = entityMap.find(e);
			if (it == entityMap.end()) continue;
			const auto& srcRef = view.get<MeshRef>(e);
			dstReg.emplace<MeshRef>(it->second, srcRef);
		}
	}

	// Copy NameComponent.
	{
		auto view = srcReg.view<NameComponent>();
		for (auto e : view)
		{
			auto it = entityMap.find(e);
			if (it == entityMap.end()) continue;
			dstReg.emplace<NameComponent>(it->second, view.get<NameComponent>(e));
		}
	}

	// Copy VisibleComponent.
	{
		auto view = srcReg.view<VisibleComponent>();
		for (auto e : view)
		{
			auto it = entityMap.find(e);
			if (it == entityMap.end()) continue;
			dstReg.emplace<VisibleComponent>(it->second, view.get<VisibleComponent>(e));
		}
	}

	// Material overrides are authored data rather than loader output today,
	// but they are valid ECSScene components and carry the same resource
	// indices. Copy the already-rebased value so this path cannot lose it if a
	// temporary import scene contains an override.
	{
		auto view = srcReg.view<MaterialOverrideComponent>();
		for (auto e : view)
		{
			auto it = entityMap.find(e);
			if (it == entityMap.end()) continue;
			dstReg.emplace<MaterialOverrideComponent>(
				it->second, view.get<MaterialOverrideComponent>(e));
		}
	}

	// Copy Hierarchy (remap parent + children via entityMap).
	{
		auto view = srcReg.view<Hierarchy>();
		for (auto e : view)
		{
			auto it = entityMap.find(e);
			if (it == entityMap.end()) continue;
			const auto& srcHier = view.get<Hierarchy>(e);
			Hierarchy dstHier;
			if (srcHier.parent != entt::null)
			{
				auto pit = entityMap.find(srcHier.parent);
				dstHier.parent = (pit != entityMap.end()) ? pit->second : entt::null;
			}
			else
				dstHier.parent = entt::null;
			for (auto child : srcHier.children)
			{
				auto cit = entityMap.find(child);
				if (cit != entityMap.end())
					dstHier.children.push_back(cit->second);
			}
			dstReg.emplace<Hierarchy>(it->second, std::move(dstHier));
		}
	}

	// Copy ImportedMeshSourceComponent + fill source path + assign sidecar ID.
	{
		auto view = srcReg.view<ImportedMeshSourceComponent>();
		for (auto e : view)
		{
			auto it = entityMap.find(e);
			if (it == entityMap.end()) continue;
			auto srcComp = view.get<ImportedMeshSourceComponent>(e);
			if (srcComp.model.path.empty())
				srcComp.model.path = sourcePath;
			if (srcComp.model.assetId.IsNull())
			{
				bool minted = false;
				rt2::core::Error idErr;
				const rt2::core::UUID id = rt2::core::ResolveOrAssign(
					sourcePath, *m_UuidProvider, minted, idErr);
				srcComp.model.assetId = id;
				if (minted)
				{
					printf("[Asset] %s: assigned new id %s%s%s\n",
					       sourcePath.c_str(), id.ToString().c_str(),
					       idErr.IsOk() ? "" : ": ",
					       idErr.IsOk() ? "" : idErr.Format().c_str());
					fflush(stdout);
				}
			}
			dstReg.emplace<ImportedMeshSourceComponent>(it->second, std::move(srcComp));
		}
	}

	// Assign UUIDs to all imported entities that lack one.
	{
		auto view = dstReg.view<Transform>();
		for (auto entity : view)
		{
			if (!dstReg.all_of<EntityIdComponent>(entity))
				m_Authoring.AssignNewUuid(entity);
		}
	}

	// Find the wrapper root in the destination.
	entt::entity dstRoot = entt::null;
	auto rootIt = entityMap.find(srcRoot);
	if (rootIt != entityMap.end())
		dstRoot = rootIt->second;

	// Update world transforms for the imported hierarchy.
	if (dstRoot != entt::null)
	{
		SceneGraph::SetLocalDirty(dstReg, dstRoot);
		SceneGraph::UpdateWorldTransforms(dstReg);
	}

	m_EntityCacheDirty = true;
	return EntityId{ dstRoot };
}

void SceneManager::SyncToGPU()
{
	printf("[Scene] SyncToGPU: building GPU scene data...\n");
	fflush(stdout);

	GPUSceneData gpuData;

	UpdateWorldTransforms();
	gpuData = BuildGPUSceneDataFromECS(m_EcsScene, &m_RenderInstanceMap);

	printf("[Scene] SyncToGPU: GPUSceneData built: meshes=%zu instances=%zu lights=%zu textures=%zu source_emissive=%u filtered_black=%u\n",
	       gpuData.meshes.size(), gpuData.instances.size(), gpuData.lights.size(), gpuData.textures.size(),
	       gpuData.sourceEmissiveTriangleCount, gpuData.filteredBlackEmissiveTriangleCount);
	fflush(stdout);

	// Add env map as an extra texture in the texture array
	if (HasEnvMap())
	{
		auto& env = m_Authoring.environment;
		SceneTexture envTex;
		envTex.isHDR = true;
		envTex.width = env.width;
		envTex.height = env.height;
		envTex.floatPixels = env.floatPixels;
		gpuData.textures.push_back(envTex);
		gpuData.envMapIndex = (int)gpuData.textures.size() - 1;

		BuildEnvMapCDF(env.floatPixels, env.width, env.height,
		               gpuData.marginalCDF, gpuData.conditionalCDF);
		gpuData.cdfWidth = env.width;
		gpuData.cdfHeight = env.height;

		printf("[Scene] Env map: idx=%d %dx%d, CDF built\n",
		       gpuData.envMapIndex, env.width, env.height);
	}

	if (m_SyncCallback)
	{
		printf("[Scene] SyncToGPU: calling sync callback (SetScene)...\n");
		fflush(stdout);
		m_SyncCallback(gpuData, m_RenderInstanceMap);
		printf("[Scene] SyncToGPU: sync callback done\n");
		fflush(stdout);
	}

	m_CurrentGpuScene = std::move(gpuData);
}

void SceneManager::SyncToGPUKeepTextures()
{
	GPUSceneData gpuData;

	UpdateWorldTransforms();
	gpuData = BuildGPUSceneDataFromECS(m_EcsScene, &m_RenderInstanceMap);

	// Preserve env map data from current GPU scene (textures aren't re-uploaded)
	if (m_CurrentGpuScene.envMapIndex >= 0)
	{
		gpuData.envMapIndex = m_CurrentGpuScene.envMapIndex;
		gpuData.envIntensity = m_CurrentGpuScene.envIntensity;
		gpuData.marginalCDF = m_CurrentGpuScene.marginalCDF;
		gpuData.conditionalCDF = m_CurrentGpuScene.conditionalCDF;
		gpuData.cdfWidth = m_CurrentGpuScene.cdfWidth;
		gpuData.cdfHeight = m_CurrentGpuScene.cdfHeight;
	}

	m_CurrentGpuScene = gpuData;
	if (m_SyncKeepTexturesCallback)
		m_SyncKeepTexturesCallback(gpuData, m_RenderInstanceMap);
}

// ============================================================================
// Entity manipulation
// ============================================================================

SceneManager::EntityId SceneManager::AddObject(const std::string& name,
                                                const glm::vec3& position,
                                                const glm::vec3& rotation,
                                                float scale,
                                                int materialIndex)
{
	auto entity = m_EcsScene.registry.create();

	Transform tf;
	tf.translation = position;
	tf.rotation = glm::quat(glm::radians(rotation));
	tf.scale = {scale, scale, scale};
	m_EcsScene.registry.emplace<Transform>(entity, tf);

	// NOTE: this references mesh 0 even when the registry is empty, which is
	// a dangling reference until geometry arrives. It is deliberate and
	// depended upon: the production caller (WalnutApp's model-load path)
	// calls this straight after SceneLoader::LoadIntoECS, when index 0 is
	// valid, and test fixtures treat AddObject as "add a renderable object"
	// and read the MeshRef back. Making it conditional breaks both.
	//
	// The reference is inert because every consumer bounds-checks it before
	// indexing: CompactMeshRegistry skips out-of-range indices, and
	// GPUSceneData guards at :292 and :451. Anything new that indexes
	// meshRegistry by a MeshRef must do the same.
	m_EcsScene.registry.emplace<MeshRef>(entity, 0u, materialIndex);

	if (!name.empty())
		m_EcsScene.registry.emplace<NameComponent>(entity, name);
	m_EcsScene.registry.emplace<VisibleComponent>(entity);

	m_Authoring.AssignNewUuid(entity);
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	return {entity};
}

SceneManager::EntityId SceneManager::AddObjectWithGeometry(const std::string& name,
                                                           MeshData&& meshData,
                                                           const glm::vec3& position,
                                                           const glm::vec3& rotation,
                                                           float scale,
                                                           int materialIndex)
{
	uint32_t meshIdx = m_EcsScene.meshRegistry.AddMesh(std::move(meshData));

	auto entity = m_EcsScene.registry.create();

	Transform tf;
	tf.translation = position;
	tf.rotation = glm::quat(glm::radians(rotation));
	tf.scale = {scale, scale, scale};
	m_EcsScene.registry.emplace<Transform>(entity, tf);
	m_EcsScene.registry.emplace<MeshRef>(entity, meshIdx, materialIndex);

	if (!name.empty())
		m_EcsScene.registry.emplace<NameComponent>(entity, name);
	m_EcsScene.registry.emplace<VisibleComponent>(entity);

	m_Authoring.AssignNewUuid(entity);
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	return {entity};
}

SceneManager::EntityId SceneManager::AddLight(const std::string& name,
                                              const glm::vec3& position,
                                              const glm::vec3& color,
                                              float intensity,
                                              LightType type)
{
	auto entity = m_EcsScene.registry.create();

	Transform tf;
	tf.translation = position;
	m_EcsScene.registry.emplace<Transform>(entity, tf);

	LightComponent light;
	light.color = color;
	light.intensity = intensity;
	light.type = type;
	m_EcsScene.registry.emplace<LightComponent>(entity, light);

	if (!name.empty())
		m_EcsScene.registry.emplace<NameComponent>(entity, name);
	m_EcsScene.registry.emplace<VisibleComponent>(entity);

	m_Authoring.AssignNewUuid(entity);
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	return {entity};
}

void SceneManager::RemoveEntity(EntityId entity)
{
	if (!entity.IsValid()) return;

	auto& reg = m_EcsScene.registry;
	if (!reg.valid(entity.id)) return;
	const auto stableId = GetEntityUuid(entity);
	if (!stableId.IsNull())
	{
		RemoveSubtrees({ stableId });
		return;
	}

	// Remove from UUID index before destruction.
	if (auto* idc = reg.try_get<EntityIdComponent>(entity.id))
		m_Authoring.uuidIndex.Erase(idc->id);

	// Remove from parent's children list if has Hierarchy
	if (auto* h = reg.try_get<Hierarchy>(entity.id))
	{
		if (h->parent != entt::null)
		{
			if (auto* parentH = reg.try_get<Hierarchy>(h->parent))
			{
				auto& children = parentH->children;
				children.erase(std::remove(children.begin(), children.end(), entity.id), children.end());
			}
		}
	}

	// Recursively destroy children
	if (auto* h = reg.try_get<Hierarchy>(entity.id))
	{
		auto children = h->children; // copy — registry modified during destroy
		for (auto child : children)
			RemoveEntity({child});
	}

	reg.destroy(entity.id);
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
}

EditorMutationResult SceneManager::CreateEmpty(
	const std::string& name,
	const std::optional<rt2::core::UUID>& parentUuid)
{
	auto& registry = m_EcsScene.registry;
	entt::entity parent = entt::null;
	if (parentUuid)
	{
		parent = m_Authoring.FindByUuid(*parentUuid);
		if (parent == entt::null || !registry.valid(parent))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				parentUuid->ToString(), "parent UUID is not present in the authoring scene");
	}
	const auto entity = registry.create();
	registry.emplace<Transform>(entity);
	registry.emplace<NameComponent>(entity, name.empty() ? "Empty" : name);
	registry.emplace<VisibleComponent>(entity);
	if (parent != entt::null)
	{
		registry.emplace<Hierarchy>(entity).parent = parent;
		auto* hierarchy = registry.try_get<Hierarchy>(parent);
		if (!hierarchy)
			hierarchy = &registry.emplace<Hierarchy>(parent);
		hierarchy->children.push_back(entity);
	}
	const auto uuid = m_Authoring.AssignNewUuid(entity);
	SceneGraph::MarkDirty(registry, entity);
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	EditorMutationResult result;
	result.affectedEntities.push_back(uuid);
	return result;
}

EditorMutationResult SceneManager::Reparent(
	const std::vector<rt2::core::UUID>& entityUuids,
	const std::optional<rt2::core::UUID>& newParentUuid,
	ReparentMode mode)
{
	if (entityUuids.empty()) return {};
	auto& registry = m_EcsScene.registry;
	rt2::core::Error error;
	auto roots = ResolveCanonicalRoots(m_Authoring, entityUuids, error);
	if (!error.IsOk())
	{
		EditorMutationResult result;
		result.success = false;
		result.error = error;
		return result;
	}
	entt::entity newParent = entt::null;
	if (newParentUuid)
	{
		newParent = m_Authoring.FindByUuid(*newParentUuid);
		if (newParent == entt::null || !registry.valid(newParent))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				newParentUuid->ToString(), "new parent UUID is not present in the authoring scene");
	}
	std::vector<entt::entity> changed;
	for (const auto root : roots)
	{
		if (newParent != entt::null && SceneHierarchy::IsDescendant(registry, root, newParent))
			return EditorMutationResult::Failure(rt2::core::Error::HierarchyCycle,
				GetEntityUuid({ root }).ToString(),
				"cannot parent an entity beneath itself or a descendant");
		const auto* hierarchy = registry.try_get<Hierarchy>(root);
		if ((hierarchy ? hierarchy->parent : entt::null) != newParent)
			changed.push_back(root);
	}
	if (changed.empty()) return {};

	UpdateWorldTransforms();
	std::vector<std::pair<entt::entity, EditableTRS>> newLocals;
	if (mode == ReparentMode::PreserveWorld)
	{
		glm::mat4 parentWorld(1.0f);
		if (newParent != entt::null)
		{
			const auto* parentTransform = registry.try_get<Transform>(newParent);
			if (!parentTransform)
				return EditorMutationResult::Failure(rt2::core::Error::InvalidTransform,
					newParentUuid->ToString(), "new parent has no transform");
			parentWorld = parentTransform->worldMatrix;
		}
		for (const auto root : changed)
		{
			const auto* transform = registry.try_get<Transform>(root);
			if (!transform)
				return EditorMutationResult::Failure(rt2::core::Error::InvalidTransform,
					GetEntityUuid({ root }).ToString(), "reparented entity has no transform");
			EditableTRS local;
			if (!TryWorldToLocalTRS(parentWorld, transform->worldMatrix, local))
				return EditorMutationResult::Failure(rt2::core::Error::InvalidTransform,
					GetEntityUuid({ root }).ToString(),
					"preserve-world reparent produced a singular or sheared transform");
			newLocals.emplace_back(root, local);
		}
	}

	for (const auto root : changed)
	{
		auto* hierarchy = registry.try_get<Hierarchy>(root);
		RemoveChild(registry, hierarchy ? hierarchy->parent : entt::null, root);
		if (!hierarchy)
			hierarchy = &registry.emplace<Hierarchy>(root);
		hierarchy->parent = newParent;
		if (newParent != entt::null)
		{
			auto* parentHierarchy = registry.try_get<Hierarchy>(newParent);
			if (!parentHierarchy)
				parentHierarchy = &registry.emplace<Hierarchy>(newParent);
			parentHierarchy->children.push_back(root);
		}
	}
	for (const auto& entry : newLocals)
	{
		auto& transform = registry.get<Transform>(entry.first);
		transform.translation = entry.second.translation;
		transform.rotation = glm::normalize(entry.second.rotation);
		transform.scale = entry.second.scale;
	}
	for (const auto root : changed)
		SceneGraph::MarkDirty(registry, root);
	RefreshCameraForwardDirections(changed);
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	EditorMutationResult result;
	result.syncImpact = rt2::core::SyncImpact::Transform;
	for (const auto root : changed)
		result.affectedEntities.push_back(GetEntityUuid({ root }));
	return result;
}

EditorMutationResult SceneManager::RemoveSubtrees(
	const std::vector<rt2::core::UUID>& rootUuids)
{
	if (rootUuids.empty()) return {};
	rt2::core::Error error;
	auto roots = ResolveCanonicalRoots(m_Authoring, rootUuids, error);
	if (!error.IsOk())
	{
		EditorMutationResult result;
		result.success = false;
		result.error = error;
		return result;
	}
	auto& registry = m_EcsScene.registry;
	std::vector<entt::entity> postOrder;
	bool removesRenderable = false;
	EditorMutationResult result;
	for (const auto root : roots)
	{
		std::vector<entt::entity> subtree;
		SceneHierarchy::CollectSubtreePostOrder(registry, root, subtree);
		for (const auto entity : subtree)
		{
			postOrder.push_back(entity);
			removesRenderable = removesRenderable || registry.all_of<MeshRef>(entity);
			if (const auto* identity = registry.try_get<EntityIdComponent>(entity))
				result.affectedEntities.push_back(identity->id);
		}
		if (const auto* hierarchy = registry.try_get<Hierarchy>(root))
			RemoveChild(registry, hierarchy->parent, root);
	}
	for (const auto entity : postOrder)
	{
		if (const auto* identity = registry.try_get<EntityIdComponent>(entity))
			m_Authoring.uuidIndex.Erase(identity->id);
		registry.destroy(entity);
	}
	const bool compacted = CompactMeshRegistry();
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	result.syncImpact = (removesRenderable || compacted)
		? rt2::core::SyncImpact::Structural : rt2::core::SyncImpact::None;
	return result;
}

EditorMutationResult SceneManager::SetVisibility(
	const std::vector<rt2::core::UUID>& entityUuids, bool visible)
{
	if (entityUuids.empty()) return {};
	auto& registry = m_EcsScene.registry;
	std::vector<entt::entity> entities;
	std::unordered_set<entt::entity> unique;
	for (const auto& uuid : entityUuids)
	{
		const auto entity = m_Authoring.FindByUuid(uuid);
		if (entity == entt::null || !registry.valid(entity))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				uuid.ToString(), "entity UUID is not present in the authoring scene");
		if (unique.insert(entity).second)
			entities.push_back(entity);
	}
	EditorMutationResult result;
	for (const auto entity : entities)
	{
		auto* component = registry.try_get<VisibleComponent>(entity);
		const bool current = component ? component->visible : true;
		if (current == visible) continue;
		if (!component)
			component = &registry.emplace<VisibleComponent>(entity);
		component->visible = visible;
		result.affectedEntities.push_back(GetEntityUuid({ entity }));
	}
	if (result.affectedEntities.empty()) return result;
	NotifyAuthoringChanged();
	result.syncImpact = rt2::core::SyncImpact::Structural;
	return result;
}

EditorMutationResult SceneManager::SetVisibilityStates(
	const std::vector<std::pair<rt2::core::UUID, bool>>& states)
{
	if (states.empty()) return {};
	auto& registry = m_EcsScene.registry;

	// Validate ALL UUIDs first. Any failure => zero mutation. Deduplicate
	// last-write-wins by walking in order and overwriting the per-entity
	// target slot.
	std::vector<std::pair<entt::entity, bool>> resolved;
	std::unordered_map<entt::entity, std::size_t> indexByEntity;
	for (const auto& [uuid, visible] : states)
	{
		const auto entity = m_Authoring.FindByUuid(uuid);
		if (entity == entt::null || !registry.valid(entity))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				uuid.ToString(), "entity UUID is not present in the authoring scene");
		const auto it = indexByEntity.find(entity);
		if (it == indexByEntity.end())
		{
			indexByEntity.emplace(entity, resolved.size());
			resolved.emplace_back(entity, visible);
		}
		else
		{
			resolved[it->second].second = visible; // last-write-wins
		}
	}

	EditorMutationResult result;
	for (const auto& [entity, visible] : resolved)
	{
		auto* component = registry.try_get<VisibleComponent>(entity);
		const bool current = component ? component->visible : true;
		if (current == visible) continue;
		if (!component)
			component = &registry.emplace<VisibleComponent>(entity);
		component->visible = visible;
		result.affectedEntities.push_back(GetEntityUuid({ entity }));
	}

	if (result.affectedEntities.empty())
	{
		// Empty-success: no entities actually changed state.
		return result;
	}

	NotifyAuthoringChanged();
	result.syncImpact = rt2::core::SyncImpact::Structural;
	return result;
}

EditorMutationResult SceneManager::DuplicateSubtrees(
	const std::vector<rt2::core::UUID>& rootUuids)
{
	if (rootUuids.empty()) return {};
	rt2::core::Error error;
	auto roots = ResolveCanonicalRoots(m_Authoring, rootUuids, error);
	if (!error.IsOk())
	{
		EditorMutationResult result;
		result.success = false;
		result.error = error;
		return result;
	}
	auto& registry = m_EcsScene.registry;
	std::vector<entt::entity> sources;
	for (const auto root : roots)
		SceneHierarchy::CollectSubtreePreOrder(registry, root, sources);

	// Phase 8 W3, S4 (review fix 2): PLAN the copied-forest prefab
	// classification from the SOURCE forest BEFORE any destination mutation,
	// then reserve every fresh instanceId. A copy is a NEW instance, never a
	// verbatim share of the source's instanceId (W3-D8).
	const CopiedPrefabPlan copiedPrefabs = PlanCopiedPrefabLinks(registry, roots);
	std::unordered_map<rt2::core::UUID, rt2::core::UUID> freshIdByOriginalId;

	// Stage the whole copy's entity UUIDs first. Provider-consumption order is
	// therefore deterministic and pinned by the fixup tests: every entity UUID
	// (sources.size() draws) comes first, then one fresh instanceId per
	// complete instance group. The create loop below consumes no provider.
	// Every staged UUID is validated BEFORE any destination mutation — nil,
	// already indexed in the authoring document (source == destination here, so
	// the source entities' ids ARE the authoring index), or repeated within
	// this operation — so a hostile provider trips a stage-specific
	// DuplicateUuid here, never midway through the create loop.
	std::vector<rt2::core::UUID> duplicateUuids;
	{
		const std::unordered_set<rt2::core::UUID> forbidden =
			EntityUuidForbiddenSet(m_Authoring, registry, &registry);
		std::unordered_set<rt2::core::UUID> operationLocal;
		rt2::core::Error reserveErr;
		if (!ReserveValidEntityUuids([this] { return ReserveKnownUuid(); }, forbidden,
			operationLocal, sources.size(), "DuplicateSubtrees", duplicateUuids, reserveErr))
		{
			return EditorMutationResult::Failure(reserveErr.code, reserveErr.path,
				reserveErr.detail);
		}
	}

	// Reserve ONE fresh instanceId per complete instance group, ALL before
	// mutation. The collision set is the authoring uuidIndex + live destination
	// PIC/PMIC instanceIds (source == destination for a duplicate) + the staged
	// entity UUIDs. ApplyCopiedPrefabLinks below makes zero provider calls;
	// failure here is transactional because nothing has been created yet.
	{
		std::unordered_set<rt2::core::UUID> forbidden =
			FreshInstanceIdForbiddenSet(m_Authoring, registry, &registry);
		for (const auto& uuid : duplicateUuids)
			forbidden.insert(uuid);
		std::unordered_set<rt2::core::UUID> operationLocal;
		freshIdByOriginalId.reserve(copiedPrefabs.rootsByGroup.size());
		for (const auto& group : copiedPrefabs.rootsByGroup)
		{
			rt2::core::Error reserveErr;
			auto fresh = ReserveFreshInstanceId(
				[this] { return ReserveKnownUuid(); }, forbidden, operationLocal, reserveErr);
			if (!fresh)
			{
				return EditorMutationResult::Failure(reserveErr.code, reserveErr.path,
					reserveErr.detail);
			}
			freshIdByOriginalId.emplace(group.first, *fresh);
		}
	}

	std::unordered_map<entt::entity, entt::entity> remap;
	std::vector<std::pair<rt2::core::UUID, rt2::core::UUID>> sourceToDuplicate;
	std::vector<entt::entity> duplicates;
	sourceToDuplicate.reserve(sources.size());
	duplicates.reserve(sources.size());
	bool duplicatesRenderable = false;
	for (std::size_t i = 0; i < sources.size(); ++i)
	{
		const auto source = sources[i];
		const auto duplicate = registry.create();
		remap.emplace(source, duplicate);
		CopyAuthoredComponents(registry, source, registry, duplicate);
		const auto sourceUuid = GetEntityUuid({ source });
		if (!m_Authoring.AssignKnownUuid(duplicate, duplicateUuids[i]))
		{
			// Rollback (staged entity UUIDs are validated pre-staging, so this
			// is defensive): destroy everything created so far.
			for (const auto& [s, d] : remap)
			{
				if (const auto* idc = registry.try_get<EntityIdComponent>(d))
					m_Authoring.uuidIndex.Erase(idc->id);
				registry.destroy(d);
			}
			return EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
				duplicateUuids[i].ToString(),
				"DuplicateSubtrees: failed to assign staged duplicate UUID");
		}
		sourceToDuplicate.emplace_back(sourceUuid, duplicateUuids[i]);
		duplicates.push_back(duplicate);
		duplicatesRenderable = duplicatesRenderable || registry.all_of<MeshRef>(duplicate);
	}
	RemapCopiedScriptFields(sourceToDuplicate, duplicates, registry);

	// APPLY the pre-reserved plan: every copied root and member of a complete
	// instance group gets its group's single fresh instanceId; orphan member
	// fragments are stripped. Zero provider calls.
	ApplyCopiedPrefabLinks(registry, copiedPrefabs, remap, freshIdByOriginalId);

	for (const auto source : sources)
	{
		const auto duplicate = remap.at(source);
		const auto* sourceHierarchy = registry.try_get<Hierarchy>(source);
		if (!sourceHierarchy || sourceHierarchy->parent == entt::null)
			continue;
		const auto mappedParent = remap.find(sourceHierarchy->parent);
		const auto duplicateParent = mappedParent != remap.end()
			? mappedParent->second : sourceHierarchy->parent;
		registry.emplace<Hierarchy>(duplicate).parent = duplicateParent;
		auto* parentHierarchy = registry.try_get<Hierarchy>(duplicateParent);
		if (!parentHierarchy)
			parentHierarchy = &registry.emplace<Hierarchy>(duplicateParent);
		parentHierarchy->children.push_back(duplicate);
	}

	EditorMutationResult result;
	for (const auto root : roots)
	{
		const auto duplicate = remap.at(root);
		if (auto* name = registry.try_get<NameComponent>(duplicate))
			name->name += " Copy";
		result.affectedEntities.push_back(GetEntityUuid({ duplicate }));
		SceneGraph::MarkDirty(registry, duplicate);
	}
	if (const auto warning = MakeCopiedPrefabRecoveryWarning("duplicate", copiedPrefabs))
		result.recoveryWarning = *warning;
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	result.syncImpact = duplicatesRenderable
		? rt2::core::SyncImpact::Structural : rt2::core::SyncImpact::None;
	return result;
}

EditorMutationResult SceneManager::PasteSubtreesFrom(
	const rt2::core::SceneDocument& snapshot,
	const std::vector<rt2::core::UUID>& rootUuids,
	const std::optional<rt2::core::UUID>& parentUuid)
{
	if (rootUuids.empty()) return {};
	rt2::core::Error error;
	auto roots = ResolveCanonicalRoots(snapshot, rootUuids, error);
	if (!error.IsOk())
	{
		EditorMutationResult result;
		result.success = false;
		result.error = error;
		return result;
	}
	auto& destination = m_EcsScene.registry;
	entt::entity destinationParent = entt::null;
	if (parentUuid)
	{
		destinationParent = m_Authoring.FindByUuid(*parentUuid);
		if (destinationParent == entt::null || !destination.valid(destinationParent))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				parentUuid->ToString(), "paste parent is not present in the authoring scene");
	}

	std::vector<entt::entity> sources;
	for (const auto root : roots)
		SceneHierarchy::CollectSubtreePreOrder(snapshot.ecs.registry, root, sources);
	for (const auto source : sources)
	{
		if (const auto* mesh = snapshot.ecs.registry.try_get<MeshRef>(source))
		{
			if (mesh->meshIndex >= m_EcsScene.meshRegistry.GetCount())
				return EditorMutationResult::Failure(rt2::core::Error::ClipboardStale,
					GetEntityUuid({ destinationParent }).ToString(),
					"clipboard mesh resources no longer match this document");
			if (mesh->materialIndex >= static_cast<int>(m_EcsScene.materials.size()))
				return EditorMutationResult::Failure(rt2::core::Error::ClipboardStale,
					{}, "clipboard material resources no longer match this document");
		}
	}

	// Phase 8 W3, S4 (review fix 2): PLAN the copied-forest classification from
	// the SOURCE (clipboard) forest BEFORE any destination mutation, then
	// reserve every fresh instanceId. A paste is a NEW instance, never a
	// verbatim share of the clipboard's instanceId (W3-D8).
	const CopiedPrefabPlan copiedPrefabs = PlanCopiedPrefabLinks(snapshot.ecs.registry, roots);
	std::unordered_map<rt2::core::UUID, rt2::core::UUID> freshIdByOriginalId;

	// Stage the whole paste's entity UUIDs first. Provider-consumption order is
	// deterministic and pinned by the fixup tests: every entity UUID
	// (sources.size() draws) first, then one fresh instanceId per complete
	// instance group. The create loop below consumes no provider.
	// Every staged UUID is validated BEFORE any destination mutation — nil,
	// already indexed in the authoring document, equal to a SOURCE entity's id
	// (the clipboard is a DISTINCT document, so a pasted entity must never
	// adopt the identity it was copied from), or repeated within this
	// operation — so a hostile provider trips a stage-specific DuplicateUuid
	// here, never midway through the create loop.
	std::vector<rt2::core::UUID> pastedUuids;
	{
		const std::unordered_set<rt2::core::UUID> forbidden =
			EntityUuidForbiddenSet(m_Authoring, destination, &snapshot.ecs.registry);
		std::unordered_set<rt2::core::UUID> operationLocal;
		rt2::core::Error reserveErr;
		if (!ReserveValidEntityUuids([this] { return ReserveKnownUuid(); }, forbidden,
			operationLocal, sources.size(), "PasteSubtreesFrom", pastedUuids, reserveErr))
		{
			return EditorMutationResult::Failure(reserveErr.code, reserveErr.path,
				reserveErr.detail);
		}
	}

	// Reserve ONE fresh instanceId per complete group against the authoring
	// uuidIndex + live destination PIC/PMIC instanceIds + the clipboard's live
	// PIC/PMIC instanceIds (the source is a DISTINCT document) + the staged
	// entity UUIDs. Zero provider calls after this point.
	{
		std::unordered_set<rt2::core::UUID> forbidden =
			FreshInstanceIdForbiddenSet(m_Authoring, destination, &snapshot.ecs.registry);
		for (const auto& uuid : pastedUuids)
			forbidden.insert(uuid);
		std::unordered_set<rt2::core::UUID> operationLocal;
		freshIdByOriginalId.reserve(copiedPrefabs.rootsByGroup.size());
		for (const auto& group : copiedPrefabs.rootsByGroup)
		{
			rt2::core::Error reserveErr;
			auto fresh = ReserveFreshInstanceId(
				[this] { return ReserveKnownUuid(); }, forbidden, operationLocal, reserveErr);
			if (!fresh)
			{
				return EditorMutationResult::Failure(reserveErr.code, reserveErr.path,
					reserveErr.detail);
			}
			freshIdByOriginalId.emplace(group.first, *fresh);
		}
	}

	std::unordered_map<entt::entity, entt::entity> remap;
	std::vector<std::pair<rt2::core::UUID, rt2::core::UUID>> sourceToPaste;
	std::vector<entt::entity> pastes;
	sourceToPaste.reserve(sources.size());
	pastes.reserve(sources.size());
	bool pastesRenderable = false;
	for (std::size_t i = 0; i < sources.size(); ++i)
	{
		const auto source = sources[i];
		const auto pasted = destination.create();
		remap.emplace(source, pasted);
		CopyAuthoredComponents(snapshot.ecs.registry, source, destination, pasted);
		const auto* sourceIdc = snapshot.ecs.registry.try_get<EntityIdComponent>(source);
		if (!m_Authoring.AssignKnownUuid(pasted, pastedUuids[i]))
		{
			// Rollback (staged entity UUIDs are validated pre-staging, so this
			// is defensive): destroy everything created so far.
			for (const auto& [s, d] : remap)
			{
				if (const auto* idc = destination.try_get<EntityIdComponent>(d))
					m_Authoring.uuidIndex.Erase(idc->id);
				destination.destroy(d);
			}
			return EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
				pastedUuids[i].ToString(),
				"PasteSubtreesFrom: failed to assign staged paste UUID");
		}
		sourceToPaste.emplace_back(sourceIdc ? sourceIdc->id : rt2::core::UUID{}, pastedUuids[i]);
		pastes.push_back(pasted);
		pastesRenderable = pastesRenderable || destination.all_of<MeshRef>(pasted);
	}
	RemapCopiedScriptFields(sourceToPaste, pastes, destination);

	// APPLY the pre-reserved plan. Zero provider calls.
	ApplyCopiedPrefabLinks(destination, copiedPrefabs, remap, freshIdByOriginalId);

	for (const auto source : sources)
	{
		const auto pasted = remap.at(source);
		const auto* sourceHierarchy = snapshot.ecs.registry.try_get<Hierarchy>(source);
		entt::entity pastedParent = destinationParent;
		if (sourceHierarchy)
		{
			const auto mappedParent = remap.find(sourceHierarchy->parent);
			if (mappedParent != remap.end())
				pastedParent = mappedParent->second;
		}
		if (pastedParent == entt::null)
			continue;
		destination.emplace<Hierarchy>(pasted).parent = pastedParent;
		auto* parentHierarchy = destination.try_get<Hierarchy>(pastedParent);
		if (!parentHierarchy)
			parentHierarchy = &destination.emplace<Hierarchy>(pastedParent);
		parentHierarchy->children.push_back(pasted);
	}

	EditorMutationResult result;
	for (const auto root : roots)
	{
		const auto pasted = remap.at(root);
		if (auto* name = destination.try_get<NameComponent>(pasted))
			name->name += " Copy";
		result.affectedEntities.push_back(GetEntityUuid({ pasted }));
		SceneGraph::MarkDirty(destination, pasted);
	}
	if (const auto warning = MakeCopiedPrefabRecoveryWarning("paste", copiedPrefabs))
		result.recoveryWarning = *warning;
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	result.syncImpact = pastesRenderable
		? rt2::core::SyncImpact::Structural : rt2::core::SyncImpact::None;
	return result;
}

// ============================================================================
// Phase 3B1 structural command APIs
// ============================================================================

namespace
{

// Build a SubtreeEntityRecord from a live entity. Reads authored component
// state only — never derived world matrices, GPU caches, or other transient
// state. This is the snapshot-side mirror of the serializer's
// BuildEntityRecord, kept in SceneManager.cpp so the structural command
// APIs and the serializer stay aligned by construction (a mismatch would
// cause Undo/Redo to restore different state than what was captured).
SubtreeEntityRecord BuildSubtreeRecord(const entt::registry& reg, entt::entity e)
{
	SubtreeEntityRecord r;
	rt2::core::UUID uuid;
	if (const auto* idc = reg.try_get<EntityIdComponent>(e))
		uuid = idc->id;
	r.uuid = uuid;

	if (const auto* nc = reg.try_get<NameComponent>(e))
		r.name = nc->name;

	r.parentUuid = rt2::core::UUID::Nil();
	if (const auto* h = reg.try_get<Hierarchy>(e))
	{
		if (h->parent != entt::null && reg.valid(h->parent))
		{
			if (const auto* pidc = reg.try_get<EntityIdComponent>(h->parent))
				r.parentUuid = pidc->id;
		}
	}

	if (const auto* tf = reg.try_get<Transform>(e))
	{
		r.translation = tf->translation;
		r.rotation    = tf->rotation;
		r.scale       = tf->scale;
	}

	if (const auto* vc = reg.try_get<VisibleComponent>(e))
		r.visible = vc->visible;

	if (const auto* ref = reg.try_get<MeshRef>(e))
	{
		r.hasMeshRef    = true;
		r.meshIndex     = ref->meshIndex;
		r.materialIndex = ref->materialIndex;
	}

	if (const auto* pc = reg.try_get<PrimitiveComponent>(e))
	{
		r.hasPrimitive = true;
		r.primitive    = *pc;
	}

	if (const auto* isrc = reg.try_get<ImportedMeshSourceComponent>(e))
	{
		r.hasImportedSource = true;
		r.importedSource    = *isrc;
	}

	if (const auto* mov = reg.try_get<MaterialOverrideComponent>(e))
	{
		r.hasMaterialOverride = true;
		r.materialOverride    = *mov;
	}

	if (const auto* lc = reg.try_get<LightComponent>(e))
	{
		r.hasLight = true;
		r.light    = *lc;
	}

	if (const auto* cc = reg.try_get<CameraComponent>(e))
	{
		r.hasCamera = true;
		r.camera    = *cc;
	}

	if (const auto* mc = reg.try_get<MotionComponent>(e))
	{
		r.hasMotion = true;
		r.motion    = *mc;
	}

	if (const auto* sc = reg.try_get<ScriptComponent>(e))
	{
		r.hasScript = true;
		r.script    = *sc;
	}

	// Phase 8 W1: prefab instance link components (scene-side).
	if (const auto* pic = reg.try_get<PrefabInstanceComponent>(e))
	{
		r.hasPrefabInstance = true;
		r.prefabInstance    = *pic;
	}

	if (const auto* pmc = reg.try_get<PrefabMemberComponent>(e))
	{
		r.hasPrefabMember = true;
		r.prefabMember    = *pmc;
	}

	return r;
}

// Restore an entity's authored component state from a SubtreeEntityRecord.
// Emplaces/overwrites every persisted component the record carries. Does NOT
// touch derived world matrices, GPU caches, or other transient state — those
// are recomputed by SceneGraph after restoration.
void ApplySubtreeRecord(const SubtreeEntityRecord& record, entt::registry& reg,
                        entt::entity e)
{
	if (!record.name.empty())
		reg.emplace_or_replace<NameComponent>(e, NameComponent{record.name});

	if (auto* tf = reg.try_get<Transform>(e))
	{
		tf->translation = record.translation;
		tf->rotation    = glm::normalize(record.rotation);
		tf->scale       = record.scale;
		tf->dirty       = true;
	}
	else
	{
		Transform fresh;
		fresh.translation = record.translation;
		fresh.rotation    = record.rotation;
		fresh.scale       = record.scale;
		reg.emplace<Transform>(e, fresh);
	}

	reg.emplace_or_replace<VisibleComponent>(e, VisibleComponent{record.visible});

	if (record.hasMeshRef)
		reg.emplace_or_replace<MeshRef>(e, MeshRef{record.meshIndex, record.materialIndex});
	else
		reg.remove<MeshRef>(e);

	if (record.hasPrimitive)
		reg.emplace_or_replace<PrimitiveComponent>(e, record.primitive);
	else
		reg.remove<PrimitiveComponent>(e);

	if (record.hasImportedSource)
		reg.emplace_or_replace<ImportedMeshSourceComponent>(e, record.importedSource);
	else
		reg.remove<ImportedMeshSourceComponent>(e);

	if (record.hasMaterialOverride)
		reg.emplace_or_replace<MaterialOverrideComponent>(e, record.materialOverride);
	else
		reg.remove<MaterialOverrideComponent>(e);

	if (record.hasLight)
		reg.emplace_or_replace<LightComponent>(e, record.light);
	else
		reg.remove<LightComponent>(e);

	if (record.hasCamera)
		reg.emplace_or_replace<CameraComponent>(e, record.camera);
	else
		reg.remove<CameraComponent>(e);

	if (record.hasMotion)
		reg.emplace_or_replace<MotionComponent>(e, record.motion);
	else
		reg.remove<MotionComponent>(e);

	if (record.hasScript)
		reg.emplace_or_replace<ScriptComponent>(e, record.script);
	else
		reg.remove<ScriptComponent>(e);

	// Phase 8 W1: prefab instance link components (scene-side).
	if (record.hasPrefabInstance)
		reg.emplace_or_replace<PrefabInstanceComponent>(e, record.prefabInstance);
	else
		reg.remove<PrefabInstanceComponent>(e);

	if (record.hasPrefabMember)
		reg.emplace_or_replace<PrefabMemberComponent>(e, record.prefabMember);
	else
		reg.remove<PrefabMemberComponent>(e);
}

// Compare authored component state on an entity against a record. Returns
// true if every persisted component matches exactly. Transient state
// (worldMatrix, prevWorldMatrix, dirty, selection, clipboard) is NOT
// compared — only authoritative authored state.
bool EntityMatchesRecord(const entt::registry& reg, entt::entity e,
                         const SubtreeEntityRecord& record)
{
	if (const auto* idc = reg.try_get<EntityIdComponent>(e))
	{
		if (!(idc->id == record.uuid)) return false;
	}
	else if (!record.uuid.IsNull()) return false;

	if (const auto* nc = reg.try_get<NameComponent>(e))
	{
		if (nc->name != record.name) return false;
	}
	else if (!record.name.empty()) return false;

	// Parent UUID
	rt2::core::UUID liveParent = rt2::core::UUID::Nil();
	if (const auto* h = reg.try_get<Hierarchy>(e))
	{
		if (h->parent != entt::null && reg.valid(h->parent))
		{
			if (const auto* pidc = reg.try_get<EntityIdComponent>(h->parent))
				liveParent = pidc->id;
		}
	}
	if (!(liveParent == record.parentUuid)) return false;

	if (const auto* tf = reg.try_get<Transform>(e))
	{
		constexpr float eps = 1e-5f;
		auto vEq = [eps](const glm::vec3& a, const glm::vec3& b) {
			return std::fabs(a.x - b.x) <= eps &&
			       std::fabs(a.y - b.y) <= eps &&
			       std::fabs(a.z - b.z) <= eps;
		};
		auto qEq = [eps](const glm::quat& a, const glm::quat& b) {
			glm::quat na = a; if (na.w < 0.0f) na = -na;
			glm::quat nb = b; if (nb.w < 0.0f) nb = -nb;
			return std::fabs(na.x - nb.x) <= eps &&
			       std::fabs(na.y - nb.y) <= eps &&
			       std::fabs(na.z - nb.z) <= eps &&
			       std::fabs(na.w - nb.w) <= eps;
		};
		if (!vEq(tf->translation, record.translation) ||
		    !qEq(tf->rotation, record.rotation) ||
		    !vEq(tf->scale, record.scale))
			return false;
	}
	else
	{
		// record always carries a TRS; if the live entity has no Transform,
		// it cannot match unless the record's TRS is identity — but a
		// structural command snapshot always carries a Transform, so treat
		// absence as a mismatch.
		return false;
	}

	bool liveVisible = true;
	if (const auto* vc = reg.try_get<VisibleComponent>(e))
		liveVisible = vc->visible;
	if (liveVisible != record.visible) return false;

	auto checkRef = [&](bool has, const MeshRef* ref) {
		if (has != record.hasMeshRef) return false;
		if (has && ref &&
		    (ref->meshIndex != record.meshIndex ||
		     ref->materialIndex != record.materialIndex))
			return false;
		return true;
	};
	if (!checkRef(reg.all_of<MeshRef>(e), reg.try_get<MeshRef>(e))) return false;

	// Per-component exact compare. Each persisted component is plain data;
	// we compare fields explicitly because PrimitiveComponent,
	// AssetReference, and SceneMaterial do not define operator==.
	if (reg.all_of<PrimitiveComponent>(e) != record.hasPrimitive) return false;
	if (record.hasPrimitive)
	{
		const auto& live = *reg.try_get<PrimitiveComponent>(e);
		if (live.kind != record.primitive.kind ||
		    std::fabs(live.size - record.primitive.size) > 1e-5f ||
		    live.segments != record.primitive.segments ||
		    live.rings != record.primitive.rings)
			return false;
	}

	if (reg.all_of<ImportedMeshSourceComponent>(e) != record.hasImportedSource) return false;
	if (record.hasImportedSource)
	{
		const auto& live = *reg.try_get<ImportedMeshSourceComponent>(e);
		if (!(live.model.kind == record.importedSource.model.kind &&
		      live.model.path == record.importedSource.model.path &&
		      live.model.sourceKey == record.importedSource.model.sourceKey &&
		      live.model.importSettings == record.importedSource.model.importSettings))
			return false;
	}

	if (reg.all_of<MaterialOverrideComponent>(e) != record.hasMaterialOverride) return false;
	if (record.hasMaterialOverride)
	{
		const auto& live = *reg.try_get<MaterialOverrideComponent>(e);
		constexpr float eps = 1e-5f;
		if (live.authored != record.materialOverride.authored) return false;
		if (live.sourceMaterialKey != record.materialOverride.sourceMaterialKey) return false;
		const auto& a = live.material;
		const auto& b = record.materialOverride.material;
		if (a.type != b.type) return false;
		if (glm::length(a.baseColor - b.baseColor) > eps) return false;
		if (std::fabs(a.baseAlpha - b.baseAlpha) > eps) return false;
		if (std::fabs(a.metallic - b.metallic) > eps) return false;
		if (std::fabs(a.roughness - b.roughness) > eps) return false;
		if (std::fabs(a.ior - b.ior) > eps) return false;
		if (std::fabs(a.transmissionFactor - b.transmissionFactor) > eps) return false;
		if (glm::length(a.emissiveColor - b.emissiveColor) > eps) return false;
		if (std::fabs(a.emissiveIntensity - b.emissiveIntensity) > eps) return false;
		if (a.baseColorTextureIndex != b.baseColorTextureIndex) return false;
		if (a.normalTextureIndex != b.normalTextureIndex) return false;
		if (a.emissiveTextureIndex != b.emissiveTextureIndex) return false;
		if (a.metallicRoughnessTextureIndex != b.metallicRoughnessTextureIndex) return false;
		if (a.alphaMode != b.alphaMode) return false;
		if (std::fabs(a.alphaCutoff - b.alphaCutoff) > eps) return false;
	}

	if (reg.all_of<LightComponent>(e) != record.hasLight) return false;
	if (record.hasLight)
	{
		const auto& live = *reg.try_get<LightComponent>(e);
		constexpr float eps = 1e-5f;
		if (glm::length(live.color - record.light.color) > eps) return false;
		if (std::fabs(live.intensity - record.light.intensity) > eps) return false;
		if (std::fabs(live.range - record.light.range) > eps) return false;
		if (std::fabs(live.innerConeAngle - record.light.innerConeAngle) > eps) return false;
		if (std::fabs(live.outerConeAngle - record.light.outerConeAngle) > eps) return false;
		if (live.type != record.light.type) return false;
	}

	if (reg.all_of<CameraComponent>(e) != record.hasCamera) return false;
	if (record.hasCamera)
	{
		const auto& live = *reg.try_get<CameraComponent>(e);
		constexpr float eps = 1e-5f;
		if (std::fabs(live.verticalFOV - record.camera.verticalFOV) > eps) return false;
		if (std::fabs(live.aperture - record.camera.aperture) > eps) return false;
		if (std::fabs(live.focusDistance - record.camera.focusDistance) > eps) return false;
		if (glm::length(live.forwardDirection - record.camera.forwardDirection) > eps) return false;
	}

	if (reg.all_of<MotionComponent>(e) != record.hasMotion) return false;
	if (record.hasMotion)
	{
		const auto& live = *reg.try_get<MotionComponent>(e);
		constexpr float eps = 1e-5f;
		if (glm::length(live.linearVelocity - record.motion.linearVelocity) > eps) return false;
	}

	// Phase 6: script component comparison. The asset reference + field
	// values must match exactly for a record to be considered consistent.
	// Field-value comparison is by structural equality (the variant and the
	// map both define operator==).
	if (reg.all_of<ScriptComponent>(e) != record.hasScript) return false;
	if (record.hasScript)
	{
		const auto& live = *reg.try_get<ScriptComponent>(e);
		if (!(live.asset.path == record.script.asset.path)) return false;
		if (live.asset.kind != record.script.asset.kind) return false;
		if (live.fieldValues.size() != record.script.fieldValues.size()) return false;
		for (const auto& [k, v] : record.script.fieldValues)
		{
			auto it = live.fieldValues.find(k);
			if (it == live.fieldValues.end()) return false;
			if (!(it->second == v)) return false;
		}
	}

	// Phase 8 W1: prefab instance link components. Exact compare — the link
	// is authored state that Undo/Redo must restore verbatim.
	if (reg.all_of<PrefabInstanceComponent>(e) != record.hasPrefabInstance) return false;
	if (record.hasPrefabInstance)
	{
		const auto& live = *reg.try_get<PrefabInstanceComponent>(e);
		if (!(live.instanceId == record.prefabInstance.instanceId)) return false;
		if (!(live.prefab.kind == record.prefabInstance.prefab.kind &&
		      live.prefab.path == record.prefabInstance.prefab.path &&
		      live.prefab.sourceKey == record.prefabInstance.prefab.sourceKey &&
		      live.prefab.assetId == record.prefabInstance.prefab.assetId))
			return false;
	}

	if (reg.all_of<PrefabMemberComponent>(e) != record.hasPrefabMember) return false;
	if (record.hasPrefabMember)
	{
		const auto& live = *reg.try_get<PrefabMemberComponent>(e);
		if (!(live.instanceId == record.prefabMember.instanceId)) return false;
		if (!(live.templateId == record.prefabMember.templateId)) return false;

		// Phase 8 W3, S3: the override set is authored state the verifier must
		// see, or a post-copy edit to a duplicate's overrides is invisible to
		// RemoveSubtreesExact and Undo destroys the edited copy.
		//
		// Compare the vectors as MULTISETS: sorted copies compared element-wise.
		// This is order-insensitive AND duplicate-correct. Neither the codec's
		// sortedness nor its uniqueness invariant is trusted here, because both
		// hold only for vectors that have passed through a file round-trip — an
		// in-memory vector need not have (the S5/S6 marking path records in edit
		// order; S2's write-sort test deliberately builds one unsorted). A
		// one-directional containment would be fooled by duplicates: size-gate
		// then "every key in live is in record" accepts `{transform, transform}`
		// (live) against `{transform, light}` (record) as equal when they are
		// not. Sorting both sides and comparing position-by-position catches that
		// (and the symmetric `{a,a,b}` vs `{a,b,b}` case, which even bidirectional
		// containment + equal size misses because counts can differ while coverage
		// is identical). No product code materialises a duplicate today — the
		// codec de-duplicates on read and S5/S6 do not exist yet — but S5 starts
		// building these vectors in memory and makes this guard load-bearing, so
		// the compare must not trust uniqueness.
		if (live.overrides.size() != record.prefabMember.overrides.size()) return false;
		std::vector<PrefabComponentKey> liveSorted = live.overrides;
		std::vector<PrefabComponentKey> recSorted = record.prefabMember.overrides;
		std::sort(liveSorted.begin(), liveSorted.end(),
		          [](const PrefabComponentKey& a, const PrefabComponentKey& b)
		          { return a.wire() < b.wire(); });
		std::sort(recSorted.begin(), recSorted.end(),
		          [](const PrefabComponentKey& a, const PrefabComponentKey& b)
		          { return a.wire() < b.wire(); });
		for (std::size_t i = 0; i < liveSorted.size(); ++i)
			if (!(liveSorted[i] == recSorted[i])) return false;
	}

	return true;
}

// Read the sibling anchor for a root entity: the prev/next sibling UUID
// among the parent's children (or the root-entity list when parent is null),
// plus the child index for diagnostic cross-check.
RootSiblingAnchor BuildSiblingAnchor(const entt::registry& reg, entt::entity root)
{
	RootSiblingAnchor anchor;
	entt::entity parent = entt::null;
	if (const auto* h = reg.try_get<Hierarchy>(root))
		parent = h->parent;

	std::vector<entt::entity> siblings;
	if (parent != entt::null)
	{
		if (const auto* ph = reg.try_get<Hierarchy>(parent))
			siblings = ph->children;
	}
	else
	{
		// Root entities: registry iteration order (unspecified).
		auto view = reg.view<EntityIdComponent>();
		for (auto e : view)
		{
			const auto* h = reg.try_get<Hierarchy>(e);
			if (!h || h->parent == entt::null)
				siblings.push_back(e);
		}
	}

	for (std::size_t i = 0; i < siblings.size(); ++i)
	{
		if (siblings[i] == root)
		{
			anchor.childIndex = i;
			if (i > 0)
			{
				if (const auto* idc = reg.try_get<EntityIdComponent>(siblings[i - 1]))
					anchor.prevSibling = idc->id;
			}
			if (i + 1 < siblings.size())
			{
				if (const auto* idc = reg.try_get<EntityIdComponent>(siblings[i + 1]))
					anchor.nextSibling = idc->id;
			}
			break;
		}
	}
	return anchor;
}

// Validate a root's anchor against the current parent's children list (or
// the root-entity list). Returns true if the anchor points to a position
// the root can be restored to consistently. The root itself need not be
// present (it was just removed).
// Validate a root's anchor against the current parent's children list.
// Returns true if the anchor points to a position the root can be restored
// to consistently. The root itself need not be present (it was just
// removed). Only called for parented roots — nil-parent roots skip anchor
// validation (root ordering is unspecified).
bool AnchorIsConsistent(const entt::registry& reg, const rt2::core::UUID& rootUuid,
                        const rt2::core::UUID& parentUuid, const RootSiblingAnchor& anchor)
{
	std::vector<entt::entity> siblings;
	// Find the parent entity by UUID.
	entt::entity found = entt::null;
	auto view = reg.view<EntityIdComponent>();
	for (auto e : view)
	{
		if (const auto* idc = reg.try_get<EntityIdComponent>(e);
		    idc && idc->id == parentUuid)
		{
			found = e;
			break;
		}
	}
	if (found == entt::null) return false;
	if (const auto* ph = reg.try_get<Hierarchy>(found))
		siblings = ph->children;

	// Convert siblings to UUIDs.
	std::vector<rt2::core::UUID> siblingUuids;
	siblingUuids.reserve(siblings.size());
	for (auto s : siblings)
	{
		if (const auto* idc = reg.try_get<EntityIdComponent>(s))
			siblingUuids.push_back(idc->id);
	}

	// Find the insertion position: prevSibling must appear immediately
	// before the gap, nextSibling immediately after.
	if (anchor.prevSibling.IsNull() && anchor.nextSibling.IsNull())
	{
		// First-and-last: only valid if the list is empty (the root was the
		// only child/root).
		return siblingUuids.empty();
	}

	if (anchor.prevSibling.IsNull())
	{
		// Root was the first child; nextSibling must now be the first.
		if (siblingUuids.empty()) return false;
		return siblingUuids.front() == anchor.nextSibling;
	}

	if (anchor.nextSibling.IsNull())
	{
		// Root was the last child; prevSibling must now be the last.
		if (siblingUuids.empty()) return false;
		return siblingUuids.back() == anchor.prevSibling;
	}

	// Middle: prevSibling and nextSibling must be adjacent in the current
	// list (the root fit between them).
	for (std::size_t i = 0; i + 1 < siblingUuids.size(); ++i)
	{
		if (siblingUuids[i] == anchor.prevSibling &&
		    siblingUuids[i + 1] == anchor.nextSibling)
			return true;
	}
	return false;
}

} // namespace

EditorMutationResult SceneManager::RemoveSubtreesNoCompact(
	const std::vector<rt2::core::UUID>& rootUuids)
{
	if (rootUuids.empty()) return {};
	rt2::core::Error error;
	auto roots = ResolveCanonicalRoots(m_Authoring, rootUuids, error);
	if (!error.IsOk())
	{
		EditorMutationResult result;
		result.success = false;
		result.error = error;
		return result;
	}
	auto& registry = m_EcsScene.registry;
	std::vector<entt::entity> postOrder;
	bool removesRenderable = false;
	EditorMutationResult result;
	for (const auto root : roots)
	{
		std::vector<entt::entity> subtree;
		SceneHierarchy::CollectSubtreePostOrder(registry, root, subtree);
		for (const auto entity : subtree)
		{
			postOrder.push_back(entity);
			removesRenderable = removesRenderable || registry.all_of<MeshRef>(entity);
			if (const auto* identity = registry.try_get<EntityIdComponent>(entity))
				result.affectedEntities.push_back(identity->id);
		}
		if (const auto* hierarchy = registry.try_get<Hierarchy>(root))
			RemoveChild(registry, hierarchy->parent, root);
	}
	for (const auto entity : postOrder)
	{
		if (const auto* identity = registry.try_get<EntityIdComponent>(entity))
			m_Authoring.uuidIndex.Erase(identity->id);
		registry.destroy(entity);
	}
	// NO CompactMeshRegistry() — Phase 3B1 invariant.
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	result.syncImpact = removesRenderable
		? rt2::core::SyncImpact::Structural : rt2::core::SyncImpact::None;
	return result;
}

EditorMutationResult SceneManager::RemoveSubtreesExact(const SubtreeSnapshot& snapshot)
{
	auto& registry = m_EcsScene.registry;

	// Phase 1: validate every expected UUID exists and authored state
	// matches the snapshot. Any mismatch => zero mutation, Failure.
	std::vector<entt::entity> toDestroy;
	toDestroy.reserve(snapshot.entities.size());
	for (const auto& record : snapshot.entities)
	{
		const auto entity = m_Authoring.FindByUuid(record.uuid);
		if (entity == entt::null || !registry.valid(entity))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				record.uuid.ToString(),
				"RemoveSubtreesExact: expected entity is not present in the scene");
		if (!EntityMatchesRecord(registry, entity, record))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				record.uuid.ToString(),
				"RemoveSubtreesExact: authored state does not match the snapshot");
		toDestroy.push_back(entity);
	}

	// Phase 2: validate no unexpected descendants. Every entity that is a
	// descendant of a snapshot root must appear in the snapshot. This
	// catches out-of-band edits that added children after the snapshot was
	// captured.
	std::unordered_set<rt2::core::UUID> snapshotUuids;
	for (const auto& record : snapshot.entities)
		snapshotUuids.insert(record.uuid);

	std::vector<entt::entity> roots;
	roots.reserve(snapshot.rootUuids.size());
	for (const auto& rootUuid : snapshot.rootUuids)
	{
		const auto root = m_Authoring.FindByUuid(rootUuid);
		if (root == entt::null || !registry.valid(root))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				rootUuid.ToString(),
				"RemoveSubtreesExact: snapshot root is not present in the scene");
		roots.push_back(root);
	}

	for (const auto root : roots)
	{
		std::vector<entt::entity> subtree;
		SceneHierarchy::CollectSubtreePostOrder(registry, root, subtree);
		for (const auto entity : subtree)
		{
			const auto* idc = registry.try_get<EntityIdComponent>(entity);
			if (!idc || snapshotUuids.find(idc->id) == snapshotUuids.end())
				return EditorMutationResult::Failure(rt2::core::Error::InvalidHierarchy,
					idc ? idc->id.ToString() : std::string{},
					"RemoveSubtreesExact: subtree contains an entity not in the snapshot");
		}
	}

	// Phase 3: all validation passed. Remove without compaction.
	EditorMutationResult result;
	bool removesRenderable = false;
	for (const auto root : roots)
	{
		std::vector<entt::entity> subtree;
		SceneHierarchy::CollectSubtreePostOrder(registry, root, subtree);
		for (const auto entity : subtree)
		{
			removesRenderable = removesRenderable || registry.all_of<MeshRef>(entity);
			if (const auto* identity = registry.try_get<EntityIdComponent>(entity))
			{
				result.affectedEntities.push_back(identity->id);
				m_Authoring.uuidIndex.Erase(identity->id);
			}
		}
		if (const auto* hierarchy = registry.try_get<Hierarchy>(root))
			RemoveChild(registry, hierarchy->parent, root);
	}
	for (const auto entity : toDestroy)
		registry.destroy(entity);

	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	result.syncImpact = removesRenderable
		? rt2::core::SyncImpact::Structural : rt2::core::SyncImpact::None;
	return result;
}

EditorMutationResult SceneManager::RestoreSubtrees(const SubtreeSnapshot& snapshot)
{
	auto& registry = m_EcsScene.registry;

	// Phase 1: validate every stored UUID is absent from the document
	// (Undo of a creation) or present with matching authored state (Undo of
	// a deletion). For Undo-of-creation, the entities were just removed by
	// the command's Execute; for Undo-of-deletion, the entities are still
	// absent and we re-create them. The anchor check happens in phase 2.
	for (const auto& record : snapshot.entities)
	{
		if (m_Authoring.uuidIndex.Contains(record.uuid))
		{
			// Entity already exists — this must be a no-op-safe restore
			// (the snapshot matches live state). Treat as success without
			// re-mutating to keep Redo idempotent when the entity is
			// already present.
			const auto existing = m_Authoring.FindByUuid(record.uuid);
			if (existing == entt::null || !registry.valid(existing))
				return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
					record.uuid.ToString(),
					"RestoreSubtrees: UUID index inconsistent with registry");
			if (!EntityMatchesRecord(registry, existing, record))
				return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
					record.uuid.ToString(),
					"RestoreSubtrees: existing entity does not match the snapshot");
		}
	}

	// Phase 2: validate root sibling anchors against the current parent's
	// children list. An inconsistent anchor fails atomically — restoration
	// never silently appends. Per the spec, root-entity ordering is
	// unspecified (registry-iteration order, no explicit authored ordering
	// vector); anchor validation is skipped for nil-parent roots (the
	// root-entity list is not a stable ordering authority) and kept
	// strict for parented roots (the parent's children list is authored
	// state).
	for (std::size_t i = 0; i < snapshot.rootUuids.size(); ++i)
	{
		const auto& rootUuid = snapshot.rootUuids[i];
		const auto& anchor = snapshot.rootAnchors[i];
		// Find the root's parent UUID from the record.
		rt2::core::UUID parentUuid;
		for (const auto& record : snapshot.entities)
		{
			if (record.uuid == rootUuid)
			{
				parentUuid = record.parentUuid;
				break;
			}
		}
		// If the root is already present, the anchor was already validated
		// by EntityMatchesRecord above (parent UUID matches). Skip the
		// anchor check in that case.
		if (m_Authoring.uuidIndex.Contains(rootUuid)) continue;
		// Skip anchor validation for nil-parent roots: root-entity
		// ordering is unspecified and the entt pool iteration order is not
		// a stable authority (it changes on every destroy/create via
		// swap-and-pop). Validating against it would cause legitimate Undo
		// to fail nondeterministically.
		if (parentUuid.IsNull()) continue;
		if (!AnchorIsConsistent(registry, rootUuid, parentUuid, anchor))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidHierarchy,
				rootUuid.ToString(),
				"RestoreSubtrees: sibling anchor is inconsistent with the current parent's children");
	}

	// Phase 3: create entities in pre-order (parents before children) so
	// Hierarchy wiring resolves. Assign known UUIDs.
	bool addsRenderable = false;
	EditorMutationResult result;
	std::unordered_map<rt2::core::UUID, entt::entity> created;
	std::unordered_set<rt2::core::UUID> newlyCreated;
	for (const auto& record : snapshot.entities)
	{
		if (m_Authoring.uuidIndex.Contains(record.uuid))
		{
			// Already present and matches — skip (idempotent Redo).
			created[record.uuid] = m_Authoring.FindByUuid(record.uuid);
			continue;
		}
		const auto entity = registry.create();
		if (!m_Authoring.AssignKnownUuid(entity, record.uuid))
		{
			// Rollback: destroy everything we created so far.
			for (const auto& [uuid, e] : created)
			{
				if (const auto* idc = registry.try_get<EntityIdComponent>(e))
					m_Authoring.uuidIndex.Erase(idc->id);
				registry.destroy(e);
			}
			return EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
				record.uuid.ToString(),
				"RestoreSubtrees: failed to assign known UUID");
		}
		ApplySubtreeRecord(record, registry, entity);
		// Reset derived transform state — recomputed by SceneGraph.
		if (auto* tf = registry.try_get<Transform>(entity))
		{
			tf->worldMatrix = glm::mat4(1.0f);
			tf->prevWorldMatrix = glm::mat4(1.0f);
			tf->dirty = true;
		}
		addsRenderable = addsRenderable || registry.all_of<MeshRef>(entity);
		created[record.uuid] = entity;
		newlyCreated.insert(record.uuid);
		result.affectedEntities.push_back(record.uuid);
	}

	// Phase 4: wire Hierarchy. Each entity's parentUuid points to either
	// nil (root) or another entity in the snapshot. Insert the entity at
	// the anchored sibling position. Skip already-present entities (the
	// idempotent path) to avoid double-inserting into the parent's children
	// list.
	for (const auto& record : snapshot.entities)
	{
		// If the entity was already present (not newly created), its
		// Hierarchy wiring is already correct — skip to avoid corruption.
		if (newlyCreated.find(record.uuid) == newlyCreated.end())
			continue;
		const auto entity = created[record.uuid];
		if (record.parentUuid.IsNull())
		{
			// Root entity — no Hierarchy parent, but may gain a Hierarchy
			// component if it has children. Skip; children wire it.
			continue;
		}
		const auto parentIt = created.find(record.parentUuid);
		const auto parent = parentIt != created.end()
			? parentIt->second : m_Authoring.FindByUuid(record.parentUuid);
		if (parent == entt::null || !registry.valid(parent))
		{
			// Parent not in snapshot and not in document — rollback.
			for (const auto& [uuid, e] : created)
			{
				if (const auto* idc = registry.try_get<EntityIdComponent>(e))
					m_Authoring.uuidIndex.Erase(idc->id);
				registry.destroy(e);
			}
			return EditorMutationResult::Failure(rt2::core::Error::MissingParent,
				record.parentUuid.ToString(),
				"RestoreSubtrees: parent UUID is not present");
		}
		auto* hierarchy = registry.try_get<Hierarchy>(entity);
		if (!hierarchy)
			hierarchy = &registry.emplace<Hierarchy>(entity);
		hierarchy->parent = parent;
		auto* parentHierarchy = registry.try_get<Hierarchy>(parent);
		if (!parentHierarchy)
			parentHierarchy = &registry.emplace<Hierarchy>(parent);
		// Find the anchored position. The anchor was validated; insert
		// between prevSibling and nextSibling.
		const auto& anchor = [&]() -> const RootSiblingAnchor& {
			for (std::size_t i = 0; i < snapshot.rootUuids.size(); ++i)
				if (snapshot.rootUuids[i] == record.uuid)
					return snapshot.rootAnchors[i];
			static RootSiblingAnchor empty;
			return empty;
		}();
		std::size_t insertPos = parentHierarchy->children.size();
		for (std::size_t i = 0; i < parentHierarchy->children.size(); ++i)
		{
			const auto* idc = registry.try_get<EntityIdComponent>(parentHierarchy->children[i]);
			if (idc && idc->id == anchor.nextSibling)
			{
				insertPos = i;
				break;
			}
		}
		parentHierarchy->children.insert(parentHierarchy->children.begin() + insertPos, entity);
	}

	// Phase 5: mark dirty and refresh camera forward directions.
	for (const auto& record : snapshot.entities)
	{
		const auto entity = created[record.uuid];
		SceneGraph::MarkDirty(registry, entity);
	}
	std::vector<entt::entity> changedEntities;
	changedEntities.reserve(snapshot.entities.size());
	for (const auto& [uuid, e] : created)
		changedEntities.push_back(e);
	RefreshCameraForwardDirections(changedEntities);

	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	result.syncImpact = addsRenderable
		? rt2::core::SyncImpact::Structural : rt2::core::SyncImpact::None;
	return result;
}

SubtreeSnapshot SceneManager::CaptureSubtreeSnapshot(
	const std::vector<rt2::core::UUID>& rootUuids) const
{
	SubtreeSnapshot snapshot;
	if (rootUuids.empty()) return snapshot;

	rt2::core::Error error;
	auto roots = ResolveCanonicalRoots(m_Authoring, rootUuids, error);
	if (!error.IsOk())
		return snapshot;

	auto& registry = m_EcsScene.registry;
	for (const auto root : roots)
	{
		std::vector<entt::entity> subtree;
		SceneHierarchy::CollectSubtreePreOrder(registry, root, subtree);
		for (const auto entity : subtree)
			snapshot.entities.push_back(BuildSubtreeRecord(registry, entity));
		snapshot.rootUuids.push_back(GetEntityUuid({ root }));
		snapshot.rootAnchors.push_back(BuildSiblingAnchor(registry, root));
	}
	return snapshot;
}

void SceneManager::CompactMeshRegistryNow()
{
	// The host contract forbids compaction while any Undo or Redo entry
	// references resource slots. The host is responsible for calling this
	// only at history.Clear(), document adoption, or save/reload. We do
	// not have access to the history here (SceneManager never depends on
	// the command layer), so we trust the host contract. The debug assert
	// lives in the host (WalnutApp) at the call site.
	CompactMeshRegistry();
}

rt2::core::UUID SceneManager::ReserveKnownUuid()
{
	return m_UuidProvider ? m_UuidProvider->CreateV4() : rt2::core::UUID::Nil();
}

std::vector<rt2::core::UUID> SceneManager::ReserveKnownUuids(size_t count)
{
	std::vector<rt2::core::UUID> uuids;
	uuids.reserve(count);
	for (size_t i = 0; i < count; ++i)
		uuids.push_back(ReserveKnownUuid());
	return uuids;
}

rt2::core::Result<size_t> SceneManager::CountCanonicalSubtreeEntities(
	const std::vector<rt2::core::UUID>& rootUuids) const
{
	return CountCanonicalDocumentSubtreeEntities(m_Authoring, rootUuids);
}

rt2::core::Result<size_t> SceneManager::CountCanonicalDocumentSubtreeEntities(
	const rt2::core::SceneDocument& document,
	const std::vector<rt2::core::UUID>& rootUuids) const
{
	if (rootUuids.empty())
		return rt2::core::Result<size_t>::Ok(0);
	rt2::core::Error error;
	auto roots = ResolveCanonicalRoots(document, rootUuids, error);
	if (!error.IsOk())
		return rt2::core::Result<size_t>::Fail(error.code, error.path, error.detail);

	size_t count = 0;
	for (const auto root : roots)
	{
		std::vector<entt::entity> subtree;
		SceneHierarchy::CollectSubtreePreOrder(document.ecs.registry, root, subtree);
		count += subtree.size();
	}
	return rt2::core::Result<size_t>::Ok(count);
}

// ============================================================================
// Phase 8 W1 prefab APIs
// ============================================================================

SceneManager::PrefabCreationResult SceneManager::CreatePrefabFromSubtree(
	const std::vector<rt2::core::UUID>& roots,
	const std::filesystem::path& prefabPath)
{
	PrefabCreationResult out;
	out.prefabPath = prefabPath;
	if (roots.empty())
	{
		out.error.code = rt2::core::Error::InvalidEntity;
		out.error.detail = "CreatePrefabFromSubtree: no roots supplied";
		return out;
	}

	// Capture the canonical subtree via the existing structural-command
	// capture path. An empty capture is a hard failure — never a silent
	// empty prefab file.
	out.sourceSnapshot = CaptureSubtreeSnapshot(roots);
	if (out.sourceSnapshot.entities.empty())
	{
		out.error.code = rt2::core::Error::InvalidEntity;
		out.error.detail = "CreatePrefabFromSubtree: no entities captured from the supplied roots";
		return out;
	}

	// One-root invariant: a prefab must have EXACTLY one top-level root.
	// This is a structural contract, not a guess: the canonical roots (after
	// ancestor dedup, so {root, descendant} collapses to {root}) must be a
	// single top-level entity. Multi-root input is rejected BEFORE any file
	// write or sidecar commit — never a partial prefab produced then failed.
	// Children beneath the single root remain fully supported.
	if (out.sourceSnapshot.rootUuids.size() != 1)
	{
		out.error.code = rt2::core::Error::InvalidEntity;
		out.error.detail = "CreatePrefabFromSubtree: a prefab must have exactly one top-level root; "
			"supplied roots resolve to " + std::to_string(out.sourceSnapshot.rootUuids.size()) +
			" top-level roots";
		return out;
	}
	const rt2::core::UUID prefabRootUuid = out.sourceSnapshot.rootUuids.front();

	// Mint ONE fresh templateId per captured entity, parallel to
	// sourceSnapshot.entities in the same pre-order. templateId is frozen in
	// the file and NEVER derived from the entity's scene UUID (amendment A1).
	out.templateIds = ReserveKnownUuids(out.sourceSnapshot.entities.size());

	// Build the document and serialize it before opening the filesystem
	// transaction. Serialization is CPU-only and cannot mutate either entry.
	rt2::core::PrefabDocument doc;
	doc.entities.reserve(out.sourceSnapshot.entities.size());
	for (std::size_t i = 0; i < out.sourceSnapshot.entities.size(); ++i)
	{
		rt2::core::PrefabEntityRecord record;
		record.templateId = out.templateIds[i];
		record.record     = out.sourceSnapshot.entities[i];
		doc.entities.push_back(std::move(record));
	}
	rt2::core::Error serializeErr;
	std::string assetBytes;
	if (!rt2::core::PrefabSerializer::Serialize(doc, assetBytes, serializeErr))
	{
		out.error = serializeErr;
		return out;
	}

	const auto sidecarPath = rt2::core::AssetSidecarPath(prefabPath);
	auto transaction = rt2::core::PrefabFileTransaction::Begin(
		prefabPath, sidecarPath, true);
	if (!transaction.IsOk()) { out.error = transaction.error; return out; }
	auto failTransaction = [&](const rt2::core::Error& original) -> PrefabCreationResult
	{
		out.error = original;
		auto rollback = transaction.value->Rollback();
		if (!rollback.IsOk())
			out.error.detail += "; rollback failed and recovery residue was preserved: " + rollback.error.detail;
		return out;
	};
	auto captured = transaction.value->CapturePair();
	if (!captured.IsOk()) return failTransaction(captured.error);

	// A valid existing sidecar is retained byte-for-byte. Missing or malformed
	// bytes mint a new durable identity, but the captured malformed bytes remain
	// transaction-owned rollback data.
	rt2::core::UUID assetId = rt2::core::UUID::Nil();
	if (captured.value.sidecar.exists)
	{
		std::string text(captured.value.sidecar.bytes.begin(), captured.value.sidecar.bytes.end());
		while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\t')) text.pop_back();
		assetId = rt2::core::UUID::Parse(text);
	}
	std::vector<uint8_t> sidecarBytes;
	if (assetId.IsNull())
	{
		assetId = m_UuidProvider->CreateV4();
		const std::string text = assetId.ToString() + "\n";
		sidecarBytes.assign(text.begin(), text.end());
	}
	else
		sidecarBytes = captured.value.sidecar.bytes;

	auto staged = transaction.value->Stage(
		std::optional<std::vector<uint8_t>>(std::move(sidecarBytes)),
		std::optional<std::vector<uint8_t>>(std::vector<uint8_t>(assetBytes.begin(), assetBytes.end())));
	if (!staged.IsOk()) return failTransaction(staged.error);
	auto installed = transaction.value->InstallSidecarThenAsset();
	if (!installed.IsOk()) return failTransaction(installed.error);
	auto committed = transaction.value->Finalize();
	if (!committed.IsOk()) return failTransaction(committed.error);
	out.recoveryWarning = committed.value.recoveryWarning;
	out.assetId = assetId;
	out.ok = true;
	return out;
}

rt2::core::Result<size_t> SceneManager::CountCanonicalPrefabEntities(
	const std::filesystem::path& prefabPath) const
{
	rt2::core::PrefabDocument doc;
	rt2::core::Error err;
	if (!rt2::core::PrefabSerializer::Load(doc, prefabPath, err))
		return rt2::core::Result<size_t>::Fail(err.code, err.path, err.detail);
	return rt2::core::Result<size_t>::Ok(doc.entities.size());
}

SceneManager::InstantiationResult SceneManager::InstantiatePrefabWithUuids(
	const std::filesystem::path& prefabPath,
	const std::vector<rt2::core::UUID>& knownInstanceUuids,
	std::vector<rt2::core::AssetDiagnostic>& diagnostics)
{
	InstantiationResult out;

	// Canonical root record index within doc.entities, derived from the
	// one-root validation below and used to install PrefabInstanceComponent
	// on the ACTUAL root entity (not necessarily record 0).
	std::size_t canonicalRootIndex = 0;

	// The instance's single fresh instanceId (review fix 2). Reserved AFTER
	// input/file validation but BEFORE any destination mutation — see the
	// reservation block below. Provider draws in this function: ResolveOrAssign
	// consumes an asset-identity draw ONLY when identity cannot be resolved
	// from a valid sidecar (an existing valid sidecar consumes ZERO draws; see
	// the corrected note), then this single draw.
	rt2::core::UUID instanceId = rt2::core::UUID::Nil();

	// Load the prefab. A missing/invalid file is a hard failure — never a
	// silent empty instance.
	rt2::core::PrefabDocument doc;
	rt2::core::Error loadErr;
	if (!rt2::core::PrefabSerializer::Load(doc, prefabPath, loadErr))
	{
		out.mutation = EditorMutationResult::Failure(loadErr.code, loadErr.path,
			"InstantiatePrefabWithUuids: failed to load prefab: " + loadErr.detail);
		return out;
	}
	if (doc.entities.empty())
	{
		out.mutation = EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			prefabPath.string(),
			"InstantiatePrefabWithUuids: prefab file contains no entities");
		return out;
	}

	// Validate the UUID count exactly matches the prefab's entity count.
	if (knownInstanceUuids.size() != doc.entities.size())
	{
		out.mutation = EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			{}, "InstantiatePrefabWithUuids: known UUID count does not match the prefab entity count");
		return out;
	}

	// Validate all supplied UUIDs are non-nil/unique/absent from the document.
	std::unordered_set<rt2::core::UUID> seen;
	for (const auto& uuid : knownInstanceUuids)
	{
		if (uuid.IsNull() || m_Authoring.uuidIndex.Contains(uuid) || !seen.insert(uuid).second)
		{
			out.mutation = EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
				uuid.ToString(),
				"InstantiatePrefabWithUuids: known UUID is nil, duplicate, or already present");
			return out;
		}
	}

	// One-root invariant: the prefab must have EXACTLY one top-level root.
	// A top-level root is a record whose parent is nil or external to the
	// prefab. This replaces the old "first record in the file is the root"
	// assumption that would silently give PrefabInstanceComponent to the
	// wrong entity on a multi-root file. Rejected BEFORE the sidecar resolve
	// and any scene mutation. The ROOT RECORD INDEX is captured so the link
	// is installed on the actual canonical root entity — a hand-authored
	// [child, root] file has exactly one root but it is record 1, and the
	// instance link must land there (Sol P1), never on liveEntities[0].
	{
		std::unordered_set<rt2::core::UUID> templateUuids;
		templateUuids.reserve(doc.entities.size());
		std::unordered_set<rt2::core::UUID> templateIds;
		templateIds.reserve(doc.entities.size());
		for (const auto& rec : doc.entities)
		{
			if (!rec.record.uuid.IsNull())
				templateUuids.insert(rec.record.uuid);
			// templateId is prefab-local identity; it must be unique across
			// the document. A duplicate would give two members the same
			// identity on instantiate (two entities mapping to one template).
			if (!templateIds.insert(rec.templateId).second)
			{
				out.mutation = EditorMutationResult::Failure(rt2::core::Error::Parse,
					prefabPath.string(),
					"InstantiatePrefabWithUuids: prefab contains duplicate templateId " +
						rec.templateId.ToString());
				return out;
			}
		}
		std::size_t rootCount = 0;
		std::size_t rootIndex = 0;
		for (std::size_t i = 0; i < doc.entities.size(); ++i)
		{
			const auto& rec = doc.entities[i].record;
			if (rec.parentUuid.IsNull() ||
			    !templateUuids.count(rec.parentUuid))
			{
				++rootCount;
				rootIndex = i;
			}
		}
		if (rootCount != 1)
		{
			out.mutation = EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				prefabPath.string(),
				"InstantiatePrefabWithUuids: prefab must have exactly one top-level root; file has " +
					std::to_string(rootCount));
			return out;
		}
		canonicalRootIndex = rootIndex;
	}

	// Resolve the prefab's durable asset identity BEFORE any scene mutation.
	// A sidecar that cannot be committed is a hard failure here (not the
	// tolerated import/load behaviour): the instance would otherwise carry a
	// session-only ID and report success. Doing this before the plan build
	// means identity failure mutates nothing.
	bool minted = false;
	rt2::core::Error idErr;
	const rt2::core::UUID prefabAssetId =
		rt2::core::ResolveOrAssign(prefabPath, *m_UuidProvider, minted, idErr);
	{
		rt2::core::Error durableErr;
		const rt2::core::UUID committed =
			rt2::core::ReadSidecarId(rt2::core::AssetSidecarPath(prefabPath), durableErr);
		if (committed.IsNull() || !durableErr.IsOk())
		{
			out.mutation = EditorMutationResult::Failure(
				idErr.IsOk() ? rt2::core::Error::Io : idErr.code,
				rt2::core::AssetSidecarPath(prefabPath).string(),
				"InstantiatePrefabWithUuids: prefab has no durable asset identity (sidecar could not be committed): " +
					idErr.detail);
			return out;
		}
	}

	// ---- Build the complete plan in a temp document before mutating. ----
	// Each record's SubtreeEntityRecord.uuid is the template entity's ORIGINAL
	// scene UUID as captured at prefab creation; it is the key for hierarchy
	// wiring inside the instance and for the script-reference remap below.
	rt2::core::SceneDocument temp;
	std::unordered_map<rt2::core::UUID, entt::entity> templateUuidToTempEntity;
	std::vector<entt::entity> tempEntities;
	tempEntities.reserve(doc.entities.size());
	templateUuidToTempEntity.reserve(doc.entities.size());
	for (std::size_t i = 0; i < doc.entities.size(); ++i)
	{
		const auto& record = doc.entities[i].record;
		const auto entity = temp.ecs.registry.create();
		ApplySubtreeRecord(record, temp.ecs.registry, entity);
		// Primitive geometry is rebuilt at instantiate, exactly as the scene
		// load path does (RegisterPrimitiveMesh in BuildDocumentFromRecords):
		// the prefab file carries the PrimitiveComponent but never resource
		// indices, so the mesh must be re-registered here. The index is
		// rebased into the live scene's registry by the merge below.
		if (record.hasPrimitive)
		{
			const uint32_t primMeshIdx = rt2::core::RegisterPrimitiveMesh(
				temp.ecs.meshRegistry, record.primitive);
			if (auto* ref = temp.ecs.registry.try_get<MeshRef>(entity))
				ref->meshIndex = primMeshIdx;
			else
				temp.ecs.registry.emplace<MeshRef>(entity, primMeshIdx, -1);
		}
		if (!temp.AssignKnownUuid(entity, knownInstanceUuids[i]))
		{
			// Cannot happen (validated unique/absent), but keep it loud.
			out.mutation = EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
				knownInstanceUuids[i].ToString(),
				"InstantiatePrefabWithUuids: failed to assign known UUID in the staging document");
			return out;
		}
		if (!record.uuid.IsNull())
		{
			if (!templateUuidToTempEntity.emplace(record.uuid, entity).second)
			{
				out.mutation = EditorMutationResult::Failure(rt2::core::Error::Parse,
					record.uuid.ToString(),
					"InstantiatePrefabWithUuids: prefab contains duplicate template scene UUID");
				return out;
			}
		}
		tempEntities.push_back(entity);
	}

	// Wire hierarchy inside the staging document. Parents are template scene
	// UUIDs; entities whose parent lies outside the prefab become instance
	// roots. RebuildChildren validates the prefab's internal topology.
	for (std::size_t i = 0; i < doc.entities.size(); ++i)
	{
		const auto& record = doc.entities[i].record;
		if (record.parentUuid.IsNull())
			continue;
		const auto parentIt = templateUuidToTempEntity.find(record.parentUuid);
		if (parentIt == templateUuidToTempEntity.end())
			continue; // external parent — entity is an instance root
		temp.ecs.registry.emplace<Hierarchy>(tempEntities[i]).parent = parentIt->second;
	}
	{
		rt2::core::Error hierErr;
		if (!SceneHierarchy::RebuildChildren(temp.ecs.registry, hierErr))
		{
			out.mutation = EditorMutationResult::Failure(hierErr.code, hierErr.path,
				"InstantiatePrefabWithUuids: prefab hierarchy is invalid: " + hierErr.detail);
			return out;
		}
	}

	// Resolve imported assets in the staging document. Missing assets emit
	// diagnostics but do not fail unless every imported entity is
	// unresolvable (SceneAssetResolver contract).
	{
		rt2::core::Error resolveErr;
		if (!rt2::core::SceneAssetResolver::ResolveAll(temp, m_AssetResolutionContext, diagnostics, resolveErr))
		{
			out.mutation = EditorMutationResult::Failure(resolveErr.code, resolveErr.path,
				"InstantiatePrefabWithUuids: asset resolution failed: " + resolveErr.detail);
			return out;
		}
	}

	// Phase 8 W3, S4 (review fix 2) — reserve the instance's single fresh
	// instanceId BEFORE any destination mutation (resource merge or entity
	// creation). The supplied entity UUIDs and the loaded prefab file have all
	// been validated at this point. The reserved id must be non-nil and absent
	// from every entity UUID, every live destination PIC/PMIC instanceId, and
	// the supplier's knownInstanceUuids. Exhaustion fails the operation
	// transactionally: no resource rows, no entities, no authoring
	// notification.
	{
		std::unordered_set<rt2::core::UUID> forbidden =
			FreshInstanceIdForbiddenSet(m_Authoring, m_EcsScene.registry, nullptr);
		for (const auto& uuid : knownInstanceUuids)
			forbidden.insert(uuid);
		std::unordered_set<rt2::core::UUID> operationLocal;
		rt2::core::Error reserveErr;
		auto reserved = ReserveFreshInstanceId(
			[this] { return ReserveKnownUuid(); }, forbidden, operationLocal, reserveErr);
		if (!reserved)
		{
			out.mutation = EditorMutationResult::Failure(reserveErr.code, reserveErr.path,
				reserveErr.detail);
			return out;
		}
		instanceId = *reserved;
	}

	// ---- Merge the resolved resources into the live scene ----
	// Base-offset rebasing mirrors MergeImportedECS: rebase the staging
	// scene's resource indices, then append its resources to the live scene.
	auto& dst = m_EcsScene;
	auto& dstReg = dst.registry;

	const uint32_t meshBase = dst.meshRegistry.GetCount();
	const int matBase = (int)dst.materials.size();
	const int texBase = (int)dst.textures.size();
	{
		IndexRebase rebase;
		rebase.mesh.SetBase(meshBase);
		rebase.material.SetBase(matBase);
		rebase.texture.SetBase(texBase);
		RebaseIndices(temp.ecs, rebase);
	}
	for (uint32_t i = 0; i < temp.ecs.meshRegistry.GetCount(); ++i)
		dst.meshRegistry.AddMesh(temp.ecs.meshRegistry.GetMesh(i));
	for (const auto& sm : temp.ecs.materials)
		dst.materials.push_back(sm);
	for (auto& st : temp.ecs.textures)
		dst.textures.push_back(std::move(st));

	// Create live entities, copy authored components, assign known UUIDs.
	// AssignKnownUuid cannot fail here (all validated above); the rollback
	// destroys created entities + index entries on any unexpected failure.
	std::unordered_map<entt::entity, entt::entity> entityMap;
	std::vector<entt::entity> liveEntities;
	std::vector<std::pair<rt2::core::UUID, rt2::core::UUID>> templateToInstance;
	liveEntities.reserve(tempEntities.size());
	entityMap.reserve(tempEntities.size());
	bool instanceRenderable = false;
	auto rollbackCreated = [&]() {
		for (const auto e : liveEntities)
		{
			if (const auto* idc = dstReg.try_get<EntityIdComponent>(e))
				m_Authoring.uuidIndex.Erase(idc->id);
			dstReg.destroy(e);
		}
		// Roll back the appended resource-table rows (meshes/materials/
		// textures) to their pre-merge bases. Without this the instance's
		// entities are removed but its subnet of the mesh/material/texture
		// registries is left behind, leaking rows on a failed instantiate.
		dst.meshRegistry.Truncate(meshBase);
		dst.materials.resize(matBase);
		dst.textures.resize(texBase);
		liveEntities.clear();
		entityMap.clear();
	};
	for (std::size_t i = 0; i < tempEntities.size(); ++i)
	{
		const auto live = dstReg.create();
		entityMap.emplace(tempEntities[i], live);
		CopyAuthoredComponents(temp.ecs.registry, tempEntities[i], dstReg, live);
		if (!m_Authoring.AssignKnownUuid(live, knownInstanceUuids[i]))
		{
			rollbackCreated();
			out.mutation = EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
				knownInstanceUuids[i].ToString(),
				"InstantiatePrefabWithUuids: failed to assign known UUID");
			return out;
		}
		liveEntities.push_back(live);
		templateToInstance.emplace_back(doc.entities[i].record.uuid, knownInstanceUuids[i]);
		instanceRenderable = instanceRenderable || dstReg.all_of<MeshRef>(live);
	}

	// Wire hierarchy in the live scene from the staging hierarchy, then let
	// RebuildChildren reconstruct every children cache (NOT a hand-wired
	// parent/children loop — the header contract).
	for (std::size_t i = 0; i < tempEntities.size(); ++i)
	{
		const auto* srcHier = temp.ecs.registry.try_get<Hierarchy>(tempEntities[i]);
		if (!srcHier || srcHier->parent == entt::null)
			continue;
		const auto parentIt = entityMap.find(srcHier->parent);
		if (parentIt == entityMap.end())
			continue;
		dstReg.emplace<Hierarchy>(liveEntities[i]).parent = parentIt->second;
	}
	{
		rt2::core::Error hierErr;
		if (!SceneHierarchy::RebuildChildren(dstReg, hierErr))
		{
			rollbackCreated();
			out.mutation = EditorMutationResult::Failure(hierErr.code, hierErr.path,
				"InstantiatePrefabWithUuids: failed to wire instance hierarchy: " + hierErr.detail);
			return out;
		}
	}

	// Remap UUID-typed script fields exactly as the duplication sibling paths
	// do: template scene UUID -> freshly assigned instance UUID, plus the
	// copied ScriptComponent views, so a script Uuid field pointing at a
	// sibling inside the prefab resolves to that sibling's instance.
	RemapCopiedScriptFields(templateToInstance, liveEntities, dstReg);

	// Link the instance. PrefabInstanceComponent sits on the ACTUAL instance
	// root (the canonical root record derived in one-root validation, which
	// may not be record 0 — a hand-authored [child, root] file has its root
	// at a later index); every member carries instanceId + the frozen
	// templateId from the file. The prefab reference carries the sidecar
	// identity via ResolveOrAssign.
	{
		// instanceId was reserved BEFORE any destination mutation (the review
		// fix 2 reservation block above) — reusing it here makes zero provider
		// calls. The prefab's durable identity was resolved before any
		// mutation, so this cannot fail here; reuse the committed ID. The
		// instance root entity is liveEntities[canonicalRootIndex] — parallel
		// to doc.entities, not to the file's first record.
		auto& inst = dstReg.emplace<PrefabInstanceComponent>(liveEntities[canonicalRootIndex]);
		inst.prefab = AssetReference{ AssetKind::Prefab,
			prefabPath.string(), {}, {},
			prefabAssetId };
		inst.instanceId = instanceId;
	}
	for (std::size_t i = 0; i < tempEntities.size(); ++i)
	{
		auto& member = dstReg.emplace<PrefabMemberComponent>(liveEntities[i]);
		member.instanceId = instanceId;
		member.templateId = doc.entities[i].templateId;
	}

	// Collect created roots (entities whose parent is external or nil) and
	// append the duplication " Copy" suffix to root names (names are not
	// identity).
	for (std::size_t i = 0; i < doc.entities.size(); ++i)
	{
		const auto& record = doc.entities[i].record;
		if (!record.parentUuid.IsNull() &&
		    templateUuidToTempEntity.count(record.parentUuid) != 0)
			continue; // not a root of the instance
		out.createdRoots.push_back(knownInstanceUuids[i]);
		if (auto* name = dstReg.try_get<NameComponent>(liveEntities[i]))
			name->name += " Copy";
		SceneGraph::MarkDirty(dstReg, liveEntities[i]);
	}

	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	out.instanceId = instanceId;
	out.mutation.success = true;
	out.mutation.syncImpact = instanceRenderable
		? rt2::core::SyncImpact::Structural : rt2::core::SyncImpact::None;
	for (const auto& root : out.createdRoots)
		out.mutation.affectedEntities.push_back(root);
	return out;
}

EditorMutationResult SceneManager::CreateEmptyWithUuid(
	const rt2::core::UUID& uuid,
	const std::string& name,
	const std::optional<rt2::core::UUID>& parentUuid,
	std::optional<std::size_t> siblingPosition)
{
	auto& registry = m_EcsScene.registry;
	if (m_Authoring.uuidIndex.Contains(uuid))
		return EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
			uuid.ToString(), "CreateEmptyWithUuid: UUID already present in the document");

	entt::entity parent = entt::null;
	if (parentUuid)
	{
		parent = m_Authoring.FindByUuid(*parentUuid);
		if (parent == entt::null || !registry.valid(parent))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				parentUuid->ToString(),
				"CreateEmptyWithUuid: parent UUID is not present in the authoring scene");
	}

	const auto entity = registry.create();
	registry.emplace<Transform>(entity);
	registry.emplace<NameComponent>(entity, name.empty() ? "Empty" : name);
	registry.emplace<VisibleComponent>(entity);
	if (!m_Authoring.AssignKnownUuid(entity, uuid))
	{
		registry.destroy(entity);
		return EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
			uuid.ToString(), "CreateEmptyWithUuid: failed to assign known UUID");
	}
	if (parent != entt::null)
	{
		registry.emplace<Hierarchy>(entity).parent = parent;
		auto* parentHierarchy = registry.try_get<Hierarchy>(parent);
		if (!parentHierarchy)
			parentHierarchy = &registry.emplace<Hierarchy>(parent);
		std::size_t insertPos = siblingPosition
			? std::min(*siblingPosition, parentHierarchy->children.size())
			: parentHierarchy->children.size();
		parentHierarchy->children.insert(parentHierarchy->children.begin() + insertPos, entity);
	}
	else if (siblingPosition)
	{
		// Root-entity ordering is unspecified; siblingPosition for a root
		// is informational only. We still honor it best-effort by leaving
		// the entity in registry-iteration order (no explicit root list).
	}
	SceneGraph::MarkDirty(registry, entity);
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	EditorMutationResult result;
	result.affectedEntities.push_back(uuid);
	result.syncImpact = rt2::core::SyncImpact::None; // empty entity adds no renderable
	return result;
}

EditorMutationResult SceneManager::CreatePrimitiveEntity(
	const rt2::core::UUID& uuid,
	const std::string& name,
	PrimitiveComponent::Kind kind,
	float size,
	const EditableTRS& localTRS,
	int materialIndex,
	const std::optional<rt2::core::UUID>& parentUuid)
{
	auto& registry = m_EcsScene.registry;
	if (m_Authoring.uuidIndex.Contains(uuid))
		return EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
			uuid.ToString(), "CreatePrimitiveEntity: UUID already present in the document");

	entt::entity parent = entt::null;
	if (parentUuid)
	{
		parent = m_Authoring.FindByUuid(*parentUuid);
		if (parent == entt::null || !registry.valid(parent))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				parentUuid->ToString(),
				"CreatePrimitiveEntity: parent UUID is not present in the authoring scene");
	}

	// Build the mesh geometry and register it. The mesh slot is stable
	// while no compaction runs (3B1 invariant).
	MeshData meshData;
	switch (kind)
	{
		case PrimitiveComponent::Cube:   meshData = PrimitiveGeometry::CreateCube(size); break;
		case PrimitiveComponent::Sphere:  meshData = PrimitiveGeometry::CreateSphere(size * 0.5f); break;
		case PrimitiveComponent::Plane:  meshData = PrimitiveGeometry::CreatePlane(size); break;
		default:
			return EditorMutationResult::Failure(rt2::core::Error::UnknownPrimitive,
				uuid.ToString(), "CreatePrimitiveEntity: unknown primitive kind");
	}
	const uint32_t meshIdx = m_EcsScene.meshRegistry.AddMesh(std::move(meshData));

	const auto entity = registry.create();
	Transform tf;
	tf.translation = localTRS.translation;
	tf.rotation = localTRS.rotation;
	tf.scale = localTRS.scale;
	registry.emplace<Transform>(entity, tf);
	registry.emplace<MeshRef>(entity, meshIdx, materialIndex);
	registry.emplace<PrimitiveComponent>(entity, PrimitiveComponent{kind, size, 24, 16});
	if (!name.empty())
		registry.emplace<NameComponent>(entity, name);
	registry.emplace<VisibleComponent>(entity);
	if (!m_Authoring.AssignKnownUuid(entity, uuid))
	{
		registry.destroy(entity);
		return EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
			uuid.ToString(), "CreatePrimitiveEntity: failed to assign known UUID");
	}
	if (parent != entt::null)
	{
		registry.emplace<Hierarchy>(entity).parent = parent;
		auto* parentHierarchy = registry.try_get<Hierarchy>(parent);
		if (!parentHierarchy)
			parentHierarchy = &registry.emplace<Hierarchy>(parent);
		parentHierarchy->children.push_back(entity);
	}
	SceneGraph::MarkDirty(registry, entity);
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	EditorMutationResult result;
	result.affectedEntities.push_back(uuid);
	result.syncImpact = rt2::core::SyncImpact::Structural;
	return result;
}

EditorMutationResult SceneManager::CreateLightEntity(
	const rt2::core::UUID& uuid,
	const std::string& name,
	const EditableTRS& localTRS,
	const glm::vec3& color,
	float intensity,
	LightType type,
	const std::optional<rt2::core::UUID>& parentUuid)
{
	auto& registry = m_EcsScene.registry;
	if (m_Authoring.uuidIndex.Contains(uuid))
		return EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
			uuid.ToString(), "CreateLightEntity: UUID already present in the document");

	entt::entity parent = entt::null;
	if (parentUuid)
	{
		parent = m_Authoring.FindByUuid(*parentUuid);
		if (parent == entt::null || !registry.valid(parent))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				parentUuid->ToString(),
				"CreateLightEntity: parent UUID is not present in the authoring scene");
	}

	const auto entity = registry.create();
	Transform tf;
	tf.translation = localTRS.translation;
	tf.rotation = localTRS.rotation;
	tf.scale = localTRS.scale;
	registry.emplace<Transform>(entity, tf);
	LightComponent light;
	light.color = color;
	light.intensity = intensity;
	light.type = type;
	registry.emplace<LightComponent>(entity, light);
	if (!name.empty())
		registry.emplace<NameComponent>(entity, name);
	registry.emplace<VisibleComponent>(entity);
	if (!m_Authoring.AssignKnownUuid(entity, uuid))
	{
		registry.destroy(entity);
		return EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
			uuid.ToString(), "CreateLightEntity: failed to assign known UUID");
	}
	if (parent != entt::null)
	{
		registry.emplace<Hierarchy>(entity).parent = parent;
		auto* parentHierarchy = registry.try_get<Hierarchy>(parent);
		if (!parentHierarchy)
			parentHierarchy = &registry.emplace<Hierarchy>(parent);
		parentHierarchy->children.push_back(entity);
	}
	SceneGraph::MarkDirty(registry, entity);
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	EditorMutationResult result;
	result.affectedEntities.push_back(uuid);
	result.syncImpact = rt2::core::SyncImpact::Structural;
	return result;
}

SceneManager::DuplicationResult SceneManager::DuplicateSubtreesWithUuids(
	const std::vector<rt2::core::UUID>& sourceRoots,
	const std::vector<rt2::core::UUID>& knownDuplicateUuids)
{
	DuplicationResult out;
	if (sourceRoots.empty()) return out;
	rt2::core::Error error;
	auto roots = ResolveCanonicalRoots(m_Authoring, sourceRoots, error);
	if (!error.IsOk())
	{
		out.mutation.success = false;
		out.mutation.error = error;
		return out;
	}
	auto& registry = m_EcsScene.registry;

	// Walk each canonical subtree in deterministic pre-order and collect
	// the source entities. This is the SAME pre-order the manager uses
	// internally to assign UUIDs positionally.
	std::vector<entt::entity> sources;
	for (const auto root : roots)
		SceneHierarchy::CollectSubtreePreOrder(registry, root, sources);

	// Validate the UUID count exactly matches the resulting entity count.
	if (knownDuplicateUuids.size() != sources.size())
	{
		out.mutation = EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			{}, "DuplicateSubtreesWithUuids: known UUID count does not match the canonical subtree size");
		return out;
	}

	// Validate all supplied UUIDs are valid/unique/absent from the document.
	std::unordered_set<rt2::core::UUID> seen;
	for (const auto& uuid : knownDuplicateUuids)
	{
		if (uuid.IsNull() || m_Authoring.uuidIndex.Contains(uuid) || !seen.insert(uuid).second)
		{
			out.mutation = EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
				uuid.ToString(), "DuplicateSubtreesWithUuids: known UUID is nil, duplicate, or already present");
			return out;
		}
	}

	// Phase 8 W3, S4 (review fix 2): PLAN the copied-forest classification
	// from the SOURCE forest BEFORE any destination mutation. The known entity
	// UUIDs are caller-supplied (consumed before this path, no provider draw),
	// so provider order here is exactly: one fresh instanceId per complete
	// instance group, all before the create loop.
	const CopiedPrefabPlan copiedPrefabs = PlanCopiedPrefabLinks(registry, roots);
	std::unordered_map<rt2::core::UUID, rt2::core::UUID> freshIdByOriginalId;

	// Reserve ONE fresh instanceId per complete instance group against the
	// authoring uuidIndex + live destination PIC/PMIC instanceIds (source ==
	// destination for a duplicate) + the caller-supplied knownDuplicateUuids.
	// Failure here is transactional: no entity has been created yet.
	{
		std::unordered_set<rt2::core::UUID> forbidden =
			FreshInstanceIdForbiddenSet(m_Authoring, registry, &registry);
		for (const auto& uuid : knownDuplicateUuids)
			forbidden.insert(uuid);
		std::unordered_set<rt2::core::UUID> operationLocal;
		freshIdByOriginalId.reserve(copiedPrefabs.rootsByGroup.size());
		for (const auto& group : copiedPrefabs.rootsByGroup)
		{
			rt2::core::Error reserveErr;
			auto fresh = ReserveFreshInstanceId(
				[this] { return ReserveKnownUuid(); }, forbidden, operationLocal, reserveErr);
			if (!fresh)
			{
				out.mutation = EditorMutationResult::Failure(reserveErr.code, reserveErr.path,
					reserveErr.detail);
				return out;
			}
			freshIdByOriginalId.emplace(group.first, *fresh);
		}
	}

	// Build the complete duplication plan before mutating.
	std::unordered_map<entt::entity, entt::entity> remap;
	std::vector<std::pair<rt2::core::UUID, rt2::core::UUID>> sourceToDuplicate;
	std::vector<entt::entity> duplicates;
	sourceToDuplicate.reserve(sources.size());
	duplicates.reserve(sources.size());
	for (std::size_t i = 0; i < sources.size(); ++i)
	{
		const auto source = sources[i];
		const auto duplicate = registry.create();
		remap.emplace(source, duplicate);
		CopyAuthoredComponents(registry, source, registry, duplicate);
		if (!m_Authoring.AssignKnownUuid(duplicate, knownDuplicateUuids[i]))
		{
			// Rollback: destroy everything we created so far.
			for (const auto& [s, d] : remap)
			{
				if (const auto* idc = registry.try_get<EntityIdComponent>(d))
					m_Authoring.uuidIndex.Erase(idc->id);
				registry.destroy(d);
			}
			out.mutation = EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
				knownDuplicateUuids[i].ToString(),
				"DuplicateSubtreesWithUuids: failed to assign known UUID");
			return out;
		}
		const auto* sourceIdc = registry.try_get<EntityIdComponent>(source);
		sourceToDuplicate.emplace_back(sourceIdc ? sourceIdc->id : rt2::core::UUID{},
		                               knownDuplicateUuids[i]);
		duplicates.push_back(duplicate);
	}
	RemapCopiedScriptFields(sourceToDuplicate, duplicates, registry);

	// APPLY the pre-reserved plan. Zero provider calls.
	ApplyCopiedPrefabLinks(registry, copiedPrefabs, remap, freshIdByOriginalId);

	// Wire Hierarchy among duplicates.
	bool duplicatesRenderable = false;
	for (const auto source : sources)
	{
		const auto duplicate = remap.at(source);
		duplicatesRenderable = duplicatesRenderable || registry.all_of<MeshRef>(duplicate);
		const auto* sourceHierarchy = registry.try_get<Hierarchy>(source);
		if (!sourceHierarchy || sourceHierarchy->parent == entt::null)
			continue;
		const auto mappedParent = remap.find(sourceHierarchy->parent);
		const auto duplicateParent = mappedParent != remap.end()
			? mappedParent->second : sourceHierarchy->parent;
		registry.emplace<Hierarchy>(duplicate).parent = duplicateParent;
		auto* parentHierarchy = registry.try_get<Hierarchy>(duplicateParent);
		if (!parentHierarchy)
			parentHierarchy = &registry.emplace<Hierarchy>(duplicateParent);
		parentHierarchy->children.push_back(duplicate);
	}

	// Append " Copy" to duplicate root names and collect created roots.
	for (const auto root : roots)
	{
		const auto duplicate = remap.at(root);
		if (auto* name = registry.try_get<NameComponent>(duplicate))
			name->name += " Copy";
		out.createdRoots.push_back(GetEntityUuid({ duplicate }));
		SceneGraph::MarkDirty(registry, duplicate);
	}
	if (const auto warning = MakeCopiedPrefabRecoveryWarning("duplicate", copiedPrefabs))
		out.mutation.recoveryWarning = *warning;

	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	out.mutation.syncImpact = duplicatesRenderable
		? rt2::core::SyncImpact::Structural : rt2::core::SyncImpact::None;
	out.mutation.success = true;
	for (const auto root : roots)
		out.mutation.affectedEntities.push_back(GetEntityUuid({ remap.at(root) }));
	out.sourceToDuplicate = std::move(sourceToDuplicate);
	return out;
}

SceneManager::DuplicationResult SceneManager::PasteSubtreesWithUuids(
	const rt2::core::SceneDocument& clipboard,
	const std::vector<rt2::core::UUID>& clipboardRoots,
	const std::optional<rt2::core::UUID>& parentUuid,
	const std::vector<rt2::core::UUID>& knownPastedUuids)
{
	DuplicationResult out;
	if (clipboardRoots.empty()) return out;
	rt2::core::Error error;
	auto roots = ResolveCanonicalRoots(clipboard, clipboardRoots, error);
	if (!error.IsOk())
	{
		out.mutation.success = false;
		out.mutation.error = error;
		return out;
	}
	auto& destination = m_EcsScene.registry;
	entt::entity destinationParent = entt::null;
	if (parentUuid)
	{
		destinationParent = m_Authoring.FindByUuid(*parentUuid);
		if (destinationParent == entt::null || !destination.valid(destinationParent))
		{
			out.mutation = EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				parentUuid->ToString(), "paste parent is not present in the authoring scene");
			return out;
		}
	}

	// Validate clipboard mesh/material resources still match this document.
	std::vector<entt::entity> sources;
	for (const auto root : roots)
		SceneHierarchy::CollectSubtreePreOrder(clipboard.ecs.registry, root, sources);
	for (const auto source : sources)
	{
		if (const auto* mesh = clipboard.ecs.registry.try_get<MeshRef>(source))
		{
			if (mesh->meshIndex >= m_EcsScene.meshRegistry.GetCount())
			{
				out.mutation = EditorMutationResult::Failure(rt2::core::Error::ClipboardStale,
					{}, "clipboard mesh resources no longer match this document");
				return out;
			}
			if (mesh->materialIndex >= static_cast<int>(m_EcsScene.materials.size()))
			{
				out.mutation = EditorMutationResult::Failure(rt2::core::Error::ClipboardStale,
					{}, "clipboard material resources no longer match this document");
				return out;
			}
		}
	}

	// Validate the UUID count exactly matches the resulting entity count.
	if (knownPastedUuids.size() != sources.size())
	{
		out.mutation = EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			{}, "PasteSubtreesWithUuids: known UUID count does not match the canonical subtree size");
		return out;
	}

	// Validate all supplied UUIDs are valid/unique/absent from the document.
	std::unordered_set<rt2::core::UUID> seen;
	for (const auto& uuid : knownPastedUuids)
	{
		if (uuid.IsNull() || m_Authoring.uuidIndex.Contains(uuid) || !seen.insert(uuid).second)
		{
			out.mutation = EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
				uuid.ToString(), "PasteSubtreesWithUuids: known UUID is nil, duplicate, or already present");
			return out;
		}
	}

	// Phase 8 W3, S4 (review fix 2): PLAN the copied-forest classification from
	// the SOURCE (clipboard) forest BEFORE any destination mutation. The known
	// entity UUIDs are caller-supplied (no provider draw), so provider order
	// here is exactly: one fresh instanceId per complete instance group, all
	// before the create loop.
	const CopiedPrefabPlan copiedPrefabs = PlanCopiedPrefabLinks(clipboard.ecs.registry, roots);
	std::unordered_map<rt2::core::UUID, rt2::core::UUID> freshIdByOriginalId;

	// Reserve ONE fresh instanceId per complete group against the authoring
	// uuidIndex + live destination PIC/PMIC instanceIds + the clipboard's live
	// PIC/PMIC instanceIds (the source is a DISTINCT document) + the supplied
	// knownPastedUuids. Failure here is transactional: no entity has been
	// created yet.
	{
		std::unordered_set<rt2::core::UUID> forbidden =
			FreshInstanceIdForbiddenSet(m_Authoring, destination, &clipboard.ecs.registry);
		for (const auto& uuid : knownPastedUuids)
			forbidden.insert(uuid);
		std::unordered_set<rt2::core::UUID> operationLocal;
		freshIdByOriginalId.reserve(copiedPrefabs.rootsByGroup.size());
		for (const auto& group : copiedPrefabs.rootsByGroup)
		{
			rt2::core::Error reserveErr;
			auto fresh = ReserveFreshInstanceId(
				[this] { return ReserveKnownUuid(); }, forbidden, operationLocal, reserveErr);
			if (!fresh)
			{
				out.mutation = EditorMutationResult::Failure(reserveErr.code, reserveErr.path,
					reserveErr.detail);
				return out;
			}
			freshIdByOriginalId.emplace(group.first, *fresh);
		}
	}

	// Build the complete paste plan before mutating.
	std::unordered_map<entt::entity, entt::entity> remap;
	std::vector<std::pair<rt2::core::UUID, rt2::core::UUID>> sourceToDuplicate;
	std::vector<entt::entity> pastes;
	sourceToDuplicate.reserve(sources.size());
	pastes.reserve(sources.size());
	bool pastesRenderable = false;
	for (std::size_t i = 0; i < sources.size(); ++i)
	{
		const auto source = sources[i];
		const auto pasted = destination.create();
		remap.emplace(source, pasted);
		CopyAuthoredComponents(clipboard.ecs.registry, source, destination, pasted);
		if (!m_Authoring.AssignKnownUuid(pasted, knownPastedUuids[i]))
		{
			// Rollback.
			for (const auto& [s, d] : remap)
			{
				if (const auto* idc = destination.try_get<EntityIdComponent>(d))
					m_Authoring.uuidIndex.Erase(idc->id);
				destination.destroy(d);
			}
			out.mutation = EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
				knownPastedUuids[i].ToString(),
				"PasteSubtreesWithUuids: failed to assign known UUID");
			return out;
		}
		pastesRenderable = pastesRenderable || destination.all_of<MeshRef>(pasted);
		const auto* sourceIdc = clipboard.ecs.registry.try_get<EntityIdComponent>(source);
		sourceToDuplicate.emplace_back(sourceIdc ? sourceIdc->id : rt2::core::UUID{},
		                                knownPastedUuids[i]);
		pastes.push_back(pasted);
	}
	RemapCopiedScriptFields(sourceToDuplicate, pastes, destination);

	// APPLY the pre-reserved plan. Zero provider calls.
	ApplyCopiedPrefabLinks(destination, copiedPrefabs, remap, freshIdByOriginalId);

	// Wire Hierarchy among pastes and to the destination parent.
	for (const auto source : sources)
	{
		const auto pasted = remap.at(source);
		const auto* sourceHierarchy = clipboard.ecs.registry.try_get<Hierarchy>(source);
		entt::entity pastedParent = destinationParent;
		if (sourceHierarchy)
		{
			const auto mappedParent = remap.find(sourceHierarchy->parent);
			if (mappedParent != remap.end())
				pastedParent = mappedParent->second;
		}
		if (pastedParent == entt::null)
			continue;
		destination.emplace<Hierarchy>(pasted).parent = pastedParent;
		auto* parentHierarchy = destination.try_get<Hierarchy>(pastedParent);
		if (!parentHierarchy)
			parentHierarchy = &destination.emplace<Hierarchy>(pastedParent);
		parentHierarchy->children.push_back(pasted);
	}

	// Append " Copy" to paste root names and collect created roots.
	for (const auto root : roots)
	{
		const auto pasted = remap.at(root);
		if (auto* name = destination.try_get<NameComponent>(pasted))
			name->name += " Copy";
		out.createdRoots.push_back(GetEntityUuid({ pasted }));
		SceneGraph::MarkDirty(destination, pasted);
	}
	if (const auto warning = MakeCopiedPrefabRecoveryWarning("paste", copiedPrefabs))
		out.mutation.recoveryWarning = *warning;

	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	out.mutation.syncImpact = pastesRenderable
		? rt2::core::SyncImpact::Structural : rt2::core::SyncImpact::None;
	out.mutation.success = true;
	for (const auto root : roots)
		out.mutation.affectedEntities.push_back(GetEntityUuid({ remap.at(root) }));
	out.sourceToDuplicate = std::move(sourceToDuplicate);
	return out;
}

EditorMutationResult SceneManager::SetLocalTransformStates(
	const std::vector<std::pair<rt2::core::UUID, EditableTRS>>& states)
{
	if (states.empty()) return {};
	auto& registry = m_EcsScene.registry;

	// Validate ALL UUIDs resolve first. Any failure => zero mutation.
	std::vector<std::pair<entt::entity, EditableTRS>> resolved;
	resolved.reserve(states.size());
	for (const auto& [uuid, trs] : states)
	{
		const auto entity = m_Authoring.FindByUuid(uuid);
		if (entity == entt::null || !registry.valid(entity))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				uuid.ToString(),
				"SetLocalTransformStates: entity UUID is not present in the authoring scene");
		resolved.emplace_back(entity, trs);
	}

	// Apply all local TRS in one pass.
	for (const auto& [entity, trs] : resolved)
	{
		if (auto* tf = registry.try_get<Transform>(entity))
		{
			tf->translation = trs.translation;
			tf->rotation = glm::normalize(trs.rotation);
			tf->scale = trs.scale;
			SceneGraph::MarkDirty(registry, entity);
		}
	}

	std::vector<entt::entity> changedEntities;
	changedEntities.reserve(resolved.size());
	EditorMutationResult result;
	result.syncImpact = rt2::core::SyncImpact::Transform;
	for (const auto& [entity, trs] : resolved)
	{
		changedEntities.push_back(entity);
		if (const auto* idc = registry.try_get<EntityIdComponent>(entity))
			result.affectedEntities.push_back(idc->id);
	}
	RefreshCameraForwardDirections(changedEntities);
	NotifyAuthoringChanged();
	return result;
}

EditorMutationResult SceneManager::ReparentBatch(
	const std::vector<ReparentEdit>& edits, ReparentMode mode)
{
	if (edits.empty()) return {};
	auto& registry = m_EcsScene.registry;

	// Phase 1: validate all entities and all new parents resolve, and no
	// cycles. Any failure => zero mutation.
	std::vector<entt::entity> entities;
	std::vector<entt::entity> newParents;
	entities.reserve(edits.size());
	newParents.reserve(edits.size());
	for (const auto& edit : edits)
	{
		const auto entity = m_Authoring.FindByUuid(edit.entity);
		if (entity == entt::null || !registry.valid(entity))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				edit.entity.ToString(),
				"ReparentBatch: entity UUID is not present in the authoring scene");
		entt::entity newParent = entt::null;
		if (!edit.newParent.IsNull())
		{
			newParent = m_Authoring.FindByUuid(edit.newParent);
			if (newParent == entt::null || !registry.valid(newParent))
				return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
					edit.newParent.ToString(),
					"ReparentBatch: new parent UUID is not present in the authoring scene");
		}
		if (newParent != entt::null && SceneHierarchy::IsDescendant(registry, entity, newParent))
			return EditorMutationResult::Failure(rt2::core::Error::HierarchyCycle,
				edit.entity.ToString(),
				"ReparentBatch: cannot parent an entity beneath itself or a descendant");
		entities.push_back(entity);
		newParents.push_back(newParent);
	}

	// Phase 1b: batch-cycle validation. The per-edit check above validates
	// each entity against the PRE-batch hierarchy. A batch like {A→under B,
	// B→under A} passes per-edit validation (neither is a descendant of the
	// other yet) but creates a cycle. Build the planned parent map and
	// check that following the planned parent chain from each entity never
	// revisits an entity in the batch.
	{
		std::unordered_map<entt::entity, entt::entity> plannedParent;
		for (std::size_t i = 0; i < edits.size(); ++i)
			plannedParent[entities[i]] = newParents[i];
		for (std::size_t i = 0; i < edits.size(); ++i)
		{
			std::unordered_set<entt::entity> visited;
			visited.insert(entities[i]);
			entt::entity cursor = newParents[i];
			while (cursor != entt::null)
			{
				if (!visited.insert(cursor).second)
				{
					// Cycle detected through the planned parent chain.
					return EditorMutationResult::Failure(rt2::core::Error::HierarchyCycle,
						edits[i].entity.ToString(),
						"ReparentBatch: batch creates a cycle through the planned parent map");
				}
				// Follow the planned parent if this entity is in the batch;
				// otherwise follow the live hierarchy.
				auto it = plannedParent.find(cursor);
				if (it != plannedParent.end())
					cursor = it->second;
				else if (const auto* h = registry.try_get<Hierarchy>(cursor))
					cursor = h->parent;
				else
					cursor = entt::null;
			}
		}
	}

	// Phase 2: for PreserveWorld, convert each desired world matrix to
	// local against the NEW parent (singular/shear => fail all). Uses the
	// world matrix stored in the ReparentEdit (captured at construction
	// time) so the command is self-contained and not dependent on live
	// state.
	std::vector<EditableTRS> newLocals;
	if (mode == ReparentMode::PreserveWorld)
	{
		UpdateWorldTransforms();
		newLocals.reserve(edits.size());
		for (std::size_t i = 0; i < edits.size(); ++i)
		{
			const auto entity = entities[i];
			const auto newParent = newParents[i];
			glm::mat4 parentWorld(1.0f);
			if (newParent != entt::null)
			{
				const auto* parentTransform = registry.try_get<Transform>(newParent);
				if (!parentTransform)
					return EditorMutationResult::Failure(rt2::core::Error::InvalidTransform,
						edits[i].newParent.ToString(),
						"ReparentBatch: new parent has no transform");
				parentWorld = parentTransform->worldMatrix;
			}
			// Use the world matrix stored in the edit (captured at
			// construction time) rather than the live transform's
			// worldMatrix. This keeps the command self-contained.
			(void)entity; // entity validity already validated in Phase 1.
			EditableTRS local;
			if (!TryWorldToLocalTRS(parentWorld, edits[i].worldMatrix, local))
				return EditorMutationResult::Failure(rt2::core::Error::InvalidTransform,
					edits[i].entity.ToString(),
					"ReparentBatch: preserve-world reparent produced a singular or sheared transform");
			newLocals.push_back(local);
		}
	}
	else
	{
		newLocals.reserve(edits.size());
		for (const auto& edit : edits)
			newLocals.push_back(edit.localTRS);
	}

	// Phase 3: apply all reparents atomically. Remove each entity from its
	// old parent's children list, set the new parent, and insert at the
	// anchored sibling position (using ReparentEdit.anchor). For
	// after-edits the anchor is typically empty (append to the end); for
	// before-edits (Undo) the anchor restores the exact original position.
	for (std::size_t i = 0; i < edits.size(); ++i)
	{
		const auto entity = entities[i];
		const auto newParent = newParents[i];
		const auto& anchor = edits[i].anchor;
		auto* hierarchy = registry.try_get<Hierarchy>(entity);
		RemoveChild(registry, hierarchy ? hierarchy->parent : entt::null, entity);
		if (!hierarchy)
			hierarchy = &registry.emplace<Hierarchy>(entity);
		hierarchy->parent = newParent;
		if (newParent != entt::null)
		{
			auto* parentHierarchy = registry.try_get<Hierarchy>(newParent);
			if (!parentHierarchy)
				parentHierarchy = &registry.emplace<Hierarchy>(newParent);
			// Find the insertion position from the anchor's nextSibling.
			// Insert before nextSibling when resolvable; otherwise append.
			std::size_t insertPos = parentHierarchy->children.size();
			if (!anchor.nextSibling.IsNull())
			{
				for (std::size_t j = 0; j < parentHierarchy->children.size(); ++j)
				{
					const auto* idc = registry.try_get<EntityIdComponent>(parentHierarchy->children[j]);
					if (idc && idc->id == anchor.nextSibling)
					{
						insertPos = j;
						break;
					}
				}
			}
			parentHierarchy->children.insert(parentHierarchy->children.begin() + insertPos, entity);
		}
	}

	// Phase 4: apply the new local TRS.
	for (std::size_t i = 0; i < edits.size(); ++i)
	{
		auto& transform = registry.get<Transform>(entities[i]);
		transform.translation = newLocals[i].translation;
		transform.rotation = glm::normalize(newLocals[i].rotation);
		transform.scale = newLocals[i].scale;
		SceneGraph::MarkDirty(registry, entities[i]);
	}

	std::vector<entt::entity> changedEntities(entities.begin(), entities.end());
	RefreshCameraForwardDirections(changedEntities);
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	EditorMutationResult result;
	result.syncImpact = rt2::core::SyncImpact::Transform;
	for (const auto entity : entities)
		result.affectedEntities.push_back(GetEntityUuid({ entity }));
	return result;
}

void SceneManager::SetTransform(EntityId entity,
                                const glm::vec3& position,
                                const glm::vec3& rotation,
                                float scale)
{
	SetTransform(entity, position, rotation, glm::vec3(scale));
}

void SceneManager::SetTransform(EntityId entity,
	const glm::vec3& position, const glm::vec3& rotation, const glm::vec3& scale)
{
	if (!entity.IsValid()) return;
	auto& reg = m_EcsScene.registry;
	if (auto* tf = reg.try_get<Transform>(entity.id))
	{
		tf->translation = position;
		tf->rotation = glm::quat(glm::radians(rotation));
		tf->scale = scale;
		SceneGraph::SetLocalDirty(reg, entity.id);
		RefreshCameraForwardDirections({ entity.id });
		NotifyAuthoringChanged();
	}
}

void SceneManager::SetLocalTransform(EntityId entity, const EditableTRS& transform)
{
	if (!entity.IsValid() || !m_EcsScene.registry.valid(entity.id)) return;
	if (auto* tf = m_EcsScene.registry.try_get<Transform>(entity.id))
	{
		tf->translation = transform.translation;
		tf->rotation = glm::normalize(transform.rotation);
		tf->scale = transform.scale;
		SceneGraph::MarkDirty(m_EcsScene.registry, entity.id);
		RefreshCameraForwardDirections({ entity.id });
		NotifyAuthoringChanged();
	}
}

bool SceneManager::TrySetWorldTransform(EntityId entity, const glm::mat4& desiredWorld)
{
	return TrySetWorldTransforms({ { entity, desiredWorld } });
}

bool SceneManager::TrySetWorldTransforms(
	const std::vector<std::pair<EntityId, glm::mat4>>& desiredWorldTransforms)
{
	if (desiredWorldTransforms.empty()) return true;
	UpdateWorldTransforms();
	auto& registry = m_EcsScene.registry;
	std::unordered_map<entt::entity, glm::mat4> desiredByEntity;
	desiredByEntity.reserve(desiredWorldTransforms.size());
	for (const auto& edit : desiredWorldTransforms)
	{
		if (!edit.first.IsValid() || !registry.valid(edit.first.id) ||
			!registry.all_of<Transform>(edit.first.id) ||
			!desiredByEntity.emplace(edit.first.id, edit.second).second)
			return false;
	}
	std::unordered_map<entt::entity, glm::mat4> predictedWorldCache;
	std::unordered_set<entt::entity> resolving;
	std::function<bool(entt::entity, glm::mat4&)> resolvePredictedWorld;
	resolvePredictedWorld = [&](entt::entity entity, glm::mat4& outWorld) -> bool
	{
		const auto desired = desiredByEntity.find(entity);
		if (desired != desiredByEntity.end())
		{
			outWorld = desired->second;
			return true;
		}
		const auto cached = predictedWorldCache.find(entity);
		if (cached != predictedWorldCache.end())
		{
			outWorld = cached->second;
			return true;
		}
		if (!registry.valid(entity) || !registry.all_of<Transform>(entity) ||
			!resolving.insert(entity).second)
			return false;
		glm::mat4 parentWorld(1.0f);
		if (const auto* hierarchy = registry.try_get<Hierarchy>(entity);
			hierarchy && hierarchy->parent != entt::null)
		{
			if (!resolvePredictedWorld(hierarchy->parent, parentWorld))
			{
				resolving.erase(entity);
				return false;
			}
		}
		outWorld = parentWorld * registry.get<Transform>(entity).localMatrix();
		predictedWorldCache.emplace(entity, outWorld);
		resolving.erase(entity);
		return true;
	};

	std::vector<std::pair<entt::entity, EditableTRS>> locals;
	locals.reserve(desiredWorldTransforms.size());
	for (const auto& edit : desiredWorldTransforms)
	{
		glm::mat4 parentWorld(1.0f);
		const EntityId parent = GetParent(edit.first);
		if (parent.IsValid() && !resolvePredictedWorld(parent.id, parentWorld))
			return false;
		EditableTRS local;
		if (!TryWorldToLocalTRS(parentWorld, edit.second, local)) return false;
		locals.emplace_back(edit.first.id, local);
	}

	for (const auto& edit : locals)
	{
		auto& transform = registry.get<Transform>(edit.first);
		transform.translation = edit.second.translation;
		transform.rotation = glm::normalize(edit.second.rotation);
		transform.scale = edit.second.scale;
		SceneGraph::MarkDirty(registry, edit.first);
	}
	std::vector<entt::entity> changedEntities;
	changedEntities.reserve(locals.size());
	for (const auto& edit : locals)
		changedEntities.push_back(edit.first);
	RefreshCameraForwardDirections(changedEntities);
	NotifyAuthoringChanged();
	return true;
}

EditorMutationResult SceneManager::AlignCameraEntityToView(
	const rt2::core::UUID& cameraEntity, const EditorCameraPose& requested)
{
	EditorCameraPose pose = requested;
	if (!TryNormalizeEditorCameraPose(pose))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidTransform,
			cameraEntity.ToString(), "editor camera pose is invalid");
	const entt::entity entity = m_Authoring.FindByUuid(cameraEntity);
	if (entity == entt::null || !m_EcsScene.registry.valid(entity) ||
		!m_EcsScene.registry.all_of<Transform, CameraComponent>(entity))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			cameraEntity.ToString(), "selected entity is not a camera");

	EditableTRS currentWorld;
	if (!GetWorldTransform({ entity }, currentWorld))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidTransform,
			cameraEntity.ToString(), "camera world transform is not representable as TRS");
	glm::quat rotation;
	if (!TryCameraRotationFromForward(pose.forward, rotation))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidTransform,
			cameraEntity.ToString(), "camera forward vector is invalid");
	EditableTRS desired = currentWorld;
	desired.translation = pose.position;
	desired.rotation = rotation;
	if (!TrySetWorldTransform({ entity }, desired.Matrix()))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidTransform,
			cameraEntity.ToString(),
			"camera alignment was rejected; its parent may be singular or non-uniformly scaled");

	auto& component = m_EcsScene.registry.get<CameraComponent>(entity);
	component.verticalFOV = pose.verticalFOV;
	component.aperture = pose.aperture;
	component.focusDistance = pose.focusDistance;
	EditorMutationResult result;
	result.syncImpact = rt2::core::SyncImpact::Transform;
	result.affectedEntities.push_back(cameraEntity);
	return result;
}

void SceneManager::RefreshCameraForwardDirections(
	const std::vector<entt::entity>& roots)
{
	if (roots.empty()) return;
	auto& registry = m_EcsScene.registry;
	SceneGraph::UpdateWorldTransforms(registry);
	std::unordered_set<entt::entity> visited;
	for (const entt::entity root : roots)
	{
		if (!registry.valid(root)) continue;
		std::vector<entt::entity> subtree;
		SceneHierarchy::CollectSubtreePreOrder(registry, root, subtree);
		for (const entt::entity entity : subtree)
		{
			if (!visited.insert(entity).second ||
				!registry.all_of<CameraComponent, Transform>(entity))
				continue;
			EditableTRS world;
			if (!TryDecomposeEditableTRS(registry.get<Transform>(entity).worldMatrix, world))
				continue;
			const glm::vec3 forward =
				world.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
			if (glm::dot(forward, forward) > 1e-8f)
				registry.get<CameraComponent>(entity).forwardDirection =
					glm::normalize(forward);
		}
	}
}

void SceneManager::ReconcileStoredCameraDirections()
{
	auto& registry = m_EcsScene.registry;
	SceneGraph::UpdateWorldTransforms(registry);
	std::vector<entt::entity> cameras;
	const auto view = registry.view<CameraComponent, Transform>();
	for (const entt::entity entity : view)
	{
		const auto& camera = view.get<CameraComponent>(entity);
		glm::quat rotation;
		if (!TryCameraRotationFromForward(camera.forwardDirection, rotation))
			continue;
		EditableTRS currentWorld;
		if (!TryDecomposeEditableTRS(view.get<Transform>(entity).worldMatrix,
			currentWorld))
			continue;
		currentWorld.rotation = rotation;
		glm::mat4 parentWorld(1.0f);
		if (const auto* hierarchy = registry.try_get<Hierarchy>(entity);
			hierarchy && hierarchy->parent != entt::null)
		{
			const auto* parentTransform = registry.try_get<Transform>(hierarchy->parent);
			if (!parentTransform) continue;
			parentWorld = parentTransform->worldMatrix;
		}
		EditableTRS local;
		if (!TryWorldToLocalTRS(parentWorld, currentWorld.Matrix(), local))
			continue;
		auto& transform = view.get<Transform>(entity);
		transform.translation = local.translation;
		transform.rotation = glm::normalize(local.rotation);
		transform.scale = local.scale;
		SceneGraph::MarkDirty(registry, entity);
		cameras.push_back(entity);
	}
	RefreshCameraForwardDirections(cameras);
}

void SceneManager::SetMaterial(EntityId entity, int materialIndex,
                               std::optional<MaterialOverrideComponent>* outBeforeOverride,
                               std::optional<MaterialOverrideComponent>* outAfterOverride)
{
	if (!entity.IsValid()) return;
	// SetMaterialIndexState returns an authoritative EditorMutationResult and
	// can legitimately fail — an out-of-range index is the common case, since
	// it is bounds-checked against the material list. Dropping that result
	// made a rejected assignment indistinguishable from an applied one.
	// This wrapper is void by design (the inspector calls it fire-and-forget),
	// so surface the failure rather than returning it. The capture out-params
	// come from SetMaterialIndexState so the before/after override ordering
	// is enforced inside the mutation, not at this call site.
	const auto result = SetMaterialIndexState(GetEntityUuid(entity), materialIndex,
	                                          outBeforeOverride, outAfterOverride);
	if (!result.success)
		printf("[SceneManager] SetMaterial rejected: %s\n",
		       result.error.Format().c_str());
}

std::string SceneManager::GetEntityName(EntityId entity) const
{
	if (!entity.IsValid()) return "";
	if (!m_EcsScene.registry.valid(entity.id)) return "";
	auto& reg = m_EcsScene.registry;
	if (auto* name = reg.try_get<NameComponent>(entity.id))
		return name->name;
	return "";
}

void SceneManager::SetEntityName(EntityId entity, const std::string& name)
{
	if (!entity.IsValid()) return;
	SetEntityNameState(GetEntityUuid(entity), name);
}

size_t SceneManager::GetEntityCount() const
{
	// Use a component view (NameComponent or Transform) as a proxy.
	// Most entities have at least one of these. For an exact count we'd
	// need registry.storage<entt::entity>().size(), but that's not
	// public in this entt version. Use Transform as the common component.
	return m_EcsScene.registry.view<Transform>().size();
}

SceneManager::EntityId SceneManager::GetEntityByIndex(size_t index) const
{
	if (m_EntityCacheDirty)
	{
		m_EntityCache.clear();
		auto view = m_EcsScene.registry.view<Transform>();
		for (auto entity : view)
			m_EntityCache.push_back(entity);
		m_EntityCacheDirty = false;
	}
	if (index >= m_EntityCache.size()) return {entt::null};
	return {m_EntityCache[index]};
}

std::vector<SceneManager::EntityId> SceneManager::GetRootEntities() const
{
	std::vector<EntityId> roots;
	auto view = m_EcsScene.registry.view<Transform>();
	for (auto entity : view)
	{
		auto* h = m_EcsScene.registry.try_get<Hierarchy>(entity);
		if (!h || h->parent == entt::null)
			roots.push_back({entity});
	}
	return roots;
}

// ============================================================================
// Entity queries (for inspector UI)
// ============================================================================

bool SceneManager::HasMeshRef(EntityId entity) const
{
	if (!entity.IsValid()) return false;
	if (!m_EcsScene.registry.valid(entity.id)) return false;
	return m_EcsScene.registry.try_get<MeshRef>(entity.id) != nullptr;
}

bool SceneManager::HasLight(EntityId entity) const
{
	if (!entity.IsValid()) return false;
	if (!m_EcsScene.registry.valid(entity.id)) return false;
	return m_EcsScene.registry.try_get<LightComponent>(entity.id) != nullptr;
}

bool SceneManager::HasScript(EntityId entity) const
{
	if (!entity.IsValid()) return false;
	if (!m_EcsScene.registry.valid(entity.id)) return false;
	return m_EcsScene.registry.try_get<ScriptComponent>(entity.id) != nullptr;
}

std::optional<ScriptComponent>
SceneManager::GetScriptState(const rt2::core::UUID& entity) const
{
	const auto e = m_Authoring.FindByUuid(entity);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return std::nullopt;
	if (auto* sc = m_EcsScene.registry.try_get<ScriptComponent>(e))
		return *sc;
	return std::nullopt;
}

bool SceneManager::HasTransform(EntityId entity) const
{
	if (!entity.IsValid()) return false;
	if (!m_EcsScene.registry.valid(entity.id)) return false;
	return m_EcsScene.registry.try_get<Transform>(entity.id) != nullptr;
}

bool SceneManager::HasCamera(EntityId entity) const
{
	if (!entity.IsValid()) return false;
	if (!m_EcsScene.registry.valid(entity.id)) return false;
	return m_EcsScene.registry.try_get<CameraComponent>(entity.id) != nullptr;
}

bool SceneManager::IsEntityAlive(EntityId entity) const
{
	if (!entity.IsValid()) return false;
	return m_EcsScene.registry.valid(entity.id);
}

bool SceneManager::GetTransform(EntityId entity, glm::vec3& outPosition, glm::vec3& outRotationEuler, float& outScale) const
{
	if (!entity.IsValid()) return false;
	if (!m_EcsScene.registry.valid(entity.id)) return false;
	auto* tf = m_EcsScene.registry.try_get<Transform>(entity.id);
	if (!tf) return false;
	outPosition = tf->translation;
	outRotationEuler = glm::degrees(glm::eulerAngles(tf->rotation));
	outScale = tf->scale.x;
	return true;
}

bool SceneManager::GetTransform(EntityId entity, glm::vec3& outPosition,
	glm::vec3& outRotationEuler, glm::vec3& outScale) const
{
	EditableTRS transform;
	if (!GetLocalTransform(entity, transform)) return false;
	outPosition = transform.translation;
	outRotationEuler = glm::degrees(glm::eulerAngles(transform.rotation));
	outScale = transform.scale;
	return true;
}

bool SceneManager::GetLocalTransform(EntityId entity, EditableTRS& outTransform) const
{
	if (!entity.IsValid() || !m_EcsScene.registry.valid(entity.id)) return false;
	const auto* transform = m_EcsScene.registry.try_get<Transform>(entity.id);
	if (!transform) return false;
	outTransform.translation = transform->translation;
	outTransform.rotation = transform->rotation;
	outTransform.scale = transform->scale;
	return true;
}

bool SceneManager::GetWorldTransform(EntityId entity, EditableTRS& outTransform)
{
	if (!entity.IsValid() || !m_EcsScene.registry.valid(entity.id)) return false;
	UpdateWorldTransforms();
	const auto* transform = m_EcsScene.registry.try_get<Transform>(entity.id);
	return transform && TryDecomposeEditableTRS(transform->worldMatrix, outTransform);
}

bool SceneManager::GetLightProperties(EntityId entity, glm::vec3& outColor, float& outIntensity, LightType& outType) const
{
	if (!entity.IsValid()) return false;
	if (!m_EcsScene.registry.valid(entity.id)) return false;
	auto* light = m_EcsScene.registry.try_get<LightComponent>(entity.id);
	if (!light) return false;
	outColor = light->color;
	outIntensity = light->intensity;
	outType = light->type;
	return true;
}

void SceneManager::SetLightProperties(EntityId entity, const glm::vec3& color, float intensity, LightType type)
{
	if (!entity.IsValid()) return;
	LightComponent value;
	if (auto* light = m_EcsScene.registry.try_get<LightComponent>(entity.id))
		value = *light;
	value.color = color;
	value.intensity = intensity;
	value.type = type;
	SetLightPropertiesState(GetEntityUuid(entity), value);
}

bool SceneManager::GetLightComponent(EntityId entity, LightComponent& outValue) const
{
	if (!entity.IsValid()) return false;
	if (!m_EcsScene.registry.valid(entity.id)) return false;
	auto* light = m_EcsScene.registry.try_get<LightComponent>(entity.id);
	if (!light) return false;
	outValue = *light;
	return true;
}

void SceneManager::SetLightComponent(EntityId entity, const LightComponent& value)
{
	if (!entity.IsValid()) return;
	SetLightPropertiesState(GetEntityUuid(entity), value);
}

bool SceneManager::SetCameraProperties(EntityId entity, float verticalFOV,
	float aperture, float focusDistance)
{
	if (!entity.IsValid() || !m_EcsScene.registry.valid(entity.id)) return false;
	CameraComponent value;
	if (auto* camera = m_EcsScene.registry.try_get<CameraComponent>(entity.id))
		value = *camera;
	else
		return false;
	value.verticalFOV = verticalFOV;
	value.aperture = aperture;
	value.focusDistance = focusDistance;
	const auto result = SetCameraPropertiesState(GetEntityUuid(entity), value);
	return result.success;
}

bool SceneManager::GetMeshRef(EntityId entity, uint32_t& outMeshIndex, int& outMaterialIndex) const
{
	if (!entity.IsValid()) return false;
	if (!m_EcsScene.registry.valid(entity.id)) return false;
	auto* ref = m_EcsScene.registry.try_get<MeshRef>(entity.id);
	if (!ref) return false;
	outMeshIndex = ref->meshIndex;
	outMaterialIndex = ref->materialIndex;
	return true;
}

void SceneManager::SetMeshRefMeshIndex(EntityId entity, uint32_t meshIndex)
{
	if (!entity.IsValid()) return;
	auto* ref = m_EcsScene.registry.try_get<MeshRef>(entity.id);
	if (!ref) return;
	ref->meshIndex = meshIndex;
	NotifyAuthoringChanged();
}

void SceneManager::SyncTransformsToGPU()
{
	if (m_EcsScene.meshRegistry.GetCount() == 0) return;

	UpdateWorldTransforms();

	GPUSceneData gpuData = m_CurrentGpuScene;
	UpdateInstancesFromECS(gpuData, m_EcsScene, &m_RenderInstanceMap);

	if (m_InstanceSyncCallback)
		m_InstanceSyncCallback(gpuData, m_RenderInstanceMap);

	m_CurrentGpuScene = gpuData;
}

// ============================================================================
// Hierarchy queries (for tree view outliner)
// ============================================================================

bool SceneManager::HasChildren(EntityId entity) const
{
	if (!entity.IsValid()) return false;
	if (!m_EcsScene.registry.valid(entity.id)) return false;
	auto* h = m_EcsScene.registry.try_get<Hierarchy>(entity.id);
	return h && !h->children.empty();
}

std::vector<SceneManager::EntityId> SceneManager::GetChildren(EntityId entity) const
{
	std::vector<EntityId> result;
	if (!entity.IsValid()) return result;
	if (!m_EcsScene.registry.valid(entity.id)) return result;
	auto* h = m_EcsScene.registry.try_get<Hierarchy>(entity.id);
	if (!h) return result;
	result.reserve(h->children.size());
	for (auto child : h->children)
	{
		if (m_EcsScene.registry.valid(child))
			result.push_back({child});
	}
	return result;
}

SceneManager::EntityId SceneManager::GetParent(EntityId entity) const
{
	if (!entity.IsValid()) return {entt::null};
	if (!m_EcsScene.registry.valid(entity.id)) return {entt::null};
	auto* h = m_EcsScene.registry.try_get<Hierarchy>(entity.id);
	if (!h || h->parent == entt::null) return {entt::null};
	if (!m_EcsScene.registry.valid(h->parent)) return {entt::null};
	return {h->parent};
}

// ============================================================================
// Material + texture management
// ============================================================================

int SceneManager::AddMaterial(const SceneMaterial& material)
{
	int idx = (int)m_EcsScene.materials.size();
	m_EcsScene.materials.push_back(material);
	NotifyAuthoringChanged();
	return idx;
}

SceneMaterial& SceneManager::GetMaterial(int index)
{
	if (index >= 0 && index < (int)m_EcsScene.materials.size())
		return m_EcsScene.materials[index];
	static SceneMaterial dummy;
	return dummy;
}

void SceneManager::SetMaterialProperties(int index, const SceneMaterial& props)
{
	SetMaterialPropertiesState(index, props);
}

void SceneManager::RecordMaterialOverride(entt::entity entity, int materialIndex)
{
	auto& reg = m_EcsScene.registry;
	if (!reg.valid(entity)) return;
	if (materialIndex < 0 || materialIndex >= (int)m_EcsScene.materials.size())
		return;

	// Mint the durable source material key from the material's loader-
	// surfaced identity (SceneMaterial::sourceKey; Phase 8 pre-work 2 D1).
	// Author-created materials carry no identity, so the key is empty and
	// the resolver falls back to slot-position behavior without a
	// diagnostic — D3's diagnostic is for a key that exists but does not
	// match, not for an absent one.
	const std::string sourceMatKey = m_EcsScene.materials[materialIndex].sourceKey;

	MaterialOverrideComponent ov;
	ov.material        = m_EcsScene.materials[materialIndex];
	ov.authored        = true;
	ov.sourceMaterialKey = sourceMatKey;
	ov.materialIndex   = materialIndex; // transient; repaired by resolver
	reg.emplace_or_replace<MaterialOverrideComponent>(entity, ov);
}

void SceneManager::NotifyAuthoringChanged()
{
	m_Authoring.metadata.dirty = true;
	++m_AuthoringRevision;
}

// ============================================================================
// Phase 3B2 atomic property state APIs. Each applies the after-value, bumps
// the revision ONCE, and returns an authoritative EditorMutationResult.
// Material APIs also capture/restore MaterialOverrideComponent atomically so
// Undo of an imported-entity material assignment does not leave a stale
// override that save/reopen would resurrect.
// ============================================================================

EditorMutationResult SceneManager::SetEntityNameState(const rt2::core::UUID& entity,
                                                      const std::string& name)
{
	const auto e = m_Authoring.FindByUuid(entity);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetEntityNameState: entity not present");
	if (auto* nc = m_EcsScene.registry.try_get<NameComponent>(e))
		nc->name = name;
	else
		m_EcsScene.registry.emplace<NameComponent>(e, name);
	NotifyAuthoringChanged();
	EditorMutationResult result;
	result.success = true;
	result.syncImpact = rt2::core::SyncImpact::None;
	result.affectedEntities.push_back(entity);
	return result;
}

EditorMutationResult SceneManager::SetLightPropertiesState(const rt2::core::UUID& entity,
                                                           const LightComponent& value)
{
	const auto e = m_Authoring.FindByUuid(entity);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetLightPropertiesState: entity not present");
	auto* light = m_EcsScene.registry.try_get<LightComponent>(e);
	if (!light)
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetLightPropertiesState: entity has no LightComponent");
	*light = value;
	NotifyAuthoringChanged();
	EditorMutationResult result;
	result.success = true;
	result.syncImpact = rt2::core::SyncImpact::Material; // keep-textures path
	result.affectedEntities.push_back(entity);
	return result;
}

EditorMutationResult SceneManager::SetCameraPropertiesState(const rt2::core::UUID& entity,
                                                            const CameraComponent& value)
{
	const auto e = m_Authoring.FindByUuid(entity);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetCameraPropertiesState: entity not present");
	auto* camera = m_EcsScene.registry.try_get<CameraComponent>(e);
	if (!camera)
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetCameraPropertiesState: entity has no CameraComponent");
	*camera = value;
	NotifyAuthoringChanged();
	EditorMutationResult result;
	result.success = true;
	result.syncImpact = rt2::core::SyncImpact::None;
	result.affectedEntities.push_back(entity);
	return result;
}

EditorMutationResult SceneManager::SetMaterialPropertiesState(int slotIndex,
                                                              const SceneMaterial& value)
{
	if (slotIndex < 0 || slotIndex >= (int)m_EcsScene.materials.size())
		return EditorMutationResult::Failure(rt2::core::Error::InvalidArgument,
			std::to_string(slotIndex), "SetMaterialPropertiesState: slot index out of range");
	m_EcsScene.materials[slotIndex] = value;

	// Propagate the edit into durable MaterialOverrideComponent on every
	// imported entity whose MeshRef points at this material slot, so saved
	// material edits survive reopen.
	{
		auto& reg = m_EcsScene.registry;
		auto view = reg.view<ImportedMeshSourceComponent>();
		for (auto e : view)
		{
			auto* ref = reg.try_get<MeshRef>(e);
			if (ref && ref->materialIndex == slotIndex)
				RecordMaterialOverride(e, slotIndex);
		}
	}

	NotifyAuthoringChanged();
	EditorMutationResult result;
	result.success = true;
	result.syncImpact = rt2::core::SyncImpact::Material;
	return result;
}

EditorMutationResult SceneManager::SetMaterialIndexState(const rt2::core::UUID& entity,
                                                         int afterIndex,
                                                         std::optional<MaterialOverrideComponent>* outBeforeOverride,
                                                         std::optional<MaterialOverrideComponent>* outAfterOverride)
{
	const auto e = m_Authoring.FindByUuid(entity);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetMaterialIndexState: entity not present");
	auto* ref = m_EcsScene.registry.try_get<MeshRef>(e);
	if (!ref)
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetMaterialIndexState: entity has no MeshRef");
	if (afterIndex < 0 || afterIndex >= (int)m_EcsScene.materials.size())
		return EditorMutationResult::Failure(rt2::core::Error::InvalidArgument,
			std::to_string(afterIndex), "SetMaterialIndexState: material index out of range");
	// Capture the displaced durable override BEFORE the mutation: the index
	// write below replaces it, so any read after this point sees the
	// after-state. This ordering is the fix for the 2026-08-03 material-index
	// undo defect — the host UI used to read the before-override after
	// SetMaterial had already run, making the command's two snapshots
	// identical and Undo restore the post-edit record.
	std::optional<MaterialOverrideComponent> beforeCapture;
	if (auto* ov = m_EcsScene.registry.try_get<MaterialOverrideComponent>(e))
		beforeCapture = *ov;
	ref->materialIndex = afterIndex;
	if (m_EcsScene.registry.all_of<ImportedMeshSourceComponent>(e))
		RecordMaterialOverride(e, afterIndex);
	if (outBeforeOverride)
		*outBeforeOverride = std::move(beforeCapture);
	if (outAfterOverride)
	{
		if (auto* ov = m_EcsScene.registry.try_get<MaterialOverrideComponent>(e))
			*outAfterOverride = *ov;
		else
			*outAfterOverride = std::nullopt;
	}
	NotifyAuthoringChanged();
	EditorMutationResult result;
	result.success = true;
	result.syncImpact = rt2::core::SyncImpact::Material;
	result.affectedEntities.push_back(entity);
	return result;
}

EditorMutationResult SceneManager::SetMotionState(const rt2::core::UUID& entity,
                                                  const std::optional<MotionComponent>& value)
{
	const auto e = m_Authoring.FindByUuid(entity);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetMotionState: entity not present");
	if (value.has_value())
		m_EcsScene.registry.emplace_or_replace<MotionComponent>(e, *value);
	else
	{
		if (m_EcsScene.registry.all_of<MotionComponent>(e))
			m_EcsScene.registry.remove<MotionComponent>(e);
	}
	NotifyAuthoringChanged();
	EditorMutationResult result;
	result.success = true;
	result.syncImpact = rt2::core::SyncImpact::None;
	result.affectedEntities.push_back(entity);
	return result;
}

EditorMutationResult SceneManager::SetScriptState(const rt2::core::UUID& entity,
                                                  const std::optional<ScriptComponent>& value)
{
	const auto e = m_Authoring.FindByUuid(entity);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetScriptState: entity not present");
	if (value.has_value())
	{
		ScriptComponent canonical;
		std::string detail;
		std::string field;
		if (!rt2::core::NormalizeAndValidateScriptComponent(
				*value, canonical, detail, &field))
		{
			return EditorMutationResult::Failure(
				rt2::core::Error::InvalidArgument,
				field.empty() ? entity.ToString()
				              : entity.ToString() + ":" + field,
				"SetScriptState: " + detail);
		}

		// Suppress canonical no-ops: present→same-present must not dirty the
		// document, bump the revision, or notify observers (W4-F1). The
		// caller's value has already been canonicalized above, so compare
		// against the currently stored component.
		std::optional<ScriptComponent> current;
		if (m_EcsScene.registry.all_of<ScriptComponent>(e))
			current = m_EcsScene.registry.get<ScriptComponent>(e);

		// Binding a script is its explicit import operation. Assign/reuse the
		// per-asset sidecar ID here, matching model/environment import while
		// keeping the shared locator read-only (W3-Q9). A changed path never
		// carries the previous file's ID. Missing paths remain bindable so the
		// Phase 6 quarantine and watcher-recovery behavior is preserved.
		if (canonical.asset.path.empty())
		{
			canonical.asset.assetId = rt2::core::UUID::Nil();
		}
		else
		{
			const bool sameBinding = current.has_value() &&
				current->asset.path == canonical.asset.path;
			if (!sameBinding)
				canonical.asset.assetId = rt2::core::UUID::Nil();
			else if (canonical.asset.assetId.IsNull())
				canonical.asset.assetId = current->asset.assetId;

			rt2::core::AssetResolutionContext context = m_AssetResolutionContext;
			if (context.assetRoot.empty() &&
				!m_Authoring.metadata.sourcePath.empty())
				context.assetRoot = m_Authoring.metadata.sourcePath.
					parent_path().lexically_normal();
			std::vector<rt2::core::AssetDiagnostic> diagnostics;
			const auto resolved = rt2::core::ResolveScriptAssetPath(
				canonical, context, entity,
				GetEntityName({ e }), diagnostics);

			const bool conflict = std::any_of(
				diagnostics.begin(), diagnostics.end(),
				[](const rt2::core::AssetDiagnostic& diagnostic) {
					return diagnostic.severity ==
						rt2::core::AssetDiagnostic::Conflict;
				});
			if (conflict)
			{
				return EditorMutationResult::Failure(
					rt2::core::Error::InvalidArgument,
					canonical.asset.path,
					"SetScriptState: script asset identity conflict");
			}

			if (resolved.success &&
				!resolved.effectiveId.IsNull())
				canonical.asset.assetId = resolved.effectiveId;

			if (resolved.success &&
				resolved.identityRepairRequired &&
				canonical.asset.assetId.IsNull())
			{
				bool minted = false;
				rt2::core::Error idError;
				canonical.asset.assetId = rt2::core::ResolveOrAssign(
					resolved.resolvedPath, *m_UuidProvider,
					minted, idError);
				if (minted || !idError.IsOk())
				{
					printf("[Asset] %s: assigned script id %s%s%s\n",
					       resolved.resolvedPath.string().c_str(),
					       canonical.asset.assetId.ToString().c_str(),
					       idError.IsOk() ? "" : ": ",
					       idError.IsOk() ? "" : idError.Format().c_str());
					fflush(stdout);
				}
			}
		}

		if (rt2::core::ScriptComponentCanonicalEqual(
				current, std::optional<ScriptComponent>{canonical}))
		{
			EditorMutationResult result;
			result.success = true;
			result.effective = false;
			result.syncImpact = rt2::core::SyncImpact::None;
			return result;
		}

		m_EcsScene.registry.emplace_or_replace<ScriptComponent>(e,
			std::move(canonical));
	}
	else
	{
		// Suppress absent→absent removal (W4-F1).
		if (!m_EcsScene.registry.all_of<ScriptComponent>(e))
		{
			EditorMutationResult result;
			result.success = true;
			result.effective = false;
			result.syncImpact = rt2::core::SyncImpact::None;
			return result;
		}
		m_EcsScene.registry.remove<ScriptComponent>(e);
	}
	NotifyAuthoringChanged();
	EditorMutationResult result;
	result.success = true;
	// D8: script bindings and field values never reach the GPU scene.
	result.syncImpact = rt2::core::SyncImpact::None;
	result.affectedEntities.push_back(entity);
	return result;
}

EditorMutationResult SceneManager::SetCameraPoseState(const rt2::core::UUID& entity,
                                                      const EditableTRS& local,
                                                      const CameraComponent& props)
{
	const auto e = m_Authoring.FindByUuid(entity);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetCameraPoseState: entity not present");
	if (!m_EcsScene.registry.all_of<Transform, CameraComponent>(e))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetCameraPoseState: entity is not a camera");

	auto& tf = m_EcsScene.registry.get<Transform>(e);
	tf.translation = local.translation;
	tf.rotation = glm::normalize(local.rotation);
	tf.scale = local.scale;
	SceneGraph::MarkDirty(m_EcsScene.registry, e);

	auto& camera = m_EcsScene.registry.get<CameraComponent>(e);
	camera = props;

	RefreshCameraForwardDirections({ e });
	NotifyAuthoringChanged();

	EditorMutationResult result;
	result.success = true;
	result.syncImpact = rt2::core::SyncImpact::Transform;
	result.affectedEntities.push_back(entity);
	return result;
}

std::optional<MaterialOverrideComponent> SceneManager::GetMaterialOverride(
	const rt2::core::UUID& entity) const
{
	const auto e = m_Authoring.FindByUuid(entity);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return std::nullopt;
	const auto* ov = m_EcsScene.registry.try_get<MaterialOverrideComponent>(e);
	if (!ov)
		return std::nullopt;
	return *ov;
}

void SceneManager::InstallMaterialOverride(
	const rt2::core::UUID& entity,
	const std::optional<MaterialOverrideComponent>& override)
{
	const auto e = m_Authoring.FindByUuid(entity);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return;
	if (override.has_value())
		m_EcsScene.registry.emplace_or_replace<MaterialOverrideComponent>(e, *override);
	else if (m_EcsScene.registry.all_of<MaterialOverrideComponent>(e))
		m_EcsScene.registry.remove<MaterialOverrideComponent>(e);
}

// ============================================================================
// Phase 8 W3 S5: prefab override query + staged marker helper
// ============================================================================

namespace
{
rt2::core::Error S5QueryError(rt2::core::Error::Code code,
                              const rt2::core::UUID& member,
                              const std::string& detail)
{
	rt2::core::Error err;
	err.code = code;
	err.path = member.ToString();
	err.detail = detail;
	return err;
}

// Canonicalize a raw override vector by frozen-table identity. Every stored key
// is re-resolved through the frozen table (the stored classification bit is
// never trusted), then sorted by wire and de-duplicated by wire. Unknown or
// non-overridable (excluded) stored wires are a loud failure: returns false and
// fills `err` without modifying `out` — the malformed-vector policy.
bool S5CanonicalizeVector(const std::vector<PrefabComponentKey>& raw,
                          std::vector<PrefabComponentKey>& out,
                          rt2::core::Error& err)
{
	out.clear();
	out.reserve(raw.size());
	for (const auto& key : raw)
	{
		const auto canonical = FindComponentByWire(key.wire());
		if (!canonical)
		{
			err.code = rt2::core::Error::InvalidArgument;
			err.detail = "stored override key wire '" + std::string(key.wire())
				+ "' is unknown (not in the frozen table)";
			return false;
		}
		if (!canonical->overridable())
		{
			err.code = rt2::core::Error::InvalidArgument;
			err.detail = "stored override key wire '" + std::string(key.wire())
				+ "' is non-overridable (excluded from override marking)";
			return false;
		}
		out.push_back(*canonical); // canonical table entry, bit re-derived
	}
	std::sort(out.begin(), out.end(),
	          [](const PrefabComponentKey& a, const PrefabComponentKey& b)
	          { return a.wire() < b.wire(); });
	out.erase(std::unique(out.begin(), out.end(),
	           [](const PrefabComponentKey& a, const PrefabComponentKey& b)
	           { return a.wire() == b.wire(); }),
	          out.end());
	return true;
}
} // namespace

rt2::core::Result<bool> SceneManager::IsOverridden(
	const rt2::core::UUID& member, const PrefabComponentKey& key) const
{
	const auto e = m_Authoring.FindByUuid(member);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return rt2::core::Result<bool>::Fail(rt2::core::Error::InvalidEntity,
			member.ToString(), "entity UUID is not present in the authoring scene");
	const auto* pm = m_EcsScene.registry.try_get<PrefabMemberComponent>(e);
	if (!pm)
		return rt2::core::Result<bool>::Fail(rt2::core::Error::NotPrefabMember,
			member.ToString(), "entity is not a prefab member");

	const auto canonical = FindComponentByWire(key.wire());
	if (!canonical)
		return rt2::core::Result<bool>::Fail(rt2::core::Error::InvalidArgument,
			member.ToString(),
			"unknown override key wire '" + std::string(key.wire()) + "'");
	if (!canonical->overridable())
		return rt2::core::Result<bool>::Fail(rt2::core::Error::InvalidArgument,
			member.ToString(),
			"non-overridable (excluded) override key wire '"
			+ std::string(key.wire()) + "'");

	rt2::core::Error vecErr;
	std::vector<PrefabComponentKey> set;
	if (!S5CanonicalizeVector(pm->overrides, set, vecErr))
		return rt2::core::Result<bool>::Fail(vecErr.code, member.ToString(),
			vecErr.detail);

	const bool present = std::find(set.begin(), set.end(), *canonical) != set.end();
	return rt2::core::Result<bool>::Ok(present);
}

rt2::core::Result<std::vector<PrefabComponentKey>> SceneManager::GetOverrides(
	const rt2::core::UUID& member) const
{
	const auto e = m_Authoring.FindByUuid(member);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return rt2::core::Result<std::vector<PrefabComponentKey>>::Fail(
			rt2::core::Error::InvalidEntity, member.ToString(),
			"entity UUID is not present in the authoring scene");
	const auto* pm = m_EcsScene.registry.try_get<PrefabMemberComponent>(e);
	if (!pm)
		return rt2::core::Result<std::vector<PrefabComponentKey>>::Fail(
			rt2::core::Error::NotPrefabMember, member.ToString(),
			"entity is not a prefab member");

	rt2::core::Error vecErr;
	std::vector<PrefabComponentKey> set;
	if (!S5CanonicalizeVector(pm->overrides, set, vecErr))
		return rt2::core::Result<std::vector<PrefabComponentKey>>::Fail(
			vecErr.code, member.ToString(), vecErr.detail);
	return rt2::core::Result<std::vector<PrefabComponentKey>>::Ok(std::move(set));
}

rt2::core::Result<PrefabMarkerPlan> SceneManager::PreparePrefabMarkerEdits(
	const std::vector<PrefabMarkerEdit>& edits,
	PrefabMarkerDirection direction,
	std::uint32_t beforeSchemaVersion,
	std::uint32_t afterSchemaVersion)
{
	// Zero-mutation staging: everything below reads and validates live state
	// into a durable plan; nothing is written until CommitPrefabMarkerPlan.
	PrefabMarkerPlan plan;
	plan.direction = direction;
	// Directional transport of the command-captured schema pair: the After
	// direction targets the command-after schema (execute), the Before
	// direction targets the command-before schema (undo/restore). Commit always
	// writes targetSchemaVersion.
	plan.sourceSchemaVersion = (direction == PrefabMarkerDirection::After)
		? beforeSchemaVersion : afterSchemaVersion;
	plan.targetSchemaVersion = (direction == PrefabMarkerDirection::After)
		? afterSchemaVersion : beforeSchemaVersion;

	const bool targetAfter = (direction == PrefabMarkerDirection::After);

	struct Staged
	{
		rt2::core::UUID member;
		entt::entity entity = entt::null;
		std::vector<PrefabComponentKey> live;      // canonical pre-batch set
		std::vector<PrefabComponentKey> pendingKeys;
		std::vector<bool>                 pendingPresent; // target presence per key
	};
	std::vector<Staged> staged;
	std::unordered_map<entt::entity, std::size_t> memberIndex;

	for (const auto& edit : edits)
	{
		// Member identity: durable UUID resolved through the authoring index.
		const auto e = m_Authoring.FindByUuid(edit.member);
		if (e == entt::null || !m_EcsScene.registry.valid(e))
			return rt2::core::Result<PrefabMarkerPlan>::Fail(
				rt2::core::Error::InvalidEntity, edit.member.ToString(),
				"entity UUID is not present in the authoring scene");
		const auto* pm = m_EcsScene.registry.try_get<PrefabMemberComponent>(e);
		if (!pm)
			return rt2::core::Result<PrefabMarkerPlan>::Fail(
				rt2::core::Error::NotPrefabMember, edit.member.ToString(),
				"entity is not a prefab member");

		// Canonical key: resolve the wire through the frozen table. The caller's
		// overridable bit is never trusted and the caller's object is never
		// stored — the resolved table entry is what gets staged.
		const auto canonical = FindComponentByWire(edit.key.wire());
		if (!canonical)
			return rt2::core::Result<PrefabMarkerPlan>::Fail(
				rt2::core::Error::InvalidArgument, edit.member.ToString(),
				"unknown override key wire '" + std::string(edit.key.wire()) + "'");
		if (!canonical->overridable())
			return rt2::core::Result<PrefabMarkerPlan>::Fail(
				rt2::core::Error::InvalidArgument, edit.member.ToString(),
				"non-overridable (excluded) override key wire '"
				+ std::string(edit.key.wire()) + "'");

		// Lazily canonicalize the member's raw vector (with loud malformed
		// rejection) once per member.
		auto it = memberIndex.find(e);
		std::size_t mi = 0;
		if (it == memberIndex.end())
		{
			rt2::core::Error vecErr;
			std::vector<PrefabComponentKey> live;
			if (!S5CanonicalizeVector(pm->overrides, live, vecErr))
				return rt2::core::Result<PrefabMarkerPlan>::Fail(
					vecErr.code, edit.member.ToString(),
					"malformed stored override vector: " + vecErr.detail);
			mi = staged.size();
			memberIndex.emplace(e, mi);
			staged.push_back(Staged{ edit.member, e, std::move(live), {}, {} });
		}
		else
			mi = it->second;
		Staged& member = staged[mi];

		const bool livePresent = std::find(member.live.begin(), member.live.end(),
			*canonical) != member.live.end();

		// Directional source validation: the non-target side of the payload must
		// equal the one pre-batch snapshot, so a stale or hand-forged payload is
		// a loud zero-change failure rather than a silently ignored boolean.
		const bool expectedSource = targetAfter ? edit.beforePresent : edit.afterPresent;
		if (expectedSource != livePresent)
			return rt2::core::Result<PrefabMarkerPlan>::Fail(
				rt2::core::Error::InvalidArgument, edit.member.ToString(),
				std::string(targetAfter ? "beforePresent" : "afterPresent")
				+ " does not match live membership (expected "
				+ (expectedSource ? "present" : "absent")
				+ ", live " + (livePresent ? "present" : "absent")
				+ ") for wire '" + std::string(canonical->wire()) + "'");

		const bool targetPresent = targetAfter ? edit.afterPresent : edit.beforePresent;

		// Duplicate policy, input-order-independent: byte-identical duplicates
		// for the same (member, canonical wire) coalesce; a contradictory
		// duplicate fails the whole batch regardless of the input order.
		const auto existing = std::find_if(member.pendingKeys.begin(), member.pendingKeys.end(),
			[&](const PrefabComponentKey& k) { return k == *canonical; });
		if (existing != member.pendingKeys.end())
		{
			const std::size_t idx = static_cast<std::size_t>(existing - member.pendingKeys.begin());
			if (member.pendingPresent[idx] != targetPresent)
				return rt2::core::Result<PrefabMarkerPlan>::Fail(
					rt2::core::Error::InvalidArgument, edit.member.ToString(),
					"contradictory duplicate edits for wire '"
					+ std::string(canonical->wire()) + "' (target presence differs)");
			continue; // byte-identical duplicate: coalesce
		}
		member.pendingKeys.push_back(*canonical);
		member.pendingPresent.push_back(targetPresent);
	}

	// Build the durable per-member transitions from the canonical snapshot.
	plan.anyStateChange = false;
	for (const auto& member : staged)
	{
		std::vector<PrefabComponentKey> after = member.live;
		for (std::size_t i = 0; i < member.pendingKeys.size(); ++i)
		{
			const PrefabComponentKey& key = member.pendingKeys[i];
			if (member.pendingPresent[i])
			{
				auto pos = std::lower_bound(after.begin(), after.end(), key,
					[](const PrefabComponentKey& a, const PrefabComponentKey& b)
					{ return a.wire() < b.wire(); });
				if (pos == after.end() || pos->wire() != key.wire())
					after.insert(pos, key);
			}
			else
			{
				auto it2 = std::remove(after.begin(), after.end(), key);
				if (it2 != after.end())
					after.erase(it2, after.end());
			}
		}

		PrefabMarkerPlan::MemberTransition transition;
		transition.member = member.member;
		transition.source = member.live;
		transition.target = std::move(after);
		if (transition.source != transition.target)
			plan.anyStateChange = true;
		plan.members.push_back(std::move(transition));
	}

	if (plan.sourceSchemaVersion != plan.targetSchemaVersion)
		plan.anyStateChange = true;

	return rt2::core::Result<PrefabMarkerPlan>::Ok(std::move(plan));
}

PrefabMarkerApplyResult SceneManager::CommitPrefabMarkerPlan(PrefabMarkerPlan plan)
{
	PrefabMarkerApplyResult result;
	result.beforeSchemaVersion = m_Authoring.metadata.schemaVersion;
	result.afterSchemaVersion = result.beforeSchemaVersion;

	// Re-resolve every durable member UUID. Commit is infallible only when
	// applied to the same document the plan was prepared against; a stale plan
	// (a member removed or un-made in between) fails loudly with zero mutation
	// rather than partially writing.
	std::vector<entt::entity> resolved;
	resolved.reserve(plan.members.size());
	for (const auto& transition : plan.members)
	{
		const auto e = m_Authoring.FindByUuid(transition.member);
		if (e == entt::null || !m_EcsScene.registry.valid(e))
		{
			result.error = S5QueryError(rt2::core::Error::InvalidEntity,
				transition.member, "stale marker plan: member no longer present");
			return result;
		}
		if (!m_EcsScene.registry.all_of<PrefabMemberComponent>(e))
		{
			result.error = S5QueryError(rt2::core::Error::NotPrefabMember,
				transition.member, "stale marker plan: member is no longer a prefab member");
			return result;
		}
		resolved.push_back(e);
	}

	// Genuine no-op: nothing changes, zero notifications (no revision/dirty/schema
	// churn). A valid plan with anyStateChange false targets identical state.
	if (!plan.anyStateChange)
		return result;

	// Apply all staged member vectors and the schema transition in one step.
	for (std::size_t i = 0; i < plan.members.size(); ++i)
	{
		auto* pm = m_EcsScene.registry.try_get<PrefabMemberComponent>(resolved[i]);
		pm->overrides = plan.members[i].target;
		++result.appliedMembers;
	}
	m_Authoring.metadata.schemaVersion = plan.targetSchemaVersion;
	result.afterSchemaVersion = m_Authoring.metadata.schemaVersion;
	result.anyStateChange = true;
	NotifyAuthoringChanged(); // exactly once
	return result;
}

// ============================================================================
// Stats + misc
// ============================================================================

void SceneManager::Clear()
{
	m_Authoring.Clear();
	m_EntityCacheDirty = true;
	++m_DocumentGeneration;
	++m_ResourceGeneration;
}

bool SceneManager::CompactMeshRegistry()
{
	auto& reg = m_EcsScene.registry;
	auto& meshReg = m_EcsScene.meshRegistry;

	// Find which mesh indices are still referenced by alive entities.
	//
	// A MeshRef may name an index the registry does not have — a stale
	// reference left by an entity built before its mesh was added, or one
	// that outlived a registry shrink. Such an index must NOT be treated as
	// live: it would flow into GetMesh() below, which is an unchecked
	// m_Meshes[index], and read out of bounds (SIGSEGV in Release, a
	// __fastfail on Debug's iterator checks). Skipping it is also correct
	// on the merits — a mesh that does not exist cannot be keeping anything
	// alive. GPUSceneData applies the same guard before indexing meshes.
	std::set<uint32_t> referenced;
	auto view = reg.view<MeshRef>();
	for (auto entity : view)
	{
		if (!reg.valid(entity)) continue;
		const auto& ref = view.get<MeshRef>(entity);
		if (ref.meshIndex >= meshReg.GetCount()) continue;
		referenced.insert(ref.meshIndex);
	}

	bool meshesChanged = false;

	// If all meshes are referenced, nothing to do
	if (referenced.size() == meshReg.GetCount())
	{
		// Meshes are fine, but still may need to compact materials/textures
	}
	// If no meshes referenced, clear the registry entirely
	else if (referenced.empty())
	{
		meshReg.Clear();
		meshesChanged = true;
	}
	else
	{
		// Build remap: old index -> new index
		std::map<uint32_t, uint32_t> remap;
		uint32_t newIndex = 0;
		for (uint32_t old : referenced)
			remap[old] = newIndex++;

		// Rebuild the mesh registry with only referenced meshes
		std::vector<MeshData> newMeshes;
		newMeshes.reserve(referenced.size());
		for (uint32_t old : referenced)
			newMeshes.push_back(std::move(meshReg.GetMesh(old)));

		meshReg.Clear();
		for (auto& mesh : newMeshes)
			meshReg.AddMesh(std::move(mesh));

		IndexRebase rebase;
		rebase.mesh.SetRemap(remap);
		RebaseIndices(m_EcsScene, rebase);

		printf("[Scene] Compacted mesh registry: %d -> %d meshes\n",
		       (int)meshReg.GetCount(), (int)referenced.size());
		meshesChanged = true;
	}

	// ---- Compact materials ----
	// Collect all referenced material indices from MeshRef components
	// and per-triangle materialIndices in meshes.
	std::set<int> referencedMats;
	for (auto entity : view)
	{
		if (!reg.valid(entity)) continue;
		const auto& ref = view.get<MeshRef>(entity);
		referencedMats.insert(ref.materialIndex);
	}
	for (uint32_t m = 0; m < meshReg.GetCount(); m++)
	{
		const auto& mesh = meshReg.GetMesh(m);
		for (int idx : mesh.materialIndices)
			referencedMats.insert(idx);
	}

	// Always include a default material at index 0 if there are meshes
	// but no materials (safety net).
	if (meshReg.GetCount() > 0 && referencedMats.empty())
		referencedMats.insert(0);

	// Build material remap: old index -> new index
	std::map<int, int> matRemap;
	int newMatIdx = 0;
	for (int old : referencedMats)
	{
		if (old >= 0 && old < (int)m_EcsScene.materials.size())
			matRemap[old] = newMatIdx++;
	}

	bool matsChanged = (matRemap.size() < m_EcsScene.materials.size());

	if (matsChanged)
	{
		// Rebuild materials vector
		std::vector<SceneMaterial> newMaterials;
		newMaterials.reserve(matRemap.size());
		for (int old : referencedMats)
		{
			if (old >= 0 && old < (int)m_EcsScene.materials.size())
				newMaterials.push_back(m_EcsScene.materials[old]);
		}
		m_EcsScene.materials = std::move(newMaterials);

		IndexRebase rebase;
		rebase.material.SetRemap(matRemap);
		RebaseIndices(m_EcsScene, rebase);

		printf("[Scene] Compacted materials: %zu -> %zu\n",
		       m_EcsScene.materials.size() + matRemap.size(), matRemap.size());
	}

	// ---- Compact textures ----
	// Collect all referenced texture indices from remaining materials.
	std::set<int> referencedTexs;
	for (auto& mat : m_EcsScene.materials)
	{
		ForEachMaterialTextureIndex(mat, [&](const int& index) {
			if (index >= 0) referencedTexs.insert(index);
		});
	}

	// Overrides carry a full material snapshot, so their textures are live
	// references even when no entry in m_EcsScene.materials mirrors them.
	// Prefab overrides are authored against an asset, not a live slot, so
	// the mirror cannot be assumed. See Phase 8 pre-work spec.
	auto overrideView = m_EcsScene.registry.view<MaterialOverrideComponent>();
	for (const auto entity : overrideView)
	{
		const auto& mat = overrideView.get<MaterialOverrideComponent>(entity).material;
		ForEachMaterialTextureIndex(mat, [&](const int& index) {
			if (index >= 0) referencedTexs.insert(index);
		});
	}

	// Build texture remap: old index -> new index
	std::map<int, int> texRemap;
	int newTexIdx = 0;
	for (int old : referencedTexs)
	{
		if (old >= 0 && old < (int)m_EcsScene.textures.size())
			texRemap[old] = newTexIdx++;
	}

	bool texsChanged = (texRemap.size() < m_EcsScene.textures.size());

	if (texsChanged)
	{
		// Rebuild textures vector
		std::vector<SceneTexture> newTextures;
		newTextures.reserve(texRemap.size());
		for (int old : referencedTexs)
		{
			if (old >= 0 && old < (int)m_EcsScene.textures.size())
				newTextures.push_back(std::move(m_EcsScene.textures[old]));
		}
		m_EcsScene.textures = std::move(newTextures);

		IndexRebase rebase;
		rebase.texture.SetRemap(texRemap);
		RebaseIndices(m_EcsScene, rebase);

		printf("[Scene] Compacted textures: %zu -> %zu\n",
		       texRemap.size() + (m_EcsScene.textures.size() - texRemap.size()),
		       m_EcsScene.textures.size());
	}

	const bool changed = meshesChanged || matsChanged || texsChanged;
	if (changed)
		++m_ResourceGeneration;
	return changed;
}

// ============================================================================
// Internal
// ============================================================================

void SceneManager::UpdateWorldTransforms()
{
	SceneGraph::UpdateWorldTransforms(m_EcsScene.registry);
}
