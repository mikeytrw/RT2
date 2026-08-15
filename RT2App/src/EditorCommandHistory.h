#pragma once

#ifndef RT2_EDITOR_COMMAND_HISTORY_H
#define RT2_EDITOR_COMMAND_HISTORY_H

#include "EditorCommand.h"
#include "SceneManager.h"

#include <cstddef>
#include <deque>
#include <memory>
#include <stack>
#include <string>

// ============================================================================
// EditorCommandHistory — bounded undo/redo history of IEditorCommand entries.
//
// Two entry points, both clearing redo only on a successful, effective
// submission:
//   - Execute(cmd): applies cmd, then records it. A failed Execute leaves
//     both stacks unchanged. A successful but ineffective Execute (the
//     manager suppressed a canonical no-op, reported via
//     EditorMutationResult::effective == false) also leaves both stacks
//     unchanged and does not clear redo.
//   - RecordApplied(cmd): records a command whose effect was already applied
//     incrementally (continuous edits / gizmo drags). Does not re-apply.
//     A successful but ineffective appliedResult is not recorded.
//
// Document-generation guard: Execute, RecordApplied, Undo, and Redo all
// compare the stored DocumentGeneration against the SceneManager's current
// one. On mismatch both stacks are cleared and history rebinds to the new
// generation (Execute/RecordApplied then proceed as the first entry).
//
// Failure policy: a failed Undo or Redo surfaces the error via the returned
// result and clears BOTH stacks — a failed inverse means history's causal
// assumptions were violated by an out-of-band change.
//
// History never touches sync callbacks or the render bridge. Callers route
// the returned EditorMutationResult.syncImpact through the host sync path.
//
// Bounded: the undo deque evicts the oldest entry when over the capacity
// (default 64, configurable via SetCapacity). The redo stack is never
// bounded beyond what fits in memory because it is always cleared by a new
// effective submission.
//
// ============================================================================

class EditorCommandHistory
{
public:
	EditorCommandHistory() = default;
	explicit EditorCommandHistory(std::size_t capacity) : m_Capacity(capacity) {}

	void SetCapacity(std::size_t capacity) { m_Capacity = capacity; }
	std::size_t GetCapacity() const { return m_Capacity; }

	// Apply cmd to scene and record it. A failed Execute leaves both stacks
	// unchanged. A successful but ineffective Execute (manager no-op) is not
	// recorded and does not clear redo. A successful, effective submission
	// clears the redo stack.
	EditorMutationResult Execute(std::unique_ptr<IEditorCommand> cmd,
	                             SceneManager& scene);

	// Record a command whose effect was already applied. Does not re-apply.
	// Records only if the command's intended mutation is "effective" (caller
	// passes the already-applied result; an unsuccessful or ineffective result
	// is not recorded). A successful, effective submission clears the redo
	// stack.
	EditorMutationResult RecordApplied(std::unique_ptr<IEditorCommand> cmd,
	                                   SceneManager& scene,
	                                   const EditorMutationResult& appliedResult);
	// CPU-testable bounded fault seam for close/recovery discrimination. It
	// rejects the next RecordApplied without touching either history stack.
	void FailNextRecordAppliedForTest() { m_FailNextRecordApplied = true; }

	// Apply the inverse of the topmost undo entry. On failure clears BOTH
	// stacks and surfaces the error. Returns the EditorMutationResult of the
	// inverse; .success == false with .syncImpact == None if there is nothing
	// to undo.
	EditorMutationResult Undo(SceneManager& scene);

	// Re-apply the topmost redo entry. On failure clears BOTH stacks and
	// surfaces the error. Returns the EditorMutationResult of the apply;
	// .success == false with .syncImpact == None if there is nothing to redo.
	EditorMutationResult Redo(SceneManager& scene);

	bool CanUndo() const { return !m_UndoStack.empty(); }
	bool CanRedo() const { return !m_RedoStack.empty(); }
	std::size_t UndoDepthForTest() const { return m_UndoStack.size(); }
	std::size_t RedoDepthForTest() const { return m_RedoStack.size(); }

	std::string UndoDescription() const;
	std::string RedoDescription() const;

	void Clear();

private:
	void CheckGenerationAndRebind(uint64_t currentGeneration);
	void PushUndo(std::unique_ptr<IEditorCommand> cmd);
	void EnforceCapacity();

	std::deque<std::unique_ptr<IEditorCommand>> m_UndoStack;
	std::stack<std::unique_ptr<IEditorCommand>>  m_RedoStack;
	std::size_t m_Capacity = 64;
	uint64_t    m_DocumentGeneration = 0;
	bool        m_FailNextRecordApplied = false;
};

// S6-B fixup (nullable-history safety): route ONE command through an OPTIONAL
// command history. A host that has not installed a history (SceneEditorUI's
// documented default construction) must never have the command dereference a
// missing history or mutate outside it — this returns a structured Failure
// (InvalidRuntimeState) before the scene is touched. With a history installed
// it forwards to EditorCommandHistory::Execute. Converted UI actions route
// through here; callers surface the returned result like any other mutation
// failure.
inline EditorMutationResult ExecuteCommandThroughHistory(
	EditorCommandHistory* history,
	SceneManager& scene,
	std::unique_ptr<IEditorCommand> cmd,
	const std::string& path = {})
{
	if (!history)
		return EditorMutationResult::Failure(
			rt2::core::Error::InvalidRuntimeState, path,
			"command history is not installed; the edit was not applied");
	return history->Execute(std::move(cmd), scene);
}

#endif // RT2_EDITOR_COMMAND_HISTORY_H
