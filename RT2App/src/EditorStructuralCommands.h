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

// Reparent command factory. Returns null if afterEdits is empty or every
// after edit matches its corresponding before edit (no-op).
std::unique_ptr<IEditorCommand> MakeReparentCommandIfEffective(
	std::vector<ReparentEdit> beforeEdits,
	std::vector<ReparentEdit> afterEdits,
	ReparentMode mode);

#endif // RT2_EDITOR_STRUCTURAL_COMMANDS_H