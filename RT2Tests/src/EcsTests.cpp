#include <doctest/doctest.h>

#include <entt/entt.hpp>
#include "ECSComponents.h"
#include "SceneGraph.h"
#include "SceneHierarchy.h"
#include "core/Error.h"
#include "MeshRegistry.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

// ============================================================================
// ECS + SceneGraph tests
// ============================================================================

// ----------------------------------------------------------------------------
// Hierarchy.children is DERIVED from Hierarchy.parent, never maintained by
// hand. SceneManager and SceneSerializer both call RebuildChildren after
// touching parentage, and SceneGraph's traversal walks `children` — so a test
// that sets only `parent` builds a half-linked graph the traversal cannot
// reach. These cases used to fail with every world position reading zero for
// exactly that reason.
// ----------------------------------------------------------------------------
static void LinkHierarchy(entt::registry& registry)
{
    rt2::core::Error err;
    REQUIRE(SceneHierarchy::RebuildChildren(registry, err));
}


TEST_CASE("ECS: registry creates and destroys entities")
{
    entt::registry registry;
    auto e1 = registry.create();
    auto e2 = registry.create();
    CHECK(registry.valid(e1));
    CHECK(registry.valid(e2));
    CHECK(e1 != e2);
    registry.destroy(e2);
    CHECK(!registry.valid(e2));
}

TEST_CASE("ECS: Transform component stores TRS")
{
    entt::registry registry;
    auto e = registry.create();
    auto& t = registry.emplace<Transform>(e);
    t.translation = {1.0f, 2.0f, 3.0f};
    t.scale = {2.0f, 2.0f, 2.0f};

    auto& got = registry.get<Transform>(e);
    CHECK(got.translation.x == 1.0f);
    CHECK(got.translation.y == 2.0f);
    CHECK(got.translation.z == 3.0f);
    CHECK(got.scale.x == 2.0f);
}

TEST_CASE("ECS: MeshRef component stores mesh and material index")
{
    entt::registry registry;
    auto e = registry.create();
    MeshRef ref;
    ref.meshIndex = 5u;
    ref.materialIndex = 3;
    registry.emplace<MeshRef>(e, ref);

    auto& got = registry.get<MeshRef>(e);
    CHECK(got.meshIndex == 5u);
    CHECK(got.materialIndex == 3);
}

TEST_CASE("SceneGraph: root entity world matrix equals local matrix")
{
    entt::registry registry;
    auto e = registry.create();
    auto& t = registry.emplace<Transform>(e);
    t.translation = {5.0f, 0.0f, 0.0f};
    t.dirty = true;

    SceneGraph::UpdateWorldTransforms(registry);

    glm::vec3 worldPos = SceneGraph::GetWorldPosition(registry, e);
    CHECK(worldPos.x == doctest::Approx(5.0f));
    CHECK(worldPos.y == doctest::Approx(0.0f));
    CHECK(worldPos.z == doctest::Approx(0.0f));
    CHECK(!t.dirty);
}

TEST_CASE("SceneGraph: child inherits parent transform")
{
    entt::registry registry;
    auto parent = registry.create();
    auto& pt = registry.emplace<Transform>(parent);
    pt.translation = {10.0f, 0.0f, 0.0f};
    pt.dirty = true;

    auto child = registry.create();
    auto& ct = registry.emplace<Transform>(child);
    ct.translation = {1.0f, 0.0f, 0.0f};
    ct.dirty = true;

    auto& hier = registry.emplace<Hierarchy>(child);
    hier.parent = parent;

    LinkHierarchy(registry);
    SceneGraph::UpdateWorldTransforms(registry);

    // Child world position = parent translation + child translation = (11, 0, 0)
    glm::vec3 worldPos = SceneGraph::GetWorldPosition(registry, child);
    CHECK(worldPos.x == doctest::Approx(11.0f));
    CHECK(worldPos.y == doctest::Approx(0.0f));
    CHECK(worldPos.z == doctest::Approx(0.0f));
}

TEST_CASE("SceneGraph: grandchild inherits concatenated transforms")
{
    entt::registry registry;

    // root at (10, 0, 0), child at (0, 5, 0), grandchild at (0, 0, 2)
    auto root = registry.create();
    registry.emplace<Transform>(root).translation = {10.0f, 0.0f, 0.0f};

    auto child = registry.create();
    registry.emplace<Transform>(child).translation = {0.0f, 5.0f, 0.0f};
    {
        auto& h = registry.emplace<Hierarchy>(child);
        h.parent = root;
    }

    auto grandchild = registry.create();
    registry.emplace<Transform>(grandchild).translation = {0.0f, 0.0f, 2.0f};
    {
        auto& h = registry.emplace<Hierarchy>(grandchild);
        h.parent = child;
    }

    LinkHierarchy(registry);
    SceneGraph::UpdateWorldTransforms(registry);

    // Expected world: (10, 5, 2)
    glm::vec3 worldPos = SceneGraph::GetWorldPosition(registry, grandchild);
    CHECK(worldPos.x == doctest::Approx(10.0f));
    CHECK(worldPos.y == doctest::Approx(5.0f));
    CHECK(worldPos.z == doctest::Approx(2.0f));
}

TEST_CASE("SceneGraph: scale affects child positions")
{
    entt::registry registry;

    auto parent = registry.create();
    auto& pt = registry.emplace<Transform>(parent);
    pt.scale = {2.0f, 2.0f, 2.0f};

    auto child = registry.create();
    registry.emplace<Transform>(child).translation = {1.0f, 0.0f, 0.0f};
    {
        auto& h = registry.emplace<Hierarchy>(child);
        h.parent = parent;
    }

    LinkHierarchy(registry);
    SceneGraph::UpdateWorldTransforms(registry);

    // Parent scales by 2, child at local (1,0,0) → world (2,0,0)
    glm::vec3 worldPos = SceneGraph::GetWorldPosition(registry, child);
    CHECK(worldPos.x == doctest::Approx(2.0f));
}

TEST_CASE("SceneGraph: rotation affects child positions")
{
    entt::registry registry;

    auto parent = registry.create();
    auto& pt = registry.emplace<Transform>(parent);
    // 90 degree rotation around Y axis
    pt.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    auto child = registry.create();
    registry.emplace<Transform>(child).translation = {1.0f, 0.0f, 0.0f};
    {
        auto& h = registry.emplace<Hierarchy>(child);
        h.parent = parent;
    }

    LinkHierarchy(registry);
    SceneGraph::UpdateWorldTransforms(registry);

    // 90° Y rotation of (1,0,0) → (0,0,-1)
    glm::vec3 worldPos = SceneGraph::GetWorldPosition(registry, child);
    CHECK(worldPos.x == doctest::Approx(0.0f).epsilon(0.01f));
    CHECK(worldPos.y == doctest::Approx(0.0f).epsilon(0.01f));
    CHECK(worldPos.z == doctest::Approx(-1.0f).epsilon(0.01f));
}

TEST_CASE("SceneGraph: MarkDirty propagates to children")
{
    entt::registry registry;

    auto parent = registry.create();
    registry.emplace<Transform>(parent);

    auto child = registry.create();
    registry.emplace<Transform>(child);
    {
        auto& h = registry.emplace<Hierarchy>(child);
        h.parent = parent;
    }

    // First update — both get world matrices
    LinkHierarchy(registry);
    SceneGraph::UpdateWorldTransforms(registry);
    CHECK(!registry.get<Transform>(parent).dirty);
    CHECK(!registry.get<Transform>(child).dirty);

    // Mark parent dirty — child should also be marked
    SceneGraph::MarkDirty(registry, parent);
    CHECK(registry.get<Transform>(parent).dirty);
    CHECK(registry.get<Transform>(child).dirty);
}

TEST_CASE("SceneGraph: SnapshotTransforms stashes prev world matrix")
{
    entt::registry registry;

    auto e = registry.create();
    auto& t = registry.emplace<Transform>(e);
    t.translation = {1.0f, 0.0f, 0.0f};
    t.dirty = true;

    SceneGraph::UpdateWorldTransforms(registry);
    glm::mat4 firstWorld = t.worldMatrix;

    // Snapshot
    SceneGraph::SnapshotTransforms(registry);
    CHECK(t.prevWorldMatrix == firstWorld);

    // Move entity
    t.translation = {2.0f, 0.0f, 0.0f};
    t.dirty = true;
    SceneGraph::UpdateWorldTransforms(registry);

    // prev should still be old position
    glm::vec3 prevPos(t.prevWorldMatrix[3]);
    CHECK(prevPos.x == doctest::Approx(1.0f));

    // current should be new position
    glm::vec3 currPos(t.worldMatrix[3]);
    CHECK(currPos.x == doctest::Approx(2.0f));
}

TEST_CASE("MeshRegistry: add and retrieve meshes")
{
    MeshRegistry registry;
    MeshData mesh;
    mesh.vertices = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    mesh.indices = {0, 1, 2};
    mesh.name = "test_triangle";

    uint32_t idx = registry.AddMesh(std::move(mesh));
    CHECK(idx == 0);
    CHECK(registry.GetCount() == 1);
    CHECK(registry.GetMesh(0).name == "test_triangle");
    CHECK(registry.GetMesh(0).vertices.size() == 9);
    CHECK(registry.GetMesh(0).indices.size() == 3);
}

TEST_CASE("MeshRegistry: multiple meshes get sequential indices")
{
    MeshRegistry registry;
    MeshData m1, m2;
    m1.name = "mesh1";
    m2.name = "mesh2";

    uint32_t idx1 = registry.AddMesh(std::move(m1));
    uint32_t idx2 = registry.AddMesh(std::move(m2));

    CHECK(idx1 == 0);
    CHECK(idx2 == 1);
    CHECK(registry.GetCount() == 2);
}

TEST_CASE("MeshRegistry: clear removes all meshes")
{
    MeshRegistry registry;
    MeshData m1, m2;
    registry.AddMesh(std::move(m1));
    registry.AddMesh(std::move(m2));
    CHECK(registry.GetCount() == 2);
    registry.Clear();
    CHECK(registry.GetCount() == 0);
}