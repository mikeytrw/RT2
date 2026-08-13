#pragma once

#ifndef RT2_EDITOR_COMMANDS_H
#define RT2_EDITOR_COMMANDS_H

#include "EditorCommand.h"
#include "PrefabCommandTransaction.h"
#include "PrefabComponentKey.h"
#include "TransformEditing.h"
#include "core/UUID.h"

#include <string>
#include <optional>
#include <utility>
#include <vector>

// ============================================================================
// Concrete Phase 3A editor commands. Both are UUID-keyed, store only the
// before/after state they touch, and resolve targets at run time. A missing
// entity is a graceful Failure.
//
// TransformCommand:
//   - Stores a vector of {UUID, beforeLocalTRS, afterLocalTRS} triples.
//     Phase 3A used a single triple; Phase 3B1 extends it to multi-entity for
//     the gizmo drag path. The single-entity factory stays for the Inspector;
//     a new multi-entity factory handles the gizmo path and drops no-op
//     entities. ALWAYS local space, even when the user edited in World mode
//     (the Inspector captures beforeLocalTRS via GetLocalTransform regardless
//     of mode). Undo/Redo restore via SetLocalTransformStates. Sync impact:
//     Transform.
//
// SetVisibilityCommand:
//   - Stores {vector<UUID,bool> beforeStates, vector<UUID,bool> afterStates}.
//     Phase 8 W3 S6-B: Execute/Undo/Redo replay a PrefabCommandTransaction
//     of Visibility value edits plus kVisible marker deltas through the S5/S6-A
//     composite, so the first write and first marker land in ONE commit.
//     Sync impact is authoritative from the composite (Structural when the
//     state changes, None otherwise).
//
// No-op suppression is enforced at construction: callers should use the
// static factory functions, which compare normalized before/after and return
// null when the command would be a no-op. A null unique_ptr from the factory
// signals "discard, do not submit".
//
// ============================================================================

struct TransformTriple
{
	rt2::core::UUID target;
	EditableTRS    beforeLocal;
	EditableTRS    afterLocal;
};

class TransformCommand final : public IEditorCommand
{
public:
	// Single-entity constructor (Phase 3A, Inspector path).
	TransformCommand(rt2::core::UUID target,
	                 EditableTRS beforeLocal,
	                 EditableTRS afterLocal,
	                 std::optional<PrefabCommandTransaction::ExplicitCapture> capture = std::nullopt);

	// Multi-entity constructor (Phase 3B1, gizmo path).
	explicit TransformCommand(std::vector<TransformTriple> triples,
		std::optional<PrefabCommandTransaction::ExplicitCapture> capture = std::nullopt);

	const std::vector<TransformTriple>& Triples() const { return m_Triples; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Transform"; }
	bool ExplicitCaptureRejected() const { return m_Transaction.ExplicitCaptureRejected(); }

private:
	std::vector<TransformTriple> m_Triples;
	PrefabCommandTransaction m_Transaction;
};

class SetVisibilityCommand final : public IEditorCommand
{
public:
	using PairList = std::vector<std::pair<rt2::core::UUID, bool>>;

	// Phase 8 W3 S6-B: the command carries a PrefabCommandTransaction of one
	// Visibility value edit and one kVisible marker delta per entity, so the
	// first value write and any first marker insertion land in ONE composite
	// commit inside Execute. Ordinary entities drop their marker deltas.
	SetVisibilityCommand(PairList beforeStates, PairList afterStates);

	const PairList& BeforeStates() const { return m_BeforeStates; }
	const PairList& AfterStates() const { return m_AfterStates; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Set Visibility"; }

private:
	PairList                m_BeforeStates;
	PairList                m_AfterStates;
	PrefabCommandTransaction m_Transaction;
};

// ---- Factories with no-op suppression ----

// Returns null if before and after normalize to the same TRS (epsilon
// component compare on translation/scale, sign-canonicalized quaternion on
// rotation).
std::unique_ptr<IEditorCommand> MakeTransformCommandIfEffective(
	rt2::core::UUID target,
	EditableTRS beforeLocal,
	EditableTRS afterLocal);

std::unique_ptr<IEditorCommand> MakeTransformCommandIfEffective(
	rt2::core::UUID target,
	EditableTRS beforeLocal,
	EditableTRS afterLocal,
	PrefabCommandTransaction::ExplicitCapture capture);

// Multi-entity factory (Phase 3B1, gizmo path). Drops no-op triples (where
// before and after normalize to the same TRS). Returns null if every triple
// is a no-op.
std::unique_ptr<IEditorCommand> MakeTransformCommandIfEffective(
	std::vector<TransformTriple> triples);

std::unique_ptr<IEditorCommand> MakeTransformCommandIfEffective(
	std::vector<TransformTriple> triples,
	PrefabCommandTransaction::ExplicitCapture capture);

// Drops pairs already in the target state; if `afterStates` ends up empty
// returns null. Validates that before/after UUID sets match.
std::unique_ptr<IEditorCommand> MakeSetVisibilityCommandIfEffective(
	SetVisibilityCommand::PairList beforeStates,
	SetVisibilityCommand::PairList afterStates);

#endif // RT2_EDITOR_COMMANDS_H
