#include "PrefabCommandTransaction.h"

#include "PrefabComponentKey.h"
#include "SceneSerializer.h"

#include <unordered_set>

void PrefabCommandTransaction::SetExplicitCapture(ExplicitCapture explicitCapture)
{
	// One-for-one validation of the caller-supplied immutable origin against
	// this transaction's DECLARED marker specs (S6-C fixup, P2 finding 5). An
	// arbitrary explicit list must never be accepted: a light command for
	// entity A given a forged camera marker for entity B would make the first
	// Undo restore A's value while changing B's membership/schema in one
	// composite. The capture is rejected (never silently applied) unless:
	//   - it is exactly as large as the declared marker-spec set;
	//   - each declared spec is matched by exactly one explicit marker with
	//     the same member, canonical key, and afterPresent (no extras, no
	//     duplicates, no member/key substitution);
	//   - every explicit marker targets the command's non-nil value-write
	//     entity (a forged ordinary/member mix for another entity is rejected).
	bool rejected = true;
	const auto& values = m_Values;
	auto valueEntityNilOnly = true;
	for (const auto& edit : values)
		if (edit.entity != rt2::core::UUID{}) { valueEntityNilOnly = false; break; }
	if (!valueEntityNilOnly
		&& explicitCapture.markers.size() == m_Markers.size())
	{
		// Every marker member must belong to the transaction's non-nil value
		// entity set. Transform gestures are multi-member; the old single
		// valueEntity check silently rejected a valid two-member capture.
		std::unordered_set<rt2::core::UUID> valueEntities;
		for (const auto& edit : m_Values)
			if (edit.entity != rt2::core::UUID{}) valueEntities.insert(edit.entity);
		bool aligned = true;
		for (const auto& em : explicitCapture.markers)
			if (valueEntities.find(em.member) == valueEntities.end()) { aligned = false; break; }
		if (aligned)
		{
			std::unordered_set<std::string> seen;
			bool bijective = true;
			for (const auto& spec : m_Markers)
			{
				bool found = false;
				for (const auto& em : explicitCapture.markers)
				{
					const auto canonical = FindComponentByWire(em.key.wire());
					if (!canonical || !canonical->overridable()) { bijective = false; break; }
					if (em.member != spec.member || em.afterPresent != spec.afterPresent)
						continue;
					if (canonical->wire() != spec.key.wire())
						continue;
					found = true;
					break;
				}
				if (!found) { bijective = false; break; }
			}
			if (bijective)
			{
				// Also reject duplicate (member, canonical key) pairs within the
				// explicit list itself, beyond the cardinality check.
				rejected = false;
				for (const auto& em : explicitCapture.markers)
				{
					const auto canonical = FindComponentByWire(em.key.wire());
					const std::string wire = canonical
						? std::string(canonical->wire())
						: std::string(em.key.wire());
					const std::string combo = em.member.ToString() + "\x1f" + wire;
					if (!seen.insert(combo).second) { rejected = true; break; }
				}
			}
		}
	}
	m_ExplicitCapture = std::move(explicitCapture);
	m_ExplicitCaptureRejected = rejected;
}

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
	m_BeforeSchema = m_ExplicitCapture
		? m_ExplicitCapture->beforeSchema
		: scene.AuthoringDoc().metadata.schemaVersion;
	m_CapturedMarkers.clear();
	bool anyMarkerAdded = false;

	// Shared per-marker validation + capture: every public key is
	// canonicalized/validated against the frozen table BEFORE the ordinary
	// entity skip (SceneManager::IsOverridden returns NotPrefabMember before
	// it ever canonicalizes the wire or checks the overridable bit, so without
	// this an ordinary entity with an unknown or excluded key would silently
	// drop its marker and proceed as a value-only edit). A marker key that is
	// not a known overridable wire aborts the capture regardless of the
	// target's class. `originPresence` is the override-set membership the
	// Before direction restores: read live for an Executed command, or the
	// explicit origin fact for a recorded one.
	const auto captureOne = [&](const rt2::core::UUID& member,
		const PrefabComponentKey& key,
		std::optional<bool> originPresence,
		bool afterPresent) -> EditorMutationResult {
		const auto canonical = FindComponentByWire(key.wire());
		if (!canonical)
			return EditorMutationResult::Failure(rt2::core::Error::InvalidArgument,
				member.ToString(),
				"unknown override key wire '" + std::string(key.wire()) + "'");
		if (!canonical->overridable())
			return EditorMutationResult::Failure(rt2::core::Error::InvalidArgument,
				member.ToString(),
				"non-overridable (excluded) override key wire '"
				+ std::string(key.wire()) + "'");

		CapturedMarker captured;
		captured.member = member;
		captured.key = key;
		captured.beforePresent = originPresence;
		captured.afterPresent = afterPresent;
		// Removing an INHERITED (currently not overridden) prefab-authored
		// wire must mark it as explicitly overridden-absent. A local removal
		// that merely drops the marker would let the prefab source resurrect
		// the component on the next reconcile; adding the wire to the override
		// set records the local "removed here" decision durably. A locally
		// added-then-removed member (beforePresent == true) still returns to
		// source with no marker.
		if (!afterPresent && originPresence && !*originPresence)
			captured.afterPresent = true;
		m_CapturedMarkers.push_back(std::move(captured));
		if (originPresence && !*originPresence && captured.afterPresent)
			anyMarkerAdded = true;
		return EditorMutationResult{};
	};

	if (m_ExplicitCapture)
	{
		// Recorded live-preview commands replay the immutable origin state
		// captured at gesture open — NOT the live membership/schema the preview
		// frames left behind. The explicit beforePresent came from an
		// IsOverridden read at open; an ordinary entity carries nullopt and the
		// delta is dropped exactly as the live path drops NotPrefabMember.
		//
		// S6-C fixup (P2 finding 5): a capture that failed SetExplicitCapture's
		// one-for-one validation is never applied — replay fails loudly instead
		// of letting a forged ordinary/member mix through.
		if (m_ExplicitCaptureRejected)
			return EditorMutationResult::Failure(rt2::core::Error::InvalidArgument,
				m_Values.empty() ? rt2::core::UUID{}.ToString()
					: m_Values.front().entity.ToString(),
				"explicit capture does not match the command's declared marker specs "
				"(rejected one-for-one at SetExplicitCapture)");
		for (const auto& marker : m_ExplicitCapture->markers)
		{
			// A forged ordinary/member mix: the explicit origin claims an
			// ordinary entity (nullopt beforePresent) for a member that is
			// really a prefab member, or vice versa. The entity class is
			// stable across the gesture, so this is checkable at replay time
			// against live state.
			const auto liveClass = scene.IsOverridden(marker.member, marker.key);
			const bool liveIsMember = liveClass.IsOk();
			if (!liveIsMember)
			{
				if (liveClass.error.code != rt2::core::Error::NotPrefabMember)
					return EditorMutationResult::Failure(liveClass.error.code,
						liveClass.error.path, liveClass.error.detail);
				if (marker.beforePresent.has_value())
					return EditorMutationResult::Failure(rt2::core::Error::InvalidArgument,
						marker.member.ToString(),
						"explicit capture claims a prefab-member origin for an ordinary entity");
			}
			else if (!marker.beforePresent.has_value())
			{
				return EditorMutationResult::Failure(rt2::core::Error::InvalidArgument,
					marker.member.ToString(),
					"explicit capture claims an ordinary-entity origin for a prefab member");
			}
			const auto result = captureOne(marker.member, marker.key,
				marker.beforePresent, marker.afterPresent);
			if (!result.success) return result;
		}
	}
	else
	{
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
			const auto result = captureOne(spec.member, spec.key,
				presence.value, spec.afterPresent);
			if (!result.success) return result;
		}
	}
	m_AfterSchema = m_ExplicitCapture && m_ExplicitCapture->afterSchema
		? *m_ExplicitCapture->afterSchema
		: (anyMarkerAdded ? rt2::core::SceneSerializer::SchemaVersion : m_BeforeSchema);
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
