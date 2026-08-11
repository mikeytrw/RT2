#pragma once

#ifndef RT2_EDITOR_PROPERTY_COMMANDS_H
#define RT2_EDITOR_PROPERTY_COMMANDS_H

#include "EditorCommand.h"
#include "SceneManager.h"
#include "PrefabCommandTransaction.h"
#include "PrefabComponentKey.h"
#include "ScriptComponentValidation.h"
#include "TransformEditing.h"
#include "core/UUID.h"
#include "ECSComponents.h"
#include "SceneTypes.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// ============================================================================
// Phase 3B2 property editor commands. Each is UUID-keyed (or slot-indexed
// for material-properties), stores only the before/after state it touches,
// and resolves targets at run time. A missing entity is a graceful Failure.
// Impact assignments are authoritative from the manager — commands never
// synthesize sync impact.
//
// Phase 8 W3 S6-B: every command below that can touch an overridable prefab
// wire (name, material index, material-slot properties, motion, script,
// camera alignment) now carries a PrefabCommandTransaction: durable
// before/after value edits + marker membership deltas + a command-captured
// schema pair, replayed directionally through the S5/S6-A atomic composite.
// Execute/Redo replay the After direction (the first value write, any marker
// insertion, and any schema promotion land in ONE composite commit with ONE
// revision bump), Undo replays Before. Ordinary entities drop their marker
// deltas and behave exactly as before.
//
// Material commands (SetMaterialIndex, SetMaterialProperties) capture and
// restore durable MaterialOverrideComponent state atomically with the
// index/material-value edit, so Undo of an imported-entity material
// assignment does not leave a stale override that save/reopen would
// resurrect. The before/after override snapshots are stored in the command
// as complete UUID + optional lists (std::optional<MaterialOverrideComponent>;
// nullopt = absent) and re-applied by the composite fan-out.
//
// AlignCameraCommand uses the atomic composite CameraPose value (local TRS +
// camera props in one pass, one revision bump, one authoritative Transform
// impact), staged from the editor camera pose via SceneManager::StageCameraPose.
//
// SetMotionCommand is a single class covering add, remove, and velocity
// edits via std::optional<MotionComponent> before/after. Add = {nullopt,
// some}; Remove = {some, nullopt}; velocity edit = {some, some}. SetScriptCommand
// covers add, remove, script-path replacement, and any typed field-map change
// via std::optional<ScriptComponent> before/after the same way. Their marker
// deltas are conditional on the after-state carrying the component.
//
// No-op suppression is enforced at construction: callers should use the
// static factory functions, which return null when the command would be a
// no-op. A null unique_ptr from the factory signals "discard, do not
// submit".
//
// A recorded command whose FIRST replay is Undo (RecordApplied, the
// out-of-scope live-preview sessions) is treated as value-only by the
// transaction: its raw mutation wrote no markers, so the marker deltas are
// dropped and the composite value replay validates against live state.
// ============================================================================

class SetNameCommand final : public IEditorCommand
{
public:
	SetNameCommand(rt2::core::UUID target, std::string beforeName, std::string afterName);

	const rt2::core::UUID& Target() const { return m_Target; }
	const std::string& BeforeName() const { return m_BeforeName; }
	const std::string& AfterName() const { return m_AfterName; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Rename"; }

private:
	rt2::core::UUID              m_Target;
	std::string                  m_BeforeName;
	std::string                  m_AfterName;
	PrefabCommandTransaction     m_Transaction;
};

class SetMaterialIndexCommand final : public IEditorCommand
{
public:
	SetMaterialIndexCommand(rt2::core::UUID target,
	                        int beforeIndex,
	                        int afterIndex,
	                        std::optional<MaterialOverrideComponent> beforeOverride,
	                        std::optional<MaterialOverrideComponent> afterOverride);

	const rt2::core::UUID& Target() const { return m_Target; }
	int BeforeIndex() const { return m_BeforeIndex; }
	int AfterIndex() const { return m_AfterIndex; }
	const std::optional<MaterialOverrideComponent>& BeforeOverride() const { return m_BeforeOverride; }
	const std::optional<MaterialOverrideComponent>& AfterOverride() const { return m_AfterOverride; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Set Material"; }

private:
	rt2::core::UUID                              m_Target;
	int                                          m_BeforeIndex;
	int                                          m_AfterIndex;
	std::optional<MaterialOverrideComponent>     m_BeforeOverride;
	std::optional<MaterialOverrideComponent>     m_AfterOverride;
	PrefabCommandTransaction                     m_Transaction;
};

class SetMaterialPropertiesCommand final : public IEditorCommand
{
public:
	// Complete per-entity snapshots: UUID -> std::optional override (nullopt
	// = absent). Includes every imported entity referencing the slot, so the
	// composite can restore the exact fan-out on Undo.
	using OverrideList = std::vector<std::pair<rt2::core::UUID,
		std::optional<MaterialOverrideComponent>>>;

	SetMaterialPropertiesCommand(int slotIndex,
	                             SceneMaterial beforeMaterial,
	                             SceneMaterial afterMaterial,
	                             OverrideList beforeOverrides,
	                             OverrideList afterOverrides);

	int SlotIndex() const { return m_SlotIndex; }
	const SceneMaterial& BeforeMaterial() const { return m_BeforeMaterial; }
	const SceneMaterial& AfterMaterial() const { return m_AfterMaterial; }
	const OverrideList& BeforeOverrides() const { return m_BeforeOverrides; }
	const OverrideList& AfterOverrides() const { return m_AfterOverrides; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Edit Material"; }

private:
	int                     m_SlotIndex;
	SceneMaterial           m_BeforeMaterial;
	SceneMaterial           m_AfterMaterial;
	OverrideList            m_BeforeOverrides;
	OverrideList            m_AfterOverrides;
	PrefabCommandTransaction m_Transaction;
};

class SetLightCommand final : public IEditorCommand
{
public:
	SetLightCommand(rt2::core::UUID target,
	                LightComponent beforeValue,
	                LightComponent afterValue)
		: m_Target(target)
		, m_BeforeValue(beforeValue)
		, m_AfterValue(afterValue) {}

	const rt2::core::UUID& Target() const { return m_Target; }
	const LightComponent& BeforeValue() const { return m_BeforeValue; }
	const LightComponent& AfterValue() const { return m_AfterValue; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Edit Light"; }

private:
	rt2::core::UUID m_Target;
	LightComponent m_BeforeValue;
	LightComponent m_AfterValue;
};

class SetCameraCommand final : public IEditorCommand
{
public:
	SetCameraCommand(rt2::core::UUID target,
	                 CameraComponent beforeValue,
	                 CameraComponent afterValue)
		: m_Target(target)
		, m_BeforeValue(beforeValue)
		, m_AfterValue(afterValue) {}

	const rt2::core::UUID& Target() const { return m_Target; }
	const CameraComponent& BeforeValue() const { return m_BeforeValue; }
	const CameraComponent& AfterValue() const { return m_AfterValue; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Edit Camera"; }

private:
	rt2::core::UUID m_Target;
	CameraComponent m_BeforeValue;
	CameraComponent m_AfterValue;
};

class SetMotionCommand final : public IEditorCommand
{
public:
	SetMotionCommand(rt2::core::UUID target,
	                 std::optional<MotionComponent> beforeValue,
	                 std::optional<MotionComponent> afterValue);

	const rt2::core::UUID& Target() const { return m_Target; }
	const std::optional<MotionComponent>& BeforeValue() const { return m_BeforeValue; }
	const std::optional<MotionComponent>& AfterValue() const { return m_AfterValue; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Edit Motion"; }

private:
	rt2::core::UUID                     m_Target;
	std::optional<MotionComponent>      m_BeforeValue;
	std::optional<MotionComponent>      m_AfterValue;
	PrefabCommandTransaction             m_Transaction;
};

// SetScriptCommand covers add, remove, script-path replacement, and any typed
// field-map change via std::optional<ScriptComponent> before/after. Add =
// {nullopt, some}; Remove = {some, nullopt}; edit = {some, some}. The command
// owns full value copies only — no entt::entity, Lua objects, registry
// pointers, file handles, descriptors or runtime callbacks. Descriptions are
// state-aware: "Add Script", "Remove Script", or "Edit Script".
class SetScriptCommand final : public IEditorCommand
{
public:
	SetScriptCommand(rt2::core::UUID target,
	                 std::optional<ScriptComponent> beforeValue,
	                 std::optional<ScriptComponent> afterValue);

	const rt2::core::UUID& Target() const { return m_Target; }
	const std::optional<ScriptComponent>& BeforeValue() const { return m_BeforeValue; }
	const std::optional<ScriptComponent>& AfterValue() const { return m_AfterValue; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override;

private:
	rt2::core::UUID                        m_Target;
	std::optional<ScriptComponent>         m_BeforeValue;
	std::optional<ScriptComponent>         m_AfterValue;
	PrefabCommandTransaction               m_Transaction;
};

class AlignCameraCommand final : public IEditorCommand
{
public:
	AlignCameraCommand(rt2::core::UUID target,
	                   EditableTRS beforeLocal,
	                   EditableTRS afterLocal,
	                   CameraComponent beforeCamera,
	                   CameraComponent afterCamera);

	const rt2::core::UUID& Target() const { return m_Target; }
	const EditableTRS& BeforeLocal() const { return m_BeforeLocal; }
	const EditableTRS& AfterLocal() const { return m_AfterLocal; }
	const CameraComponent& BeforeCamera() const { return m_BeforeCamera; }
	const CameraComponent& AfterCamera() const { return m_AfterCamera; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Align Camera to View"; }

private:
	rt2::core::UUID         m_Target;
	EditableTRS             m_BeforeLocal;
	EditableTRS             m_AfterLocal;
	CameraComponent         m_BeforeCamera;
	CameraComponent         m_AfterCamera;
	PrefabCommandTransaction m_Transaction;
};

// ---- Factories with no-op suppression ----

// Returns null if beforeName == afterName.
std::unique_ptr<IEditorCommand> MakeSetNameCommandIfEffective(
	rt2::core::UUID target,
	std::string beforeName,
	std::string afterName);

// Returns null if beforeIndex == afterIndex AND beforeOverride == afterOverride.
// The host supplies the before/after override snapshots: the before read live
// before Execute and the after staged canonically via
// SceneManager::StageMaterialIndex (construct-then-Execute, so no captured
// state can be inverted at the call site — the 2026-08-03 material-index undo
// defect shape).
std::unique_ptr<IEditorCommand> MakeSetMaterialIndexCommandIfEffective(
	rt2::core::UUID target,
	int beforeIndex,
	int afterIndex,
	std::optional<MaterialOverrideComponent> beforeOverride,
	std::optional<MaterialOverrideComponent> afterOverride);

// Returns null if the before/after material values are equal AND the
// before/after override lists match. The host supplies a complete
// UUID + optional before snapshot (captured when the material session opens);
// the after override list is generally empty and derived by the composite.
std::unique_ptr<IEditorCommand> MakeSetMaterialPropertiesCommandIfEffective(
	int slotIndex,
	SceneMaterial beforeMaterial,
	SceneMaterial afterMaterial,
	SetMaterialPropertiesCommand::OverrideList beforeOverrides,
	SetMaterialPropertiesCommand::OverrideList afterOverrides);

// Returns null if the before/after LightComponent values are equal.
std::unique_ptr<IEditorCommand> MakeSetLightCommandIfEffective(
	rt2::core::UUID target,
	LightComponent beforeValue,
	LightComponent afterValue);

// Returns null if the before/after CameraComponent values are equal.
std::unique_ptr<IEditorCommand> MakeSetCameraCommandIfEffective(
	rt2::core::UUID target,
	CameraComponent beforeValue,
	CameraComponent afterValue);

// Returns null if beforeValue == afterValue (both present and equal) OR
// (both nullopt). Add = {nullopt, some}; Remove = {some, nullopt};
// velocity edit = {some, some}.
std::unique_ptr<IEditorCommand> MakeSetMotionCommandIfEffective(
	rt2::core::UUID target,
	std::optional<MotionComponent> beforeValue,
	std::optional<MotionComponent> afterValue);

// Returns null if both are absent OR both present and canonically equal
// (same path, derived sourceKey, and exact typed field map). A present
// before-state is validated/canonicalized first — an invalid before-snapshot
// returns null (it must never enter history, since a later failed Undo would
// clear the entire history under the established policy). An invalid
// after-state is NOT suppressed: the command is returned so
// EditorCommandHistory::Execute surfaces the manager's actionable failure
// without recording.
std::unique_ptr<IEditorCommand> MakeSetScriptCommandIfEffective(
	rt2::core::UUID target,
	std::optional<ScriptComponent> beforeValue,
	std::optional<ScriptComponent> afterValue);

// Returns null if before/after local TRS are equal AND before/after camera
// props are equal. Uses construct-then-Execute: the host stages the alignment
// target via SceneManager::StageCameraPose and passes it as the after-state;
// command Execute performs the composite camera-pose write + marker insertion
// atomically.
std::unique_ptr<IEditorCommand> MakeAlignCameraCommandIfEffective(
	rt2::core::UUID target,
	EditableTRS beforeLocal,
	EditableTRS afterLocal,
	CameraComponent beforeCamera,
	CameraComponent afterCamera);

#endif // RT2_EDITOR_PROPERTY_COMMANDS_H