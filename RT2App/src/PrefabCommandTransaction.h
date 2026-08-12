#pragma once

#ifndef RT2_PREFAB_COMMAND_TRANSACTION_H
#define RT2_PREFAB_COMMAND_TRANSACTION_H

#include "SceneManager.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// ============================================================================
// PrefabCommandTransaction — S6-B command transaction state (Phase 8 W3).
//
// A command built on this stores ONLY durable data: the before/after value
// payloads it touches (as PrefabValueEdit), the marker membership deltas it
// produces (as MarkerSpec), and — captured from live state on the FIRST
// replay — the before/after document schema pair.
//
// Execute and Redo replay the After direction; Undo replays Before. Every
// replay runs the S5/S6-A atomic composite (PreparePrefabCompositeEdits ->
// CommitPrefabCompositePlan -> ToEditorMutationResult), so the first value
// write, any marker insertion, and any schema promotion land in ONE commit
// with ONE revision bump and at most one NotifyAuthoringChanged().
//
// Marker capture: every public MarkerSpec key is validated/canonicalized
// against the frozen table (FindComponentByWire) BEFORE any membership query,
// and a key that is not a known overridable wire — unknown, or present but
// excluded — aborts the capture with InvalidArgument regardless of the
// target's entity class. Only then is override-set membership read via
// SceneManager::IsOverridden. An ordinary entity (NotPrefabMember) drops the
// delta and the edit degrades to value-only, so non-prefab behavior is
// unchanged. ANY other IsOverridden failure — an absent member
// (InvalidEntity) or a malformed stored override vector (InvalidArgument) —
// aborts the capture: the command cannot commit a value-only composite while
// the marker/schema/history stay untouched. Removing a wire that is currently
// INHERITED (not overridden) is marked as explicitly overridden-absent so the
// prefab source cannot resurrect the component; a locally added-then-removed
// member still returns to source with no marker.
//
// Schema capture: m_BeforeSchema is the live document schema at first replay;
// m_AfterSchema is promoted to SceneSerializer::SchemaVersion exactly when a
// marker transitions absent -> present. The pair is then fixed for the life
// of the command, so Undo of a first add restores the captured prior schema
// and Redo re-applies the promotion (D3.6/D3.10).
//
// Explicit capture (S6-C): a RECORDED live-preview command (RecordApplied) is
// never Executed — its first replay is Undo, by which time the preview frames
// already inserted the marker and promoted the schema. Live capture would
// therefore read beforePresent=true and Undo would remove nothing. The
// live-preview session calls SetExplicitCapture with the immutable origin
// membership/schema pair it captured at open; Capture() then replays that
// state verbatim (keys still canonicalized/validated) so Undo removes exactly
// the markers the gesture introduced and restores the origin schema.
// ============================================================================

class SceneManager;

class PrefabCommandTransaction
{
public:
	// One marker membership delta. `key` must be a KNOWN OVERRIDABLE prefab
	// wire (validated at capture regardless of the target's entity class: an
	// unknown or excluded wire aborts the command with InvalidArgument before
	// the NotPrefabMember skip); `afterPresent` is the membership the After
	// direction produces. A delta whose member is an ordinary entity is
	// dropped at capture.
	struct MarkerSpec
	{
		rt2::core::UUID member;
		PrefabComponentKey key;
		bool afterPresent = true;
	};

	// S6-C: one immutable origin marker membership fact for a RECORDED
	// live-preview command. `beforePresent` is the override-set membership read
	// at gesture open (nullopt = ordinary entity; the delta is dropped exactly
	// as live capture drops NotPrefabMember); `afterPresent` is the membership
	// the After direction ends at. Every S6-C live-preview edit targets an
	// existing component (light, camera, motion velocity, script field), so
	// afterPresent is true for all of them.
	struct ExplicitMarker
	{
		rt2::core::UUID member;
		PrefabComponentKey key;
		std::optional<bool> beforePresent; // nullopt = not a prefab member
		bool afterPresent = true;
	};

	// The immutable origin marker/schema pair a recorded live-preview command
	// replays on its first Undo (see SetExplicitCapture). A live-preview
	// session captures this at open via SceneManager::IsOverridden + the live
	// document schema and passes it through the command factories.
	struct ExplicitCapture
	{
		std::vector<ExplicitMarker> markers;
		std::uint32_t beforeSchema = 0;
	};

	PrefabCommandTransaction() = default;
	PrefabCommandTransaction(std::vector<PrefabValueEdit> values,
	                         std::vector<MarkerSpec> markers)
		: m_Values(std::move(values))
		, m_Markers(std::move(markers)) {}

	// Apply the After direction. First replay captures the schema pair and
	// marker membership from live state.
	EditorMutationResult Execute(SceneManager& scene);

	// Apply the Before direction (inverse).
	EditorMutationResult Undo(SceneManager& scene);

	// Same pure "apply the after-state" call as Execute.
	EditorMutationResult Redo(SceneManager& scene);

	// S6-C: fix the marker/schema capture to the immutable origin state in
	// place of live read-back. A RECORDED (RecordApplied) live-preview command
	// is never Executed; its first replay is Undo, by which time the preview
	// frames already inserted the marker and promoted the schema, so live
	// capture would read beforePresent=true and Undo would remove nothing.
	// Replaying the explicit origin makes Undo remove exactly the markers the
	// gesture introduced and restore the origin schema. Must be called before
	// the first replay (the command built through a factory does this at
	// construction). Capture() still canonicalizes/validates each key the same
	// way as live capture. IMPORTANT (S6-C fixup, P2 finding 5): the supplied
	// capture is validated ONE-FOR-ONE against the transaction's declared
	// MarkerSpecs — same member, canonical key, and afterPresent, with no
	// duplicates and no member that differs from the value-write entity. An
	// invalid capture is never silently accepted: SetExplicitCapture records a
	// rejection (see ExplicitCaptureRejected) and every later replay fails
	// loudly instead of applying a forged ordinary/member mix.
	void SetExplicitCapture(ExplicitCapture explicitCapture);

	// True when the supplied explicit capture failed one-for-one validation
	// against the declared marker specs / value target (P2 finding 5). A
	// rejected capture is surfaced by Capture()/Replay() as a loud failure;
	// it is never applied.
	bool ExplicitCaptureRejected() const { return m_ExplicitCaptureRejected; }

private:
	EditorMutationResult Replay(SceneManager& scene, PrefabMarkerDirection direction);
	// Fallible: returns a Failure (and does NOT set m_Captured) when a marker
	// key is unknown or excluded, or when any IsOverridden error other than
	// NotPrefabMember occurs during capture.
	EditorMutationResult Capture(SceneManager& scene);

	std::vector<PrefabValueEdit> m_Values;
	std::vector<MarkerSpec> m_Markers;
	bool m_Captured = false;
	std::uint32_t m_BeforeSchema = 0;
	std::uint32_t m_AfterSchema = 0;
	struct CapturedMarker
	{
		rt2::core::UUID member;
		PrefabComponentKey key;
		std::optional<bool> beforePresent; // nullopt = not a prefab member
		bool afterPresent = true;
	};
	std::vector<CapturedMarker> m_CapturedMarkers;
	// S6-C: when set, Capture() replays this immutable origin state verbatim
	// instead of reading live membership/schema (recorded live-preview
	// commands, whose first replay is Undo).
	std::optional<ExplicitCapture> m_ExplicitCapture;
	// S6-C fixup (P2 finding 5): the caller-supplied explicit capture failed
	// one-for-one validation against the declared marker specs / value target.
	// Capture()/Replay() surface it as a loud failure rather than applying a
	// forged capture.
	bool m_ExplicitCaptureRejected = false;
};

#endif // RT2_PREFAB_COMMAND_TRANSACTION_H
