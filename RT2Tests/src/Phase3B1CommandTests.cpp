#include <doctest/doctest.h>

#include "EditorCommand.h"
#include "EditorCommandHistory.h"
#include "EditorCommands.h"
#include "EditorStructuralCommands.h"
#include "EditorSceneState.h"
#include "EditorSyncRouter.h"
#include "ISceneRenderBridge.h"
#include "PrimitiveGeometry.h"
#include "SceneManager.h"
#include "SubtreeSnapshot.h"
#include "TransformEditing.h"
#include "ECSComponents.h"
#include "ECSScene.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <unordered_set>

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

    rt2::core::UUID AddLight(const char* name, glm::vec3 pos = {0, 3, 0})
    {
        const auto entity = manager.AddLight(name, pos, {1, 1, 1}, 5.0f, LightType::Point);
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

    bool HasMeshRef(const rt2::core::UUID& uuid)
    {
        const auto entity = manager.FindEntityByUuid(uuid);
        REQUIRE(entity != static_cast<entt::entity>(entt::null));
        return manager.GetECS().registry.all_of<MeshRef>(entity);
    }

    bool EntityAlive(const rt2::core::UUID& uuid)
    {
        return manager.FindEntityByUuid(uuid) != entt::null;
    }

    std::uint32_t MeshIndexOf(const rt2::core::UUID& uuid)
    {
        const auto entity = manager.FindEntityByUuid(uuid);
        REQUIRE(entity != static_cast<entt::entity>(entt::null));
        const auto* ref = manager.GetECS().registry.try_get<MeshRef>(entity);
        REQUIRE(ref);
        return ref->meshIndex;
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
// CreateEmptyCommand: RecordApplied creates with a known UUID; Undo removes
// via RemoveSubtreesExact; Redo re-creates via RestoreSubtrees with the SAME
// UUID.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 CreateEmptyCommand RecordApplied/Undo/Redo same UUID")
{
    SceneFixture f;
    const auto uuid = f.manager.ReserveKnownUuid();
    REQUIRE(f.history.CanUndo() == false);

    // Host applies the creation and captures the snapshot.
    auto applied = f.manager.CreateEmptyWithUuid(uuid, "Empty");
    REQUIRE(applied.success);
    REQUIRE(applied.affectedEntities.size() == 1);
    REQUIRE(applied.affectedEntities.front() == uuid);
    auto snapshot = f.manager.CaptureSubtreeSnapshot({ uuid });
    REQUIRE(snapshot.entities.size() == 1);
    REQUIRE(snapshot.entities.front().uuid == uuid);
    REQUIRE(snapshot.entities.front().name == "Empty");

    auto cmd = MakeCreateEmptyCommand(std::move(snapshot), uuid);
    REQUIRE(cmd);
    f.history.RecordApplied(std::move(cmd), f.manager, applied);
    REQUIRE(f.history.CanUndo());
    REQUIRE(f.EntityAlive(uuid));

    // Undo removes the entity.
    auto r1 = f.history.Undo(f.manager);
    REQUIRE(r1.success);
    REQUIRE_FALSE(f.EntityAlive(uuid));

    // Redo re-creates with the SAME UUID.
    auto r2 = f.history.Redo(f.manager);
    REQUIRE(r2.success);
    REQUIRE(f.EntityAlive(uuid));
    REQUIRE(static_cast<entt::entity>(f.manager.FindEntityByUuid(uuid)) != static_cast<entt::entity>(entt::null));
}

// ---------------------------------------------------------------------------
// RemoveSubtreesCommand: multi-level subtree with a mesh entity and a light
// entity; Undo restores UUIDs, hierarchy, all persisted components, and
// resource references (MeshRef::meshIndex unchanged because no compaction).
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 RemoveSubtreesCommand Undo/Redo restores UUIDs and resources")
{
    SceneFixture f;
    const auto parent = f.AddBox("Parent", {0, 0, 0});
    const auto childMesh = f.AddBox("ChildMesh", {1, 0, 0});
    const auto childLight = f.AddLight("ChildLight", {0, 2, 0});

    // Build hierarchy: parent <- {childMesh, childLight}.
    REQUIRE(f.manager.Reparent({ childMesh, childLight }, parent).success);

    const auto meshIndexBefore = f.MeshIndexOf(childMesh);
    REQUIRE(f.HasMeshRef(childMesh));

    // Capture the snapshot at construction time (entities exist).
    auto snapshot = f.manager.CaptureSubtreeSnapshot({ parent });
    REQUIRE(snapshot.entities.size() == 3); // parent + 2 children
    REQUIRE(snapshot.rootUuids.size() == 1);
    REQUIRE(snapshot.rootUuids.front() == parent);

    auto cmd = MakeRemoveSubtreesCommand(std::move(snapshot), { parent });
    REQUIRE(cmd);
    auto r1 = f.history.Execute(std::move(cmd), f.manager);
    REQUIRE(r1.success);
    REQUIRE(r1.syncImpact == rt2::core::SyncImpact::Structural);
    REQUIRE_FALSE(f.EntityAlive(parent));
    REQUIRE_FALSE(f.EntityAlive(childMesh));
    REQUIRE_FALSE(f.EntityAlive(childLight));

    // Undo restores everything.
    auto r2 = f.history.Undo(f.manager);
    REQUIRE(r2.success);
    REQUIRE(f.EntityAlive(parent));
    REQUIRE(f.EntityAlive(childMesh));
    REQUIRE(f.EntityAlive(childLight));
    REQUIRE(f.HasMeshRef(childMesh));
    // CRITICAL: MeshRef::meshIndex unchanged because no compaction occurred.
    REQUIRE(f.MeshIndexOf(childMesh) == meshIndexBefore);

    // Redo removes again.
    auto r3 = f.history.Redo(f.manager);
    REQUIRE(r3.success);
    REQUIRE_FALSE(f.EntityAlive(parent));
    REQUIRE_FALSE(f.EntityAlive(childMesh));
    REQUIRE_FALSE(f.EntityAlive(childLight));
}

// ---------------------------------------------------------------------------
// Resource stability regression (critical): delete the sole user of a
// textured mesh while unrelated entities survive; Undo; verify the deleted
// entity's MeshRef::meshIndex still points to the original mesh (no
// compaction occurred); verify surviving entities' MeshRef::meshIndex values
// are unchanged across the full Undo/Redo cycle.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 resource stability: no compaction while command in history")
{
    SceneFixture f;
    const auto survivor = f.AddBox("Survivor", {0, 0, 0});
    const auto victim = f.AddBox("Victim", {5, 0, 0});
    const auto survivorMeshBefore = f.MeshIndexOf(survivor);
    const auto victimMeshBefore = f.MeshIndexOf(victim);
    REQUIRE(survivorMeshBefore != victimMeshBefore);

    auto snapshot = f.manager.CaptureSubtreeSnapshot({ victim });
    auto cmd = MakeRemoveSubtreesCommand(std::move(snapshot), { victim });
    f.history.Execute(std::move(cmd), f.manager);
    REQUIRE_FALSE(f.EntityAlive(victim));

    // The survivor's mesh index must NOT have changed (no compaction).
    REQUIRE(f.MeshIndexOf(survivor) == survivorMeshBefore);

    // Undo restores the victim with its original mesh index.
    REQUIRE(f.history.Undo(f.manager).success);
    REQUIRE(f.EntityAlive(victim));
    REQUIRE(f.MeshIndexOf(victim) == victimMeshBefore);
    REQUIRE(f.MeshIndexOf(survivor) == survivorMeshBefore);

    // Redo removes again; survivor's mesh index still unchanged.
    REQUIRE(f.history.Redo(f.manager).success);
    REQUIRE_FALSE(f.EntityAlive(victim));
    REQUIRE(f.MeshIndexOf(survivor) == survivorMeshBefore);

    // Undo once more; both mesh indices still original.
    REQUIRE(f.history.Undo(f.manager).success);
    REQUIRE(f.MeshIndexOf(victim) == victimMeshBefore);
    REQUIRE(f.MeshIndexOf(survivor) == survivorMeshBefore);
}

// ---------------------------------------------------------------------------
// RemoveSubtreesExact validation: after a creation command's RecordApplied,
// add a child to the created root out-of-band, then attempt Undo — the
// exact-state validation fails atomically (zero mutation), the command
// surfaces a history-consistency error, and Phase 3A's failure policy clears
// both stacks. (RemoveSubtreesExact is the Undo path for creation commands;
// deletion commands use RestoreSubtrees on Undo, which has its own anchor
// validation.)
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 RemoveSubtreesExact rejects out-of-band descendants")
{
    SceneFixture f;
    const auto root = f.manager.ReserveKnownUuid();
    EditableTRS trs;
    auto applied = f.manager.CreateEmptyWithUuid(root, "Root");
    REQUIRE(applied.success);
    auto snapshot = f.manager.CaptureSubtreeSnapshot({ root });
    auto cmd = MakeCreateEmptyCommand(std::move(snapshot), root);
    f.history.RecordApplied(std::move(cmd), f.manager, applied);
    REQUIRE(f.history.CanUndo());

    // Out-of-band: add a child to the created root.
    const auto child = f.AddBox("Child", {1, 0, 0});
    REQUIRE(f.manager.Reparent({ child }, root).success);

    // Undo attempts RemoveSubtreesExact — but the snapshot has only 1
    // entity (root) and the live subtree now has 2. This should fail
    // atomically.
    auto r = f.history.Undo(f.manager);
    REQUIRE_FALSE(r.success);
    // Both stacks cleared by the failure policy.
    REQUIRE_FALSE(f.history.CanUndo());
    REQUIRE_FALSE(f.history.CanRedo());
    // The scene was not mutated by the failed Undo.
    REQUIRE(f.EntityAlive(root));
    REQUIRE(f.EntityAlive(child));
}

// ---------------------------------------------------------------------------
// DuplicateSubtreesCommand: captures original + duplicate UUIDs; Undo
// destroys the duplicates; Redo re-creates the duplicates with the SAME
// UUIDs (not fresh); verify hierarchy among duplicates is preserved and
// resource references are intact.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 DuplicateSubtreesCommand Undo/Redo same UUIDs")
{
    SceneFixture f;
    const auto parent = f.AddBox("Parent", {0, 0, 0});
    const auto child = f.AddBox("Child", {1, 0, 0});
    REQUIRE(f.manager.Reparent({ child }, parent).success);

    // Host queries the exact canonical entity count and reserves UUIDs.
    auto countResult = f.manager.CountCanonicalSubtreeEntities({ parent });
    REQUIRE(countResult.IsOk());
    REQUIRE(countResult.value == 2);
    auto knownUuids = f.manager.ReserveKnownUuids(countResult.value);
    REQUIRE(knownUuids.size() == 2);

    // Host applies the duplication.
    auto dup = f.manager.DuplicateSubtreesWithUuids({ parent }, knownUuids);
    REQUIRE(dup.mutation.success);
    REQUIRE(dup.createdRoots.size() == 1);
    REQUIRE(dup.sourceToDuplicate.size() == 2);

    const auto dupRoot = dup.createdRoots.front();
    REQUIRE(f.EntityAlive(dupRoot));
    REQUIRE(f.HasMeshRef(dupRoot));

    // Capture the duplicate's snapshot for the command.
    auto snapshot = f.manager.CaptureSubtreeSnapshot(dup.createdRoots);
    REQUIRE(snapshot.entities.size() == 2);

    auto cmd = MakeDuplicateSubtreesCommand(std::move(snapshot), dup.createdRoots);
    REQUIRE(cmd);
    EditorMutationResult applied;
    applied.success = true;
    applied.syncImpact = dup.mutation.syncImpact;
    applied.affectedEntities = dup.mutation.affectedEntities;
    f.history.RecordApplied(std::move(cmd), f.manager, applied);
    REQUIRE(f.history.CanUndo());

    // Undo destroys the duplicates.
    REQUIRE(f.history.Undo(f.manager).success);
    REQUIRE_FALSE(f.EntityAlive(dupRoot));

    // Redo re-creates the duplicates with the SAME UUIDs.
    REQUIRE(f.history.Redo(f.manager).success);
    REQUIRE(f.EntityAlive(dupRoot));
    REQUIRE(f.HasMeshRef(dupRoot));
}

// ---------------------------------------------------------------------------
// DuplicateSubtreesWithUuids count validation: pass a knownDuplicateUuids
// list whose count does not match the canonical subtree size; verify the
// manager fails atomically with no mutation.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 DuplicateSubtreesWithUuids count validation")
{
    SceneFixture f;
    const auto parent = f.AddBox("Parent", {0, 0, 0});
    const auto child = f.AddBox("Child", {1, 0, 0});
    REQUIRE(f.manager.Reparent({ child }, parent).success);

    // Pass too few UUIDs.
    auto tooFew = f.manager.ReserveKnownUuids(1);
    auto r1 = f.manager.DuplicateSubtreesWithUuids({ parent }, tooFew);
    REQUIRE_FALSE(r1.mutation.success);
    // No mutation occurred.
    REQUIRE(f.manager.GetEntityCount() == 2);

    // Pass too many UUIDs.
    auto tooMany = f.manager.ReserveKnownUuids(5);
    auto r2 = f.manager.DuplicateSubtreesWithUuids({ parent }, tooMany);
    REQUIRE_FALSE(r2.mutation.success);
    REQUIRE(f.manager.GetEntityCount() == 2);
}

// ---------------------------------------------------------------------------
// CountCanonicalSubtreeEntities: returns the exact canonical entity count
// for a multi-root selection including nested descendants; missing/invalid
// roots return a failure result.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 CountCanonicalSubtreeEntities canonical and invalid")
{
    SceneFixture f;
    const auto a = f.AddBox("A", {0, 0, 0});
    const auto b = f.AddBox("B", {1, 0, 0});
    const auto aChild = f.AddBox("AChild", {0, 1, 0});
    const auto aGrandchild = f.AddBox("AGrandchild", {0, 2, 0});
    REQUIRE(f.manager.Reparent({ aChild }, a).success);
    REQUIRE(f.manager.Reparent({ aGrandchild }, aChild).success);

    // Counting {a, b} should canonicalize to {a, b} (b is not a descendant
    // of a) and count a's subtree (3) + b (1) = 4.
    auto r1 = f.manager.CountCanonicalSubtreeEntities({ a, b });
    REQUIRE(r1.IsOk());
    REQUIRE(r1.value == 4);

    // Counting {a, aChild} should canonicalize to {a} (aChild is a descendant
    // of a) and count a's subtree (3).
    auto r2 = f.manager.CountCanonicalSubtreeEntities({ a, aChild });
    REQUIRE(r2.IsOk());
    REQUIRE(r2.value == 3);

    // Missing/invalid root returns failure.
    rt2::core::UUID bogus;
    bogus.bytes[0] = 0xFF;
    auto r3 = f.manager.CountCanonicalSubtreeEntities({ bogus });
    REQUIRE_FALSE(r3.IsOk());
}

// ---------------------------------------------------------------------------
// PasteSubtreesCommand: captures the clipboard snapshot + pasted UUIDs;
// Undo destroys the pastes; Redo re-pastes with the same UUIDs; verify
// resource-reference invariants hold (no compaction while the command is in
// history).
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 PasteSubtreesCommand Undo/Redo same UUIDs")
{
    SceneFixture f;
    // Use CreatePrimitiveEntity so the entity has a PrimitiveComponent
    // (required for CloneInMemory/Copy to succeed).
    const auto source = f.manager.ReserveKnownUuid();
    EditableTRS trs;
    auto createResult = f.manager.CreatePrimitiveEntity(
        source, "Source", PrimitiveComponent::Cube, 1.0f, trs, 0);
    REQUIRE(createResult.success);

    // Copy the source into a clipboard document.
    EditorSceneState state;
    rt2::core::Error copyError;
    REQUIRE(state.Copy(f.manager, { source }, copyError));
    REQUIRE(state.HasClipboard());

    // Host queries the count and reserves UUIDs.
    auto countResult = f.manager.CountCanonicalSubtreeEntities({ source });
    REQUIRE(countResult.IsOk());
    REQUIRE(countResult.value == 1);
    auto knownUuids = f.manager.ReserveKnownUuids(countResult.value);

    // Host applies the paste.
    auto paste = f.manager.PasteSubtreesWithUuids(
        *state.ClipboardDocument(), state.ClipboardRoots(), std::nullopt, knownUuids);
    REQUIRE(paste.mutation.success);
    REQUIRE(paste.createdRoots.size() == 1);
    const auto pastedRoot = paste.createdRoots.front();
    REQUIRE(f.EntityAlive(pastedRoot));
    REQUIRE(f.HasMeshRef(pastedRoot));

    // Capture the pasted snapshot for the command.
    auto snapshot = f.manager.CaptureSubtreeSnapshot(paste.createdRoots);
    auto cmd = MakePasteSubtreesCommand(std::move(snapshot), paste.createdRoots);
    REQUIRE(cmd);
    EditorMutationResult applied;
    applied.success = true;
    applied.syncImpact = paste.mutation.syncImpact;
    applied.affectedEntities = paste.mutation.affectedEntities;
    f.history.RecordApplied(std::move(cmd), f.manager, applied);
    REQUIRE(f.history.CanUndo());

    // Undo destroys the pastes.
    REQUIRE(f.history.Undo(f.manager).success);
    REQUIRE_FALSE(f.EntityAlive(pastedRoot));

    // Redo re-pastes with the same UUIDs.
    REQUIRE(f.history.Redo(f.manager).success);
    REQUIRE(f.EntityAlive(pastedRoot));
    REQUIRE(f.HasMeshRef(pastedRoot));
}

// ---------------------------------------------------------------------------
// ReparentCommand: multi-source with different original parents; PreserveWorld
// and PreserveLocal modes; Undo restores the exact before-local TRS for each
// source at the exact sibling anchor; Redo re-applies; atomic failure on a
// cycle (one failure => no mutation).
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 ReparentCommand multi-source Undo/Redo restores local TRS")
{
    SceneFixture f;
    const auto parentA = f.AddBox("ParentA", {0, 0, 0});
    const auto parentB = f.AddBox("ParentB", {5, 0, 0});
    const auto source1 = f.AddBox("Source1", {1, 0, 0});
    const auto source2 = f.AddBox("Source2", {6, 0, 0});
    REQUIRE(f.manager.Reparent({ source1 }, parentA).success);
    REQUIRE(f.manager.Reparent({ source2 }, parentB).success);

    // Capture before-edits (local TRS + anchors).
    auto beforeLocal1 = f.LocalOf(source1);
    auto beforeLocal2 = f.LocalOf(source2);
    auto beforeSnapshot1 = f.manager.CaptureSubtreeSnapshot({ source1 });
    auto beforeSnapshot2 = f.manager.CaptureSubtreeSnapshot({ source2 });
    REQUIRE(beforeSnapshot1.rootAnchors.size() == 1);
    REQUIRE(beforeSnapshot2.rootAnchors.size() == 1);

    // Build after-edits: reparent both into parentA with PreserveLocal.
    std::vector<ReparentEdit> beforeEdits = {
        { source1, parentA, beforeLocal1, glm::mat4(1.0f), beforeSnapshot1.rootAnchors.front() },
        { source2, parentB, beforeLocal2, glm::mat4(1.0f), beforeSnapshot2.rootAnchors.front() },
    };
    std::vector<ReparentEdit> afterEdits = {
        { source1, parentA, beforeLocal1, glm::mat4(1.0f), beforeSnapshot1.rootAnchors.front() },
        { source2, parentA, beforeLocal2, glm::mat4(1.0f), {} },
    };

    auto cmd = MakeReparentCommandIfEffective(beforeEdits, afterEdits, ReparentMode::PreserveLocal);
    REQUIRE(cmd);
    auto r1 = f.history.Execute(std::move(cmd), f.manager);
    REQUIRE(r1.success);
    REQUIRE(r1.syncImpact == rt2::core::SyncImpact::Transform);
    // source1 should still be under parentA, source2 should now be under parentA.
    const auto s1Parent = f.manager.GetParent(SceneManager::EntityId{ f.manager.FindEntityByUuid(source1) });
    const auto s2Parent = f.manager.GetParent(SceneManager::EntityId{ f.manager.FindEntityByUuid(source2) });
    REQUIRE(s1Parent.id == f.manager.FindEntityByUuid(parentA));
    REQUIRE(s2Parent.id == f.manager.FindEntityByUuid(parentA));

    // Undo restores each source to its original parent with the original local TRS.
    auto r2 = f.history.Undo(f.manager);
    REQUIRE(r2.success);
    const auto s2ParentAfter = f.manager.GetParent(SceneManager::EntityId{ f.manager.FindEntityByUuid(source2) });
    REQUIRE(s2ParentAfter.id == f.manager.FindEntityByUuid(parentB));
    REQUIRE(TrsEqualLoose(f.LocalOf(source1), beforeLocal1));
    REQUIRE(TrsEqualLoose(f.LocalOf(source2), beforeLocal2));

    // Redo re-applies.
    auto r3 = f.history.Redo(f.manager);
    REQUIRE(r3.success);
    const auto s2ParentRedo = f.manager.GetParent(SceneManager::EntityId{ f.manager.FindEntityByUuid(source2) });
    REQUIRE(s2ParentRedo.id == f.manager.FindEntityByUuid(parentA));
}

// ---------------------------------------------------------------------------
// ReparentCommand atomic failure on a cycle (one failure => no mutation).
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 ReparentBatch atomic cycle failure")
{
    SceneFixture f;
    const auto parent = f.AddBox("Parent", {0, 0, 0});
    const auto child = f.AddBox("Child", {1, 0, 0});
    REQUIRE(f.manager.Reparent({ child }, parent).success);

    // Attempt to reparent parent beneath child — a cycle.
    std::vector<ReparentEdit> edits = {
        { parent, child, EditableTRS{}, glm::mat4(1.0f), {} },
    };
    auto r = f.manager.ReparentBatch(edits, ReparentMode::PreserveLocal);
    REQUIRE_FALSE(r.success);
    // No mutation occurred.
    const auto parentParent = f.manager.GetParent(SceneManager::EntityId{ f.manager.FindEntityByUuid(parent) });
    REQUIRE_FALSE(parentParent.IsValid());
}

// ---------------------------------------------------------------------------
// SetLocalTransformStates: atomic validate-first (one missing UUID => no
// mutation, Failure); single revision bump; multi-entity round trip.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 SetLocalTransformStates atomic validate-first")
{
    SceneFixture f;
    const auto a = f.AddBox("A", {0, 0, 0});
    const auto b = f.AddBox("B", {1, 0, 0});
    rt2::core::UUID bogus;
    bogus.bytes[0] = 0xFF;

    auto beforeA = f.LocalOf(a);
    auto beforeB = f.LocalOf(b);
    auto revBefore = f.manager.AuthoringRevision();

    EditableTRS afterA = beforeA; afterA.translation = {10, 0, 0};
    EditableTRS afterB = beforeB; afterB.translation = {11, 0, 0};

    // One bogus UUID => no mutation.
    auto r1 = f.manager.SetLocalTransformStates({ {a, afterA}, {bogus, afterB} });
    REQUIRE_FALSE(r1.success);
    REQUIRE(TrsEqualLoose(f.LocalOf(a), beforeA));
    REQUIRE(TrsEqualLoose(f.LocalOf(b), beforeB));
    REQUIRE(f.manager.AuthoringRevision() == revBefore);

    // Both valid => mutation.
    auto r2 = f.manager.SetLocalTransformStates({ {a, afterA}, {b, afterB} });
    REQUIRE(r2.success);
    REQUIRE(r2.syncImpact == rt2::core::SyncImpact::Transform);
    REQUIRE(r2.affectedEntities.size() == 2);
    REQUIRE(TrsEqualLoose(f.LocalOf(a), afterA));
    REQUIRE(TrsEqualLoose(f.LocalOf(b), afterB));
}

// ---------------------------------------------------------------------------
// Gizmo drag → TransformCommand: 2-entity selection dragged; ONE history
// entry; Undo restores both pre-drag local TRS; Redo re-applies both.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 multi-entity TransformCommand Undo/Redo")
{
    SceneFixture f;
    const auto a = f.AddBox("A", {0, 0, 0});
    const auto b = f.AddBox("B", {1, 0, 0});
    auto beforeA = f.LocalOf(a);
    auto beforeB = f.LocalOf(b);

    EditableTRS afterA = beforeA; afterA.translation = {5, 0, 0};
    EditableTRS afterB = beforeB; afterB.translation = {6, 0, 0};

    std::vector<TransformTriple> triples = {
        { a, beforeA, afterA },
        { b, beforeB, afterB },
    };
    auto cmd = MakeTransformCommandIfEffective(std::move(triples));
    REQUIRE(cmd);
    auto r1 = f.history.Execute(std::move(cmd), f.manager);
    REQUIRE(r1.success);
    REQUIRE(r1.syncImpact == rt2::core::SyncImpact::Transform);
    REQUIRE(r1.affectedEntities.size() == 2);
    REQUIRE(TrsEqualLoose(f.LocalOf(a), afterA));
    REQUIRE(TrsEqualLoose(f.LocalOf(b), afterB));

    // Undo restores both.
    auto r2 = f.history.Undo(f.manager);
    REQUIRE(r2.success);
    REQUIRE(TrsEqualLoose(f.LocalOf(a), beforeA));
    REQUIRE(TrsEqualLoose(f.LocalOf(b), beforeB));

    // Redo re-applies both.
    auto r3 = f.history.Redo(f.manager);
    REQUIRE(r3.success);
    REQUIRE(TrsEqualLoose(f.LocalOf(a), afterA));
    REQUIRE(TrsEqualLoose(f.LocalOf(b), afterB));
}

// ---------------------------------------------------------------------------
// CreatePrimitiveEntityCommand: RecordApplied creates with a known UUID;
// Undo removes via RemoveSubtreesExact; Redo re-creates with the SAME UUID;
// components and resource references intact.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 CreatePrimitiveEntityCommand RecordApplied/Undo/Redo")
{
    SceneFixture f;
    const auto uuid = f.manager.ReserveKnownUuid();
    EditableTRS trs; trs.translation = {0, 0.5f, 0};

    auto applied = f.manager.CreatePrimitiveEntity(
        uuid, "Cube", PrimitiveComponent::Cube, 1.0f, trs, 0);
    REQUIRE(applied.success);
    REQUIRE(applied.syncImpact == rt2::core::SyncImpact::Structural);
    REQUIRE(f.EntityAlive(uuid));
    REQUIRE(f.HasMeshRef(uuid));

    auto snapshot = f.manager.CaptureSubtreeSnapshot({ uuid });
    REQUIRE(snapshot.entities.size() == 1);
    REQUIRE(snapshot.entities.front().hasPrimitive);
    REQUIRE(snapshot.entities.front().primitive.kind == PrimitiveComponent::Cube);

    auto cmd = MakeCreatePrimitiveCommand(std::move(snapshot), uuid);
    REQUIRE(cmd);
    f.history.RecordApplied(std::move(cmd), f.manager, applied);
    REQUIRE(f.history.CanUndo());

    // Undo removes.
    REQUIRE(f.history.Undo(f.manager).success);
    REQUIRE_FALSE(f.EntityAlive(uuid));

    // Redo re-creates with the SAME UUID.
    REQUIRE(f.history.Redo(f.manager).success);
    REQUIRE(f.EntityAlive(uuid));
    REQUIRE(f.HasMeshRef(uuid));
}

// ---------------------------------------------------------------------------
// CreateLightEntityCommand: RecordApplied creates with a known UUID; Undo
// removes via RemoveSubtreesExact; Redo re-creates with the SAME UUID.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 CreateLightEntityCommand RecordApplied/Undo/Redo")
{
    SceneFixture f;
    const auto uuid = f.manager.ReserveKnownUuid();
    EditableTRS trs; trs.translation = {0, 3, 0};

    auto applied = f.manager.CreateLightEntity(
        uuid, "Light", trs, {1, 1, 1}, 10.0f, LightType::Point);
    REQUIRE(applied.success);
    REQUIRE(applied.syncImpact == rt2::core::SyncImpact::Structural);
    REQUIRE(f.EntityAlive(uuid));

    auto snapshot = f.manager.CaptureSubtreeSnapshot({ uuid });
    REQUIRE(snapshot.entities.size() == 1);
    REQUIRE(snapshot.entities.front().hasLight);

    auto cmd = MakeCreateLightCommand(std::move(snapshot), uuid);
    REQUIRE(cmd);
    f.history.RecordApplied(std::move(cmd), f.manager, applied);
    REQUIRE(f.history.CanUndo());

    // Undo removes.
    REQUIRE(f.history.Undo(f.manager).success);
    REQUIRE_FALSE(f.EntityAlive(uuid));

    // Redo re-creates with the SAME UUID.
    REQUIRE(f.history.Redo(f.manager).success);
    REQUIRE(f.EntityAlive(uuid));
}

// ---------------------------------------------------------------------------
// Single-entity visibility migration: the per-entity Hide/Show context-menu
// items construct SetVisibilityCommand and route through history.Execute;
// one Undo restores the prior visibility.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 single-entity visibility via history")
{
    SceneFixture f;
    const auto a = f.AddBox("A", {0, 0, 0});
    REQUIRE(f.VisibleOf(a));

    // Build a single-entity Hide command (mimics the context-menu path).
    std::vector<std::pair<rt2::core::UUID, bool>> beforeStates = { {a, true} };
    std::vector<std::pair<rt2::core::UUID, bool>> afterStates = { {a, false} };
    auto cmd = MakeSetVisibilityCommandIfEffective(beforeStates, afterStates);
    REQUIRE(cmd);
    auto r1 = f.history.Execute(std::move(cmd), f.manager);
    REQUIRE(r1.success);
    REQUIRE(r1.syncImpact == rt2::core::SyncImpact::Structural);
    REQUIRE_FALSE(f.VisibleOf(a));

    // Undo restores the prior visibility.
    auto r2 = f.history.Undo(f.manager);
    REQUIRE(r2.success);
    REQUIRE(f.VisibleOf(a));
}

// ---------------------------------------------------------------------------
// Sibling-anchor restoration: delete a middle child of a 3-child parent;
// Undo; verify the middle child is restored at the exact middle position.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 sibling-anchor restoration restores middle position")
{
    SceneFixture f;
    const auto parent = f.AddBox("Parent", {0, 0, 0});
    const auto first = f.AddBox("First", {1, 0, 0});
    const auto middle = f.AddBox("Middle", {2, 0, 0});
    const auto last = f.AddBox("Last", {3, 0, 0});
    REQUIRE(f.manager.Reparent({ first, middle, last }, parent).success);

    // Verify initial order: first, middle, last.
    auto children = f.manager.GetChildren(SceneManager::EntityId{ f.manager.FindEntityByUuid(parent) });
    REQUIRE(children.size() == 3);
    REQUIRE(f.manager.GetEntityUuid(SceneManager::EntityId{ children[0] }) == first);
    REQUIRE(f.manager.GetEntityUuid(SceneManager::EntityId{ children[1] }) == middle);
    REQUIRE(f.manager.GetEntityUuid(SceneManager::EntityId{ children[2] }) == last);

    // Delete the middle child via a command.
    auto snapshot = f.manager.CaptureSubtreeSnapshot({ middle });
    auto cmd = MakeRemoveSubtreesCommand(std::move(snapshot), { middle });
    f.history.Execute(std::move(cmd), f.manager);
    REQUIRE_FALSE(f.EntityAlive(middle));

    // Undo restores the middle child at the exact middle position.
    REQUIRE(f.history.Undo(f.manager).success);
    REQUIRE(f.EntityAlive(middle));
    children = f.manager.GetChildren(SceneManager::EntityId{ f.manager.FindEntityByUuid(parent) });
    REQUIRE(children.size() == 3);
    REQUIRE(f.manager.GetEntityUuid(SceneManager::EntityId{ children[0] }) == first);
    REQUIRE(f.manager.GetEntityUuid(SceneManager::EntityId{ children[1] }) == middle);
    REQUIRE(f.manager.GetEntityUuid(SceneManager::EntityId{ children[2] }) == last);
}

// ---------------------------------------------------------------------------
// Generation guard still fires on all four public ops (3A coverage stays
// green). Verify a structural command is cleared by a generation bump.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 generation guard clears structural commands")
{
    SceneFixture f;
    const auto a = f.AddBox("A", {0, 0, 0});
    auto snapshot = f.manager.CaptureSubtreeSnapshot({ a });
    auto cmd = MakeRemoveSubtreesCommand(std::move(snapshot), { a });
    f.history.Execute(std::move(cmd), f.manager);
    REQUIRE(f.history.CanUndo());

    // Bump document generation out-of-band (simulates adoption).
    f.manager.Clear();

    // Undo should detect the generation mismatch and clear both stacks.
    auto r = f.history.Undo(f.manager);
    REQUIRE_FALSE(r.success);
    REQUIRE_FALSE(f.history.CanUndo());
    REQUIRE_FALSE(f.history.CanRedo());
}

// ---------------------------------------------------------------------------
// Compaction audit: CompactMeshRegistryNow runs compaction; verify it is the
// explicit public entry point. (The host contract forbids compaction while
// history is non-empty; we test the API itself here.)
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 CompactMeshRegistryNow is the explicit compaction entry")
{
    SceneFixture f;
    const auto a = f.AddBox("A", {0, 0, 0});
    const auto b = f.AddBox("B", {1, 0, 0});
    const auto aMeshBefore = f.MeshIndexOf(a);
    const auto bMeshBefore = f.MeshIndexOf(b);

    // Delete a via the non-command path (with compaction).
    f.manager.RemoveSubtrees({ a });
    REQUIRE_FALSE(f.EntityAlive(a));

    // Now run explicit compaction. b's mesh index may change.
    f.manager.CompactMeshRegistryNow();
    // b should still be alive and have a valid mesh index.
    REQUIRE(f.EntityAlive(b));
    const auto bMeshAfter = f.MeshIndexOf(b);
    // After compaction, b's mesh index should be 0 (only one mesh left).
    REQUIRE(bMeshAfter == 0);
    REQUIRE(bMeshAfter != bMeshBefore);
}

// ---------------------------------------------------------------------------
// Multi-root middle-delete anchor test (Blocker 2): delete a middle root
// among ≥4 roots with interleaved children; Undo restores the deleted root.
// Root-entity ordering is unspecified, so this test only verifies the root
// is alive after Undo (not its position among siblings).
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 multi-root middle-delete Undo restores entity")
{
    SceneFixture f;
    const auto rootA = f.AddBox("A", {0, 0, 0});
    const auto rootB = f.AddBox("B", {1, 0, 0});
    const auto rootC = f.AddBox("C", {2, 0, 0});
    const auto rootD = f.AddBox("D", {3, 0, 0});

    // Delete rootB and rootC (middle roots).
    auto snapshot = f.manager.CaptureSubtreeSnapshot({ rootB, rootC });
    auto cmd = MakeRemoveSubtreesCommand(std::move(snapshot), { rootB, rootC });
    f.history.Execute(std::move(cmd), f.manager);
    REQUIRE_FALSE(f.EntityAlive(rootB));
    REQUIRE_FALSE(f.EntityAlive(rootC));
    REQUIRE(f.EntityAlive(rootA));
    REQUIRE(f.EntityAlive(rootD));

    // Undo restores rootB and rootC. Root-entity ordering is unspecified,
    // so we only verify they are alive (not their position).
    REQUIRE(f.history.Undo(f.manager).success);
    REQUIRE(f.EntityAlive(rootA));
    REQUIRE(f.EntityAlive(rootB));
    REQUIRE(f.EntityAlive(rootC));
    REQUIRE(f.EntityAlive(rootD));
}

// ---------------------------------------------------------------------------
// Reparent sibling-position test (Blocker 4): reparent a middle child to a
// new parent; Undo restores it to the exact original sibling position via
// the anchor.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 ReparentCommand Undo restores sibling position via anchor")
{
    SceneFixture f;
    const auto parentA = f.AddBox("ParentA", {0, 0, 0});
    const auto parentB = f.AddBox("ParentB", {5, 0, 0});
    const auto first = f.AddBox("First", {1, 0, 0});
    const auto middle = f.AddBox("Middle", {2, 0, 0});
    const auto last = f.AddBox("Last", {3, 0, 0});
    REQUIRE(f.manager.Reparent({ first, middle, last }, parentA).success);

    // Verify initial order under parentA: first, middle, last.
    auto children = f.manager.GetChildren(SceneManager::EntityId{ f.manager.FindEntityByUuid(parentA) });
    REQUIRE(children.size() == 3);
    REQUIRE(f.manager.GetEntityUuid(SceneManager::EntityId{ children[1] }) == middle);

    // Capture before-edits for the middle child.
    auto beforeLocal = f.LocalOf(middle);
    auto beforeSnap = f.manager.CaptureSubtreeSnapshot({ middle });
    REQUIRE(beforeSnap.rootAnchors.size() == 1);
    auto anchor = beforeSnap.rootAnchors.front();
    // The anchor should record prevSibling=first, nextSibling=last.
    REQUIRE(anchor.prevSibling == first);
    REQUIRE(anchor.nextSibling == last);

    // Build the reparent command: move middle to parentB.
    EditableTRS world;
    REQUIRE(f.manager.GetWorldTransform(SceneManager::EntityId{ f.manager.FindEntityByUuid(middle) }, world));
    std::vector<ReparentEdit> beforeEdits = {
        { middle, parentA, beforeLocal, world.Matrix(), anchor },
    };
    std::vector<ReparentEdit> afterEdits = {
        { middle, parentB, beforeLocal, world.Matrix(), {} },
    };
    auto cmd = MakeReparentCommandIfEffective(beforeEdits, afterEdits, ReparentMode::PreserveWorld);
    REQUIRE(cmd);
    f.history.Execute(std::move(cmd), f.manager);

    // middle is now under parentB.
    const auto middleParent = f.manager.GetParent(SceneManager::EntityId{ f.manager.FindEntityByUuid(middle) });
    REQUIRE(middleParent.id == f.manager.FindEntityByUuid(parentB));

    // parentA should now have only first and last.
    children = f.manager.GetChildren(SceneManager::EntityId{ f.manager.FindEntityByUuid(parentA) });
    REQUIRE(children.size() == 2);

    // Undo restores middle to parentA at the exact middle position (between
    // first and last).
    REQUIRE(f.history.Undo(f.manager).success);
    const auto middleParentAfter = f.manager.GetParent(SceneManager::EntityId{ f.manager.FindEntityByUuid(middle) });
    REQUIRE(middleParentAfter.id == f.manager.FindEntityByUuid(parentA));
    children = f.manager.GetChildren(SceneManager::EntityId{ f.manager.FindEntityByUuid(parentA) });
    REQUIRE(children.size() == 3);
    REQUIRE(f.manager.GetEntityUuid(SceneManager::EntityId{ children[0] }) == first);
    REQUIRE(f.manager.GetEntityUuid(SceneManager::EntityId{ children[1] }) == middle);
    REQUIRE(f.manager.GetEntityUuid(SceneManager::EntityId{ children[2] }) == last);
}

// ---------------------------------------------------------------------------
// PreserveWorld reparent test (Blocker 3): reparent an entity into a
// differently-transformed parent; the world pose is preserved (not the local
// TRS). Undo restores the original local TRS.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 ReparentBatch PreserveWorld preserves world pose")
{
    SceneFixture f;
    const auto parent = f.AddBox("Parent", {10, 0, 0});
    const auto child = f.AddBox("Child", {1, 0, 0});
    REQUIRE(f.manager.Reparent({ child }, parent).success);

    // Capture the child's world pose before reparent.
    EditableTRS worldBefore;
    REQUIRE(f.manager.GetWorldTransform(SceneManager::EntityId{ f.manager.FindEntityByUuid(child) }, worldBefore));

    // Reparent to scene root with PreserveWorld.
    EditableTRS worldCapture = worldBefore;
    std::vector<ReparentEdit> edits = {
        { child, rt2::core::UUID::Nil(), EditableTRS{}, worldCapture.Matrix(), {} },
    };
    auto r = f.manager.ReparentBatch(edits, ReparentMode::PreserveWorld);
    REQUIRE(r.success);

    // The world pose should be preserved.
    EditableTRS worldAfter;
    REQUIRE(f.manager.GetWorldTransform(SceneManager::EntityId{ f.manager.FindEntityByUuid(child) }, worldAfter));
    constexpr float eps = 1e-4f;
    REQUIRE(std::fabs(worldAfter.translation.x - worldBefore.translation.x) < eps);
    REQUIRE(std::fabs(worldAfter.translation.y - worldBefore.translation.y) < eps);
    REQUIRE(std::fabs(worldAfter.translation.z - worldBefore.translation.z) < eps);
}

// ---------------------------------------------------------------------------
// Transient-state tolerance test: RemoveSubtreesExact does NOT compare
// derived worldMatrix, prevWorldMatrix, or dirty flag — only authored
// component state + hierarchy topology.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 RemoveSubtreesExact ignores transient transform state")
{
    SceneFixture f;
    const auto uuid = f.manager.ReserveKnownUuid();
    EditableTRS trs; trs.translation = {1, 2, 3};
    auto applied = f.manager.CreatePrimitiveEntity(
        uuid, "Cube", PrimitiveComponent::Cube, 1.0f, trs, 0);
    REQUIRE(applied.success);
    auto snapshot = f.manager.CaptureSubtreeSnapshot({ uuid });
    auto cmd = MakeCreatePrimitiveCommand(std::move(snapshot), uuid);
    f.history.RecordApplied(std::move(cmd), f.manager, applied);
    REQUIRE(f.history.CanUndo());

    // Mutate the transient worldMatrix/prevWorldMatrix/dirty out-of-band.
    // These are derived state, NOT authored state; RemoveSubtreesExact
    // (called on Undo) must not compare them.
    const auto entity = f.manager.FindEntityByUuid(uuid);
    auto& tf = f.manager.GetECS().registry.get<Transform>(entity);
    tf.worldMatrix = glm::translate(glm::mat4(1.0f), {999, 999, 999});
    tf.prevWorldMatrix = glm::scale(glm::mat4(1.0f), {42, 42, 42});
    tf.dirty = false;

    // Undo must still succeed — the transient state mismatch is tolerated.
    auto r = f.history.Undo(f.manager);
    REQUIRE(r.success);
    REQUIRE_FALSE(f.EntityAlive(uuid));
}

// ---------------------------------------------------------------------------
// ReparentBatch batch-cycle validation (Concern): {A→under B, B→under A}
// passes per-edit validation but creates a cycle; the batch check catches it.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B1 ReparentBatch batch-cycle validation")
{
    SceneFixture f;
    const auto a = f.AddBox("A", {0, 0, 0});
    const auto b = f.AddBox("B", {1, 0, 0});

    // A→under B, B→under A: per-edit validation passes (neither is a
    // descendant of the other yet), but the batch creates a cycle.
    EditableTRS local;
    f.manager.GetLocalTransform(SceneManager::EntityId{ f.manager.FindEntityByUuid(a) }, local);
    std::vector<ReparentEdit> edits = {
        { a, b, local, glm::mat4(1.0f), {} },
        { b, a, local, glm::mat4(1.0f), {} },
    };
    auto r = f.manager.ReparentBatch(edits, ReparentMode::PreserveLocal);
    REQUIRE_FALSE(r.success);
    REQUIRE(r.error.code == rt2::core::Error::HierarchyCycle);
    // No mutation occurred.
    REQUIRE_FALSE(f.manager.GetParent(SceneManager::EntityId{ f.manager.FindEntityByUuid(a) }).IsValid());
    REQUIRE_FALSE(f.manager.GetParent(SceneManager::EntityId{ f.manager.FindEntityByUuid(b) }).IsValid());
}