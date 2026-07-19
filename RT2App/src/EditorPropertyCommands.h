#pragma once

#ifndef RT2_EDITOR_PROPERTY_COMMANDS_H
#define RT2_EDITOR_PROPERTY_COMMANDS_H

#include "EditorCommand.h"
#include "SceneManager.h"
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
// Material commands (SetMaterialIndex, SetMaterialProperties) capture and
// restore durable MaterialOverrideComponent state atomically with the
// index/material-value edit, so Undo of an imported-entity material
// assignment does not leave a stale override that save/reopen would
// resurrect. The before/after override snapshots are stored in the command
// (std::optional<MaterialOverrideComponent>; nullopt = absent) and
// restored via SceneManager::InstallMaterialOverride.
//
// AlignCameraCommand uses the atomic SetCameraPoseState API (composite
// local TRS + camera props in one pass, one revision bump, one
// authoritative Transform impact). Composing SetLocalTransformStates +
// SetCameraPropertiesState would bump the revision twice and require a
// synthesized combined impact — both violations.
//
// SetMotionCommand is a single class covering add, remove, and velocity
// edits via std::optional<MotionComponent> before/after. Add = {nullopt,
// some}; Remove = {some, nullopt}; velocity edit = {some, some}.
//
// No-op suppression is enforced at construction: callers should use the
// static factory functions, which return null when the command would be a
// no-op. A null unique_ptr from the factory signals "discard, do not
// submit".
// ============================================================================

class SetNameCommand final : public IEditorCommand
{
public:
	SetNameCommand(rt2::core::UUID target, std::string beforeName, std::string afterName)
		: m_Target(target)
		, m_BeforeName(std::move(beforeName))
		, m_AfterName(std::move(afterName)) {}

	const rt2::core::UUID& Target() const { return m_Target; }
	const std::string& BeforeName() const { return m_BeforeName; }
	const std::string& AfterName() const { return m_AfterName; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Rename"; }

private:
	rt2::core::UUID m_Target;
	std::string     m_BeforeName;
	std::string     m_AfterName;
};

class SetMaterialIndexCommand final : public IEditorCommand
{
public:
	SetMaterialIndexCommand(rt2::core::UUID target,
	                        int beforeIndex,
	                        int afterIndex,
	                        std::optional<MaterialOverrideComponent> beforeOverride,
	                        std::optional<MaterialOverrideComponent> afterOverride)
		: m_Target(target)
		, m_BeforeIndex(beforeIndex)
		, m_AfterIndex(afterIndex)
		, m_BeforeOverride(std::move(beforeOverride))
		, m_AfterOverride(std::move(afterOverride)) {}

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
};

class SetMaterialPropertiesCommand final : public IEditorCommand
{
public:
	using OverrideList = std::vector<std::pair<rt2::core::UUID, MaterialOverrideComponent>>;

	SetMaterialPropertiesCommand(int slotIndex,
	                             SceneMaterial beforeMaterial,
	                             SceneMaterial afterMaterial,
	                             OverrideList beforeOverrides,
	                             OverrideList afterOverrides)
		: m_SlotIndex(slotIndex)
		, m_BeforeMaterial(std::move(beforeMaterial))
		, m_AfterMaterial(std::move(afterMaterial))
		, m_BeforeOverrides(std::move(beforeOverrides))
		, m_AfterOverrides(std::move(afterOverrides)) {}

	int SlotIndex() const { return m_SlotIndex; }
	const SceneMaterial& BeforeMaterial() const { return m_BeforeMaterial; }
	const SceneMaterial& AfterMaterial() const { return m_AfterMaterial; }
	const OverrideList& BeforeOverrides() const { return m_BeforeOverrides; }
	const OverrideList& AfterOverrides() const { return m_AfterOverrides; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Edit Material"; }

private:
	int            m_SlotIndex;
	SceneMaterial  m_BeforeMaterial;
	SceneMaterial  m_AfterMaterial;
	OverrideList   m_BeforeOverrides;
	OverrideList   m_AfterOverrides;
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
	                 std::optional<MotionComponent> afterValue)
		: m_Target(target)
		, m_BeforeValue(std::move(beforeValue))
		, m_AfterValue(std::move(afterValue)) {}

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
};

class AlignCameraCommand final : public IEditorCommand
{
public:
	AlignCameraCommand(rt2::core::UUID target,
	                   EditableTRS beforeLocal,
	                   EditableTRS afterLocal,
	                   CameraComponent beforeCamera,
	                   CameraComponent afterCamera)
		: m_Target(target)
		, m_BeforeLocal(beforeLocal)
		, m_AfterLocal(afterLocal)
		, m_BeforeCamera(beforeCamera)
		, m_AfterCamera(afterCamera) {}

	const rt2::core::UUID& Target() const { return m_Target; }
	const EditableTRS& BeforeLocal() const { return m_BeforeLocal; }
	const EditableTRS& AfterLocal() const { return m_AfterLocal; }
	const CameraComponent& BeforeCamera() const { return m_BeforeCamera; }
	const CameraComponent& AfterCamera() const { return m_AfterCamera; }

	EditorMutationResult Execute(SceneManager& scene) override;
	EditorMutationResult Undo(SceneManager& scene) override;
	std::string Description() const override { return "Align Camera to View"; }

private:
	rt2::core::UUID m_Target;
	EditableTRS     m_BeforeLocal;
	EditableTRS     m_AfterLocal;
	CameraComponent m_BeforeCamera;
	CameraComponent m_AfterCamera;
};

// ---- Factories with no-op suppression ----

// Returns null if beforeName == afterName.
std::unique_ptr<IEditorCommand> MakeSetNameCommandIfEffective(
	rt2::core::UUID target,
	std::string beforeName,
	std::string afterName);

// Returns null if beforeIndex == afterIndex AND beforeOverride == afterOverride.
// The host captures the before-override from SceneManager::GetMaterialOverride
// at construction time and the after-override by calling SetMaterialIndexState
// then GetMaterialOverride (or by constructing it from the known new material).
std::unique_ptr<IEditorCommand> MakeSetMaterialIndexCommandIfEffective(
	rt2::core::UUID target,
	int beforeIndex,
	int afterIndex,
	std::optional<MaterialOverrideComponent> beforeOverride,
	std::optional<MaterialOverrideComponent> afterOverride);

// Returns null if the before/after material values are equal AND the
// before/after override lists match. The host captures the before-overrides
// of all imported entities referencing the slot at construction time and
// the after-overrides by calling SetMaterialPropertiesState then re-querying.
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

// Returns null if before/after local TRS are equal AND before/after camera
// props are equal. Used by AlignCameraToView — the host has ALREADY applied
// the alignment via AlignCameraEntityToView and captured the composite
// after-state; the factory wraps it for RecordApplied.
std::unique_ptr<IEditorCommand> MakeAlignCameraCommandIfEffective(
	rt2::core::UUID target,
	EditableTRS beforeLocal,
	EditableTRS afterLocal,
	CameraComponent beforeCamera,
	CameraComponent afterCamera);

#endif // RT2_EDITOR_PROPERTY_COMMANDS_H