#include "PreviewSessionClose.h"

#include "EditorPropertyCommands.h"

#include <utility>

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
	}
	return nullptr;
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
		outcome.mutation = history.RecordApplied(std::move(cmd), scene,
			session.LastEffectiveResult());
		if (outcome.mutation.success && outcome.mutation.effective)
		{
			outcome.recorded = true;
			session.Discard();
			return outcome;
		}
		// The record did not land (e.g. the explicit capture was rejected, or
		// a stale history-generation rebind suppressed it): the gesture is
		// still applied to the document, so compensate it. If compensation
		// also fails, the session stays open for retry.
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
