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
// Marker capture: for each MarkerSpec the transaction reads the CURRENT
// override-set membership via SceneManager::IsOverridden. An ordinary entity
// (NotPrefabMember) drops the delta and the edit degrades to value-only, so
// non-prefab behavior is unchanged. Any OTHER IsOverridden failure — an absent
// member (InvalidEntity) or an unknown/non-overridable/malformed stored
// override vector (InvalidArgument) — aborts the capture: the command cannot
// commit a value-only composite while the marker/schema/history stay
// untouched. Removing a wire that is currently INHERITED (not overridden) is
// marked as explicitly overridden-absent so the prefab source cannot resurrect
// the component; a locally added-then-removed member still returns to source
// with no marker.
//
// Schema capture: m_BeforeSchema is the live document schema at first replay;
// m_AfterSchema is promoted to SceneSerializer::SchemaVersion exactly when a
// marker transitions absent -> present. The pair is then fixed for the life
// of the command, so Undo of a first add restores the captured prior schema
// and Redo re-applies the promotion (D3.6/D3.10).
// ============================================================================

class SceneManager;

class PrefabCommandTransaction
{
public:
	// One marker membership delta. `key` names the overridable prefab wire;
	// `afterPresent` is the membership the After direction produces. A delta
	// whose member is an ordinary entity is dropped at capture.
	struct MarkerSpec
	{
		rt2::core::UUID member;
		PrefabComponentKey key;
		bool afterPresent = true;
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

private:
	EditorMutationResult Replay(SceneManager& scene, PrefabMarkerDirection direction);
	// Fallible: returns a Failure (and does NOT set m_Captured) when any
	// IsOverridden error other than NotPrefabMember occurs during capture.
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
};

#endif // RT2_PREFAB_COMMAND_TRANSACTION_H
