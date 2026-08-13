#include "PreviewSessionClose.h"

#include "EditorPropertyCommands.h"
#include "EditorCommands.h"

#include <algorithm>
#include <utility>

namespace
{

// The concrete S6-C command family carries the explicit-capture validity state
// on its transaction; BuildPreviewSessionCommand always produces exactly the
// type matching `kind`, so the downcast is safe.
bool CaptureRejectedForKind(PreviewSessionKind kind, const IEditorCommand& cmd)
{
	switch (kind)
	{
	case PreviewSessionKind::Light:
		return static_cast<const SetLightCommand&>(cmd).ExplicitCaptureRejected();
	case PreviewSessionKind::Camera:
		return static_cast<const SetCameraCommand&>(cmd).ExplicitCaptureRejected();
	case PreviewSessionKind::Motion:
		return static_cast<const SetMotionCommand&>(cmd).ExplicitCaptureRejected();
	case PreviewSessionKind::Script:
		return static_cast<const SetScriptCommand&>(cmd).ExplicitCaptureRejected();
	case PreviewSessionKind::Transform:
		return true;
	}
	return true;
}

} // namespace

std::unique_ptr<IEditorCommand> BuildPreviewSessionCommand(
	PreviewSessionKind kind, const CompositePreviewSession& session,
	bool suppressNoOp)
{
	switch (kind)
	{
	case PreviewSessionKind::Light:
		return suppressNoOp
			? MakeSetLightCommandIfEffective(session.Target(),
				std::get<LightComponent>(session.OriginValue()),
				std::get<LightComponent>(session.RollingValue()),
				&session.Origin())
			: MakeSetLightRestoreCommand(session.Target(),
				std::get<LightComponent>(session.OriginValue()),
				std::get<LightComponent>(session.RollingValue()),
				session.Origin());
	case PreviewSessionKind::Camera:
		return suppressNoOp
			? MakeSetCameraCommandIfEffective(session.Target(),
				std::get<CameraComponent>(session.OriginValue()),
				std::get<CameraComponent>(session.RollingValue()),
				&session.Origin())
			: MakeSetCameraRestoreCommand(session.Target(),
				std::get<CameraComponent>(session.OriginValue()),
				std::get<CameraComponent>(session.RollingValue()),
				session.Origin());
	case PreviewSessionKind::Motion:
		return suppressNoOp
			? MakeSetMotionCommandIfEffective(session.Target(),
				std::get<std::optional<MotionComponent>>(session.OriginValue()),
				std::get<std::optional<MotionComponent>>(session.RollingValue()),
				&session.Origin())
			: MakeSetMotionRestoreCommand(session.Target(),
				std::get<std::optional<MotionComponent>>(session.OriginValue()),
				std::get<std::optional<MotionComponent>>(session.RollingValue()),
				session.Origin());
	case PreviewSessionKind::Script:
		return suppressNoOp
			? MakeSetScriptCommandIfEffective(session.Target(),
				std::get<std::optional<ScriptComponent>>(session.OriginValue()),
				std::get<std::optional<ScriptComponent>>(session.RollingValue()),
				&session.Origin())
			: MakeSetScriptRestoreCommand(session.Target(),
				std::get<std::optional<ScriptComponent>>(session.OriginValue()),
				std::get<std::optional<ScriptComponent>>(session.RollingValue()),
				session.Origin());
	case PreviewSessionKind::Transform:
		return nullptr;
	}
	return nullptr;
}

std::optional<EditorMutationResult> PreflightPreviewForRecord(
	SceneManager& scene, PreviewSessionKind kind,
	const CompositePreviewSession& session, const IEditorCommand& cmd)
{
	// The recorded close command is only trusted if the already-applied
	// preview is still the live state. RecordApplied never preflights; it
	// pushes on the caller's result alone, so a stale or rejected command that
	// reaches it would poison history (a later Undo fails and clears both
	// stacks). Validate here, validate-only, before the record (S6-C re-review,
	// P1 finding 1).

	// 1. Explicit capture validity: a rejected capture must never be recorded.
	if (CaptureRejectedForKind(kind, cmd))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidArgument,
			session.Target().ToString(),
			"explicit capture was rejected; the live-preview close cannot be recorded");

	// 2. Live VALUE must still equal the rolling final (what the preview frames
	// committed). The read is EXACT: a Light/Camera component removed out of
	// band is a hard preflight failure (never substituted with the rolling
	// final), and a removed Motion/Script reads as exact optional absence
	// (never as the rolling present state) — S6-C final closure, P1 finding 1.
	const auto liveValue = session.ReadLiveValueExact(scene);
	if (!liveValue.has_value())
		return EditorMutationResult::Failure(rt2::core::Error::InvalidArgument,
			session.Target().ToString(),
			"the previewed component is missing from the live entity; the close cannot be recorded");
	if (!PrefabValuePayloadEqual(*liveValue, session.RollingValue()))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidArgument,
			session.Target().ToString(),
			"live value differs from the rolling preview final; the close cannot be recorded");

	// 3. After-marker MEMBERSHIP must still match the gesture's marker outcome.
	// Only a confirmed prefab member carries the marker; an ordinary entity
	// drops marker work (consistent with the nullopt origin fact).
	const auto presence = scene.IsOverridden(session.Target(), session.Key());
	if (presence.IsOk())
	{
		const bool afterPresent = session.Origin().markers.empty()
			? true : session.Origin().markers.front().afterPresent;
		if (presence.value != afterPresent)
			return EditorMutationResult::Failure(rt2::core::Error::InvalidArgument,
				session.Target().ToString(),
				"live override membership differs from the preview marker outcome; "
				"the close cannot be recorded");
	}
	else if (presence.error.code != rt2::core::Error::NotPrefabMember)
	{
		return EditorMutationResult::Failure(presence.error.code,
			presence.error.path, presence.error.detail);
	}
	else if (!session.Origin().markers.empty()
		&& session.Origin().markers.front().beforePresent.has_value())
	{
		// Forged ordinary/member mix: live says ordinary but the origin claims
		// a prefab-member fact. Never record it.
		return EditorMutationResult::Failure(rt2::core::Error::InvalidArgument,
			session.Target().ToString(),
			"explicit capture claims a prefab-member origin for an ordinary entity; "
			"the close cannot be recorded");
	}

	// 4. Promoted SCHEMA must still be live. An out-of-band schema change since
	// the preview leaves the recorded command's schema transport stale.
	if (scene.AuthoringDoc().metadata.schemaVersion != session.ExpectedAfterSchema())
		return EditorMutationResult::Failure(rt2::core::Error::InvalidArgument,
			session.Target().ToString(),
			"live document schema differs from the preview-promoted schema; "
			"the close cannot be recorded");

	return std::nullopt;
}

PreviewSessionCloseOutcome FinalizePreviewSession(
	EditorCommandHistory& history, SceneManager& scene,
	PreviewSessionKind kind, CompositePreviewSession& session)
{
	PreviewSessionCloseOutcome outcome;
	if (!session.IsOpen()) return outcome;

	// Confirmed document replacement or target removal: the preview no longer
	// belongs to the active document, so discard is permitted (and required).
	// This is the ONLY discard proof besides a successful record/compensate —
	// editability is never discard proof (S6-C fixup, P1 finding 4).
	if (session.DocumentReplaced(scene)
		|| scene.FindEntityByUuid(session.Target()) == entt::null)
	{
		session.Discard();
		return outcome;
	}

	// Zero-churn: no frame ever committed an effective change (value, marker
	// or schema). Nothing to record and nothing to roll back.
	if (!session.HadEffectiveFrame())
	{
		session.Discard();
		return outcome;
	}

	// The preview frames already applied the rolling-final state through the
	// composite (value + marker + promoted schema). Record ONE command whose
	// first Undo replays the immutable origin capture.
	auto cmd = BuildPreviewSessionCommand(kind, session, /*suppressNoOp=*/true);
	if (cmd)
	{
		// Already-applied preflight (S6-C re-review, P1 finding 1): prove the
		// rolling-final value, after-marker membership, promoted schema, and
		// explicit-capture validity are still live BEFORE the record can push
		// a history entry. A stale or rejected command must never poison
		// history — enter the compensate/PendingRetry flow instead.
		const auto preflight = PreflightPreviewForRecord(scene, kind, session, *cmd);
		if (preflight.has_value())
		{
			// Do NOT record. Compensate exactly when possible; when the
			// compensation also fails (stale source, stale schema, rejected
			// capture) the session stays open (PendingRetry) with recovery
			// surfaced and no history entry.
			const auto restore = RestorePreviewSession(scene, kind, session);
			outcome.mutation = restore.mutation;
			outcome.needsSyncApply = restore.needsSyncApply;
			if (restore.result == PreviewSessionCloseOutcome::Result::Closed)
				return outcome;
			outcome.result = PreviewSessionCloseOutcome::Result::PendingRetry;
			// Surface the preflight's actionable reason (stale value/marker/
			// schema, rejected capture) rather than the downstream replay error.
			outcome.lastError = *preflight;
			return outcome;
		}

		outcome.mutation = history.RecordApplied(std::move(cmd), scene,
			session.LastEffectiveResult());
		if (outcome.mutation.success && outcome.mutation.effective)
		{
			outcome.recorded = true;
			session.Discard();
			return outcome;
		}
		// The record did not land (e.g. a stale history-generation rebind
		// suppressed it): the gesture is still applied to the document, so
		// compensate it. If compensation also fails, the session stays open.
		const auto restore = RestorePreviewSession(scene, kind, session);
		if (restore.result == PreviewSessionCloseOutcome::Result::Closed)
		{
			outcome.mutation = restore.mutation;
			outcome.needsSyncApply = restore.needsSyncApply;
			return outcome;
		}
		outcome.result = PreviewSessionCloseOutcome::Result::PendingRetry;
		outcome.lastError = restore.lastError;
		outcome.needsSyncApply = true;
		return outcome;
	}

	// The final value returned to the origin (the no-op suppression fired):
	// the transient frames still left marker/schema work behind, so roll that
	// back with the unconditional compensate replay instead of recording a
	// phantom entry.
	return RestorePreviewSession(scene, kind, session);
}

PreviewSessionCloseOutcome RestorePreviewSession(
	SceneManager& scene, PreviewSessionKind kind, CompositePreviewSession& session)
{
	PreviewSessionCloseOutcome outcome;
	if (!session.IsOpen()) return outcome;

	// Confirmed document replacement or target removal: the preview state is
	// unreachable; discard without attempting the compensation. Editability is
	// never discard proof (S6-C fixup, P1 finding 4).
	if (session.DocumentReplaced(scene)
		|| scene.FindEntityByUuid(session.Target()) == entt::null)
	{
		session.Discard();
		return outcome;
	}

	if (!session.HadEffectiveFrame())
	{
		session.Discard();
		return outcome;
	}

	auto cmd = BuildPreviewSessionCommand(kind, session, /*suppressNoOp=*/false);
	if (!cmd)
	{
		// The unconditional factories only fail for an invalid script
		// before-state. Keep the session open so the failure stays visible
		// and retryable rather than silently dropping the compensation.
		outcome.result = PreviewSessionCloseOutcome::Result::PendingRetry;
		outcome.lastError = EditorMutationResult::Failure(
			rt2::core::Error::InvalidArgument, session.Target().ToString(),
			"cannot build the restore command for the live-preview gesture");
		return outcome;
	}

	// Replay the Before direction directly — not through history — so the
	// compensation records no undo entry and clears no redo.
	outcome.mutation = cmd->Undo(scene);
	outcome.needsSyncApply = true;
	if (outcome.mutation.success)
	{
		session.Discard();
		return outcome;
	}
	// A live-target failure keeps the session open with origin/rolling/owner
	// identity intact so the host can surface recovery and retry.
	outcome.result = PreviewSessionCloseOutcome::Result::PendingRetry;
	outcome.lastError = outcome.mutation;
	return outcome;
}

void ClosePreviewSessionsBeforeAction(SceneManager& scene,
	EditorCommandHistory& history, const PreviewSessionSlot* slots,
	std::size_t count, PreviewSessionsBeforeActionResult& result)
{
	result.allClosed = true;
	result.slotCount = count;
	for (std::size_t i = 0; i < count && i < 5; ++i)
	{
		const PreviewSessionSlot& slot = slots[i];
		PreviewSessionCloseSlotOutcome& slotOutcome = result.slots[i];
		const bool transform = slot.kind == PreviewSessionKind::Transform;
		if (transform)
		{
			if (!slot.transformSession || !slot.transformToken ||
				!slot.transformSession->IsOpen()) continue;
			slotOutcome.sessionWasOpen = true;
			slotOutcome.outcome = slot.finalize
				? FinalizeTransformPreviewSession(history, scene,
					*slot.transformSession, *slot.transformToken)
				: RestoreTransformPreviewSession(scene, *slot.transformSession,
					*slot.transformToken);
		}
		else
		{
			if (!slot.session || !slot.session->IsOpen()) continue;
			slotOutcome.sessionWasOpen = true;
			slotOutcome.outcome = slot.finalize
				? FinalizePreviewSession(history, scene, slot.kind, *slot.session)
				: RestorePreviewSession(scene, slot.kind, *slot.session);
		}
		if (slotOutcome.outcome.result == PreviewSessionCloseOutcome::Result::Closed)
		{
			// Owner clearing happens here; the host consumes the reopened
			// outcomes for scene sync and recovery-error surfacing.
			if (slot.owningWidgetId) *slot.owningWidgetId = 0;
		}
		else
		{
			result.allClosed = false;
		}
	}
}

namespace {
PrefabCommandTransaction::ExplicitCapture TransformCapture(
	const TransformPreviewSession& session,
	const std::vector<TransformPreviewSession::Member>& members,
	std::uint32_t beforeSchema, std::uint32_t afterSchema,
	bool useRollingMarkers, bool markerAfterPresent = true)
{
	PrefabCommandTransaction::ExplicitCapture capture;
	capture.beforeSchema = beforeSchema;
	capture.afterSchema = afterSchema;
	for (const auto& member : members)
	{
		std::optional<bool> marker;
		if (useRollingMarkers) marker = member.rollingMarker;
		else marker = member.originMarker;
		capture.markers.push_back({ member.uuid,
			PrefabComponentKeyFor<Transform>::value, marker, markerAfterPresent });
	}
	return capture;
}
}

std::unique_ptr<IEditorCommand> BuildTransformPreviewCommand(
	const TransformPreviewSession& session, bool suppressNoOp)
{
	if (!session.IsOpen()) return nullptr;
	std::vector<TransformTriple> triples;
	for (const auto& member : session.Members())
	{
		if (suppressNoOp &&
			PrefabValuePayloadEqual(PrefabValuePayload{member.originLocal},
				PrefabValuePayload{member.rollingLocal})) continue;
		triples.push_back({ member.uuid, member.originLocal, member.rollingLocal });
	}
	if (triples.empty()) return nullptr;
	std::vector<TransformPreviewSession::Member> selected;
	for (const auto& triple : triples)
		for (const auto& member : session.Members())
			if (member.uuid == triple.target) selected.push_back(member);
	bool hasCarrier = false;
	for (const auto& member : selected)
		if (member.originMarker.has_value() && member.rollingMarker.has_value()
			&& *member.originMarker != *member.rollingMarker)
			hasCarrier = true;
	const std::uint32_t replayBeforeSchema = hasCarrier
		? session.OriginSchema() : session.RollingSchema();
	return MakeTransformCommandIfEffective(std::move(triples),
		TransformCapture(session, selected, replayBeforeSchema,
			session.RollingSchema(), false));
}

PreviewSessionCloseOutcome FinalizeTransformPreviewSession(
	EditorCommandHistory& history, SceneManager& scene,
	TransformPreviewSession& session, const TransformGestureToken& token)
{
	PreviewSessionCloseOutcome outcome;
	if (!session.TokenMatches(token))
	{
		outcome.result = PreviewSessionCloseOutcome::Result::PendingRetry;
		outcome.lastError = EditorMutationResult::Failure(
			rt2::core::Error::InvalidArgument, "transform-session",
			"transform finalize token is stale or foreign");
		return outcome;
	}
	if (session.DocumentReplaced(scene) || session.PruneMissingMembers(scene) == 0)
	{
		session.Discard();
		return outcome;
	}
	if (!session.HadEffectiveFrame()) { session.Discard(); return outcome; }

	// A gesture can add a marker and then return that member's value to its
	// origin while another member remains effective.  The marker is still a
	// real live mutation, but it must not become a history entry of its own:
	// remove only the fresh marker in the same close attempt, retaining every
	// pre-existing marker.  This partition is deliberately based on the
	// immutable origin marker fact plus the current rolling value, never on a
	// guessed "last frame" classification.
	std::vector<TransformPreviewSession::Member> cleanupMembers;
	std::vector<PrefabValueEdit> cleanupValues;
	std::vector<PrefabCommandTransaction::MarkerSpec> cleanupSpecs;
	for (const auto& member : session.Members())
	{
		const bool netZero = PrefabValuePayloadEqual(
			PrefabValuePayload{member.originLocal},
			PrefabValuePayload{member.rollingLocal});
		const bool freshMarker = member.originMarker.has_value()
			&& !*member.originMarker && member.rollingMarker.has_value()
			&& *member.rollingMarker && member.introducedMarker;
		if (!netZero || !freshMarker) continue;
		cleanupMembers.push_back(member);
		cleanupValues.push_back({ PrefabValueKind::LocalTransform,
			member.uuid, PrefabMarkerDirection::After,
			PrefabValuePayload{member.rollingLocal},
			PrefabValuePayload{member.rollingLocal} });
		cleanupSpecs.push_back({ member.uuid,
			PrefabComponentKeyFor<Transform>::value, false });
	}
	EditorMutationResult cleanupMutation;
	bool cleanupApplied = false;
	if (!cleanupMembers.empty())
	{
		bool survivingMarker = false;
		for (const auto& member : session.Members())
		{
			const bool isCleaned = std::any_of(cleanupMembers.begin(),
				cleanupMembers.end(), [&](const auto& cleaned) {
					return cleaned.uuid == member.uuid;
				});
			if (!isCleaned && member.rollingMarker.has_value()
				&& *member.rollingMarker)
				survivingMarker = true;
		}
		const std::uint32_t cleanupAfterSchema = survivingMarker
			? session.RollingSchema() : session.OriginSchema();
		PrefabCommandTransaction::ExplicitCapture cleanupCapture =
			TransformCapture(session, cleanupMembers, session.RollingSchema(),
				cleanupAfterSchema, true, false);
		PrefabCommandTransaction cleanupTx(std::move(cleanupValues),
			std::move(cleanupSpecs));
		cleanupTx.SetExplicitCapture(std::move(cleanupCapture));
		cleanupMutation = cleanupTx.Execute(scene);
		if (!cleanupMutation.success)
		{
			outcome.result = PreviewSessionCloseOutcome::Result::PendingRetry;
			outcome.lastError = cleanupMutation;
			return outcome;
		}
		for (const auto& member : cleanupMembers)
			session.MarkMarkerRemoved(member.uuid);
		session.SetRollingSchema(scene.AuthoringDoc().metadata.schemaVersion);
		cleanupApplied = cleanupMutation.effective;
	}

	// Exact preflight immediately before RecordApplied: prior frame results do
	// not authorize a changed value, marker classification, or schema.
	for (const auto& member : session.Members())
	{
		const auto entity = scene.FindEntityByUuid(member.uuid);
		EditableTRS live;
		if (entity == entt::null || !scene.GetLocalTransform({ entity }, live) ||
			!PrefabValuePayloadEqual(PrefabValuePayload{live},
				PrefabValuePayload{member.rollingLocal}))
		{
			outcome.result = PreviewSessionCloseOutcome::Result::PendingRetry;
			outcome.lastError = EditorMutationResult::Failure(
				 rt2::core::Error::InvalidArgument, member.uuid.ToString(),
				"transform finalize preflight found a stale local value");
			return outcome;
		}
		const auto presence = scene.IsOverridden(member.uuid,
			PrefabComponentKeyFor<Transform>::value);
		if (member.rollingMarker.has_value() != presence.IsOk() ||
			(member.rollingMarker && presence.IsOk() &&
				*member.rollingMarker != presence.value))
		{
			outcome.result = PreviewSessionCloseOutcome::Result::PendingRetry;
			outcome.lastError = EditorMutationResult::Failure(
				rt2::core::Error::InvalidArgument, member.uuid.ToString(),
				"transform finalize preflight found a stale marker membership");
			return outcome;
		}
	}
	if (scene.AuthoringDoc().metadata.schemaVersion != session.RollingSchema())
	{
		outcome.result = PreviewSessionCloseOutcome::Result::PendingRetry;
		outcome.lastError = EditorMutationResult::Failure(
			rt2::core::Error::InvalidArgument, "transform-schema",
			"transform finalize preflight found a stale schema");
		return outcome;
	}
	auto command = BuildTransformPreviewCommand(session, true);
	if (!command)
	{
		session.Discard();
		if (cleanupApplied)
		{
			outcome.mutation = cleanupMutation;
			outcome.needsSyncApply = true;
		}
		return outcome;
	}
	const auto* transformCommand = dynamic_cast<const TransformCommand*>(command.get());
	if (!transformCommand || transformCommand->ExplicitCaptureRejected())
	{
		outcome.result = PreviewSessionCloseOutcome::Result::PendingRetry;
		outcome.lastError = EditorMutationResult::Failure(
			rt2::core::Error::InvalidArgument, "transform-session",
			"transform finalize preflight rejected the explicit capture");
		return outcome;
	}
	const EditorMutationResult recordResult = history.RecordApplied(
		std::move(command), scene, session.LastEffectiveResult());
	outcome.mutation = recordResult;
	if (cleanupApplied)
	{
		// Recording is already-applied and therefore does not own scene sync.
		// The only scene mutation at close is the fresh-marker cleanup; keep its
		// result separate from the history result so exactly one sync is routed.
		outcome.mutation = cleanupMutation;
		outcome.needsSyncApply = true;
	}
	if (!recordResult.success)
	{
		outcome.result = PreviewSessionCloseOutcome::Result::PendingRetry;
		outcome.lastError = recordResult;
		return outcome;
	}
	outcome.recorded = true;
	session.Discard();
	return outcome;
}

PreviewSessionCloseOutcome RestoreTransformPreviewSession(
	SceneManager& scene, TransformPreviewSession& session,
	const TransformGestureToken& token)
{
	PreviewSessionCloseOutcome outcome;
	if (!session.TokenMatches(token))
	{
		outcome.result = PreviewSessionCloseOutcome::Result::PendingRetry;
		outcome.lastError = EditorMutationResult::Failure(
			rt2::core::Error::InvalidArgument, "transform-session",
			"transform restore token is stale or foreign");
		return outcome;
	}
	if (session.DocumentReplaced(scene) || session.PruneMissingMembers(scene) == 0)
	{
		session.Discard();
		return outcome;
	}
	if (!session.HadEffectiveFrame()) { session.Discard(); return outcome; }
	std::vector<PrefabValueEdit> values;
	std::vector<PrefabCommandTransaction::MarkerSpec> specs;
	std::vector<TransformPreviewSession::Member> selected;
	for (const auto& member : session.Members())
	{
		const bool valueChanged = !PrefabValuePayloadEqual(
			PrefabValuePayload{member.originLocal}, PrefabValuePayload{member.rollingLocal});
		const bool markerChanged = member.introducedMarker;
		if (!valueChanged && !markerChanged) continue;
		values.push_back({ PrefabValueKind::LocalTransform, member.uuid,
			PrefabMarkerDirection::After, PrefabValuePayload{member.originLocal},
			PrefabValuePayload{member.rollingLocal} });
		// The Before direction uses the captured origin membership. For an
		// ordinary entity the marker is ignored by the composite.
		const bool afterPresent = member.rollingMarker.has_value() ?
			*member.rollingMarker : true;
		specs.push_back({ member.uuid, PrefabComponentKeyFor<Transform>::value,
			afterPresent });
		selected.push_back(member);
	}
	if (values.empty()) { session.Discard(); return outcome; }
	// Restoring the gesture-origin schema is legal only when this same
	// composite still carries a real marker-vector transition.  If the sole
	// fresh carrier was removed before Escape/recovery, a surviving ordinary
	// value must restore against the already-live schema (normally v6/v6),
	// never request an illegal schema-only downgrade.
	bool survivingCarrier = false;
	for (const auto& member : selected)
	{
		if (member.originMarker.has_value() && member.rollingMarker.has_value()
			&& *member.originMarker != *member.rollingMarker)
		{
			survivingCarrier = true;
			break;
		}
	}
	const std::uint32_t restoreAfterSchema = survivingCarrier
		? session.OriginSchema() : session.RollingSchema();
	// Undo reads the captured After side as its directional source.  Keep the
	// durable command endpoints aligned with the value direction (origin ->
	// rolling), while allowing the no-carrier case to remain live/live.
	auto capture = TransformCapture(session, selected, restoreAfterSchema,
		session.RollingSchema(), false);
	for (std::size_t i = 0; i < specs.size(); ++i)
		capture.markers[i].afterPresent = specs[i].afterPresent;
	PrefabCommandTransaction tx(std::move(values), std::move(specs));
	tx.SetExplicitCapture(std::move(capture));
	outcome.mutation = tx.Undo(scene);
	if (!outcome.mutation.success)
	{
		outcome.result = PreviewSessionCloseOutcome::Result::PendingRetry;
		outcome.lastError = outcome.mutation;
		return outcome;
	}
	outcome.needsSyncApply = outcome.mutation.effective;
	session.Discard();
	return outcome;
}
