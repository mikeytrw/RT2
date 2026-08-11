#include "PrefabCommandTransaction.h"

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

void PrefabCommandTransaction::Capture(SceneManager& scene)
{
	m_BeforeSchema = scene.AuthoringDoc().metadata.schemaVersion;
	m_CapturedMarkers.clear();
	bool anyMarkerAdded = false;
	for (const auto& spec : m_Markers)
	{
		const auto presence = scene.IsOverridden(spec.member, spec.key);
		if (!presence.IsOk())
		{
			// An ordinary entity has no override set to join; the delta is
			// dropped and the edit is value-only. Any other failure (absent
			// member, malformed stored vector) omits the marker too and lets
			// the composite value resolve fail loudly during Prepare.
			if (presence.error.code == rt2::core::Error::NotPrefabMember)
				continue;
			continue;
		}
		CapturedMarker captured;
		captured.member = spec.member;
		captured.key = spec.key;
		captured.beforePresent = presence.value;
		captured.afterPresent = spec.afterPresent;
		m_CapturedMarkers.push_back(std::move(captured));
		if (!presence.value && spec.afterPresent)
			anyMarkerAdded = true;
	}
	m_AfterSchema = anyMarkerAdded
		? rt2::core::SceneSerializer::SchemaVersion : m_BeforeSchema;
	m_Captured = true;
}

EditorMutationResult PrefabCommandTransaction::Replay(SceneManager& scene,
	PrefabMarkerDirection direction)
{
	if (!m_Captured)
		Capture(scene);

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
