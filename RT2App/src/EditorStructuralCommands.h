#pragma once

#ifndef RT2_EDITOR_STRUCTURAL_COMMANDS_H
#define RT2_EDITOR_STRUCTURAL_COMMANDS_H

#include "EditorCommand.h"
#include "SceneManager.h"
#include "SubtreeSnapshot.h"
#include "TransformEditing.h"
#include "core/UUID.h"
#include "ECSComponents.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// ============================================================================
// Phase 3B1 structural editor commands. Each is UUID-keyed, stores only the
// before/after state it touches, and resolves targets at run time. A missing
// entity is a graceful Failure. Impact assignments are authoritative from
// the manager — commands never synthesize sync impact.
//
// Creation commands (CreateEmpty, CreatePrimitive, CreateLight) use the
// RecordApplied seam: the host reserves known UUIDs, the manager creates
// with those UUIDs, the host captures the resulting SubtreeSnapshot, the
// command stores it. Redo calls RestoreSubtrees(snapshot) (re-creates with
// stored UUIDs); Undo calls RemoveSubtreesExact(snapshot).
//
// Deletion commands (RemoveSubtrees) use Execute: the snapshot is captured
// at construction time, Execute/Redo call RemoveSubtreesNoCompact(roots),
// Undo calls RestoreSubtrees(snapshot).
//
// Duplication/Paste commands capture the resulting duplicate SubtreeSnapshot
// so Redo restores the same entities with the same UUIDs rather than
// repeating the source walk or generating new IDs.
//
// ReparentCommand stores before/after ReparentEdit lists; Undo always uses
// PreserveLocal with the stored before-local TRS.
//
// No compaction runs while any of these commands is in history (the 3B1
// invariant), so stored MeshRef::meshIndex values stay valid across
// Undo/Redo.
//
// No-op suppression is enforced at construction: callers should use the
// static factory functions, which return null when the command would be a
// no-op. A null unique_ptr from the factory signals "discard, do not
// submit".
// ============================================================================

// ---- Creation commands ----

class CreateEmptyCommand final : public IEditorCommand
{
public:
	CreateEmptyCommand(SubtreeSnapshot snapshot, rt2::core::UUID createdRoot)
		: m_Snapshot(std::move(snapshot)), m_CreatedRoot(createdRoot) {}

	const SubtreeSnapshot& Snapshot() const { return m_Snapshot; }
	const rt2::core::UUID& CreatedRoot() const { return m_CreatedRoot; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Create Empty"; }

private:
	SubtreeSnapshot   m_Snapshot;
	rt2::core::UUID   m_CreatedRoot;
};

class CreatePrimitiveCommand final : public IEditorCommand
{
public:
	CreatePrimitiveCommand(SubtreeSnapshot snapshot, rt2::core::UUID createdRoot)
		: m_Snapshot(std::move(snapshot)), m_CreatedRoot(createdRoot) {}

	const SubtreeSnapshot& Snapshot() const { return m_Snapshot; }
	const rt2::core::UUID& CreatedRoot() const { return m_CreatedRoot; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Create Primitive"; }

private:
	SubtreeSnapshot   m_Snapshot;
	rt2::core::UUID   m_CreatedRoot;
};

class CreateLightCommand final : public IEditorCommand
{
public:
	CreateLightCommand(SubtreeSnapshot snapshot, rt2::core::UUID createdRoot)
		: m_Snapshot(std::move(snapshot)), m_CreatedRoot(createdRoot) {}

	const SubtreeSnapshot& Snapshot() const { return m_Snapshot; }
	const rt2::core::UUID& CreatedRoot() const { return m_CreatedRoot; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Create Light"; }

private:
	SubtreeSnapshot   m_Snapshot;
	rt2::core::UUID   m_CreatedRoot;
};

// ---- Deletion command ----

class RemoveSubtreesCommand final : public IEditorCommand
{
public:
	// Snapshot is captured at construction time (entity exists). Execute/
	// Redo call RemoveSubtreesNoCompact(roots); Undo calls
	// RestoreSubtrees(snapshot).
	RemoveSubtreesCommand(SubtreeSnapshot snapshot,
	                      std::vector<rt2::core::UUID> rootUuids)
		: m_Snapshot(std::move(snapshot))
		, m_RootUuids(std::move(rootUuids)) {}

	const SubtreeSnapshot& Snapshot() const { return m_Snapshot; }
	const std::vector<rt2::core::UUID>& RootUuids() const { return m_RootUuids; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Delete"; }

private:
	SubtreeSnapshot                  m_Snapshot;
	std::vector<rt2::core::UUID>    m_RootUuids;
};

// ---- Duplication / Paste commands ----

class DuplicateSubtreesCommand final : public IEditorCommand
{
public:
	// snapshot is the duplicate's captured state (NOT the source's). Redo
	// restores the same entities with the same UUIDs.
	DuplicateSubtreesCommand(SubtreeSnapshot snapshot,
	                         std::vector<rt2::core::UUID> createdRoots)
		: m_Snapshot(std::move(snapshot))
		, m_CreatedRoots(std::move(createdRoots)) {}

	const SubtreeSnapshot& Snapshot() const { return m_Snapshot; }
	const std::vector<rt2::core::UUID>& CreatedRoots() const { return m_CreatedRoots; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Duplicate"; }

private:
	SubtreeSnapshot                  m_Snapshot;
	std::vector<rt2::core::UUID>    m_CreatedRoots;
};

class PasteSubtreesCommand final : public IEditorCommand
{
public:
	// snapshot is the pasted subtree's captured state. Redo restores the
	// same entities with the same UUIDs. The source mapping is not retained
	// after the initial operation.
	PasteSubtreesCommand(SubtreeSnapshot snapshot,
	                     std::vector<rt2::core::UUID> createdRoots)
		: m_Snapshot(std::move(snapshot))
		, m_CreatedRoots(std::move(createdRoots)) {}

	const SubtreeSnapshot& Snapshot() const { return m_Snapshot; }
	const std::vector<rt2::core::UUID>& CreatedRoots() const { return m_CreatedRoots; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Paste"; }

private:
	SubtreeSnapshot                  m_Snapshot;
	std::vector<rt2::core::UUID>    m_CreatedRoots;
};

// ---- Phase 8 W1 prefab commands ----

// Scene-side instantiation command. Mirrors DuplicateSubtreesCommand: the
// host has ALREADY applied InstantiatePrefabWithUuids and captured the
// resulting SubtreeSnapshot of the created instance. Execute/Redo call
// RestoreSubtrees(snapshot) (re-creates with stored UUIDs, restoring the
// PrefabInstance/PrefabMember link verbatim); Undo calls
// RemoveSubtreesExact(snapshot).
class InstantiatePrefabCommand final : public IEditorCommand
{
public:
	InstantiatePrefabCommand(SubtreeSnapshot snapshot,
	                         std::vector<rt2::core::UUID> createdRoots)
		: m_Snapshot(std::move(snapshot))
		, m_CreatedRoots(std::move(createdRoots)) {}

	const SubtreeSnapshot& Snapshot() const { return m_Snapshot; }
	const std::vector<rt2::core::UUID>& CreatedRoots() const { return m_CreatedRoots; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Instantiate Prefab"; }

private:
	SubtreeSnapshot                  m_Snapshot;
	std::vector<rt2::core::UUID>    m_CreatedRoots;
};

// Asset-side creation command. The scene is NOT mutated by
// CreatePrefabFromSubtree, so this command stores the FILE-REWRITE state
// rather than a scene snapshot:
//   - `result` is the authoritative post-mutation state from
//     CreatePrefabFromSubtree; Execute/Redo deterministically regenerate the
//     .rt2prefab file from sourceSnapshot + templateIds via
//     PrefabSerializer::Save.
//   - `beforeFileContents` is the file's byte content captured BEFORE the
//     create (may be empty for a pre-existing zero-byte file).
//   - `fileExistedBefore` disambiguates an absent file (remove on Undo) from
//     a pre-existing zero-byte file (restore verbatim on Undo). An empty
//     contents vector alone is ambiguous between the two.
// Undo restores those bytes, or removes the file when it did not exist,
// through the SAME atomic tmp+replace path as create — a failed restore never
// truncates the recoverable file first. Undo/Redo also verify the file's
// current bytes match the state this command last left it in before touching
// the file (an "out-of-band external edit" is a loud conflict Failure, never
// a silent clobber).
// The sidecar (asset identity) minted by CreatePrefabFromSubtree is left in
// place by both Undo and Redo: identity is assign-once per asset path, so an
// orphaned sidecar for a deleted asset is the established asset-system
// behaviour and is not a per-instance mutation to unwind.
class CreatePrefabCommand final : public IEditorCommand
{
public:
	CreatePrefabCommand(std::filesystem::path prefabPath,
	                    SceneManager::PrefabCreationResult result,
	                    std::vector<uint8_t> beforeFileContents,
	                    bool fileExistedBefore)
		: m_PrefabPath(std::move(prefabPath))
		, m_Result(std::move(result))
		, m_BeforeContents(std::move(beforeFileContents))
		, m_FileExistedBefore(fileExistedBefore)
	{
		ComputeAfterContents();
	}

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Create Prefab"; }

private:
	std::filesystem::path              m_PrefabPath;
	SceneManager::PrefabCreationResult m_Result;
	std::vector<uint8_t>               m_BeforeContents;
	bool                               m_FileExistedBefore = false;
	std::vector<uint8_t>               m_AfterContents;

	// The byte state this command last wrote the file to ("after" after a
	// successful Execute/Redo, "before"/absent after a successful Undo). Used
	// to detect an out-of-band external edit before Undo/Redo touches the file.
	bool m_FileIsAfterState = true;

	// Pre-serialize the deterministic AFTER bytes from m_Result.
	void ComputeAfterContents();

	// True when the file on disk is in the expected state. `expectedExists` is
	// the authoritative existence expectation and is disjoint from the bytes:
	//   - expectedExists=true  the file MUST exist (as a regular file) and its
	//                          bytes must equal `expectedBytes` verbatim. A
	//                          missing file never matches, even when
	//                          expectedBytes is empty.
	//   - expectedExists=false the file MUST be absent. An existing file never
	//                          matches, even an empty (zero-byte) one.
	// This delineates "missing" from "existing zero-byte" and "existing
	// zero-byte" from "expected absence", which an empty bytes vector alone
	// conflates. A stat/open/read failure is a loud MISMATCH (false), never a
	// silent "empty expected bytes", so an out-of-band deletion or an
	// unreadable file surfaces as a conflict rather than a clobber.
};

// ---- Reparent command ----

class ReparentCommand final : public IEditorCommand
{
public:
	ReparentCommand(std::vector<ReparentEdit> beforeEdits,
	                std::vector<ReparentEdit> afterEdits,
	                ReparentMode mode)
		: m_BeforeEdits(std::move(beforeEdits))
		, m_AfterEdits(std::move(afterEdits))
		, m_Mode(mode) {}

	const std::vector<ReparentEdit>& BeforeEdits() const { return m_BeforeEdits; }
	const std::vector<ReparentEdit>& AfterEdits() const { return m_AfterEdits; }
	ReparentMode Mode() const { return m_Mode; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Reparent"; }

private:
	std::vector<ReparentEdit> m_BeforeEdits;
	std::vector<ReparentEdit> m_AfterEdits;
	ReparentMode              m_Mode;
};

// ---- Factories with no-op suppression ----

// All factories return null when the command would be a no-op.

// Creation command factories. The host has ALREADY applied the creation
// (via the manager's CreateEmptyWithUuid / CreatePrimitiveEntity /
// CreateLightEntity) and captured the resulting SubtreeSnapshot. The
// factory wraps the snapshot + created root UUID into a command for
// RecordApplied. Returns null if the snapshot is empty (creation failed).
std::unique_ptr<IEditorCommand> MakeCreateEmptyCommand(
	SubtreeSnapshot snapshot, rt2::core::UUID createdRoot);
std::unique_ptr<IEditorCommand> MakeCreatePrimitiveCommand(
	SubtreeSnapshot snapshot, rt2::core::UUID createdRoot);
std::unique_ptr<IEditorCommand> MakeCreateLightCommand(
	SubtreeSnapshot snapshot, rt2::core::UUID createdRoot);

// Deletion command factory. The host captures the snapshot at construction
// time (entity exists). Returns null if rootUuids is empty.
std::unique_ptr<IEditorCommand> MakeRemoveSubtreesCommand(
	SubtreeSnapshot snapshot,
	std::vector<rt2::core::UUID> rootUuids);

// Duplication/Paste command factories. The host has ALREADY applied the
// duplication/paste (via the manager's DuplicateSubtreesWithUuids /
// PasteSubtreesWithUuids) and captured the resulting SubtreeSnapshot. The
// factory wraps the snapshot + created root UUIDs into a command for
// RecordApplied. Returns null if createdRoots is empty.
std::unique_ptr<IEditorCommand> MakeDuplicateSubtreesCommand(
	SubtreeSnapshot snapshot,
	std::vector<rt2::core::UUID> createdRoots);
std::unique_ptr<IEditorCommand> MakePasteSubtreesCommand(
	SubtreeSnapshot snapshot,
	std::vector<rt2::core::UUID> createdRoots);

// Phase 8 W1 prefab command factories.
//
// MakeCreatePrefabCommand wraps the authoritative post-mutation
// PrefabCreationResult (regenerated deterministically on Redo) plus the
// pre-mutation file contents (empty = file absent) captured by the host
// BEFORE calling CreatePrefabFromSubtree. Returns null when the result is
// not ok or the after-state cannot be regenerated (templateIds/snapshot
// mismatch — that would be a silent-file corruption on Redo).
std::unique_ptr<IEditorCommand> MakeCreatePrefabCommand(
	std::filesystem::path prefabPath,
	SceneManager::PrefabCreationResult result,
	std::vector<uint8_t> beforeFileContents,
	bool fileExistedBefore);

// MakeInstantiatePrefabCommand mirrors the duplication factory: the host has
// ALREADY applied InstantiatePrefabWithUuids and captured the resulting
// SubtreeSnapshot. Returns null if createdRoots is empty.
std::unique_ptr<IEditorCommand> MakeInstantiatePrefabCommand(
	SubtreeSnapshot snapshot,
	std::vector<rt2::core::UUID> createdRoots);

// Reparent command factory. Returns null if afterEdits is empty or every
// after edit matches its corresponding before edit (no-op).
std::unique_ptr<IEditorCommand> MakeReparentCommandIfEffective(
	std::vector<ReparentEdit> beforeEdits,
	std::vector<ReparentEdit> afterEdits,
	ReparentMode mode);

#endif // RT2_EDITOR_STRUCTURAL_COMMANDS_H
