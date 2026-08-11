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

// Canonical per-wire equality mirroring the composite's S5 canonicalization
// (SceneManager.cpp S5CanonicalTRS / S5CanonicalCamera + S5EqualTRS /
// S5EqualCamera): a wire is "changed" only when its canonical form differs
// exactly. AlignCameraCommand must mark ONLY the wires a camera-pose edit
// actually changes — always marking Transform diverges an inherited transform
// with no explicit edit (F4).
bool CanonicalTrsEqual(const EditableTRS& a, const EditableTRS& b)
{
	return a.translation == b.translation && a.scale == b.scale
		&& glm::normalize(a.rotation) == glm::normalize(b.rotation);
}

bool CanonicalCameraEqual(const CameraComponent& a, const CameraComponent& b)
{
	glm::vec3 forwardA = a.forwardDirection;
	glm::vec3 forwardB = b.forwardDirection;
	if (glm::dot(forwardA, forwardA) > 1e-8f) forwardA = glm::normalize(forwardA);
	if (glm::dot(forwardB, forwardB) > 1e-8f) forwardB = glm::normalize(forwardB);
	return a.verticalFOV == b.verticalFOV && a.aperture == b.aperture
		&& a.focusDistance == b.focusDistance && forwardA == forwardB;
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
	const std::vector<std::pair<rt2::core::UUID,
		std::optional<MaterialOverrideComponent>>>& a,
	const std::vector<std::pair<rt2::core::UUID,
		std::optional<MaterialOverrideComponent>>>& b)
{
	if (a.size() != b.size()) return false;
	// Order-independent comparison: build a UUID -> optional override map for
	// b and look up each a entry.
	std::unordered_map<rt2::core::UUID,
		const std::optional<MaterialOverrideComponent>*> bMap;
	bMap.reserve(b.size());
	for (const auto& p : b)
		bMap.emplace(p.first, &p.second);
	for (const auto& p : a)
	{
		const auto it = bMap.find(p.first);
		if (it == bMap.end()) return false;
		if (p.second.has_value() != it->second->has_value()) return false;
		if (p.second.has_value() &&
		    !OverrideEqual(*p.second, **it->second)) return false;
	}
	return true;
}

} // namespace

// ---- SetNameCommand ----

SetNameCommand::SetNameCommand(rt2::core::UUID target,
                               std::string beforeName,
                               std::string afterName)
	: m_Target(target)
	, m_BeforeName(std::move(beforeName))
	, m_AfterName(std::move(afterName))
	, m_Transaction(
		std::vector<PrefabValueEdit>{
			{ PrefabValueKind::EntityName, m_Target, PrefabMarkerDirection::After,
				std::string(m_BeforeName), std::string(m_AfterName) } },
		std::vector<PrefabCommandTransaction::MarkerSpec>{
			{ m_Target, PrefabComponentKeyFor<NameComponent>::value, true } })
{
}

EditorMutationResult SetNameCommand::Execute(SceneManager& scene)
{
	return m_Transaction.Execute(scene);
}

EditorMutationResult SetNameCommand::Undo(SceneManager& scene)
{
	return m_Transaction.Undo(scene);
}

// ---- SetMaterialIndexCommand ----

SetMaterialIndexCommand::SetMaterialIndexCommand(
	rt2::core::UUID target,
	int beforeIndex,
	int afterIndex,
	std::optional<MaterialOverrideComponent> beforeOverride,
	std::optional<MaterialOverrideComponent> afterOverride)
	: m_Target(target)
	, m_BeforeIndex(beforeIndex)
	, m_AfterIndex(afterIndex)
	, m_BeforeOverride(std::move(beforeOverride))
	, m_AfterOverride(std::move(afterOverride))
	, m_Transaction(
		std::vector<PrefabValueEdit>{
			{ PrefabValueKind::MaterialIndex, m_Target, PrefabMarkerDirection::After,
				PrefabMaterialIndexValue{ m_BeforeIndex, m_BeforeOverride },
				PrefabMaterialIndexValue{ m_AfterIndex, m_AfterOverride } } },
		std::vector<PrefabCommandTransaction::MarkerSpec>{
			{ m_Target, PrefabComponentKeyFor<MaterialOverrideComponent>::value, true } })
{
}

EditorMutationResult SetMaterialIndexCommand::Execute(SceneManager& scene)
{
	return m_Transaction.Execute(scene);
}

EditorMutationResult SetMaterialIndexCommand::Undo(SceneManager& scene)
{
	return m_Transaction.Undo(scene);
}

// ---- SetMaterialPropertiesCommand ----

SetMaterialPropertiesCommand::SetMaterialPropertiesCommand(
	int slotIndex,
	SceneMaterial beforeMaterial,
	SceneMaterial afterMaterial,
	OverrideList beforeOverrides,
	OverrideList afterOverrides)
	: m_SlotIndex(slotIndex)
	, m_BeforeMaterial(std::move(beforeMaterial))
	, m_AfterMaterial(std::move(afterMaterial))
	, m_BeforeOverrides(std::move(beforeOverrides))
	, m_AfterOverrides(std::move(afterOverrides))
{
	// One materialOverride marker delta per affected prefab member. The
	// complete fan-out lists (StageMaterialSlot shape) carry the exact live
	// member set; non-members are dropped at capture, so including every
	// member here is safe.
	std::vector<PrefabCommandTransaction::MarkerSpec> markers;
	markers.reserve(m_BeforeOverrides.size() + m_AfterOverrides.size());
	auto addMember = [&](const rt2::core::UUID& member) {
		for (const auto& existing : markers)
			if (existing.member == member) return;
		markers.push_back({ member,
			PrefabComponentKeyFor<MaterialOverrideComponent>::value, true });
	};
	for (const auto& pair : m_BeforeOverrides) addMember(pair.first);
	for (const auto& pair : m_AfterOverrides) addMember(pair.first);

	m_Transaction = PrefabCommandTransaction(
		std::vector<PrefabValueEdit>{
			{ PrefabValueKind::MaterialSlotProperties, rt2::core::UUID{},
				PrefabMarkerDirection::After,
				PrefabMaterialSlotValue{ m_SlotIndex, m_BeforeMaterial, m_BeforeOverrides },
				PrefabMaterialSlotValue{ m_SlotIndex, m_AfterMaterial, m_AfterOverrides } } },
		std::move(markers));
}

EditorMutationResult SetMaterialPropertiesCommand::Execute(SceneManager& scene)
{
	return m_Transaction.Execute(scene);
}

EditorMutationResult SetMaterialPropertiesCommand::Undo(SceneManager& scene)
{
	return m_Transaction.Undo(scene);
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

SetMotionCommand::SetMotionCommand(rt2::core::UUID target,
                                   std::optional<MotionComponent> beforeValue,
                                   std::optional<MotionComponent> afterValue)
	: m_Target(target)
	, m_BeforeValue(std::move(beforeValue))
	, m_AfterValue(std::move(afterValue))
	, m_Transaction(
		std::vector<PrefabValueEdit>{
			{ PrefabValueKind::MotionState, m_Target, PrefabMarkerDirection::After,
				m_BeforeValue, m_AfterValue } },
		std::vector<PrefabCommandTransaction::MarkerSpec>{
			{ m_Target, PrefabComponentKeyFor<MotionComponent>::value,
				m_AfterValue.has_value() } })
{
}

EditorMutationResult SetMotionCommand::Execute(SceneManager& scene)
{
	return m_Transaction.Execute(scene);
}

EditorMutationResult SetMotionCommand::Undo(SceneManager& scene)
{
	return m_Transaction.Undo(scene);
}

// ---- SetScriptCommand ----

SetScriptCommand::SetScriptCommand(rt2::core::UUID target,
                                   std::optional<ScriptComponent> beforeValue,
                                   std::optional<ScriptComponent> afterValue)
	: m_Target(target)
	, m_BeforeValue(std::move(beforeValue))
	, m_AfterValue(std::move(afterValue))
	, m_Transaction(
		std::vector<PrefabValueEdit>{
			{ PrefabValueKind::ScriptState, m_Target, PrefabMarkerDirection::After,
				m_BeforeValue, m_AfterValue } },
		std::vector<PrefabCommandTransaction::MarkerSpec>{
			{ m_Target, PrefabComponentKeyFor<ScriptComponent>::value,
				m_AfterValue.has_value() } })
{
}

EditorMutationResult SetScriptCommand::Execute(SceneManager& scene)
{
	return m_Transaction.Execute(scene);
}

EditorMutationResult SetScriptCommand::Undo(SceneManager& scene)
{
	return m_Transaction.Undo(scene);
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

AlignCameraCommand::AlignCameraCommand(rt2::core::UUID target,
                                       EditableTRS beforeLocal,
                                       EditableTRS afterLocal,
                                       CameraComponent beforeCamera,
                                       CameraComponent afterCamera)
	: m_Target(target)
	, m_BeforeLocal(beforeLocal)
	, m_AfterLocal(afterLocal)
	, m_BeforeCamera(beforeCamera)
	, m_AfterCamera(afterCamera)
{
	// Mark ONLY the wires whose canonical form actually changes. A pose edit
	// that touches only the camera (or only the transform) must not diverge
	// the other inherited wire with a marker for an edit that never happened.
	std::vector<PrefabCommandTransaction::MarkerSpec> markers;
	if (!CanonicalTrsEqual(m_BeforeLocal, m_AfterLocal))
		markers.push_back({ m_Target, PrefabComponentKeyFor<Transform>::value, true });
	if (!CanonicalCameraEqual(m_BeforeCamera, m_AfterCamera))
		markers.push_back({ m_Target, PrefabComponentKeyFor<CameraComponent>::value, true });
	m_Transaction = PrefabCommandTransaction(
		std::vector<PrefabValueEdit>{
			{ PrefabValueKind::CameraPose, m_Target, PrefabMarkerDirection::After,
				PrefabCameraPoseValue{ m_BeforeLocal, m_BeforeCamera },
				PrefabCameraPoseValue{ m_AfterLocal, m_AfterCamera } } },
		std::move(markers));
}

EditorMutationResult AlignCameraCommand::Execute(SceneManager& scene)
{
	return m_Transaction.Execute(scene);
}

EditorMutationResult AlignCameraCommand::Undo(SceneManager& scene)
{
	return m_Transaction.Undo(scene);
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
