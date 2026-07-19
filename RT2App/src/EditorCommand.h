#pragma once

#ifndef RT2_EDITOR_COMMAND_H
#define RT2_EDITOR_COMMAND_H

#include "SceneMutation.h"
#include "SceneManager.h"

#include <string>

// ============================================================================
// IEditorCommand — CPU-only, UUID-keyed editor mutation abstraction.
//
// A command stores only the before/after state it touches, keyed by stable
// UUIDs (never entt::entity). Execute and Undo resolve UUIDs at run time and
// return EditorMutationResult; a missing entity is a graceful Failure, never
// a crash. Redo is the same pure "apply the after-state" call as Execute.
//
// Construction is separated from application: a command is built with the
// COMPLETE before/after state up front (never captured inside Execute), so
// Execute/Redo can never recapture a stale "before".
//
// History never calls sync callbacks or the render bridge itself. The caller
// routes the returned EditorMutationResult.syncImpact through the existing
// host sync path.
//
// ============================================================================

class IEditorCommand
{
public:
	virtual ~IEditorCommand() = default;

	// Apply the after-state. Returns the mutation result from SceneManager.
	// On a missing entity, returns Failure (success == false).
	virtual EditorMutationResult Execute(SceneManager& scene) = 0;

	// Apply the before-state (inverse). Returns the mutation result from
	// SceneManager. On a missing entity, returns Failure.
	virtual EditorMutationResult Undo(SceneManager& scene) = 0;

	// Human-readable label for menu/tooltip display.
	virtual std::string Description() const = 0;
};

#endif // RT2_EDITOR_COMMAND_H