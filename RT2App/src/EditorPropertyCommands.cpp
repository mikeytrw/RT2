#include "EditorPropertyCommands.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace
{

constexpr float kEpsilon = 1e-6f;

bool Vec3Equal(const glm::vec3& a, const glm::vec3& b)
{
	return std::fabs(a.x - b.x) <= kEpsilon &&
	       std::fabs(a.y - b.y) <= kEpsilon &&
	       std::fabs(a.z - b.z) <= kEpsilon;
}

bool QuatEqual(const glm::quat& a, const glm::quat& b)
{
	glm::quat na = a; if (na.w < 0.0f) na = -na;
	glm::quat nb = b; if (nb.w < 0.0f) nb = -nb;
	return std::fabs(na.x - nb.x) <= kEpsilon &&
	       std::fabs(na.y - nb.y) <= kEpsilon &&
	       std::fabs(na.z - nb.z) <= kEpsilon &&
	       std::fabs(na.w - nb.w) <= kEpsilon;
}

bool TrsEqual(const EditableTRS& a, const EditableTRS& b)
{
	return Vec3Equal(a.translation, b.translation) &&
	       QuatEqual(a.rotation, b.rotation) &&
	       Vec3Equal(a.scale, b.scale);
}

bool FloatEq(float a, float b)
{
	return std::fabs(a - b) <= kEpsilon;
}

bool MaterialEqual(const SceneMaterial& a, const SceneMaterial& b)
{
	// SceneMaterial has no operator==; compare the authored fields the
	// Inspector exposes plus the type. Texture indices and alpha mode are
	// also compared so a non-Inspector edit (resolver, paste) is detected.
	return a.type == b.type &&
	       Vec3Equal(a.baseColor, b.baseColor) &&
	       FloatEq(a.baseAlpha, b.baseAlpha) &&
	       FloatEq(a.metallic, b.metallic) &&
	       FloatEq(a.roughness, b.roughness) &&
	       FloatEq(a.ior, b.ior) &&
	       FloatEq(a.transmissionFactor, b.transmissionFactor) &&
	       Vec3Equal(a.emissiveColor, b.emissiveColor) &&
	       FloatEq(a.emissiveIntensity, b.emissiveIntensity) &&
	       a.baseColorTextureIndex == b.baseColorTextureIndex &&
	       a.normalTextureIndex == b.normalTextureIndex &&
	       a.emissiveTextureIndex == b.emissiveTextureIndex &&
	       a.metallicRoughnessTextureIndex == b.metallicRoughnessTextureIndex &&
	       a.alphaMode == b.alphaMode &&
	       FloatEq(a.alphaCutoff, b.alphaCutoff);
}

bool LightEqual(const LightComponent& a, const LightComponent& b)
{
	return Vec3Equal(a.color, b.color) &&
	       FloatEq(a.intensity, b.intensity) &&
	       FloatEq(a.range, b.range) &&
	       FloatEq(a.innerConeAngle, b.innerConeAngle) &&
	       FloatEq(a.outerConeAngle, b.outerConeAngle) &&
	       a.type == b.type;
}

bool CameraEqual(const CameraComponent& a, const CameraComponent& b)
{
	return FloatEq(a.verticalFOV, b.verticalFOV) &&
	       FloatEq(a.aperture, b.aperture) &&
	       FloatEq(a.focusDistance, b.focusDistance) &&
	       Vec3Equal(a.forwardDirection, b.forwardDirection);
}

bool MotionEqual(const MotionComponent& a, const MotionComponent& b)
{
	return Vec3Equal(a.linearVelocity, b.linearVelocity);
}

bool OverrideEqual(const MaterialOverrideComponent& a,
                   const MaterialOverrideComponent& b)
{
	return a.authored == b.authored &&
	       a.sourceMaterialKey == b.sourceMaterialKey &&
	       a.materialIndex == b.materialIndex &&
	       MaterialEqual(a.material, b.material);
}

bool OverrideListEqual(
	const std::vector<std::pair<rt2::core::UUID, MaterialOverrideComponent>>& a,
	const std::vector<std::pair<rt2::core::UUID, MaterialOverrideComponent>>& b)
{
	if (a.size() != b.size()) return false;
	// Order-independent comparison: build a UUID -> override map for b and
	// look up each a entry.
	std::unordered_map<rt2::core::UUID, const MaterialOverrideComponent*> bMap;
	bMap.reserve(b.size());
	for (const auto& p : b)
		bMap.emplace(p.first, &p.second);
	for (const auto& p : a)
	{
		const auto it = bMap.find(p.first);
		if (it == bMap.end()) return false;
		if (!OverrideEqual(p.second, *it->second)) return false;
	}
	return true;
}

} // namespace

// ---- SetNameCommand ----

EditorMutationResult SetNameCommand::Execute(SceneManager& scene)
{
	return scene.SetEntityNameState(m_Target, m_AfterName);
}

EditorMutationResult SetNameCommand::Undo(SceneManager& scene)
{
	return scene.SetEntityNameState(m_Target, m_BeforeName);
}

// ---- SetMaterialIndexCommand ----

EditorMutationResult SetMaterialIndexCommand::Execute(SceneManager& scene)
{
	auto result = scene.SetMaterialIndexState(m_Target, m_AfterIndex);
	if (!result.success) return result;
	// SetMaterialIndexState re-derived the override from the material at
	// m_AfterIndex via RecordMaterialOverride. Overwrite with the exact
	// stored after-override (or remove if absent) so Undo can restore the
	// precise before-state.
	scene.InstallMaterialOverride(m_Target, m_AfterOverride);
	return result;
}

EditorMutationResult SetMaterialIndexCommand::Undo(SceneManager& scene)
{
	auto result = scene.SetMaterialIndexState(m_Target, m_BeforeIndex);
	if (!result.success) return result;
	scene.InstallMaterialOverride(m_Target, m_BeforeOverride);
	return result;
}

// ---- SetMaterialPropertiesCommand ----

EditorMutationResult SetMaterialPropertiesCommand::Execute(SceneManager& scene)
{
	auto result = scene.SetMaterialPropertiesState(m_SlotIndex, m_AfterMaterial);
	if (!result.success) return result;
	// Restore the exact after-overrides for the entities tracked at capture
	// time. SetMaterialPropertiesState re-derived overrides for every
	// imported entity referencing the slot; the command's after-overrides
	// list is the authoritative snapshot.
	for (const auto& [uuid, ov] : m_AfterOverrides)
		scene.InstallMaterialOverride(uuid, ov);
	// Entities that had a before-override but are NOT in the after-list must
	// have their override removed (the slot no longer references them, or
	// the after-state deliberately dropped the override). Compute the diff.
	std::unordered_map<rt2::core::UUID, bool> afterPresent;
	afterPresent.reserve(m_AfterOverrides.size());
	for (const auto& p : m_AfterOverrides)
		afterPresent.emplace(p.first, true);
	for (const auto& [uuid, ov] : m_BeforeOverrides)
	{
		if (!afterPresent.count(uuid))
			scene.InstallMaterialOverride(uuid, std::nullopt);
	}
	return result;
}

EditorMutationResult SetMaterialPropertiesCommand::Undo(SceneManager& scene)
{
	auto result = scene.SetMaterialPropertiesState(m_SlotIndex, m_BeforeMaterial);
	if (!result.success) return result;
	for (const auto& [uuid, ov] : m_BeforeOverrides)
		scene.InstallMaterialOverride(uuid, ov);
	// Remove overrides for entities that gained one during Execute but were
	// not present before.
	std::unordered_map<rt2::core::UUID, bool> beforePresent;
	beforePresent.reserve(m_BeforeOverrides.size());
	for (const auto& p : m_BeforeOverrides)
		beforePresent.emplace(p.first, true);
	for (const auto& [uuid, ov] : m_AfterOverrides)
	{
		if (!beforePresent.count(uuid))
			scene.InstallMaterialOverride(uuid, std::nullopt);
	}
	return result;
}

// ---- SetLightCommand ----

EditorMutationResult SetLightCommand::Execute(SceneManager& scene)
{
	return scene.SetLightPropertiesState(m_Target, m_AfterValue);
}

EditorMutationResult SetLightCommand::Undo(SceneManager& scene)
{
	return scene.SetLightPropertiesState(m_Target, m_BeforeValue);
}

// ---- SetCameraCommand ----

EditorMutationResult SetCameraCommand::Execute(SceneManager& scene)
{
	return scene.SetCameraPropertiesState(m_Target, m_AfterValue);
}

EditorMutationResult SetCameraCommand::Undo(SceneManager& scene)
{
	return scene.SetCameraPropertiesState(m_Target, m_BeforeValue);
}

// ---- SetMotionCommand ----

EditorMutationResult SetMotionCommand::Execute(SceneManager& scene)
{
	return scene.SetMotionState(m_Target, m_AfterValue);
}

EditorMutationResult SetMotionCommand::Undo(SceneManager& scene)
{
	return scene.SetMotionState(m_Target, m_BeforeValue);
}

// ---- SetScriptCommand ----

EditorMutationResult SetScriptCommand::Execute(SceneManager& scene)
{
	return scene.SetScriptState(m_Target, m_AfterValue);
}

EditorMutationResult SetScriptCommand::Undo(SceneManager& scene)
{
	return scene.SetScriptState(m_Target, m_BeforeValue);
}

std::string SetScriptCommand::Description() const
{
	if (!m_BeforeValue.has_value() && m_AfterValue.has_value())
		return "Add Script";
	if (m_BeforeValue.has_value() && !m_AfterValue.has_value())
		return "Remove Script";
	if (!m_BeforeValue.has_value() && !m_AfterValue.has_value())
		return "Script (no change)";
	return "Edit Script";
}

// ---- AlignCameraCommand ----

EditorMutationResult AlignCameraCommand::Execute(SceneManager& scene)
{
	return scene.SetCameraPoseState(m_Target, m_AfterLocal, m_AfterCamera);
}

EditorMutationResult AlignCameraCommand::Undo(SceneManager& scene)
{
	return scene.SetCameraPoseState(m_Target, m_BeforeLocal, m_BeforeCamera);
}

// ---- Factories ----

std::unique_ptr<IEditorCommand> MakeSetNameCommandIfEffective(
	rt2::core::UUID target,
	std::string beforeName,
	std::string afterName)
{
	if (beforeName == afterName) return nullptr;
	return std::make_unique<SetNameCommand>(target, std::move(beforeName),
	                                        std::move(afterName));
}

std::unique_ptr<IEditorCommand> MakeSetMaterialIndexCommandIfEffective(
	rt2::core::UUID target,
	int beforeIndex,
	int afterIndex,
	std::optional<MaterialOverrideComponent> beforeOverride,
	std::optional<MaterialOverrideComponent> afterOverride)
{
	if (beforeIndex == afterIndex)
	{
		const bool beforeHas = beforeOverride.has_value();
		const bool afterHas = afterOverride.has_value();
		if (!beforeHas && !afterHas) return nullptr;
		if (beforeHas && afterHas && OverrideEqual(*beforeOverride, *afterOverride))
			return nullptr;
	}
	return std::make_unique<SetMaterialIndexCommand>(target, beforeIndex,
	                                                 afterIndex,
	                                                 std::move(beforeOverride),
	                                                 std::move(afterOverride));
}

std::unique_ptr<IEditorCommand> MakeSetMaterialPropertiesCommandIfEffective(
	int slotIndex,
	SceneMaterial beforeMaterial,
	SceneMaterial afterMaterial,
	SetMaterialPropertiesCommand::OverrideList beforeOverrides,
	SetMaterialPropertiesCommand::OverrideList afterOverrides)
{
	if (MaterialEqual(beforeMaterial, afterMaterial) &&
	    OverrideListEqual(beforeOverrides, afterOverrides))
		return nullptr;
	return std::make_unique<SetMaterialPropertiesCommand>(slotIndex,
		std::move(beforeMaterial), std::move(afterMaterial),
		std::move(beforeOverrides), std::move(afterOverrides));
}

std::unique_ptr<IEditorCommand> MakeSetLightCommandIfEffective(
	rt2::core::UUID target,
	LightComponent beforeValue,
	LightComponent afterValue)
{
	if (LightEqual(beforeValue, afterValue)) return nullptr;
	return std::make_unique<SetLightCommand>(target, beforeValue, afterValue);
}

std::unique_ptr<IEditorCommand> MakeSetCameraCommandIfEffective(
	rt2::core::UUID target,
	CameraComponent beforeValue,
	CameraComponent afterValue)
{
	if (CameraEqual(beforeValue, afterValue)) return nullptr;
	return std::make_unique<SetCameraCommand>(target, beforeValue, afterValue);
}

std::unique_ptr<IEditorCommand> MakeSetMotionCommandIfEffective(
	rt2::core::UUID target,
	std::optional<MotionComponent> beforeValue,
	std::optional<MotionComponent> afterValue)
{
	const bool beforeHas = beforeValue.has_value();
	const bool afterHas = afterValue.has_value();
	if (!beforeHas && !afterHas) return nullptr;
	if (beforeHas && afterHas && MotionEqual(*beforeValue, *afterValue))
		return nullptr;
	return std::make_unique<SetMotionCommand>(target,
		std::move(beforeValue), std::move(afterValue));
}

std::unique_ptr<IEditorCommand> MakeSetScriptCommandIfEffective(
	rt2::core::UUID target,
	std::optional<ScriptComponent> beforeValue,
	std::optional<ScriptComponent> afterValue)
{
	const bool beforeHas = beforeValue.has_value();
	const bool afterHas = afterValue.has_value();

	// Validate/canonicalize a present before-state first. An invalid
	// before-snapshot returns null — it must never enter history because a
	// later failed Undo would clear the entire history under the established
	// policy (W4-F6).
	if (beforeHas)
	{
		ScriptComponent canonical;
		std::string detail;
		std::string field;
		if (!rt2::core::NormalizeAndValidateScriptComponent(
				*beforeValue, canonical, detail, &field))
		{
			return nullptr;
		}
		beforeValue = std::move(canonical);
	}

	// Both absent → no-op.
	if (!beforeHas && !afterHas) return nullptr;

	// Validate/canonicalize a present after-state. An invalid after-state
	// is NOT suppressed: the command is returned so EditorCommandHistory::
	// Execute surfaces the manager's actionable failure without recording.
	// When valid, the canonical form replaces the raw input so the command
	// stores exactly what the manager will store (no stale sourceKey or
	// non-default importSettings on the add path).
	bool afterValid = false;
	if (afterHas)
	{
		ScriptComponent canonicalAfter;
		std::string detail;
		std::string field;
		afterValid = rt2::core::NormalizeAndValidateScriptComponent(
				*afterValue, canonicalAfter, detail, &field);
		if (afterValid)
			afterValue = std::move(canonicalAfter);
	}

	// Both present and canonically equal → no-op. Only checked when both
	// are valid (before was validated above; after was just validated).
	if (beforeHas && afterHas && afterValid &&
	    rt2::core::ScriptComponentCanonicalEqual(beforeValue, afterValue))
		return nullptr;

	return std::make_unique<SetScriptCommand>(target,
		std::move(beforeValue), std::move(afterValue));
}

std::unique_ptr<IEditorCommand> MakeAlignCameraCommandIfEffective(
	rt2::core::UUID target,
	EditableTRS beforeLocal,
	EditableTRS afterLocal,
	CameraComponent beforeCamera,
	CameraComponent afterCamera)
{
	if (TrsEqual(beforeLocal, afterLocal) && CameraEqual(beforeCamera, afterCamera))
		return nullptr;
	return std::make_unique<AlignCameraCommand>(target,
		beforeLocal, afterLocal, beforeCamera, afterCamera);
}