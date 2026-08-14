#pragma once

#ifndef RT2_COMPOSITE_PREVIEW_SESSION_H
#define RT2_COMPOSITE_PREVIEW_SESSION_H

#include "PrefabCommandTransaction.h"
#include "SceneManager.h"
#include "SceneMutation.h"

#include <functional>
#include <cstdint>
#include <optional>
#include <atomic>
#include <utility>
#include <vector>

// ============================================================================
// CompositePreviewSession — S6-C live-preview composite session (Phase 8 W3).
//
// The S6-B live-preview editors (light, camera, motion velocity, and the
// Int/Float/Vec3/Color script fields) mutated the scene per frame OUTSIDE the
// S5/S6-A composite seam and recorded a fabricated success on close. A
// live-preview edit therefore changed an inherited prefab wire without marking
// it overridden — the W3 D-list defect S6-C removes.
//
// This session replaces that path. It owns:
//   - an immutable gesture origin — the pre-activation value, per-member
//     override-set membership, and document schema + generation, captured at
//     Begin() BEFORE any preview frame, stored as a
//     PrefabCommandTransaction::ExplicitCapture so the recorded command's
//     first Undo can replay it verbatim;
//   - a rolling preview source — the last successfully committed value,
//     advanced ONLY after a successful, effective preview composite;
//   - the real EditorMutationResult of every preview commit (no fabricated
//     successes);
//   - whether any frame was ever effective (the record-vs-restore-vs-
//     zero-churn discriminator).
//
// Every preview frame runs a fresh PrefabCommandTransaction in the After
// direction through the atomic composite. Frame one stages rolling(origin) ->
// target with an absent->present marker and promotes the live schema; frame
// two+ stage lastCommitted -> target with the marker already present and the
// live schema on both sides — the rolling source never reuses the stale
// frame-zero source. Prepare validates the staged source against live state,
// so a rolling source that diverged from the document fails loudly instead of
// silently re-marking.
//
// Two-phase close: the host calls CloseCommit (record origin->final with the
// gesture's last effective composite result) or CloseRestore (compensate with
// a Before replay that removes only the markers this gesture introduced and
// restores the origin schema; records no history). The session is discarded
// only after a successful record, a successful restore, zero-churn, or proof
// that the document generation moved on (the preview no longer belongs to the
// active document). A restore failure against a still-live target keeps the
// session open for retry and the failure is surfaced.
//
// CPU-linkable: no ImGui, no EnTT handles, no pointers into the scene beyond
// the SceneManager reference passed per call.
// ============================================================================

class SceneManager;

class CompositePreviewSession
{
public:
	// Reads the durable value of `member` back from the live scene AFTER a
	// successful preview commit, so the rolling source holds the committed
	// (possibly manager-canonicalized) form rather than the pre-commit target.
	// `rawTarget` is the value the commit was asked to stage and is the
	// fallback when the live read fails (entity or component vanished), so a
	// stale read can never poison the rolling source with a default value.
	// Only meaningful for the member the session targets.
	using ValueReader = std::function<PrefabValuePayload(const rt2::core::UUID&,
		const PrefabValuePayload& rawTarget)>;

	CompositePreviewSession() = default;

	bool IsOpen() const { return m_Open; }
	const rt2::core::UUID& Target() const { return m_Target; }
	// True once any preview frame committed a change (value, marker, or
	// schema). The record-vs-restore-vs-zero-churn discriminator.
	bool HadEffectiveFrame() const { return m_HadEffectiveFrame; }
	// The last frame's real composite result (success/effective arbitrary).
	const EditorMutationResult& LastResult() const { return m_LastResult; }
	// The most recent EFFECTIVE composite result of the gesture — the real
	// aggregate applied to the document, used to record the close command.
	const EditorMutationResult& LastEffectiveResult() const { return m_LastEffectiveResult; }
	// The immutable origin capture a recorded/restore command replays on its
	// first Undo (see the S6-C command factories).
	const PrefabCommandTransaction::ExplicitCapture& Origin() const { return m_Origin; }
	// The pre-activation value (the recorded command's Before state).
	const PrefabValuePayload& OriginValue() const { return m_OriginValue; }
	// The last successfully committed value (== the live state absent any
	// out-of-band change; the recorded command's After state).
	const PrefabValuePayload& RollingValue() const { return m_RollingValue; }
	// The component wire this gesture overrides (the recorded command's marker
	// key).
	const PrefabComponentKey& Key() const { return m_Key; }
	// The durable value currently stored on the target, read back from live via
	// the session's reader (falls back to the rolling committed value when the
	// target or component vanished). Used to advance the rolling source after a
	// successful preview commit.
	PrefabValuePayload ReadLiveValue(SceneManager& scene) const;
	// EXACT validate-only live read (S6-C final closure, P1 finding 1): unlike
	// ReadLiveValue, this never substitutes the rolling committed value. It
	// reports absence explicitly so the finalize preflight can distinguish
	// "component removed out of band" from "value equals rolling":
	//   - Light/Camera: nullopt when the component is missing (a preflight
	//     failure);
	//   - Motion/Script: an exact optional{} (or present) payload — a removed
	//     component reads as absence, never as the rolling final.
	// nullopt is also returned when the target entity is gone or the kind is
	// not one of the four live-preview kinds.
	std::optional<PrefabValuePayload> ReadLiveValueExact(SceneManager& scene) const;
	// The document schema the gesture should have left behind: promoted to the
	// serializer's current schema when the gesture introduced an absent->present
	// marker, else the pre-gesture (origin) schema. Used by the finalize
	// preflight to prove the promoted schema is still live (S6-C re-review P1).
	std::uint32_t ExpectedAfterSchema() const;

	// Begin a gesture on `member`. `originValue` must be the live value read
	// BEFORE this frame's mutation. Captures the immutable origin override-set
	// membership for `key` and the live document schema + generation. Returns
	// false when origin capture fails (absent member, malformed override
	// vector, unknown or excluded wire) — the caller must not open the
	// session. A NOT-prefab-member target is NOT a failure: it becomes a
	// value-only session whose marker delta is dropped at capture.
	bool Begin(SceneManager& scene, const rt2::core::UUID& member,
		PrefabValueKind kind, PrefabComponentKey key,
		PrefabValuePayload originValue, ValueReader reader);

	// Apply one preview frame: stages rolling -> target through the S5/S6-A
	// atomic composite. On a successful, effective commit the rolling source
	// advances to the committed live value (via the reader) and the result is
	// remembered as the gesture's last effective result. A failed preview or a
	// canonical no-op leaves the rolling source untouched. Never-effective
	// sessions remain zero-churn.
	EditorMutationResult Preview(SceneManager& scene, PrefabValuePayload target);

	// True when the authoring document was replaced since Begin() (the preview
	// state no longer belongs to the active document).
	bool DocumentReplaced(const SceneManager& scene) const
	{
		return m_OriginDocumentGeneration != scene.DocumentGeneration();
	}

	void Discard();

private:
	rt2::core::UUID                          m_Target;
	PrefabValueKind                          m_Kind = PrefabValueKind::EntityName;
	PrefabComponentKey                       m_Key;
	PrefabValuePayload                       m_OriginValue;
	PrefabValuePayload                       m_RollingValue;
	PrefabCommandTransaction::ExplicitCapture m_Origin;
	std::uint64_t                            m_OriginDocumentGeneration = 0;
	ValueReader                              m_Reader;
	bool                                     m_Open = false;
	bool                                     m_HadEffectiveFrame = false;
	EditorMutationResult                     m_LastResult;
	EditorMutationResult                     m_LastEffectiveResult;
};

// Opaque authorization for a transform gesture.  Callers can only compare
// or pass the token they received from Begin; owner and sequence are not
// caller-controlled continuation fields.
class TransformGestureToken
{
public:
	bool IsValid() const { return m_Owner != 0 && m_Sequence != 0; }
	friend bool operator==(const TransformGestureToken& a, const TransformGestureToken& b)
	{ return a.m_Owner == b.m_Owner && a.m_Sequence == b.m_Sequence; }
	friend bool operator!=(const TransformGestureToken& a, const TransformGestureToken& b)
	{ return !(a == b); }

private:
	friend class TransformPreviewSession;
	TransformGestureToken(std::uint64_t owner, std::uint64_t sequence)
		: m_Owner(owner), m_Sequence(sequence) {}
	std::uint64_t m_Owner = 0;
	std::uint64_t m_Sequence = 0;
};

inline bool TransformBeginAdmissionAllowed(bool editable,
	bool recoveryPending, std::uint64_t opaqueOwner)
{
	return editable && !recoveryPending && opaqueOwner != 0;
}

class TransformPreviewSession
{
public:
	enum class ClosePhase
	{
		LivePreview,
		// Finalize already completed any net-zero marker cleanup and
		// RecordApplied failed.  Retry is compensation-only: it must not
		// rebuild/record the command or recreate a cleaned marker.
		CompensationPending,
	};

	struct Member
	{
		rt2::core::UUID uuid;
		EditableTRS originLocal;
		EditableTRS rollingLocal;
		std::optional<bool> originMarker;
		std::optional<bool> rollingMarker;
		bool everEffective = false;
		bool introducedMarker = false;
	};

	std::optional<TransformGestureToken> Begin(SceneManager& scene,
		std::uint64_t opaqueOwner,
		const std::vector<rt2::core::UUID>& orderedUuids);
	bool IsOpen() const { return m_Open; }
	const TransformGestureToken& Token() const { return m_Token; }
	const std::vector<Member>& Members() const { return m_Members; }
	std::uint32_t OriginSchema() const { return m_OriginSchema; }
	std::uint32_t RollingSchema() const { return m_RollingSchema; }
	std::uint64_t DocumentGeneration() const { return m_DocumentGeneration; }
	bool DocumentReplaced(const SceneManager& scene) const
	{ return m_DocumentGeneration != scene.DocumentGeneration(); }
	bool HadEffectiveFrame() const { return m_HadEffectiveFrame; }
	ClosePhase GetClosePhase() const { return m_ClosePhase; }
	const EditorMutationResult& LastResult() const { return m_LastResult; }
	const EditorMutationResult& LastEffectiveResult() const { return m_LastEffectiveResult; }
	const PrefabCommandTransaction::ExplicitCapture& Origin() const { return m_Origin; }
	bool TokenMatches(const TransformGestureToken& token) const
	{ return m_Open && token.IsValid() && token == m_Token; }

	EditorMutationResult PreviewLocals(SceneManager& scene,
		const TransformGestureToken& token,
		const std::vector<std::pair<rt2::core::UUID, EditableTRS>>& targets);
	EditorMutationResult PreviewWorlds(SceneManager& scene,
		const TransformGestureToken& token,
		const std::vector<std::pair<rt2::core::UUID, glm::mat4>>& targets);
	// Remove members that were deleted out-of-band. The durable survivors keep
	// their captured origin/rolling state and remain closeable; returning zero
	// means the gesture has no live members and may be discarded.
	std::size_t PruneMissingMembers(const SceneManager& scene);
	void SetRollingSchema(std::uint32_t schema) { m_RollingSchema = schema; }
	void MarkMarkerRemoved(const rt2::core::UUID& uuid)
	{
		if (auto* member = FindMember(uuid))
		{
			member->rollingMarker = false;
			member->introducedMarker = false;
		}
	}
	void EnterCompensationPending() { m_ClosePhase = ClosePhase::CompensationPending; }
	// Bounded CPU fault seam: only the next compensation attempt is rejected.
	// It exists beside EditorCommandHistory::FailNextRecordAppliedForTest so
	// the cleanup-complete recovery phase is directly discriminable.
	void FailNextCompensationForTest() { m_FailNextCompensation = true; }
	void FailNextCleanupForTest() { m_FailNextCleanup = true; }
	bool ConsumeCleanupFailureForTest()
	{
		const bool fail = m_FailNextCleanup;
		m_FailNextCleanup = false;
		return fail;
	}
	bool ConsumeCompensationFailureForTest()
	{
		const bool fail = m_FailNextCompensation;
		m_FailNextCompensation = false;
		return fail;
	}

	void Discard();

private:
	static std::uint64_t NextSequence();
	EditorMutationResult Fail(rt2::core::Error::Code code,
		const std::string& detail) const;
	Member* FindMember(const rt2::core::UUID& uuid);
	const Member* FindMember(const rt2::core::UUID& uuid) const;

	std::vector<Member> m_Members;
	PrefabCommandTransaction::ExplicitCapture m_Origin;
	TransformGestureToken m_Token{0, 0};
	std::uint32_t m_OriginSchema = 0;
	std::uint32_t m_RollingSchema = 0;
	std::uint64_t m_DocumentGeneration = 0;
	bool m_Open = false;
	bool m_HadEffectiveFrame = false;
	ClosePhase m_ClosePhase = ClosePhase::LivePreview;
	bool m_FailNextCompensation = false;
	bool m_FailNextCleanup = false;
	EditorMutationResult m_LastResult;
	EditorMutationResult m_LastEffectiveResult;
};

#endif // RT2_COMPOSITE_PREVIEW_SESSION_H
