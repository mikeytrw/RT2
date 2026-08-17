#include <doctest/doctest.h>

#include "EditorCommand.h"
#include "EditorCommandHistory.h"
#include "EditorCommands.h"
#include "EditorSceneState.h"
#include "EditorSyncRouter.h"
#include "ISceneRenderBridge.h"
#include "PrimitiveGeometry.h"
#include "SceneManager.h"
#include "TransformEditing.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace
{

class RecordingBridge final : public rt2::core::ISceneRenderBridge
{
public:
    int fullSync = 0;
    int materialSync = 0;
    int transformSync = 0;
    int temporalReset = 0;
    int renderRequests = 0;
    void FullSync(GPUSceneData&) override { ++fullSync; }
    void MaterialSync(GPUSceneData&) override { ++materialSync; }
    void TransformSync(GPUSceneData&) override { ++transformSync; }
    void ResetTemporalState() override { ++temporalReset; }
    void RequestRender() override { ++renderRequests; }
};

struct SceneFixture
{
    rt2::core::DeterministicUuidProvider ids;
    SceneManager manager;
    EditorCommandHistory history;

    SceneFixture()
    {
        manager.SetUuidProvider(&ids);
        manager.AddMaterial(SceneMaterial{});
    }

    rt2::core::UUID AddBox(const char* name, glm::vec3 pos = {0, 0, 0})
    {
        const auto entity = manager.AddObjectWithGeometry(
            name, PrimitiveGeometry::CreateCube(2.0f), pos, {}, 1.0f, 0);
        return manager.GetEntityUuid(entity);
    }

    EditableTRS LocalOf(const rt2::core::UUID& uuid)
    {
        EditableTRS trs;
        const auto entity = manager.FindEntityByUuid(uuid);
        REQUIRE(entity != static_cast<entt::entity>(entt::null));
        REQUIRE(manager.GetLocalTransform(SceneManager::EntityId{ entity }, trs));
        return trs;
    }

    bool VisibleOf(const rt2::core::UUID& uuid)
    {
        const auto entity = manager.FindEntityByUuid(uuid);
        REQUIRE(entity != static_cast<entt::entity>(entt::null));
        const auto* vc = manager.GetECS().registry.try_get<VisibleComponent>(entity);
        return vc ? vc->visible : true;
    }
};

bool TrsEqualLoose(const EditableTRS& a, const EditableTRS& b)
{
    constexpr float eps = 1e-5f;
    auto vEq = [eps](const glm::vec3& x, const glm::vec3& y) {
        return std::fabs(x.x - y.x) <= eps &&
               std::fabs(x.y - y.y) <= eps &&
               std::fabs(x.z - y.z) <= eps;
    };
    auto qEq = [eps](const glm::quat& x, const glm::quat& y) {
        glm::quat a2 = x; if (a2.w < 0) a2 = -a2;
        glm::quat b2 = y; if (b2.w < 0) b2 = -b2;
        return std::fabs(a2.x - b2.x) <= eps &&
               std::fabs(a2.y - b2.y) <= eps &&
               std::fabs(a2.z - b2.z) <= eps &&
               std::fabs(a2.w - b2.w) <= eps;
    };
    return vEq(a.translation, b.translation) && qEq(a.rotation, b.rotation) && vEq(a.scale, b.scale);
}

} // namespace

// ---------------------------------------------------------------------------
// TransformCommand: Execute / Undo / Redo restores precise TRS, and an
// execute/undo/redo/undo cycle restores the original before-state on the
// second undo.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3A TransformCommand execute/undo/redo/undo restores before/after")
{
    SceneFixture f;
    const auto uuid = f.AddBox("Box", {1, 2, 3});
    const EditableTRS before = f.LocalOf(uuid);

    EditableTRS after = before;
    after.translation = {10, 11, 12};
    after.scale = {2, 3, 4};

    auto cmd = MakeTransformCommandIfEffective(uuid, before, after);
    REQUIRE(cmd);

    EditorMutationResult r1 = f.history.Execute(std::move(cmd), f.manager);
    REQUIRE(r1.success);
    REQUIRE(r1.syncImpact == rt2::core::SyncImpact::Transform);
    REQUIRE(TrsEqualLoose(f.LocalOf(uuid), after));

    EditorMutationResult r2 = f.history.Undo(f.manager);
    REQUIRE(r2.success);
    REQUIRE(r2.syncImpact == rt2::core::SyncImpact::Transform);
    REQUIRE(TrsEqualLoose(f.LocalOf(uuid), before));

    EditorMutationResult r3 = f.history.Redo(f.manager);
    REQUIRE(r3.success);
    REQUIRE(TrsEqualLoose(f.LocalOf(uuid), after));

    EditorMutationResult r4 = f.history.Undo(f.manager);
    REQUIRE(r4.success);
    REQUIRE(TrsEqualLoose(f.LocalOf(uuid), before));
}

// ---------------------------------------------------------------------------
// A new effective command after Undo empties the redo stack; a no-op
// submission does not.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3A new effective command clears redo; no-op does not")
{
    SceneFixture f;
    const auto uuid = f.AddBox("Box");
    const EditableTRS b0 = f.LocalOf(uuid);

    EditableTRS a1 = b0; a1.translation = {1, 0, 0};
    EditableTRS a2 = b0; a2.translation = {0, 5, 0};

    f.history.Execute(MakeTransformCommandIfEffective(uuid, b0, a1), f.manager);
    REQUIRE(f.history.Undo(f.manager).success);
    REQUIRE(f.history.CanRedo());

    // No-op submission: factory returns null, do not execute.
    auto noop = MakeTransformCommandIfEffective(uuid, b0, b0);
    REQUIRE(!noop);
    REQUIRE(f.history.CanRedo());

    // A new effective command clears redo.
    f.history.Execute(MakeTransformCommandIfEffective(uuid, b0, a2), f.manager);
    REQUIRE_FALSE(f.history.CanRedo());
    REQUIRE(TrsEqualLoose(f.LocalOf(uuid), a2));
}

// ---------------------------------------------------------------------------
// Bounded history (default 64, configurable) evicts oldest; every retained
// entry still undoes correctly.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3A bounded history evicts oldest and retained entries undo")
{
    SceneFixture f;
    const auto uuid = f.AddBox("Box");
    const EditableTRS b0 = f.LocalOf(uuid);

    EditorCommandHistory small(3);
    for (int i = 0; i < 5; ++i)
    {
		const EditableTRS before = f.LocalOf(uuid);
        EditableTRS after = b0;
        after.translation = {(float)i, 0, 0};
		small.Execute(MakeTransformCommandIfEffective(uuid, before, after), f.manager);
    }
    // Only the last 3 should be retained; 2 undos bring us to the state of
    // submission #3, and a 3rd undo would attempt submission #2 which has
    // been evicted — so we expect CanUndo after exactly 2 undos then a 3rd
    // undo still succeeds (3 entries retained).
    for (int i = 0; i < 3; ++i)
    {
        REQUIRE(small.CanUndo());
        REQUIRE(small.Undo(f.manager).success);
    }
    REQUIRE_FALSE(small.CanUndo());

    // Re-do them; all retained entries should redo.
    for (int i = 0; i < 3; ++i)
    {
        REQUIRE(small.CanRedo());
        REQUIRE(small.Redo(f.manager).success);
    }
    REQUIRE_FALSE(small.CanRedo());
}

// ---------------------------------------------------------------------------
// SetVisibilityStates: atomic validate-first failure (no partial mutation),
// single revision bump, mixed-state round trip, empty/no-change None result.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3A SetVisibilityStates atomic validate-first failure")
{
    SceneFixture f;
    const auto a = f.AddBox("A");
    const auto b = f.AddBox("B");
    rt2::core::UUID bogus;
    bogus.bytes[0] = 0xFF;

    const auto beforeA = f.VisibleOf(a);
    const auto beforeB = f.VisibleOf(b);
    const auto revBefore = f.manager.AuthoringRevision();

    auto result = f.manager.SetVisibilityStates({ {a, false}, {bogus, true} });

    REQUIRE_FALSE(result.success);
    // No mutation occurred.
    REQUIRE(f.VisibleOf(a) == beforeA);
    REQUIRE(f.VisibleOf(b) == beforeB);
    REQUIRE(f.manager.AuthoringRevision() == revBefore);
}

TEST_CASE("Phase 3A SetVisibilityStates mixed-state round trip and None result")
{
    SceneFixture f;
    const auto a = f.AddBox("A");
    const auto b = f.AddBox("B");
    const auto c = f.AddBox("C");

    // Hide A and B (C stays visible).
    REQUIRE(f.manager.SetVisibilityStates({ {a, false}, {b, false} }).success);
    REQUIRE_FALSE(f.VisibleOf(a));
    REQUIRE_FALSE(f.VisibleOf(b));
    REQUIRE(f.VisibleOf(c));

    // Mixed-state round trip: hide all three (A and B already hidden).
    auto r1 = f.manager.SetVisibilityStates({ {a, false}, {b, false}, {c, false} });
    REQUIRE(r1.success);
    REQUIRE(r1.syncImpact == rt2::core::SyncImpact::Structural);
    REQUIRE_FALSE(f.VisibleOf(c));
    // affectedEntities should only include c (a and b were already hidden).
    REQUIRE(r1.affectedEntities.size() == 1);

    // Now restore via inverted states: a->true, b->true, c->true.
    auto r2 = f.manager.SetVisibilityStates({ {a, true}, {b, true}, {c, true} });
    REQUIRE(r2.success);
    REQUIRE(f.VisibleOf(a));
    REQUIRE(f.VisibleOf(b));
    REQUIRE(f.VisibleOf(c));

    // No-change None result: every entity already in the target state.
    const auto revBefore = f.manager.AuthoringRevision();
    auto r3 = f.manager.SetVisibilityStates({ {a, true}, {b, true}, {c, true} });
    REQUIRE(r3.success);
    REQUIRE(r3.syncImpact == rt2::core::SyncImpact::None);
    REQUIRE(r3.affectedEntities.empty());
    REQUIRE(f.manager.AuthoringRevision() == revBefore);
}

TEST_CASE("Phase 3A SetVisibilityStates deduplicates last-write-wins")
{
    SceneFixture f;
    const auto a = f.AddBox("A");
    REQUIRE(f.manager.SetVisibilityStates({ {a, false} }).success);
    REQUIRE_FALSE(f.VisibleOf(a));

    // Duplicate entries with last-write-wins: hide -> show -> hide again.
    auto r = f.manager.SetVisibilityStates({ {a, false}, {a, true}, {a, false} });
    REQUIRE(r.success);
    // The final write is `false`, and the entity is already hidden (no change).
    REQUIRE(r.syncImpact == rt2::core::SyncImpact::None);
    REQUIRE_FALSE(f.VisibleOf(a));
}

// ---------------------------------------------------------------------------
// Sync-impact spies: with counting SyncCallback installed, Execute/Undo
// invoke ZERO sync callbacks; returned impact matches expected; TransformCommand
// never reports Material/Structural.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3A history invokes no sync callbacks; impact is authoritative")
{
    SceneFixture f;
    int transformSyncs = 0;
    int fullSyncs = 0;
    int materialSyncs = 0;
    f.manager.SetInstanceSyncCallback([&](GPUSceneData&, const RenderInstanceMap&) {
        ++transformSyncs;
    });
    f.manager.SetSyncCallback([&](GPUSceneData&, const RenderInstanceMap&) {
        ++fullSyncs;
    });
    f.manager.SetSyncKeepTexturesCallback([&](GPUSceneData&, const RenderInstanceMap&) {
        ++materialSyncs;
    });

    const auto uuid = f.AddBox("Box");
    const EditableTRS b0 = f.LocalOf(uuid);
    EditableTRS after = b0; after.translation = {5, 0, 0};

    auto r1 = f.history.Execute(MakeTransformCommandIfEffective(uuid, b0, after), f.manager);
    REQUIRE(r1.success);
    REQUIRE(r1.syncImpact == rt2::core::SyncImpact::Transform);
    REQUIRE(r1.syncImpact != rt2::core::SyncImpact::Material);
    REQUIRE(r1.syncImpact != rt2::core::SyncImpact::Structural);

    auto r2 = f.history.Undo(f.manager);
    REQUIRE(r2.success);
    REQUIRE(r2.syncImpact == rt2::core::SyncImpact::Transform);

    REQUIRE(transformSyncs == 0);
    REQUIRE(fullSyncs == 0);
    REQUIRE(materialSyncs == 0);
}

// ---------------------------------------------------------------------------
// EditorSyncRouter: each impact triggers exactly its own sync path, None
// triggers nothing, and the resource-generation downgrade check is honored.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3A EditorSyncRouter routes impacts to the correct path")
{
    SceneManager manager;
    int transforms = 0, materials = 0, fulls = 0, resets = 0;
    bool rendererAvailable = true;
    bool texPending = false;

    EditorSyncRouter router;
    router.SetTransformSync([&] { ++transforms; });
    router.SetMaterialSync([&] { ++materials; });
    router.SetFullSync([&] { ++fulls; });
    router.SetResetAccum([&] { ++resets; });
    router.SetRendererAvailable([&] { return rendererAvailable; });
    router.SetTextureUploadPending([&] { return texPending; });

    EditorMutationResult none;
    none.success = true;
    none.syncImpact = rt2::core::SyncImpact::None;
    router.Route(none, manager);
    REQUIRE(transforms == 0);
    REQUIRE(materials == 0);
    REQUIRE(fulls == 0);
    REQUIRE(resets == 0);

    EditorMutationResult t;
    t.success = true;
    t.syncImpact = rt2::core::SyncImpact::Transform;
    router.Route(t, manager);
    REQUIRE(transforms == 1);
    REQUIRE(resets == 1);
    REQUIRE(materials == 0);
    REQUIRE(fulls == 0);

    // Structural with resource generation change -> full sync.
    // Force a resource generation bump by clearing the scene.
    manager.Clear();
    EditorMutationResult s;
    s.success = true;
    s.syncImpact = rt2::core::SyncImpact::Structural;
    router.Route(s, manager);
    REQUIRE(fulls == 1);
    REQUIRE(resets == 2);

    // Another Structural without a resource generation change (visibility
    // toggle in real life): downgrade to material sync.
    router.Route(s, manager);
    REQUIRE(fulls == 1);
    REQUIRE(materials == 1);
    REQUIRE(resets == 3);

    // Material impact -> material sync (when no texture upload pending).
    EditorMutationResult m;
    m.success = true;
    m.syncImpact = rt2::core::SyncImpact::Material;
    router.Route(m, manager);
    REQUIRE(materials == 2);

    // Material impact with texture upload pending -> no material sync, but
    // reset still fires.
    texPending = true;
    router.Route(m, manager);
    REQUIRE(materials == 2);
    REQUIRE(resets == 5);
}

// ---------------------------------------------------------------------------
// Generation guard on all four public operations; explicit Clear empties
// both stacks.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3A generation guard clears both stacks on mismatch")
{
    SceneFixture f;
    const auto uuid = f.AddBox("Box");
    const EditableTRS b0 = f.LocalOf(uuid);
    EditableTRS after = b0; after.translation = {1, 0, 0};
    f.history.Execute(MakeTransformCommandIfEffective(uuid, b0, after), f.manager);
    REQUIRE(f.history.CanUndo());

    // Bump document generation out-of-band (simulates adoption).
    f.manager.Clear();

    // Undo should detect the generation mismatch and clear both stacks.
    auto r = f.history.Undo(f.manager);
    REQUIRE_FALSE(r.success);
    REQUIRE(f.history.CanUndo() == false);
    REQUIRE(f.history.CanRedo() == false);

    // A new Execute after the clear should succeed and become the first entry.
    // The Clear() above wiped the entity; recreate it with the same UUID
    // provider so the same UUID is handed out again.
    const auto uuid2 = f.AddBox("Box2");
    EditableTRS b0b;
    REQUIRE(f.manager.GetLocalTransform(
        SceneManager::EntityId{ f.manager.FindEntityByUuid(uuid2) }, b0b));
    EditableTRS after2 = b0b; after2.translation = {2, 0, 0};
    REQUIRE(f.history.Execute(MakeTransformCommandIfEffective(uuid2, b0b, after2), f.manager).success);
    REQUIRE(f.history.CanUndo());

    // Explicit Clear empties both stacks.
    f.history.Clear();
    REQUIRE_FALSE(f.history.CanUndo());
    REQUIRE_FALSE(f.history.CanRedo());
}

TEST_CASE("Phase 3A generation guard covers RecordApplied and Redo")
{
    SceneFixture f;
    const auto uuid = f.AddBox("Box");
    const EditableTRS b0 = f.LocalOf(uuid);

    EditableTRS after = b0; after.translation = {1, 0, 0};
    auto cmd = MakeTransformCommandIfEffective(uuid, b0, after);
    EditorMutationResult applied; applied.success = true;
    applied.syncImpact = rt2::core::SyncImpact::Transform;
    applied.affectedEntities.push_back(uuid);
    f.history.RecordApplied(std::move(cmd), f.manager, applied);
    REQUIRE(f.history.CanUndo());

    f.manager.Clear();
    auto r = f.history.Redo(f.manager);
    REQUIRE_FALSE(r.success);
    REQUIRE_FALSE(f.history.CanUndo());
    REQUIRE_FALSE(f.history.CanRedo());
}

// ---------------------------------------------------------------------------
// Failed Undo (target UUID no longer resolves) surfaces an error and clears
// both stacks; the scene is not further mutated.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3A failed Undo clears both stacks and does not mutate")
{
    SceneFixture f;
    const auto uuid = f.AddBox("Box");
    const EditableTRS b0 = f.LocalOf(uuid);
    EditableTRS after = b0; after.translation = {1, 0, 0};

    f.history.Execute(MakeTransformCommandIfEffective(uuid, b0, after), f.manager);
    REQUIRE(f.history.CanUndo());

    // Delete the entity out-of-band.
    f.manager.RemoveSubtrees({ uuid });

    const auto revBefore = f.manager.AuthoringRevision();
    auto r = f.history.Undo(f.manager);
    REQUIRE_FALSE(r.success);
    REQUIRE_FALSE(f.history.CanUndo());
    REQUIRE_FALSE(f.history.CanRedo());
    REQUIRE(f.manager.AuthoringRevision() == revBefore);
}

// ---------------------------------------------------------------------------
// No-op suppression: identical before/after records no entry.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3A no-op suppression records no entry")
{
    SceneFixture f;
    const auto uuid = f.AddBox("Box");
    const EditableTRS b0 = f.LocalOf(uuid);

    auto cmd = MakeTransformCommandIfEffective(uuid, b0, b0);
    REQUIRE(!cmd);
    REQUIRE_FALSE(f.history.CanUndo());
    REQUIRE_FALSE(f.history.CanRedo());
}

// ---------------------------------------------------------------------------
// RecordApplied records without re-mutating and still clears redo.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3A RecordApplied records without re-mutating and clears redo")
{
    SceneFixture f;
    const auto uuid = f.AddBox("Box");
    const EditableTRS b0 = f.LocalOf(uuid);

    // Apply a transform directly (simulating a gizmo drag's per-frame edits).
    EditableTRS a1 = b0; a1.translation = {1, 0, 0};
    f.manager.SetLocalTransform(SceneManager::EntityId{ f.manager.FindEntityByUuid(uuid) }, a1);
    const auto revAfterDirect = f.manager.AuthoringRevision();

    EditorMutationResult applied; applied.success = true;
    applied.syncImpact = rt2::core::SyncImpact::Transform;
    applied.affectedEntities.push_back(uuid);
    f.history.RecordApplied(MakeTransformCommandIfEffective(uuid, b0, a1), f.manager, applied);

    REQUIRE(f.history.CanUndo());
    // RecordApplied must not re-mutate: revision unchanged.
    REQUIRE(f.manager.AuthoringRevision() == revAfterDirect);

    // Redo stack should be empty after a successful RecordApplied.
    REQUIRE_FALSE(f.history.CanRedo());

    // Undo restores the before-state.
    REQUIRE(f.history.Undo(f.manager).success);
    REQUIRE(TrsEqualLoose(f.LocalOf(uuid), b0));
}

// ---------------------------------------------------------------------------
// Lock bypass: an entity locked after a transform edit is still restored
// by Undo.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3A Undo bypasses editor locks to restore state")
{
    SceneFixture f;
    const auto uuid = f.AddBox("Box");
    const EditableTRS b0 = f.LocalOf(uuid);
    EditableTRS after = b0; after.translation = {5, 0, 0};

    f.history.Execute(MakeTransformCommandIfEffective(uuid, b0, after), f.manager);

    // Lock the entity out-of-band (simulating the UI's lock toggle).
    EditorSceneState state;
    state.ToggleLocked(uuid);
    REQUIRE(state.IsLocked(uuid));

    // Undo must still restore the before-state; the command layer sits below
    // the lock gate.
    REQUIRE(f.history.Undo(f.manager).success);
    REQUIRE(TrsEqualLoose(f.LocalOf(uuid), b0));
}

// ---------------------------------------------------------------------------
// SetVisibilityCommand via history: Execute / Undo / Redo round trip on a
// mixed-state multi-selection.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3A SetVisibilityCommand round trip via history")
{
    SceneFixture f;
    const auto a = f.AddBox("A");
    const auto b = f.AddBox("B");
    const auto c = f.AddBox("C");

    // Initial: A visible, B hidden, C visible.
    REQUIRE(f.manager.SetVisibilityStates({ {b, false} }).success);
    REQUIRE_FALSE(f.VisibleOf(b));

    // Build a Hide Selection command over {A, B, C} with mixed priors.
    std::vector<std::pair<rt2::core::UUID, bool>> beforeStates = {
        {a, true}, {b, false}, {c, true}
    };
    std::vector<std::pair<rt2::core::UUID, bool>> afterStates = {
        {a, false}, {b, false}, {c, false}
    };
    auto cmd = MakeSetVisibilityCommandIfEffective(beforeStates, afterStates);
    REQUIRE(cmd);

    auto r1 = f.history.Execute(std::move(cmd), f.manager);
    REQUIRE(r1.success);
    REQUIRE(r1.syncImpact == rt2::core::SyncImpact::Structural);
    REQUIRE_FALSE(f.VisibleOf(a));
    REQUIRE_FALSE(f.VisibleOf(b));
    REQUIRE_FALSE(f.VisibleOf(c));

    // Undo restores the mixed-state mix.
    auto r2 = f.history.Undo(f.manager);
    REQUIRE(r2.success);
    REQUIRE(f.VisibleOf(a));
    REQUIRE_FALSE(f.VisibleOf(b));
    REQUIRE(f.VisibleOf(c));

    // Redo re-hides A and C.
    auto r3 = f.history.Redo(f.manager);
    REQUIRE(r3.success);
    REQUIRE_FALSE(f.VisibleOf(a));
    REQUIRE_FALSE(f.VisibleOf(c));
}

// ---------------------------------------------------------------------------
// SetVisibilityCommand no-op suppression: an already-hidden selection stays
// hidden and records no entry.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3A SetVisibilityCommand no-op suppression records nothing")
{
    SceneFixture f;
    const auto a = f.AddBox("A");
    const auto b = f.AddBox("B");

    // Both hidden.
    REQUIRE(f.manager.SetVisibilityStates({ {a, false}, {b, false} }).success);

    // Try to build a Hide command over an already-hidden selection: every
    // after-pair matches its before-pair, so the factory returns null.
    auto cmd = MakeSetVisibilityCommandIfEffective(
        { {a, false}, {b, false} }, { {a, false}, {b, false} });
    REQUIRE(!cmd);
}
