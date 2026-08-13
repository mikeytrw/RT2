#include "EditorCommands.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

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
                                   EditableTRS afterLocal,
                                   std::optional<PrefabCommandTransaction::ExplicitCapture> capture)
{
	m_Triples.push_back({target, beforeLocal, afterLocal});
	std::vector<PrefabValueEdit> values{{ PrefabValueKind::LocalTransform, target,
		PrefabMarkerDirection::After, PrefabValuePayload{beforeLocal},
		PrefabValuePayload{afterLocal} }};
	std::vector<PrefabCommandTransaction::MarkerSpec> markers{{ target,
		PrefabComponentKeyFor<Transform>::value, true }};
	m_Transaction = PrefabCommandTransaction(std::move(values), std::move(markers));
	if (capture) m_Transaction.SetExplicitCapture(std::move(*capture));
}

TransformCommand::TransformCommand(std::vector<TransformTriple> triples,
	std::optional<PrefabCommandTransaction::ExplicitCapture> capture)
	: m_Triples(std::move(triples))
{
	std::vector<PrefabValueEdit> values;
	std::vector<PrefabCommandTransaction::MarkerSpec> markers;
	values.reserve(m_Triples.size());
	markers.reserve(m_Triples.size());
	for (const auto& triple : m_Triples)
	{
		values.push_back({ PrefabValueKind::LocalTransform, triple.target,
			PrefabMarkerDirection::After, PrefabValuePayload{triple.beforeLocal},
			PrefabValuePayload{triple.afterLocal} });
		markers.push_back({ triple.target, PrefabComponentKeyFor<Transform>::value, true });
	}
	m_Transaction = PrefabCommandTransaction(std::move(values), std::move(markers));
	if (capture) m_Transaction.SetExplicitCapture(std::move(*capture));
}

EditorMutationResult TransformCommand::Execute(SceneManager& scene)
{
	auto result = m_Transaction.Execute(scene);
	if (result.success) return result;
	// Legacy raw callers historically supplied a fixed before value for a
	// sequence of ordinary-entity commands. Preserve that raw compatibility
	// without weakening prefab composite replay (where a stale source is a
	// real integrity failure).
	bool allOrdinary = !m_Triples.empty();
	for (const auto& triple : m_Triples)
		if (scene.IsOverridden(triple.target,
			PrefabComponentKeyFor<Transform>::value).IsOk()) { allOrdinary = false; break; }
	if (allOrdinary)
	{
		std::vector<std::pair<rt2::core::UUID, EditableTRS>> states;
		for (const auto& triple : m_Triples) states.emplace_back(triple.target, triple.afterLocal);
		return scene.SetLocalTransformStates(states);
	}
	return result;
}

EditorMutationResult TransformCommand::Undo(SceneManager& scene)
{
	auto result = m_Transaction.Undo(scene);
	if (result.success) return result;
	bool allOrdinary = !m_Triples.empty();
	for (const auto& triple : m_Triples)
		if (scene.IsOverridden(triple.target,
			PrefabComponentKeyFor<Transform>::value).IsOk()) { allOrdinary = false; break; }
	if (allOrdinary)
	{
		std::vector<std::pair<rt2::core::UUID, EditableTRS>> states;
		for (const auto& triple : m_Triples) states.emplace_back(triple.target, triple.beforeLocal);
		return scene.SetLocalTransformStates(states);
	}
	return result;
}

SetVisibilityCommand::SetVisibilityCommand(PairList beforeStates,
                                           PairList afterStates)
	: m_BeforeStates(std::move(beforeStates))
	, m_AfterStates(std::move(afterStates))
{
	// One Visibility value edit plus one kVisible marker delta per entity.
	// The before and after UUID sets MUST match exactly (enforced by
	// MakeSetVisibilityCommandIfEffective); a before entry is required for
	// every after entry and is never fabricated — a missing entry throws
	// loudly rather than building a wrong Undo.
	std::unordered_map<rt2::core::UUID, bool> beforeMap;
	beforeMap.reserve(m_BeforeStates.size());
	for (const auto& p : m_BeforeStates)
		beforeMap.emplace(p.first, p.second);

	std::vector<PrefabValueEdit> values;
	std::vector<PrefabCommandTransaction::MarkerSpec> markers;
	values.reserve(m_AfterStates.size());
	markers.reserve(m_AfterStates.size());
	for (const auto& p : m_AfterStates)
	{
		values.push_back({ PrefabValueKind::Visibility, p.first,
			PrefabMarkerDirection::After, beforeMap.at(p.first), p.second });
		markers.push_back({ p.first,
			PrefabComponentKeyFor<VisibleComponent>::value, true });
	}
	m_Transaction = PrefabCommandTransaction(std::move(values), std::move(markers));
}

EditorMutationResult SetVisibilityCommand::Execute(SceneManager& scene)
{
	return m_Transaction.Execute(scene);
}

EditorMutationResult SetVisibilityCommand::Undo(SceneManager& scene)
{
	return m_Transaction.Undo(scene);
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
	rt2::core::UUID target, EditableTRS beforeLocal, EditableTRS afterLocal,
	PrefabCommandTransaction::ExplicitCapture capture)
{
	if (TrsEqual(beforeLocal, afterLocal)) return nullptr;
	return std::make_unique<TransformCommand>(target, beforeLocal, afterLocal,
		std::move(capture));
}

std::unique_ptr<IEditorCommand> MakeTransformCommandIfEffective(
	std::vector<TransformTriple> triples)
{
	std::vector<TransformTriple> effective;
	effective.reserve(triples.size());
	std::unordered_set<rt2::core::UUID> seen;
	for (auto& t : triples)
	{
		if (t.target.IsNull() || !seen.insert(t.target).second) return nullptr;
		if (!TrsEqual(t.beforeLocal, t.afterLocal))
			effective.push_back(std::move(t));
	}
	if (effective.empty()) return nullptr;
	return std::make_unique<TransformCommand>(std::move(effective));
}

std::unique_ptr<IEditorCommand> MakeTransformCommandIfEffective(
	std::vector<TransformTriple> triples,
	PrefabCommandTransaction::ExplicitCapture capture)
{
	std::vector<TransformTriple> effective;
	effective.reserve(triples.size());
	std::unordered_set<rt2::core::UUID> seen;
	for (auto& t : triples)
	{
		if (t.target.IsNull() || !seen.insert(t.target).second) return nullptr;
		if (!TrsEqual(t.beforeLocal, t.afterLocal)) effective.push_back(std::move(t));
	}
	if (effective.empty()) return nullptr;
	return std::make_unique<TransformCommand>(std::move(effective),
		std::move(capture));
}

std::unique_ptr<IEditorCommand> MakeSetVisibilityCommandIfEffective(
	SetVisibilityCommand::PairList beforeStates,
	SetVisibilityCommand::PairList afterStates)
{
	// Strict, proactive input validation: the before and after UUID sets must
	// match exactly and neither list may contain a duplicate UUID. A mismatch
	// means the host's before-state is incomplete or fabricated — proceeding
	// would silently build a wrong Undo, so reject the command instead.
	const auto hasDuplicate = [](const SetVisibilityCommand::PairList& list) {
		std::unordered_set<rt2::core::UUID> seen;
		for (const auto& p : list)
			if (!seen.insert(p.first).second) return true;
		return false;
	};
	if (hasDuplicate(beforeStates) || hasDuplicate(afterStates)) return nullptr;
	if (beforeStates.size() != afterStates.size()) return nullptr;

	std::unordered_map<rt2::core::UUID, bool> beforeMap;
	beforeMap.reserve(beforeStates.size());
	for (const auto& p : beforeStates)
		beforeMap.emplace(p.first, p.second);
	std::unordered_map<rt2::core::UUID, bool> afterMap;
	afterMap.reserve(afterStates.size());
	for (const auto& p : afterStates)
		afterMap.emplace(p.first, p.second);
	for (const auto& p : afterStates)
		if (beforeMap.find(p.first) == beforeMap.end()) return nullptr;
	for (const auto& p : beforeStates)
		if (afterMap.find(p.first) == afterMap.end()) return nullptr;

	// Drop after-pairs whose state matches the corresponding before-state.
	SetVisibilityCommand::PairList cleanedAfter;
	cleanedAfter.reserve(afterStates.size());
	for (const auto& p : afterStates)
		if (beforeMap.at(p.first) != p.second) cleanedAfter.push_back(p);
	if (cleanedAfter.empty()) return nullptr;

	// The UUID sets are equal, so every cleaned-after UUID has a before entry
	// — no before value is ever fabricated.
	SetVisibilityCommand::PairList cleanedBefore;
	cleanedBefore.reserve(cleanedAfter.size());
	for (const auto& p : cleanedAfter)
		cleanedBefore.emplace_back(p.first, beforeMap.at(p.first));

	return std::make_unique<SetVisibilityCommand>(std::move(cleanedBefore),
	                                              std::move(cleanedAfter));
}
