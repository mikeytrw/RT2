#include "EditorStructuralCommands.h"

#include "PrefabSerializer.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
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
	// Execute is both the first apply (the host already wrote the AFTER file
	// via CreatePrefabFromSubtree) and Redo (which returns the file to the
	// AFTER state from a prior Undo's BEFORE/absent state).
	//
	// if m_FileIsAfterState is true the file is already the AFTER bytes this
	// command owns — a first apply or a redundant Execute. Verify the file is
	// still those bytes (an out-of-band edit is a conflict), then re-write
	// them deterministically.
	//
	// if m_FileIsAfterState is false the previous operation was an Undo (file
	// is in the BEFORE/absent state). This is a Redo: verify the file is still
	// the BEFORE state, then write the AFTER bytes — an external edit between
	// Undo and Redo must be a loud conflict, never a silent clobber.
	if (m_FileIsAfterState)
	{
		if (!FileMatches(true, m_AfterContents))
			return EditorMutationResult::Failure(rt2::core::Error::Io,
				m_PrefabPath.string(),
				"CreatePrefabCommand::Execute: the prefab file changed out-of-band; "
				"refusing to overwrite external edits");
	}
	else
	{
		if (m_FileExistedBefore)
		{
			if (!FileMatches(true, m_BeforeContents))
				return EditorMutationResult::Failure(rt2::core::Error::Io,
					m_PrefabPath.string(),
					"CreatePrefabCommand::Execute: the prefab file changed out-of-band since Undo; "
					"refusing to overwrite external edits");
		}
		else if (!FileMatches(false, {}))
		{
			return EditorMutationResult::Failure(rt2::core::Error::Io,
				m_PrefabPath.string(),
				"CreatePrefabCommand::Execute: the prefab file reappeared out-of-band; "
				"refusing to overwrite external edits");
		}
	}

	EditorMutationResult written = WriteAfter();
	if (written.success)
		m_FileIsAfterState = true;
	return written;
}

EditorMutationResult CreatePrefabCommand::Undo(SceneManager& scene)
{
	// Undo moves the file from its AFTER state back to its BEFORE/absent
	// state. Verify the file is still the "after" bytes this command wrote —
	// an external edit after create must surface as a loud conflict, never a
	// silent truncation of external work.
	if (!FileMatches(true, m_AfterContents))
		return EditorMutationResult::Failure(rt2::core::Error::Io,
			m_PrefabPath.string(),
			"CreatePrefabCommand::Undo: the prefab file changed out-of-band since create; "
			"refusing to overwrite external edits");

	EditorMutationResult restored = RestoreBefore();
	if (restored.success)
		m_FileIsAfterState = false;
	return restored;
}

bool CreatePrefabCommand::FileMatches(bool expectedExists,
                                      const std::vector<uint8_t>& expectedBytes) const
{
	// Checked existence is authoritative and is kept DISJOINT from the bytes:
	// a missing file is a different state from an existing zero-byte file, and
	// an existing zero-byte file is a different state from an expected absence.
	std::error_code ec;
	const bool exists = std::filesystem::exists(m_PrefabPath, ec);
	if (ec)
		return false; // cannot even stat the file — loud mismatch
	if (!exists)
		return !expectedExists; // absent matches only an expected-absent state
	if (!expectedExists)
		return false; // present, but we expect absence
	std::ifstream in(m_PrefabPath, std::ios::binary);
	if (!in)
		return false; // exists but unreadable — loud mismatch
	std::stringstream ss;
	ss << in.rdbuf();
	const std::string raw = ss.str();
	return raw.size() == expectedBytes.size() &&
	       std::equal(raw.begin(), raw.end(), expectedBytes.begin(), expectedBytes.end());
}

EditorMutationResult CreatePrefabCommand::WriteAfter()
{
	rt2::core::Error err;
	const std::string after(m_AfterContents.begin(), m_AfterContents.end());
	if (!rt2::core::PrefabSerializer::WriteBytesAtomic(m_PrefabPath, after, err))
		return EditorMutationResult::Failure(err.code, err.path,
			"CreatePrefabCommand::Execute: " + err.detail);
	EditorMutationResult result;
	result.syncImpact = rt2::core::SyncImpact::None;
	return result;
}

EditorMutationResult CreatePrefabCommand::RestoreBefore()
{
	// If the file did not exist before the create, Undo removes the file;
	// otherwise (including a pre-existing zero-byte file, which an empty
	// contents vector alone cannot distinguish) it restores the prior bytes
	// verbatim. Writes and removals are checked (never a silent no-op) and
	// both go through the same atomic path as create, so a failure leaves the
	// recoverable file intact rather than truncating it first.
	if (!m_FileExistedBefore)
	{
		std::error_code ec;
		std::filesystem::remove(m_PrefabPath, ec);
		if (ec)
			return EditorMutationResult::Failure(rt2::core::Error::Io,
				m_PrefabPath.string(),
				"CreatePrefabCommand::Undo: failed to remove the created prefab file: " + ec.message());
		EditorMutationResult result;
		result.syncImpact = rt2::core::SyncImpact::None;
		return result;
	}
	rt2::core::Error err;
	const std::string prior(m_BeforeContents.begin(), m_BeforeContents.end());
	if (!rt2::core::PrefabSerializer::WriteBytesAtomic(m_PrefabPath, prior, err))
		return EditorMutationResult::Failure(err.code, err.path,
			"CreatePrefabCommand::Undo: failed to restore prior prefab contents: " + err.detail);
	EditorMutationResult result;
	result.syncImpact = rt2::core::SyncImpact::None;
	return result;
}

void CreatePrefabCommand::ComputeAfterContents()
{
	rt2::core::PrefabDocument doc;
	doc.entities.reserve(m_Result.sourceSnapshot.entities.size());
	for (std::size_t i = 0; i < m_Result.sourceSnapshot.entities.size(); ++i)
	{
		rt2::core::PrefabEntityRecord record;
		record.templateId = m_Result.templateIds[i];
		record.record     = m_Result.sourceSnapshot.entities[i];
		doc.entities.push_back(std::move(record));
	}
	std::string afterBytes;
	rt2::core::Error sErr;
	if (rt2::core::PrefabSerializer::Serialize(doc, afterBytes, sErr))
		m_AfterContents.assign(afterBytes.begin(), afterBytes.end());
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
	std::vector<uint8_t> beforeFileContents,
	bool fileExistedBefore)
{
	if (prefabPath.empty() || !result.ok) return nullptr;
	if (result.sourceSnapshot.entities.size() != result.templateIds.size())
		return nullptr;
	if (result.sourceSnapshot.entities.empty()) return nullptr;
	return std::make_unique<CreatePrefabCommand>(std::move(prefabPath),
	                                             std::move(result),
	                                             std::move(beforeFileContents),
	                                             fileExistedBefore);
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