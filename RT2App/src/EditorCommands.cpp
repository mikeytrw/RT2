#include "EditorCommands.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace
{

// Epsilon compare for normalized no-op detection.
constexpr float kEpsilon = 1e-6f;

bool Vec3Equal(const glm::vec3& a, const glm::vec3& b)
{
	return std::fabs(a.x - b.x) <= kEpsilon &&
	       std::fabs(a.y - b.y) <= kEpsilon &&
	       std::fabs(a.z - b.z) <= kEpsilon;
}

// Sign-canonicalized quaternion compare: q and -q represent the same rotation.
bool QuatEqual(const glm::quat& a, const glm::quat& b)
{
	glm::quat na = a;
	glm::quat nb = b;
	if (na.w < 0.0f) na = -na;
	if (nb.w < 0.0f) nb = -nb;
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

EditorMutationResult TransformResultFor(const rt2::core::UUID& target)
{
	EditorMutationResult result;
	result.success = true;
	result.syncImpact = rt2::core::SyncImpact::Transform;
	result.affectedEntities.push_back(target);
	return result;
}

EditorMutationResult FailureFor(const rt2::core::UUID& target, const char* detail)
{
	return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
		target.IsNull() ? std::string{} : target.ToString(),
		detail);
}

} // namespace

EditorMutationResult TransformCommand::Execute(SceneManager& scene)
{
	const auto entity = scene.FindEntityByUuid(m_Target);
	if (entity == entt::null)
		return FailureFor(m_Target, "transform target UUID is not present in the scene");
	scene.SetLocalTransform(SceneManager::EntityId{ entity }, m_AfterLocal);
	return TransformResultFor(m_Target);
}

EditorMutationResult TransformCommand::Undo(SceneManager& scene)
{
	const auto entity = scene.FindEntityByUuid(m_Target);
	if (entity == entt::null)
		return FailureFor(m_Target, "transform target UUID is not present in the scene");
	scene.SetLocalTransform(SceneManager::EntityId{ entity }, m_BeforeLocal);
	return TransformResultFor(m_Target);
}

EditorMutationResult SetVisibilityCommand::Execute(SceneManager& scene)
{
	auto result = scene.SetVisibilityStates(m_AfterStates);
	if (!result.success) return result;
	return result;
}

EditorMutationResult SetVisibilityCommand::Undo(SceneManager& scene)
{
	auto result = scene.SetVisibilityStates(m_BeforeStates);
	if (!result.success) return result;
	return result;
}

std::unique_ptr<IEditorCommand> MakeTransformCommandIfEffective(
	rt2::core::UUID target,
	EditableTRS beforeLocal,
	EditableTRS afterLocal)
{
	if (TrsEqual(beforeLocal, afterLocal)) return nullptr;
	return std::make_unique<TransformCommand>(target, beforeLocal, afterLocal);
}

std::unique_ptr<IEditorCommand> MakeSetVisibilityCommandIfEffective(
	SetVisibilityCommand::PairList beforeStates,
	SetVisibilityCommand::PairList afterStates)
{
	// Drop after-pairs whose state matches the corresponding before-state.
	// Build a UUID -> before-state lookup; if a UUID is missing from before,
	// keep the after-pair (the manager will validate the UUID on apply).
	std::unordered_map<rt2::core::UUID, bool> beforeMap;
	beforeMap.reserve(beforeStates.size());
	for (const auto& p : beforeStates)
		beforeMap.emplace(p.first, p.second);

	SetVisibilityCommand::PairList cleanedAfter;
	cleanedAfter.reserve(afterStates.size());
	for (const auto& p : afterStates)
	{
		const auto it = beforeMap.find(p.first);
		if (it != beforeMap.end() && it->second == p.second) continue;
		cleanedAfter.push_back(p);
	}
	if (cleanedAfter.empty()) return nullptr;

	// Compose a matching before list: for each cleaned-after pair, use the
	// before-state if known, otherwise default to the after-state (so the
	// manager sees a no-op for that entity on Undo if it wasn't tracked).
	// In practice the Inspector always supplies a before entry for every
	// after entry; this is just defensive.
	SetVisibilityCommand::PairList cleanedBefore;
	cleanedBefore.reserve(cleanedAfter.size());
	for (const auto& p : cleanedAfter)
	{
		const auto it = beforeMap.find(p.first);
		cleanedBefore.emplace_back(p.first, it != beforeMap.end() ? it->second : p.second);
	}

	return std::make_unique<SetVisibilityCommand>(std::move(cleanedBefore),
	                                              std::move(cleanedAfter));
}