#include "PrefabCommandTransaction.h"

#include "PrefabComponentKey.h"
#include "SceneSerializer.h"

EditorMutationResult PrefabCommandTransaction::Execute(SceneManager& scene)
{
	return Replay(scene, PrefabMarkerDirection::After);
}

EditorMutationResult PrefabCommandTransaction::Undo(SceneManager& scene)
{
	return Replay(scene, PrefabMarkerDirection::Before);
}

EditorMutationResult PrefabCommandTransaction::Redo(SceneManager& scene)
{
	return Replay(scene, PrefabMarkerDirection::After);
}

EditorMutationResult PrefabCommandTransaction::Capture(SceneManager& scene)
{
	m_BeforeSchema = scene.AuthoringDoc().metadata.schemaVersion;
	m_CapturedMarkers.clear();
	bool anyMarkerAdded = false;
	for (const auto& spec : m_Markers)
	{
		// Validate/canonicalize EVERY public MarkerSpec key against the frozen
		// table BEFORE the ordinary-entity skip. SceneManager::IsOverridden
		// returns NotPrefabMember before it ever canonicalizes the wire or
		// checks the overridable bit, so without this an ordinary entity with
		// an unknown or excluded key silently dropped its marker and proceeded
		// as a value-only edit — validation was entity-class-dependent. A
		// marker key that is not a known overridable wire aborts the capture
		// regardless of the target's class.
		const auto canonical = FindComponentByWire(spec.key.wire());
		if (!canonical)
			return EditorMutationResult::Failure(rt2::core::Error::InvalidArgument,
				spec.member.ToString(),
				"unknown override key wire '" + std::string(spec.key.wire()) + "'");
		if (!canonical->overridable())
			return EditorMutationResult::Failure(rt2::core::Error::InvalidArgument,
				spec.member.ToString(),
				"non-overridable (excluded) override key wire '"
				+ std::string(spec.key.wire()) + "'");

		const auto presence = scene.IsOverridden(spec.member, *canonical);
		if (!presence.IsOk())
		{
			// An ordinary entity has no override set to join; the delta is
			// dropped and the edit is value-only. ANY other failure — an
			// absent member (InvalidEntity) or a malformed stored override
			// vector (InvalidArgument) — aborts the capture so the command
			// cannot silently commit a value-only composite while the
			// marker/schema/history stay untouched.
			if (presence.error.code == rt2::core::Error::NotPrefabMember)
				continue;
			return EditorMutationResult::Failure(presence.error.code,
				presence.error.path, presence.error.detail);
		}
		CapturedMarker captured;
		captured.member = spec.member;
		captured.key = spec.key;
		captured.beforePresent = presence.value;
		captured.afterPresent = spec.afterPresent;
		// Removing an INHERITED (currently not overridden) prefab-authored
		// wire must mark it as explicitly overridden-absent. A local removal
		// that merely drops the marker would let the prefab source resurrect
		// the component on the next reconcile; adding the wire to the override
		// set records the local "removed here" decision durably. A locally
		// added-then-removed member (beforePresent == true) still returns to
		// source with no marker.
		if (!spec.afterPresent && !presence.value)
			captured.afterPresent = true;
		m_CapturedMarkers.push_back(std::move(captured));
		if (!presence.value && captured.afterPresent)
			anyMarkerAdded = true;
	}
	m_AfterSchema = anyMarkerAdded
		? rt2::core::SceneSerializer::SchemaVersion : m_BeforeSchema;
	m_Captured = true;
	return EditorMutationResult{};
}

EditorMutationResult PrefabCommandTransaction::Replay(SceneManager& scene,
	PrefabMarkerDirection direction)
{
	if (!m_Captured)
	{
		const auto capture = Capture(scene);
		if (!capture.success) return capture;
	}

	std::vector<PrefabValueEdit> values;
	values.reserve(m_Values.size());
	for (const auto& edit : m_Values)
	{
		PrefabValueEdit replay = edit;
		replay.direction = direction;
		values.push_back(std::move(replay));
	}
	std::vector<PrefabMarkerEdit> markers;
	markers.reserve(m_CapturedMarkers.size());
	for (const auto& captured : m_CapturedMarkers)
	{
		if (!captured.beforePresent.has_value())
			continue;
		markers.push_back(PrefabMarkerEdit{ captured.member, captured.key,
			*captured.beforePresent, captured.afterPresent });
	}

	auto plan = scene.PreparePrefabCompositeEdits(values, markers, direction,
		m_BeforeSchema, m_AfterSchema);
	if (!plan.IsOk())
		return ToEditorMutationResult(plan.error);

	if (direction == PrefabMarkerDirection::After)
	{
		// Absorb the composite's canonical After targets (durable identities
		// minted during Prepare, e.g. the assetId assigned on a bound script
		// add or path change) so every later Before/After replay re-stages the
		// same payloads the first Execute committed. Without this, Undo
		// re-stages the pre-mint payload and the binding-source check fails.
		for (const auto& op : plan.value.values.operations)
		{
			for (auto& edit : m_Values)
			{
				if (edit.kind != op.kind || edit.entity != op.entity)
					continue;
				if (edit.kind == PrefabValueKind::MaterialSlotProperties)
				{
					const auto* slot = std::get_if<PrefabMaterialSlotValue>(&op.target);
					const auto* editSlot = std::get_if<PrefabMaterialSlotValue>(&edit.after);
					if (!slot || !editSlot || editSlot->slotIndex != slot->slotIndex)
						continue;
				}
				edit.after = op.target;
			}
		}
	}

	const auto applied = scene.CommitPrefabCompositePlan(std::move(plan.value));
	return ToEditorMutationResult(applied);
}
