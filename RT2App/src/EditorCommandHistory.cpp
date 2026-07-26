#include "EditorCommandHistory.h"

EditorMutationResult EditorCommandHistory::Execute(
	std::unique_ptr<IEditorCommand> cmd, SceneManager& scene)
{
	const uint64_t currentGen = scene.DocumentGeneration();
	CheckGenerationAndRebind(currentGen);

	EditorMutationResult empty;
	empty.success = false;
	empty.syncImpact = rt2::core::SyncImpact::None;
	if (!cmd) return empty;

	EditorMutationResult result = cmd->Execute(scene);
	if (!result.success)
	{
		// Failed initial Execute: both stacks unchanged.
		return result;
	}

	// A successful but ineffective mutation (the manager suppressed a
	// canonical no-op) is not recorded: the document did not change, so
	// a phantom undo entry must not be created and the redo stack must
	// survive.
	if (!result.effective)
		return result;

	// A successful, effective submission clears the redo stack.
	while (!m_RedoStack.empty()) m_RedoStack.pop();
	PushUndo(std::move(cmd));
	m_DocumentGeneration = scene.DocumentGeneration();
	return result;
}

EditorMutationResult EditorCommandHistory::RecordApplied(
	std::unique_ptr<IEditorCommand> cmd, SceneManager& scene,
	const EditorMutationResult& appliedResult)
{
	const uint64_t currentGen = scene.DocumentGeneration();
	CheckGenerationAndRebind(currentGen);

	EditorMutationResult empty;
	empty.success = false;
	empty.syncImpact = rt2::core::SyncImpact::None;
	if (!cmd) return empty;

	if (!appliedResult.success)
	{
		// Not effective — do not record, leave redo intact.
		return appliedResult;
	}

	// A successful but ineffective mutation (manager suppressed a no-op)
	// is not recorded. The redo stack survives.
	if (!appliedResult.effective)
		return appliedResult;

	while (!m_RedoStack.empty()) m_RedoStack.pop();
	PushUndo(std::move(cmd));
	m_DocumentGeneration = scene.DocumentGeneration();
	return appliedResult;
}

EditorMutationResult EditorCommandHistory::Undo(SceneManager& scene)
{
	const uint64_t currentGen = scene.DocumentGeneration();
	CheckGenerationAndRebind(currentGen);

	EditorMutationResult empty;
	empty.success = false;
	empty.syncImpact = rt2::core::SyncImpact::None;
	if (m_UndoStack.empty()) return empty;

	auto cmd = std::move(m_UndoStack.back());
	m_UndoStack.pop_back();

	EditorMutationResult result = cmd->Undo(scene);
	if (!result.success)
	{
		// Failed Undo: clear BOTH stacks. The command we popped is discarded.
		while (!m_RedoStack.empty()) m_RedoStack.pop();
		m_UndoStack.clear();
		m_DocumentGeneration = scene.DocumentGeneration();
		return result;
	}

	m_RedoStack.push(std::move(cmd));
	m_DocumentGeneration = scene.DocumentGeneration();
	return result;
}

EditorMutationResult EditorCommandHistory::Redo(SceneManager& scene)
{
	const uint64_t currentGen = scene.DocumentGeneration();
	CheckGenerationAndRebind(currentGen);

	EditorMutationResult empty;
	empty.success = false;
	empty.syncImpact = rt2::core::SyncImpact::None;
	if (m_RedoStack.empty()) return empty;

	auto cmd = std::move(m_RedoStack.top());
	m_RedoStack.pop();

	EditorMutationResult result = cmd->Execute(scene);
	if (!result.success)
	{
		// Failed Redo: clear BOTH stacks. The command we popped is discarded.
		while (!m_RedoStack.empty()) m_RedoStack.pop();
		m_UndoStack.clear();
		m_DocumentGeneration = scene.DocumentGeneration();
		return result;
	}

	m_UndoStack.push_back(std::move(cmd));
	m_DocumentGeneration = scene.DocumentGeneration();
	return result;
}

std::string EditorCommandHistory::UndoDescription() const
{
	if (m_UndoStack.empty()) return {};
	return m_UndoStack.back()->Description();
}

std::string EditorCommandHistory::RedoDescription() const
{
	if (m_RedoStack.empty()) return {};
	return m_RedoStack.top()->Description();
}

void EditorCommandHistory::Clear()
{
	m_UndoStack.clear();
	while (!m_RedoStack.empty()) m_RedoStack.pop();
	m_DocumentGeneration = 0;
}

void EditorCommandHistory::CheckGenerationAndRebind(uint64_t currentGeneration)
{
	if (m_DocumentGeneration == 0)
	{
		// First interaction with this history instance.
		m_DocumentGeneration = currentGeneration;
		return;
	}
	if (m_DocumentGeneration != currentGeneration)
	{
		// Document adoption happened out-of-band: clear and rebind.
		m_UndoStack.clear();
		while (!m_RedoStack.empty()) m_RedoStack.pop();
		m_DocumentGeneration = currentGeneration;
	}
}

void EditorCommandHistory::PushUndo(std::unique_ptr<IEditorCommand> cmd)
{
	m_UndoStack.push_back(std::move(cmd));
	EnforceCapacity();
}

void EditorCommandHistory::EnforceCapacity()
{
	while (m_UndoStack.size() > m_Capacity)
		m_UndoStack.pop_front();
}