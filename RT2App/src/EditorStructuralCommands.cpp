#include "EditorStructuralCommands.h"

#include "PrefabSerializer.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>

namespace
{

bool ReparentEditEqual(const ReparentEdit& a, const ReparentEdit& b)
{
	return a.entity == b.entity && a.newParent == b.newParent;
}

} // namespace

// ============================================================================
// Creation commands — Execute/Redo restore the snapshot, Undo removes exact.
// ============================================================================

EditorMutationResult CreateEmptyCommand::Execute(SceneManager& scene)
{
	return scene.RestoreSubtrees(m_Snapshot);
}

EditorMutationResult CreateEmptyCommand::Undo(SceneManager& scene)
{
	return scene.RemoveSubtreesExact(m_Snapshot);
}

EditorMutationResult CreatePrimitiveCommand::Execute(SceneManager& scene)
{
	return scene.RestoreSubtrees(m_Snapshot);
}

EditorMutationResult CreatePrimitiveCommand::Undo(SceneManager& scene)
{
	return scene.RemoveSubtreesExact(m_Snapshot);
}

EditorMutationResult CreateLightCommand::Execute(SceneManager& scene)
{
	return scene.RestoreSubtrees(m_Snapshot);
}

EditorMutationResult CreateLightCommand::Undo(SceneManager& scene)
{
	return scene.RemoveSubtreesExact(m_Snapshot);
}

// ============================================================================
// Deletion command — Execute/Redo remove without compaction, Undo restores.
// ============================================================================

EditorMutationResult RemoveSubtreesCommand::Execute(SceneManager& scene)
{
	return scene.RemoveSubtreesNoCompact(m_RootUuids);
}

EditorMutationResult RemoveSubtreesCommand::Undo(SceneManager& scene)
{
	return scene.RestoreSubtrees(m_Snapshot);
}

// ============================================================================
// Duplication/Paste commands — Execute/Redo restore the duplicate snapshot,
// Undo removes exact.
// ============================================================================

EditorMutationResult DuplicateSubtreesCommand::Execute(SceneManager& scene)
{
	return scene.RestoreSubtrees(m_Snapshot);
}

EditorMutationResult DuplicateSubtreesCommand::Undo(SceneManager& scene)
{
	return scene.RemoveSubtreesExact(m_Snapshot);
}

EditorMutationResult PasteSubtreesCommand::Execute(SceneManager& scene)
{
	return scene.RestoreSubtrees(m_Snapshot);
}

EditorMutationResult PasteSubtreesCommand::Undo(SceneManager& scene)
{
	return scene.RemoveSubtreesExact(m_Snapshot);
}

// ============================================================================
// Phase 8 W1 prefab commands.
//
// InstantiatePrefabCommand is the scene-side command: Execute/Redo restore
// the captured instance snapshot (same stored UUIDs, PrefabInstance/
// PrefabMember link verbatim), Undo removes it exactly. CreatePrefabCommand
// is the asset-side command: the scene is never mutated, so Execute/Redo
// deterministically regenerate the .rt2prefab file from the authoritative
// post-mutation result, and Undo restores the pre-mutation file contents.
// ============================================================================

EditorMutationResult InstantiatePrefabCommand::Execute(SceneManager& scene)
{
	return scene.RestoreSubtrees(m_Snapshot);
}

EditorMutationResult InstantiatePrefabCommand::Undo(SceneManager& scene)
{
	return scene.RemoveSubtreesExact(m_Snapshot);
}

EditorMutationResult CreatePrefabCommand::Execute(SceneManager& scene)
{
	// Regenerate the file deterministically from the authoritative
	// post-mutation result. PrefabSerializer::Save is atomic (tmp+replace)
	// and dump(2) is deterministic, so Redo reproduces the exact bytes the
	// initial create wrote.
	rt2::core::PrefabDocument doc;
	doc.entities.reserve(m_Result.sourceSnapshot.entities.size());
	for (std::size_t i = 0; i < m_Result.sourceSnapshot.entities.size(); ++i)
	{
		rt2::core::PrefabEntityRecord record;
		record.templateId = m_Result.templateIds[i];
		record.record     = m_Result.sourceSnapshot.entities[i];
		doc.entities.push_back(std::move(record));
	}
	rt2::core::Error err;
	if (!rt2::core::PrefabSerializer::Save(doc, m_PrefabPath, err))
		return EditorMutationResult::Failure(err.code, err.path,
			"CreatePrefabCommand::Execute: " + err.detail);
	EditorMutationResult result;
	result.syncImpact = rt2::core::SyncImpact::None;
	return result;
}

EditorMutationResult CreatePrefabCommand::Undo(SceneManager& scene)
{
	// Empty before-contents means the file did not exist before the create:
	// Undo removes it. Otherwise restore the prior bytes. Writes are checked
	// (never a silent no-op); a restore failure surfaces as a loud Failure.
	if (m_BeforeContents.empty())
	{
		std::error_code ec;
		std::filesystem::remove(m_PrefabPath, ec);
		EditorMutationResult result;
		result.syncImpact = rt2::core::SyncImpact::None;
		return result;
	}
	{
		std::ofstream out(m_PrefabPath, std::ios::binary | std::ios::trunc);
		if (!out)
			return EditorMutationResult::Failure(rt2::core::Error::Io,
				m_PrefabPath.string(),
				"CreatePrefabCommand::Undo: failed to open file to restore prior contents");
		out.write(reinterpret_cast<const char*>(m_BeforeContents.data()),
		          static_cast<std::streamsize>(m_BeforeContents.size()));
		out.flush();
		if (!out)
			return EditorMutationResult::Failure(rt2::core::Error::Io,
				m_PrefabPath.string(),
				"CreatePrefabCommand::Undo: failed to restore prior file contents");
	}
	EditorMutationResult result;
	result.syncImpact = rt2::core::SyncImpact::None;
	return result;
}

// ============================================================================
// Reparent command — Execute/Redo apply after-edits, Undo applies before-edits
// with PreserveLocal (the command stored the exact before-local TRS).
// ============================================================================

EditorMutationResult ReparentCommand::Execute(SceneManager& scene)
{
	return scene.ReparentBatch(m_AfterEdits, m_Mode);
}

EditorMutationResult ReparentCommand::Undo(SceneManager& scene)
{
	// Undo always uses PreserveLocal — the command stored the exact
	// before-local TRS for each source, so we restore verbatim regardless
	// of the original mode.
	return scene.ReparentBatch(m_BeforeEdits, ReparentMode::PreserveLocal);
}

// ============================================================================
// Factories with no-op suppression
// ============================================================================

std::unique_ptr<IEditorCommand> MakeCreateEmptyCommand(
	SubtreeSnapshot snapshot, rt2::core::UUID createdRoot)
{
	if (snapshot.entities.empty() || createdRoot.IsNull()) return nullptr;
	return std::make_unique<CreateEmptyCommand>(std::move(snapshot), createdRoot);
}

std::unique_ptr<IEditorCommand> MakeCreatePrimitiveCommand(
	SubtreeSnapshot snapshot, rt2::core::UUID createdRoot)
{
	if (snapshot.entities.empty() || createdRoot.IsNull()) return nullptr;
	return std::make_unique<CreatePrimitiveCommand>(std::move(snapshot), createdRoot);
}

std::unique_ptr<IEditorCommand> MakeCreateLightCommand(
	SubtreeSnapshot snapshot, rt2::core::UUID createdRoot)
{
	if (snapshot.entities.empty() || createdRoot.IsNull()) return nullptr;
	return std::make_unique<CreateLightCommand>(std::move(snapshot), createdRoot);
}

std::unique_ptr<IEditorCommand> MakeRemoveSubtreesCommand(
	SubtreeSnapshot snapshot,
	std::vector<rt2::core::UUID> rootUuids)
{
	if (rootUuids.empty()) return nullptr;
	return std::make_unique<RemoveSubtreesCommand>(std::move(snapshot),
	                                              std::move(rootUuids));
}

std::unique_ptr<IEditorCommand> MakeDuplicateSubtreesCommand(
	SubtreeSnapshot snapshot,
	std::vector<rt2::core::UUID> createdRoots)
{
	if (createdRoots.empty()) return nullptr;
	return std::make_unique<DuplicateSubtreesCommand>(std::move(snapshot),
	                                                 std::move(createdRoots));
}

std::unique_ptr<IEditorCommand> MakePasteSubtreesCommand(
	SubtreeSnapshot snapshot,
	std::vector<rt2::core::UUID> createdRoots)
{
	if (createdRoots.empty()) return nullptr;
	return std::make_unique<PasteSubtreesCommand>(std::move(snapshot),
	                                              std::move(createdRoots));
}

std::unique_ptr<IEditorCommand> MakeCreatePrefabCommand(
	std::filesystem::path prefabPath,
	SceneManager::PrefabCreationResult result,
	std::vector<uint8_t> beforeFileContents)
{
	if (prefabPath.empty() || !result.ok) return nullptr;
	if (result.sourceSnapshot.entities.size() != result.templateIds.size())
		return nullptr;
	if (result.sourceSnapshot.entities.empty()) return nullptr;
	return std::make_unique<CreatePrefabCommand>(std::move(prefabPath),
	                                             std::move(result),
	                                             std::move(beforeFileContents));
}

std::unique_ptr<IEditorCommand> MakeInstantiatePrefabCommand(
	SubtreeSnapshot snapshot,
	std::vector<rt2::core::UUID> createdRoots)
{
	if (createdRoots.empty()) return nullptr;
	return std::make_unique<InstantiatePrefabCommand>(std::move(snapshot),
	                                                  std::move(createdRoots));
}

std::unique_ptr<IEditorCommand> MakeReparentCommandIfEffective(
	std::vector<ReparentEdit> beforeEdits,
	std::vector<ReparentEdit> afterEdits,
	ReparentMode mode)
{
	if (afterEdits.empty()) return nullptr;

	// No-op suppression: if every after edit matches its corresponding
	// before edit (same entity, same new parent), the command is a no-op.
	// We compare by entity UUID + new parent UUID only — the local TRS and
	// anchor are the applied state, not the no-op signal.
	bool anyChange = false;
	for (std::size_t i = 0; i < afterEdits.size(); ++i)
	{
		const auto& after = afterEdits[i];
		// Find the matching before edit for this entity.
		bool found = false;
		for (const auto& before : beforeEdits)
		{
			if (before.entity == after.entity)
			{
				found = true;
				if (!ReparentEditEqual(before, after))
					anyChange = true;
				break;
			}
		}
		if (!found) anyChange = true;
	}
	if (!anyChange) return nullptr;

	return std::make_unique<ReparentCommand>(std::move(beforeEdits),
	                                         std::move(afterEdits), mode);
}