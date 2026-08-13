#pragma once

#ifndef RT2_PREVIEW_SESSION_CLOSE_H
#define RT2_PREVIEW_SESSION_CLOSE_H

#include "CompositePreviewSession.h"
#include "EditorCommand.h"
#include "EditorCommandHistory.h"
#include "SceneManager.h"

#include <cstddef>
#include <memory>
#include <optional>

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

// Already-applied preflight (S6-C re-review, P1 finding 1): prove that a
// recorded close command is still exact before RecordApplied pushes it. Pass
// the command FinalizePreviewSession is about to record. Returns nullopt when
// the command is safe to record — the preview's rolling-final VALUE, its
// after-marker MEMBERSHIP, the promoted SCHEMA, and the command's EXPLICIT
// CAPTURE validity are all still the live state. Returns an EditorMutationResult
// describing the failing condition otherwise; the caller must NOT record the
// command and must enter the compensate / PendingRetry flow instead, so stale
// or rejected state can never poison history.
std::optional<EditorMutationResult> PreflightPreviewForRecord(
	SceneManager& scene, PreviewSessionKind kind, const CompositePreviewSession& session,
	const IEditorCommand& cmd);

// Transform-session close seam. Transform sessions use durable UUID-keyed
// batches and the opaque gesture token; they do not reuse the single-member
// CompositePreviewSession payload.
std::unique_ptr<IEditorCommand> BuildTransformPreviewCommand(
	const TransformPreviewSession& session, bool suppressNoOp = true);
PreviewSessionCloseOutcome FinalizeTransformPreviewSession(
	EditorCommandHistory& history, SceneManager& scene,
	TransformPreviewSession& session, const TransformGestureToken& token);
PreviewSessionCloseOutcome RestoreTransformPreviewSession(
	SceneManager& scene, TransformPreviewSession& session,
	const TransformGestureToken& token);

// One host preview-session slot for the global-action reducer below.
struct PreviewSessionSlot
{
	PreviewSessionKind kind = PreviewSessionKind::Light;
	CompositePreviewSession* session = nullptr;
	// Host owning-widget ID; cleared when the session closes (may be null in
	// host-less tests).
	unsigned int* owningWidgetId = nullptr;
	// Close mode: true = record (finalize); false = abandon (restore/escape).
	bool finalize = false;
};

// CPU-linkable publish-admission decision (S6-C final-verdict P1 finding 1):
// a live-preview FRAME may be published only when the session is open, the
// changed widget is the widget that OPENED the session, the target matches the
// session target, and the session's recovery is not pending. While any session
// of this kind is PendingRetry the frame is frozen — an unresolved gesture must
// be immutable except for Retry/Reconcile or a proven replacement/removal, and
// a same-target different-widget change must not absorb into the pending
// session. SceneEditorUI gates every publish block with this function so the
// freeze contract is CPU-testable without ImGui.
inline bool PreviewPublishAllowed(const CompositePreviewSession& session,
	const rt2::core::UUID& target, unsigned int changedWidgetId,
	unsigned int owningWidgetId, bool recoveryPending)
{
	if (!session.IsOpen()) return false;
	if (session.Target() != target) return false;
	if (changedWidgetId != owningWidgetId) return false;
	if (recoveryPending) return false;
	return true;
}

// Per-slot outcome for the global-action reducer.
struct PreviewSessionCloseSlotOutcome
{
	// The slot held an open session this round (so the host knows to inspect
	// `outcome`).
	bool sessionWasOpen = false;
	PreviewSessionCloseOutcome outcome;
};

// Result of closing every open preview session before a document-preserving
// global action. `allClosed` is false when any session remains pending — the
// caller must ABORT the requested action. Per-slot outcomes are rich enough
// for the host's owner/error/sync responsibilities: the reducer clears each
// closed slot's owning-widget ID, and the host routes scene sync from
// outcome.needsSyncApply and surfaces recovery from outcome.lastError.
struct PreviewSessionsBeforeActionResult
{
	bool allClosed = false;
	std::size_t slotCount = 0;
	PreviewSessionCloseSlotOutcome slots[4];
};

// CPU-linkable recovery reducer (S6-C re-review P1 finding 2 / final closure):
// close every open preview session through the two-phase policy BEFORE a
// document-preserving global action (Undo/Redo). This is the product decision
// seam SceneEditorUI::Undo/Redo actually call (not a test-only duplicate), so
// the abort-on-pending, owner-clearing, and per-slot outcome contract is
// testable without ImGui. When a slot's session closes, its owning-widget ID is
// cleared; when it stays pending, the ID is preserved and result.allClosed is
// set false, and the caller must ABORT the requested action and leave the
// pending session surfaced for recovery.
void ClosePreviewSessionsBeforeAction(SceneManager& scene,
	EditorCommandHistory& history, const PreviewSessionSlot* slots,
	std::size_t count, PreviewSessionsBeforeActionResult& result);

// Host-edge P1 finding 1: the shared ordinary admission / Undo/Redo gate.
// Returns false IMMEDIATELY when `anyRecoveryPending` — calling it does not run
// the reducer, so there is no implicit close retry, no owner/error overwrite,
// no scene sync, no UUID draw, and no mutation. Only the explicit Retry action
// (and proven replacement/removal handling) re-runs a pending close. When not
// pending it runs the reducer over `slots` and returns whether every session
// closed. SceneEditorUI::CloseAllPreviewSessionsForAction delegates here so the
// short-circuit contract is CPU-testable without ImGui.
inline bool ClosePreviewSessionsAndAdmit(SceneManager& scene,
	EditorCommandHistory& history, PreviewSessionSlot* slots, std::size_t count,
	PreviewSessionsBeforeActionResult& result, bool anyRecoveryPending)
{
	if (anyRecoveryPending) return false; // no implicit retry
	ClosePreviewSessionsBeforeAction(scene, history, slots, count, result);
	return result.allClosed;
}

#endif // RT2_PREVIEW_SESSION_CLOSE_H
