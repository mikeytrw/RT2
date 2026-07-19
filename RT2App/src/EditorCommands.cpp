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

EditorMutationResult TransformResultFor(const std::vector<TransformTriple>& triples)
{
	EditorMutationResult result;
	result.success = true;
	result.syncImpact = rt2::core::SyncImpact::Transform;
	result.affectedEntities.reserve(triples.size());
	for (const auto& t : triples)
		result.affectedEntities.push_back(t.target);
	return result;
}

EditorMutationResult FailureFor(const rt2::core::UUID& target, const char* detail)
{
	return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
		target.IsNull() ? std::string{} : target.ToString(),
		detail);
}

} // namespace

TransformCommand::TransformCommand(rt2::core::UUID target,
                                   EditableTRS beforeLocal,
                                   EditableTRS afterLocal)
{
	m_Triples.push_back({target, beforeLocal, afterLocal});
}

TransformCommand::TransformCommand(std::vector<TransformTriple> triples)
	: m_Triples(std::move(triples)) {}

EditorMutationResult TransformCommand::Execute(SceneManager& scene)
{
	std::vector<std::pair<rt2::core::UUID, EditableTRS>> states;
	states.reserve(m_Triples.size());
	for (const auto& t : m_Triples)
		states.emplace_back(t.target, t.afterLocal);
	auto result = scene.SetLocalTransformStates(states);
	if (!result.success) return result;
	// SetLocalTransformStates already populated affectedEntities; if it
	// returned success with empty affectedEntities (empty input), fall back
	// to the triple-derived result.
	if (result.affectedEntities.empty())
		return TransformResultFor(m_Triples);
	return result;
}

EditorMutationResult TransformCommand::Undo(SceneManager& scene)
{
	std::vector<std::pair<rt2::core::UUID, EditableTRS>> states;
	states.reserve(m_Triples.size());
	for (const auto& t : m_Triples)
		states.emplace_back(t.target, t.beforeLocal);
	auto result = scene.SetLocalTransformStates(states);
	if (!result.success) return result;
	if (result.affectedEntities.empty())
		return TransformResultFor(m_Triples);
	return result;
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

std::unique_ptr<IEditorCommand> MakeTransformCommandIfEffective(
	std::vector<TransformTriple> triples)
{
	std::vector<TransformTriple> effective;
	effective.reserve(triples.size());
	for (auto& t : triples)
		if (!TrsEqual(t.beforeLocal, t.afterLocal))
			effective.push_back(std::move(t));
	if (effective.empty()) return nullptr;
	return std::make_unique<TransformCommand>(std::move(effective));
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