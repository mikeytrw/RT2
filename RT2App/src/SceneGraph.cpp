#include "SceneGraph.h"
#include "RTLog.h"

void SceneGraph::MarkDirty(entt::registry& registry, entt::entity entity)
{
	SetLocalDirty(registry, entity);

	// Propagate to children — find all entities whose parent is this entity
	if (const auto* hierarchy = registry.try_get<Hierarchy>(entity))
		for (const auto child : hierarchy->children)
			if (registry.valid(child))
				MarkDirty(registry, child);
}

void SceneGraph::SetLocalDirty(entt::registry& registry, entt::entity entity)
{
	if (auto* t = registry.try_get<Transform>(entity))
		t->dirty = true;
}

void SceneGraph::UpdateWorldTransforms(entt::registry& registry)
{
	// Find all root entities (have Transform, no Hierarchy or Hierarchy.parent == null)
	auto view = registry.view<Transform>();

	for (auto entity : view)
	{
		Hierarchy* hier = registry.try_get<Hierarchy>(entity);
		if (hier && hier->parent != entt::null)
			continue;  // not a root

		UpdateNode(registry, entity, glm::mat4(1.0f), false);
	}
}

void SceneGraph::SnapshotTransforms(entt::registry& registry)
{
	auto view = registry.view<Transform>();
	for (auto entity : view)
	{
		auto& t = view.get<Transform>(entity);
		t.prevWorldMatrix = t.worldMatrix;
	}
}

glm::vec3 SceneGraph::GetWorldPosition(const entt::registry& registry, entt::entity entity)
{
	const auto* t = registry.try_get<Transform>(entity);
	if (t)
		return glm::vec3(t->worldMatrix[3]);
	return {0.0f, 0.0f, 0.0f};
}

void SceneGraph::UpdateNode(entt::registry& registry, entt::entity entity,
                            const glm::mat4& parentWorld, bool parentDirty)
{
	auto* t = registry.try_get<Transform>(entity);
	if (!t)
		return;

	bool dirty = parentDirty || t->dirty;

	if (dirty)
	{
		glm::mat4 local = t->localMatrix();
		t->worldMatrix = parentWorld * local;
		t->dirty = false;
	}

	// Recurse into the cached children list. NOTE: this does NOT scan the
	// registry for entities whose parent is this one — `children` is derived
	// state, rebuilt from the parent links by SceneHierarchy::RebuildChildren.
	// Anything that sets Hierarchy.parent must call that before expecting the
	// traversal to reach the child.
	if (const auto* hierarchy = registry.try_get<Hierarchy>(entity))
		for (const auto child : hierarchy->children)
			if (registry.valid(child))
				UpdateNode(registry, child, t->worldMatrix, dirty);
}
