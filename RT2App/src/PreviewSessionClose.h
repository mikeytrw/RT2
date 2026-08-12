#pragma once

#ifndef RT2_PREVIEW_SESSION_CLOSE_H
#define RT2_PREVIEW_SESSION_CLOSE_H

#include "CompositePreviewSession.h"
#include "EditorCommand.h"
#include "EditorCommandHistory.h"
#include "SceneManager.h"

#include <memory>

// ============================================================================
// PreviewSessionClose — S6-C live-preview two-phase close (Phase 8 W3).
//
// The four live-preview editors (light, camera, motion velocity, and the
// Int/Float/Vec3/Color script fields) preview each frame through
// CompositePreviewSession and close through this CPU-linkable core. It is the
// decision machine the SceneEditorUI ImGui glue delegates to, so the
// retry/recovery/discard discrimination is testable without ImGui:
//   - FinalizePreviewSession records ONE command (immutable origin -> rolling
//     final, explicit capture attached, already applied by the preview frames)
//     via RecordApplied, or discards on zero-churn.
//   - RestorePreviewSession compensates the preview frames by replaying the
//     gesture's explicit capture in the Before direction (removes only this
//     gesture's markers, restores the origin schema + value). Records no
//     history.
//
// Discard-vs-retry discrimination (S6-C fixup, P1 findings 2 + 4): an
// effective session is discarded ONLY after a confirmed document replacement
// (session.DocumentReplaced), a confirmed target removal
// (SceneManager::FindEntityByUuid == null), or a successful record/compensate.
// Editability is NOT discard proof — leaving edit mode must finalize or
// compensate the live session, never silently orphan the authored preview
// state. A close that fails against a still-live target returns PendingRetry
// with the session left OPEN (origin/rolling/owner identity preserved) so the
// host can surface a recovery action and retry.
// ============================================================================

// Which command family a CompositePreviewSession finalizes into.
enum class PreviewSessionKind { Light, Camera, Motion, Script };

// Outcome of a two-phase close attempt.
struct PreviewSessionCloseOutcome
{
	enum class Result
	{
		Closed,       // recorded, compensated, or discarded -> session is closed
		PendingRetry, // close failed against a live target -> session stays OPEN
	};
	Result result = Result::Closed;
	// The last mutation applied during the close (the record result or the
	// restore replay result). Success/effective when the close succeeded.
	EditorMutationResult mutation;
	// True when the close mutated the SCENE (a restore/compensate replay or a
	// retryable failure), so the host must route mutation.syncImpact through
	// its sync path. False for a pure record (the preview frames already
	// applied and notified the value) and for a discard.
	bool needsSyncApply = false;
	// True when the close recorded a history entry (finalize path).
	bool recorded = false;
	// The failure that left the session pending (PendingRetry only).
	EditorMutationResult lastError;
};

// Build the origin->final command for a preview session, attaching its
// immutable explicit capture. suppressNoOp mirrors the MakeSet*IfEffective
// suppression (used by the record path); when false the command is built
// unconditionally (used by the compensate/restore path, where even a
// value-equal origin->final must still roll back markers + schema).
std::unique_ptr<IEditorCommand> BuildPreviewSessionCommand(
	PreviewSessionKind kind, const CompositePreviewSession& session,
	bool suppressNoOp);

// Close a live-preview session by recording ONE command (or discarding on
// zero-churn / confirmed document replacement / confirmed target removal). A
// failed record falls back to compensation; if that also fails the session
// stays OPEN (PendingRetry) and the host must surface a recovery path.
PreviewSessionCloseOutcome FinalizePreviewSession(
	EditorCommandHistory& history, SceneManager& scene,
	PreviewSessionKind kind, CompositePreviewSession& session);

// Escape/return-to-start: compensate the preview frames (removes only this
// gesture's markers, restores the origin schema + value). Records no history.
// On failure against a live target the session stays OPEN (PendingRetry).
PreviewSessionCloseOutcome RestorePreviewSession(
	SceneManager& scene, PreviewSessionKind kind, CompositePreviewSession& session);

#endif // RT2_PREVIEW_SESSION_CLOSE_H
