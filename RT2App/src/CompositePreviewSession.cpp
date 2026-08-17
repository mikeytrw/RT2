#include "CompositePreviewSession.h"

#include "PrefabComponentKey.h"
#include "SceneSerializer.h"

#include <utility>
#include <atomic>
#include <unordered_set>
#include <algorithm>

bool CompositePreviewSession::Begin(SceneManager& scene,
	const rt2::core::UUID& member, PrefabValueKind kind,
	PrefabComponentKey key, PrefabValuePayload originValue, ValueReader reader)
{
	// Validate the wire exactly like PrefabCommandTransaction::Capture: an
	// unknown or excluded wire is a hard failure, never a silent ordinary-
	// entity skip.
	const auto canonical = FindComponentByWire(key.wire());
	if (!canonical || !canonical->overridable())
		return false;

	// Capture the immutable origin marker membership and schema BEFORE any
	// preview frame. An ordinary entity (NotPrefabMember) is not a failure:
	// it becomes a value-only session whose marker delta is dropped.
	PrefabCommandTransaction::ExplicitCapture origin;
	const auto presence = scene.IsOverridden(member, *canonical);
	if (!presence.IsOk())
	{
		if (presence.error.code != rt2::core::Error::NotPrefabMember)
			return false;
		origin.markers.push_back({ member, key, std::nullopt, true });
	}
	else
	{
		origin.markers.push_back({ member, key, presence.value, true });
	}
	origin.beforeSchema = scene.AuthoringDoc().metadata.schemaVersion;

	m_Target = member;
	m_Kind = kind;
	m_Key = key;
	m_OriginValue = std::move(originValue);
	m_RollingValue = m_OriginValue;
	m_Origin = std::move(origin);
	m_OriginDocumentGeneration = scene.DocumentGeneration();
	m_Reader = std::move(reader);
	m_Open = true;
	m_HadEffectiveFrame = false;
	// "No frame yet" reads as failure so a consumer never mistakes the empty
	// result for a successful no-op frame.
	m_LastResult = EditorMutationResult::Failure(rt2::core::Error::InvalidRuntimeState,
		m_Target.ToString(), "no preview frame committed yet");
	m_LastEffectiveResult = EditorMutationResult::Failure(rt2::core::Error::InvalidRuntimeState,
		m_Target.ToString(), "no effective preview frame yet");
	return true;
}

EditorMutationResult CompositePreviewSession::Preview(SceneManager& scene,
	PrefabValuePayload target)
{
	if (!m_Open)
		return EditorMutationResult::Failure(rt2::core::Error::InvalidRuntimeState,
			m_Target.ToString(), "preview frame on a closed session");

	// Gate marker construction on canonical value effectiveness (S6-C fixup,
	// P1 finding 1). A frame whose target is canonically identical to the
	// last committed (rolling) source is a value no-op: submitting the
	// absent->present marker would mark the member overridden and promote the
	// schema with no value write. Skip the frame entirely — zero marker, zero
	// schema, zero revision, zero notify, zero history. The comparison is the
	// composite's own canonical equality (S5EqualPayload), so a canonical
	// no-op is caught even when the raw target differs by only a
	// canonicalization step. An effective session (marker already present)
	// is unaffected: a later-frame no-op already committed nothing.
	if (PrefabValuePayloadEqual(m_RollingValue, target))
	{
		m_LastResult = EditorMutationResult{};
		m_LastResult.effective = false;
		return m_LastResult;
	}

	// A fresh transaction per frame: frame one stages origin -> target with an
	// absent->present marker and promotes the schema; later frames stage the
	// last committed value -> target with the marker already present and the
	// live schema on both sides. Because Prepare validates the staged source
	// against live state, a rolling source that diverged from the document
	// (stale frame-zero reuse, out-of-band mutation) fails loudly instead of
	// silently re-marking.
	PrefabCommandTransaction tx(
		std::vector<PrefabValueEdit>{
			{ m_Kind, m_Target, PrefabMarkerDirection::After, m_RollingValue, target } },
		std::vector<PrefabCommandTransaction::MarkerSpec>{
			{ m_Target, m_Key, true } });
	m_LastResult = tx.Execute(scene);

	// Advance the rolling source ONLY after a successful, effective commit,
	// and read the committed form back from the live scene so the next frame
	// re-stages what the manager actually stored (e.g. a canonicalized camera
	// or staged script binding).
	if (m_LastResult.success && m_LastResult.effective)
	{
		m_HadEffectiveFrame = true;
		m_LastEffectiveResult = m_LastResult;
		m_RollingValue = m_Reader ? m_Reader(m_Target, target) : target;
	}
	return m_LastResult;
}

void CompositePreviewSession::Discard()
{
	m_Open = false;
	m_Target = rt2::core::UUID{};
	m_OriginDocumentGeneration = 0;
	m_Reader = {};
	m_HadEffectiveFrame = false;
}

PrefabValuePayload CompositePreviewSession::ReadLiveValue(SceneManager& scene) const
{
	if (m_Reader)
		return m_Reader(m_Target, m_RollingValue);
	return m_RollingValue;
}

std::optional<PrefabValuePayload> CompositePreviewSession::ReadLiveValueExact(
	SceneManager& scene) const
{
	const auto entity = scene.FindEntityByUuid(m_Target);
	if (entity == entt::null) return std::nullopt;
	auto& reg = scene.GetECS().registry;
	switch (m_Kind)
	{
	case PrefabValueKind::LightProperties:
		if (!reg.all_of<LightComponent>(entity)) return std::nullopt;
		return PrefabValuePayload{ reg.get<LightComponent>(entity) };
	case PrefabValueKind::CameraProperties:
		if (!reg.all_of<CameraComponent>(entity)) return std::nullopt;
		return PrefabValuePayload{ reg.get<CameraComponent>(entity) };
	case PrefabValueKind::MotionState:
		if (reg.all_of<MotionComponent>(entity))
			return PrefabValuePayload{
				std::optional<MotionComponent>(reg.get<MotionComponent>(entity)) };
		return PrefabValuePayload{ std::optional<MotionComponent>{} };
	case PrefabValueKind::ScriptState:
		return PrefabValuePayload{ scene.GetScriptState(m_Target) };
	default:
		// Not one of the four live-preview kinds: no exact read exists.
		return std::nullopt;
	}
}

std::uint32_t CompositePreviewSession::ExpectedAfterSchema() const
{
	std::uint32_t schema = m_Origin.beforeSchema;
	// Mirror the transaction's anyMarkerAdded rule: an absent->present marker
	// promotes the document to the serializer's current schema; an already
	// present (or ordinary/absent) marker leaves the origin schema unchanged.
	for (const auto& marker : m_Origin.markers)
	{
		if (marker.beforePresent && !*marker.beforePresent && marker.afterPresent)
		{
			schema = rt2::core::SceneSerializer::SchemaVersion;
			break;
		}
	}
	return schema;
}

namespace {
std::atomic<std::uint64_t> g_TransformGestureSequence{0};
}

std::uint64_t TransformPreviewSession::NextSequence()
{
	// Never reset: a replacement document or a new publisher cannot make a
	// delayed token valid again through ABA reuse.
	return ++g_TransformGestureSequence;
}

EditorMutationResult TransformPreviewSession::Fail(rt2::core::Error::Code code,
	const std::string& detail) const
{
	return EditorMutationResult::Failure(code, "transform-session", detail);
}

TransformPreviewSession::Member* TransformPreviewSession::FindMember(
	const rt2::core::UUID& uuid)
{
	for (auto& member : m_Members) if (member.uuid == uuid) return &member;
	return nullptr;
}

const TransformPreviewSession::Member* TransformPreviewSession::FindMember(
	const rt2::core::UUID& uuid) const
{
	for (const auto& member : m_Members) if (member.uuid == uuid) return &member;
	return nullptr;
}

std::optional<TransformGestureToken> TransformPreviewSession::Begin(
	SceneManager& scene, std::uint64_t opaqueOwner,
	const std::vector<rt2::core::UUID>& orderedUuids)
{
	if (m_Open || opaqueOwner == 0 || orderedUuids.empty()) return std::nullopt;
	std::unordered_set<rt2::core::UUID> seen;
	std::vector<Member> members;
	PrefabCommandTransaction::ExplicitCapture origin;
	members.reserve(orderedUuids.size());
	origin.markers.reserve(orderedUuids.size());
	for (const auto& uuid : orderedUuids)
	{
		if (uuid.IsNull() || !seen.insert(uuid).second) return std::nullopt;
		const auto entity = scene.FindEntityByUuid(uuid);
		EditableTRS local;
		if (entity == entt::null || !scene.GetLocalTransform({ entity }, local))
			return std::nullopt;
		const auto markerKey = PrefabComponentKeyFor<Transform>::value;
		const auto presence = scene.IsOverridden(uuid, markerKey);
		std::optional<bool> marker;
		if (presence.IsOk()) marker = presence.value;
		else if (presence.error.code != rt2::core::Error::NotPrefabMember)
			return std::nullopt;
		members.push_back({ uuid, local, local, marker, marker, false, false });
		origin.markers.push_back({ uuid, markerKey, marker, true });
	}
	origin.beforeSchema = scene.AuthoringDoc().metadata.schemaVersion;
	// All fallible capture work completed before minting authority.
	m_Members = std::move(members);
	m_Origin = std::move(origin);
	m_OriginSchema = m_Origin.beforeSchema;
	m_RollingSchema = m_OriginSchema;
	m_DocumentGeneration = scene.DocumentGeneration();
	m_Token = TransformGestureToken(opaqueOwner, NextSequence());
	m_Open = true;
	m_HadEffectiveFrame = false;
	m_ClosePhase = ClosePhase::LivePreview;
	m_FailNextCompensation = false;
	m_FailNextCleanup = false;
	m_LastResult = Fail(rt2::core::Error::InvalidRuntimeState,
		"no preview frame committed yet");
	m_LastEffectiveResult = m_LastResult;
	return m_Token;
}

EditorMutationResult TransformPreviewSession::PreviewLocals(SceneManager& scene,
	const TransformGestureToken& token,
	const std::vector<std::pair<rt2::core::UUID, EditableTRS>>& targets)
{
	if (!TokenMatches(token)) return Fail(rt2::core::Error::InvalidArgument,
		"transform gesture token is stale, foreign, or invalid");
	if (m_ClosePhase != ClosePhase::LivePreview)
		return Fail(rt2::core::Error::InvalidRuntimeState,
			"transform compensation is pending; retry close explicitly");
	if (scene.DocumentGeneration() != m_DocumentGeneration)
		return Fail(rt2::core::Error::InvalidArgument,
			"transform gesture document generation changed");
	if (targets.size() != m_Members.size())
		return Fail(rt2::core::Error::InvalidArgument,
			"transform preview target set does not match Begin");
	std::unordered_set<rt2::core::UUID> seen;
	std::vector<PrefabValueEdit> values;
	std::vector<PrefabCommandTransaction::MarkerSpec> specs;
	PrefabCommandTransaction::ExplicitCapture rollingCapture;
	values.reserve(targets.size()); specs.reserve(targets.size());
	rollingCapture.markers.reserve(targets.size());
	for (const auto& [uuid, target] : targets)
	{
		if (!seen.insert(uuid).second) return Fail(rt2::core::Error::InvalidArgument,
			"transform preview contains a duplicate UUID");
		Member* member = FindMember(uuid);
		if (!member) return Fail(rt2::core::Error::InvalidEntity,
			"transform preview UUID is outside the captured set");
		if (PrefabValuePayloadEqual(PrefabValuePayload{member->rollingLocal},
			PrefabValuePayload{target})) continue;
		values.push_back({ PrefabValueKind::LocalTransform, uuid,
			PrefabMarkerDirection::After, PrefabValuePayload{member->rollingLocal},
			PrefabValuePayload{target} });
		specs.push_back({ uuid, PrefabComponentKeyFor<Transform>::value, true });
		rollingCapture.markers.push_back({ uuid, PrefabComponentKeyFor<Transform>::value,
			member->rollingMarker, true });
	}
	if (seen.size() != m_Members.size())
		return Fail(rt2::core::Error::InvalidArgument,
			"transform preview target set is incomplete");
	if (values.empty())
	{
		m_LastResult = EditorMutationResult{};
		m_LastResult.effective = false;
		return m_LastResult;
	}
	rollingCapture.beforeSchema = m_RollingSchema;
	PrefabCommandTransaction tx(std::move(values), std::move(specs));
	tx.SetExplicitCapture(std::move(rollingCapture));
	m_LastResult = tx.Execute(scene);
	if (!m_LastResult.success) return m_LastResult;
	if (!m_LastResult.effective) return m_LastResult;
	for (const auto& [uuid, target] : targets)
	{
		Member* member = FindMember(uuid);
		if (!member || PrefabValuePayloadEqual(PrefabValuePayload{member->rollingLocal},
			PrefabValuePayload{target})) continue;
		EditableTRS committed;
		const auto entity = scene.FindEntityByUuid(uuid);
		if (entity == entt::null || !scene.GetLocalTransform({ entity }, committed))
			return Fail(rt2::core::Error::InvalidEntity,
				"committed transform could not be read back");
		member->rollingLocal = committed;
		if (member->rollingMarker.has_value())
			member->rollingMarker = true;
		member->introducedMarker = member->introducedMarker ||
			(member->originMarker.has_value() && !*member->originMarker &&
			 member->rollingMarker.has_value() && *member->rollingMarker);
		member->everEffective = true;
	}
	m_RollingSchema = scene.AuthoringDoc().metadata.schemaVersion;
	m_HadEffectiveFrame = true;
	if (!m_LastEffectiveResult.success)
		m_LastEffectiveResult = m_LastResult;
	else
	{
		m_LastEffectiveResult.success = true;
		m_LastEffectiveResult.effective = true;
		if (static_cast<int>(m_LastResult.syncImpact) >
			static_cast<int>(m_LastEffectiveResult.syncImpact))
			m_LastEffectiveResult.syncImpact = m_LastResult.syncImpact;
		for (const auto& uuid : m_LastResult.affectedEntities)
			if (std::find(m_LastEffectiveResult.affectedEntities.begin(),
				m_LastEffectiveResult.affectedEntities.end(), uuid) ==
				m_LastEffectiveResult.affectedEntities.end())
				m_LastEffectiveResult.affectedEntities.push_back(uuid);
	}
	return m_LastResult;
}

EditorMutationResult TransformPreviewSession::PreviewWorlds(SceneManager& scene,
	const TransformGestureToken& token,
	const std::vector<std::pair<rt2::core::UUID, glm::mat4>>& targets)
{
	if (!TokenMatches(token)) return Fail(rt2::core::Error::InvalidArgument,
		"transform gesture token is stale, foreign, or invalid");
	std::vector<std::pair<SceneManager::EntityId, glm::mat4>> resolved;
	resolved.reserve(targets.size());
	for (const auto& [uuid, world] : targets)
	{
		if (!FindMember(uuid)) return Fail(rt2::core::Error::InvalidEntity,
			"world preview UUID is outside the captured set");
		const auto entity = scene.FindEntityByUuid(uuid);
		if (entity == entt::null) return Fail(rt2::core::Error::InvalidEntity,
			"world preview target was removed");
		resolved.emplace_back(SceneManager::EntityId{ entity }, world);
	}
	const auto staged = scene.StageWorldTransforms(resolved);
	if (!staged.IsOk())
		return EditorMutationResult::Failure(staged.error.code, staged.error.path,
			staged.error.detail);
	return PreviewLocals(scene, token, staged.value.localStates);
}

std::size_t TransformPreviewSession::PruneMissingMembers(const SceneManager& scene)
{
	if (!m_Open) return 0;
	std::unordered_set<rt2::core::UUID> live;
	for (const auto& member : m_Members)
		if (scene.FindEntityByUuid(member.uuid) != entt::null) live.insert(member.uuid);
	m_Members.erase(std::remove_if(m_Members.begin(), m_Members.end(),
		[&](const Member& member) { return live.find(member.uuid) == live.end(); }),
		m_Members.end());
	m_Origin.markers.erase(std::remove_if(m_Origin.markers.begin(), m_Origin.markers.end(),
		[&](const PrefabCommandTransaction::ExplicitMarker& marker) {
			return live.find(marker.member) == live.end();
		}), m_Origin.markers.end());
	return m_Members.size();
}

void TransformPreviewSession::Discard()
{
	m_Open = false;
	m_Members.clear();
	m_Origin = {};
	m_Token = TransformGestureToken(0, 0);
	m_DocumentGeneration = 0;
	m_HadEffectiveFrame = false;
	m_ClosePhase = ClosePhase::LivePreview;
	m_FailNextCompensation = false;
	m_FailNextCleanup = false;
}
