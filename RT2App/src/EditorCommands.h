#pragma once

#ifndef RT2_EDITOR_COMMANDS_H
#define RT2_EDITOR_COMMANDS_H

#include "EditorCommand.h"
#include "TransformEditing.h"
#include "core/UUID.h"

#include <string>
#include <utility>
#include <vector>

// ============================================================================
// Concrete Phase 3A editor commands. Both are UUID-keyed, store only the
// before/after state they touch, and resolve targets at run time. A missing
// entity is a graceful Failure.
//
// TransformCommand:
//   - Stores {UUID, beforeLocalTRS, afterLocalTRS}. ALWAYS local space, even
//     when the user edited in World mode (the Inspector captures
//     beforeLocalTRS via GetLocalTransform regardless of mode). Undo/Redo
//     restore via SetLocalTransform. Sync impact: Transform.
//
// SetVisibilityCommand:
//   - Stores {vector<UUID,bool> beforeStates, vector<UUID,bool> afterStates}.
//     Built on SceneManager::SetVisibilityStates. Sync impact: Structural if
//     any change, None otherwise (the manager decides).
//
// No-op suppression is enforced at construction: callers should use the
// static factory functions, which compare normalized before/after and return
// null when the command would be a no-op. A null unique_ptr from the factory
// signals "discard, do not submit".
//
// ============================================================================

class TransformCommand final : public IEditorCommand
{
public:
	TransformCommand(rt2::core::UUID target,
	                 EditableTRS beforeLocal,
	                 EditableTRS afterLocal)
		: m_Target(target)
		, m_BeforeLocal(beforeLocal)
		, m_AfterLocal(afterLocal) {}

	const rt2::core::UUID& Target() const { return m_Target; }
	const EditableTRS& BeforeLocal() const { return m_BeforeLocal; }
	const EditableTRS& AfterLocal() const { return m_AfterLocal; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Transform"; }

private:
	rt2::core::UUID m_Target;
	EditableTRS     m_BeforeLocal;
	EditableTRS     m_AfterLocal;
};

class SetVisibilityCommand final : public IEditorCommand
{
public:
	using PairList = std::vector<std::pair<rt2::core::UUID, bool>>;

	SetVisibilityCommand(PairList beforeStates, PairList afterStates)
		: m_BeforeStates(std::move(beforeStates))
		, m_AfterStates(std::move(afterStates)) {}

	const PairList& BeforeStates() const { return m_BeforeStates; }
	const PairList& AfterStates() const { return m_AfterStates; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Set Visibility"; }

private:
	PairList m_BeforeStates;
	PairList m_AfterStates;
};

// ---- Factories with no-op suppression ----

// Returns null if before and after normalize to the same TRS (epsilon
// component compare on translation/scale, sign-canonicalized quaternion on
// rotation).
std::unique_ptr<IEditorCommand> MakeTransformCommandIfEffective(
	rt2::core::UUID target,
	EditableTRS beforeLocal,
	EditableTRS afterLocal);

// Drops pairs already in the target state; if `afterStates` ends up empty
// returns null. Validates that before/after UUID sets match.
std::unique_ptr<IEditorCommand> MakeSetVisibilityCommandIfEffective(
	SetVisibilityCommand::PairList beforeStates,
	SetVisibilityCommand::PairList afterStates);

#endif // RT2_EDITOR_COMMANDS_H