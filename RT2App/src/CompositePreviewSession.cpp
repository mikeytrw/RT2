#include "CompositePreviewSession.h"

#include "PrefabComponentKey.h"
#include "SceneSerializer.h"

#include <utility>

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
