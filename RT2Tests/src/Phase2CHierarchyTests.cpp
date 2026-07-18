#include <doctest/doctest.h>

#include "SceneHierarchy.h"
#include "SceneVisibility.h"
#include "SceneDocument.h"
#include "GPUSceneData.h"
#include "SceneManager.h"
#include "SceneGraph.h"
#include "EditorSceneState.h"

namespace
{
entt::entity AddEntity(rt2::core::SceneDocument& document, bool renderable = false)
{
    const auto entity = document.ecs.registry.create();
    document.ecs.registry.emplace<Transform>(entity);
    document.ecs.registry.emplace<VisibleComponent>(entity);
    if (renderable)
        document.ecs.registry.emplace<MeshRef>(entity);
    document.AssignNewUuid(entity);
    return entity;
}
}

TEST_CASE("Phase 2C hierarchy rebuild treats parent as authoritative")
{
    rt2::core::DeterministicUuidProvider ids;
    rt2::core::SceneDocument document;
    document.SetUuidProvider(&ids);
    const auto root = AddEntity(document);
    const auto child = AddEntity(document);
    const auto grandchild = AddEntity(document);
    document.ecs.registry.emplace<Hierarchy>(child).parent = root;
    document.ecs.registry.emplace<Hierarchy>(grandchild).parent = child;

    rt2::core::Error error;
    REQUIRE(SceneHierarchy::RebuildChildren(document.ecs.registry, error));
    CHECK(document.ecs.registry.get<Hierarchy>(root).children ==
          std::vector<entt::entity>{ child });
    CHECK(document.ecs.registry.get<Hierarchy>(child).children ==
          std::vector<entt::entity>{ grandchild });
    CHECK(SceneHierarchy::Validate(document.ecs.registry, error));
}

TEST_CASE("Phase 2C hierarchy rebuild rejects cycles without rewriting caches")
{
    entt::registry registry;
    const auto a = registry.create();
    const auto b = registry.create();
    registry.emplace<Hierarchy>(a).parent = b;
    registry.emplace<Hierarchy>(b).parent = a;

    rt2::core::Error error;
    CHECK_FALSE(SceneHierarchy::RebuildChildren(registry, error));
    CHECK(error.code == rt2::core::Error::HierarchyCycle);
    CHECK(registry.get<Hierarchy>(a).children.empty());
    CHECK(registry.get<Hierarchy>(b).children.empty());
}

TEST_CASE("Phase 2C effective visibility inherits hidden ancestors")
{
    rt2::core::DeterministicUuidProvider ids;
    rt2::core::SceneDocument document;
    document.SetUuidProvider(&ids);
    const auto root = AddEntity(document);
    const auto child = AddEntity(document, true);
    document.ecs.registry.emplace<Hierarchy>(child).parent = root;
    document.ecs.registry.get<VisibleComponent>(root).visible = false;
    rt2::core::Error error;
    REQUIRE(SceneHierarchy::RebuildChildren(document.ecs.registry, error));

    CHECK_FALSE(SceneVisibility::IsEffectivelyVisible(document.ecs.registry, child));
    RenderInstanceMap map;
    const auto gpu = BuildGPUSceneDataFromECS(document.ecs, &map);
    CHECK(gpu.instances.empty());
    CHECK(map.empty());
}

TEST_CASE("Phase 2C full and transform GPU paths share visible instance ordering")
{
    rt2::core::DeterministicUuidProvider ids;
    rt2::core::SceneDocument document;
    document.SetUuidProvider(&ids);
    AddEntity(document, true);
    const auto hidden = AddEntity(document, true);
    AddEntity(document, true);
    document.ecs.registry.get<VisibleComponent>(hidden).visible = false;

    RenderInstanceMap buildMap;
    auto gpu = BuildGPUSceneDataFromECS(document.ecs, &buildMap);
    REQUIRE(gpu.instances.size() == 2);
    REQUIRE(buildMap.size() == 2);

    RenderInstanceMap updateMap;
    UpdateInstancesFromECS(gpu, document.ecs, &updateMap);
    CHECK(updateMap == buildMap);
}

TEST_CASE("Phase 2C UUID reparent preserves world transform and rejects cycles atomically")
{
    rt2::core::DeterministicUuidProvider ids;
    SceneManager manager;
    manager.SetUuidProvider(&ids);
    const auto parentA = manager.CreateEmpty("A").affectedEntities.front();
    const auto parentB = manager.CreateEmpty("B").affectedEntities.front();
    const auto child = manager.CreateEmpty("Child", parentA).affectedEntities.front();
    manager.SetTransform({ manager.FindEntityByUuid(parentA) }, { 5.0f, 0.0f, 0.0f });
    manager.SetTransform({ manager.FindEntityByUuid(parentB) }, { -3.0f, 0.0f, 0.0f });
    manager.SetTransform({ manager.FindEntityByUuid(child) }, { 2.0f, 0.0f, 0.0f });
    SceneGraph::UpdateWorldTransforms(manager.GetECS().registry);
    const glm::mat4 before = manager.GetECS().registry.get<Transform>(
        manager.FindEntityByUuid(child)).worldMatrix;

    const auto revision = manager.AuthoringRevision();
    const auto moved = manager.Reparent({ child }, parentB, ReparentMode::PreserveWorld);
    REQUIRE(moved.success);
    CHECK(moved.syncImpact == rt2::core::SyncImpact::Transform);
    CHECK(manager.AuthoringRevision() == revision + 1);
    SceneGraph::UpdateWorldTransforms(manager.GetECS().registry);
    const glm::mat4 after = manager.GetECS().registry.get<Transform>(
        manager.FindEntityByUuid(child)).worldMatrix;
    CHECK(glm::length(glm::vec3(before[3]) - glm::vec3(after[3])) < 0.0001f);

    const auto rejectedRevision = manager.AuthoringRevision();
    const auto rejected = manager.Reparent({ parentB }, child);
    CHECK_FALSE(rejected.success);
    CHECK(rejected.error.code == rt2::core::Error::HierarchyCycle);
    CHECK(manager.AuthoringRevision() == rejectedRevision);
    CHECK(static_cast<uint32_t>(manager.GetParent({ manager.FindEntityByUuid(parentB) }).id) ==
          static_cast<uint32_t>(entt::null));
}

TEST_CASE("Phase 2C batch delete canonicalizes selected descendants and bumps revision once")
{
    rt2::core::DeterministicUuidProvider ids;
    SceneManager manager;
    manager.SetUuidProvider(&ids);
    const auto root = manager.CreateEmpty("Root").affectedEntities.front();
    const auto child = manager.CreateEmpty("Child", root).affectedEntities.front();
    const auto leaf = manager.CreateEmpty("Leaf", child).affectedEntities.front();
    const auto revision = manager.AuthoringRevision();

    const auto removed = manager.RemoveSubtrees({ child, leaf });
    REQUIRE(removed.success);
    CHECK(manager.AuthoringRevision() == revision + 1);
    CHECK(static_cast<uint32_t>(manager.FindEntityByUuid(child)) == static_cast<uint32_t>(entt::null));
    CHECK(static_cast<uint32_t>(manager.FindEntityByUuid(leaf)) == static_cast<uint32_t>(entt::null));
    CHECK(static_cast<uint32_t>(manager.FindEntityByUuid(root)) != static_cast<uint32_t>(entt::null));
    CHECK_FALSE(manager.HasChildren({ manager.FindEntityByUuid(root) }));
}

TEST_CASE("Phase 2C visibility mutation has no-op semantics and inherited rendering")
{
    rt2::core::DeterministicUuidProvider ids;
    SceneManager manager;
    manager.SetUuidProvider(&ids);
    const auto parent = manager.CreateEmpty("Parent").affectedEntities.front();
    const auto childEntity = manager.AddObject("Child");
    const auto child = manager.GetEntityUuid(childEntity);
    REQUIRE(manager.Reparent({ child }, parent, ReparentMode::PreserveLocal).success);
    const auto revision = manager.AuthoringRevision();

    const auto hidden = manager.SetVisibility({ parent }, false);
    REQUIRE(hidden.success);
    CHECK(hidden.syncImpact == rt2::core::SyncImpact::Structural);
    CHECK(manager.AuthoringRevision() == revision + 1);
    CHECK(BuildGPUSceneDataFromECS(manager.GetECS()).instances.empty());

    const auto noOpRevision = manager.AuthoringRevision();
    const auto noOp = manager.SetVisibility({ parent }, false);
    CHECK(noOp.success);
    CHECK(noOp.syncImpact == rt2::core::SyncImpact::None);
    CHECK(manager.AuthoringRevision() == noOpRevision);
}

TEST_CASE("Phase 2C duplicate hierarchy assigns fresh UUIDs and preserves authored components")
{
    rt2::core::DeterministicUuidProvider ids;
    SceneManager manager;
    manager.SetUuidProvider(&ids);
    const auto root = manager.CreateEmpty("Assembly").affectedEntities.front();
    const auto sourceChild = manager.AddObject("Part");
    const auto child = manager.GetEntityUuid(sourceChild);
    REQUIRE(manager.Reparent({ child }, root, ReparentMode::PreserveLocal).success);
    manager.GetECS().registry.emplace<LightComponent>(sourceChild.id);
    manager.GetECS().registry.emplace<MotionComponent>(sourceChild.id,
        MotionComponent{ { 1.0f, 2.0f, 3.0f } });
    const auto sourceMesh = manager.GetECS().registry.get<MeshRef>(sourceChild.id).meshIndex;
    const auto revision = manager.AuthoringRevision();

    const auto duplicated = manager.DuplicateSubtrees({ root, child });
    REQUIRE(duplicated.success);
    REQUIRE(duplicated.affectedEntities.size() == 1);
    CHECK(duplicated.affectedEntities.front() != root);
    CHECK(manager.AuthoringRevision() == revision + 1);
    const auto duplicateRoot = manager.FindEntityByUuid(duplicated.affectedEntities.front());
    CHECK(manager.GetEntityName({ duplicateRoot }) == "Assembly Copy");
    const auto duplicateChildren = manager.GetChildren({ duplicateRoot });
    REQUIRE(duplicateChildren.size() == 1);
    const auto duplicateChild = duplicateChildren.front().id;
    CHECK(manager.GetEntityUuid({ duplicateChild }) != child);
    CHECK(manager.GetECS().registry.get<MeshRef>(duplicateChild).meshIndex == sourceMesh);
    CHECK(manager.GetECS().registry.all_of<LightComponent>(duplicateChild));
    CHECK(manager.GetECS().registry.get<MotionComponent>(duplicateChild).linearVelocity ==
          glm::vec3(1.0f, 2.0f, 3.0f));
}

TEST_CASE("Phase 2C editor locks are direct-only and duplicates remain unlocked")
{
    rt2::core::DeterministicUuidProvider ids;
    SceneManager manager;
    manager.SetUuidProvider(&ids);
    const auto parent = manager.CreateEmpty("Parent").affectedEntities.front();
    const auto child = manager.CreateEmpty("Child", parent).affectedEntities.front();
    EditorSceneState state;
    state.SetLocked(parent, true);
    CHECK(state.AnyDirectlyLocked({ parent }));
    CHECK_FALSE(state.AnyDirectlyLocked({ child }));

    const auto duplicate = manager.DuplicateSubtrees({ parent });
    REQUIRE(duplicate.success);
    CHECK_FALSE(state.IsLocked(duplicate.affectedEntities.front()));
}

TEST_CASE("Phase 2C clipboard is immutable and document scoped")
{
    rt2::core::DeterministicUuidProvider ids;
    SceneManager manager;
    manager.SetUuidProvider(&ids);
    const auto source = manager.CreateEmpty("Original").affectedEntities.front();
    EditorSceneState state;
    rt2::core::Error error;
    REQUIRE(state.Copy(manager, { source }, error));
    manager.SetEntityName({ manager.FindEntityByUuid(source) }, "Changed After Copy");

    const auto pasted = state.Paste(manager);
    REQUIRE(pasted.success);
    REQUIRE(pasted.affectedEntities.size() == 1);
    CHECK(manager.GetEntityName({ manager.FindEntityByUuid(pasted.affectedEntities.front()) }) ==
          "Original Copy");

    manager.Clear();
    const auto stale = state.Paste(manager);
    CHECK_FALSE(stale.success);
    CHECK(stale.error.code == rt2::core::Error::ClipboardStale);
}
