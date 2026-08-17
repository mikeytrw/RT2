#include <doctest/doctest.h>

#include "EditorCommand.h"
#include "EditorCommandHistory.h"
#include "EditorCommands.h"
#include "EditorPropertyCommands.h"
#include "EditorSceneState.h"
#include "EditorSyncRouter.h"
#include "ISceneRenderBridge.h"
#include "PrimitiveGeometry.h"
#include "PropertyEditSession.h"
#include "SceneManager.h"
#include "TransformEditing.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "core/UUID.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <optional>
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

    rt2::core::UUID AddCamera(const char* name = "Camera", glm::vec3 pos = {0, 0, 5})
    {
        const auto entity = manager.AddObjectWithGeometry(
            name, PrimitiveGeometry::CreateCube(0.1f), pos, {}, 1.0f, 0);
        manager.GetECS().registry.emplace<CameraComponent>(entity.id, CameraComponent{});
        return manager.GetEntityUuid(entity);
    }

    // Mark an entity as imported (attaches ImportedMeshSourceComponent) so
    // the material-override side effects fire. Uses a synthetic but valid
    // AssetReference.
    void MarkImported(const rt2::core::UUID& uuid)
    {
        const auto e = manager.FindEntityByUuid(uuid);
        REQUIRE(e != static_cast<entt::entity>(entt::null));
        ImportedMeshSourceComponent src;
        src.model.kind = AssetKind::Model;
        src.model.path = "test.gltf";
        src.model.sourceKey = "gltf:mesh=0:primitive=0";
        manager.GetECS().registry.emplace_or_replace<ImportedMeshSourceComponent>(e, src);
    }

    bool EntityAlive(const rt2::core::UUID& uuid)
    {
        return manager.FindEntityByUuid(uuid) != static_cast<entt::entity>(entt::null);
    }

    bool HasMotion(const rt2::core::UUID& uuid)
    {
        const auto e = manager.FindEntityByUuid(uuid);
        if (e == static_cast<entt::entity>(entt::null)) return false;
        return manager.GetECS().registry.all_of<MotionComponent>(e);
    }

    MotionComponent GetMotion(const rt2::core::UUID& uuid)
    {
        const auto e = manager.FindEntityByUuid(uuid);
        REQUIRE(e != static_cast<entt::entity>(entt::null));
        return manager.GetECS().registry.get<MotionComponent>(e);
    }

    std::string NameOf(const rt2::core::UUID& uuid)
    {
        const auto e = manager.FindEntityByUuid(uuid);
        if (e == static_cast<entt::entity>(entt::null)) return std::string{};
        return manager.GetEntityName(SceneManager::EntityId{ e });
    }

    LightComponent GetLight(const rt2::core::UUID& uuid)
    {
        const auto e = manager.FindEntityByUuid(uuid);
        REQUIRE(e != static_cast<entt::entity>(entt::null));
        return *manager.GetECS().registry.try_get<LightComponent>(e);
    }

    CameraComponent GetCamera(const rt2::core::UUID& uuid)
    {
        const auto e = manager.FindEntityByUuid(uuid);
        REQUIRE(e != static_cast<entt::entity>(entt::null));
        return *manager.GetECS().registry.try_get<CameraComponent>(e);
    }

    int MaterialIndexOf(const rt2::core::UUID& uuid)
    {
        const auto e = manager.FindEntityByUuid(uuid);
        REQUIRE(e != static_cast<entt::entity>(entt::null));
        return manager.GetECS().registry.try_get<MeshRef>(e)->materialIndex;
    }

    std::optional<MaterialOverrideComponent> OverrideOf(const rt2::core::UUID& uuid)
    {
        return manager.GetMaterialOverride(uuid);
    }

    bool OverrideAbsent(const rt2::core::UUID& uuid)
    {
        return !manager.GetMaterialOverride(uuid).has_value();
    }

    SceneMaterial MaterialAt(int idx)
    {
        return manager.GetMaterial(idx);
    }
};

bool LightEq(const LightComponent& a, const LightComponent& b)
{
    constexpr float eps = 1e-5f;
    auto vEq = [eps](const glm::vec3& x, const glm::vec3& y) {
        return std::fabs(x.x - y.x) <= eps && std::fabs(x.y - y.y) <= eps && std::fabs(x.z - y.z) <= eps;
    };
    // Cone angles are part of the component and are compared by the
    // production LightEqual, so they belong here too: without them a command
    // that silently dropped the spot cone would satisfy every assertion.
    return vEq(a.color, b.color) && std::fabs(a.intensity - b.intensity) <= eps &&
           std::fabs(a.range - b.range) <= eps &&
           std::fabs(a.innerConeAngle - b.innerConeAngle) <= eps &&
           std::fabs(a.outerConeAngle - b.outerConeAngle) <= eps &&
           a.type == b.type;
}

bool CameraEq(const CameraComponent& a, const CameraComponent& b)
{
    constexpr float eps = 1e-5f;
    return std::fabs(a.verticalFOV - b.verticalFOV) <= eps &&
           std::fabs(a.aperture - b.aperture) <= eps &&
           std::fabs(a.focusDistance - b.focusDistance) <= eps;
}

bool MotionEq(const MotionComponent& a, const MotionComponent& b)
{
    constexpr float eps = 1e-5f;
    return std::fabs(a.linearVelocity.x - b.linearVelocity.x) <= eps &&
           std::fabs(a.linearVelocity.y - b.linearVelocity.y) <= eps &&
           std::fabs(a.linearVelocity.z - b.linearVelocity.z) <= eps;
}

bool TrsEq(const EditableTRS& a, const EditableTRS& b)
{
    constexpr float eps = 1e-5f;
    auto vEq = [eps](const glm::vec3& x, const glm::vec3& y) {
        return std::fabs(x.x - y.x) <= eps && std::fabs(x.y - y.y) <= eps && std::fabs(x.z - y.z) <= eps;
    };
    auto qEq = [eps](const glm::quat& x, const glm::quat& y) {
        glm::quat a2 = x; if (a2.w < 0) a2 = -a2;
        glm::quat b2 = y; if (b2.w < 0) b2 = -b2;
        return std::fabs(a2.x - b2.x) <= eps && std::fabs(a2.y - b2.y) <= eps &&
               std::fabs(a2.z - b2.z) <= eps && std::fabs(a2.w - b2.w) <= eps;
    };
    return vEq(a.translation, b.translation) && qEq(a.rotation, b.rotation) && vEq(a.scale, b.scale);
}

bool MaterialEq(const SceneMaterial& a, const SceneMaterial& b)
{
    constexpr float eps = 1e-5f;
    auto vEq = [eps](const glm::vec3& x, const glm::vec3& y) {
        return std::fabs(x.x - y.x) <= eps && std::fabs(x.y - y.y) <= eps && std::fabs(x.z - y.z) <= eps;
    };
    return a.type == b.type && vEq(a.baseColor, b.baseColor) &&
           std::fabs(a.metallic - b.metallic) <= eps &&
           std::fabs(a.roughness - b.roughness) <= eps &&
           std::fabs(a.ior - b.ior) <= eps &&
           vEq(a.emissiveColor, b.emissiveColor) &&
           std::fabs(a.emissiveIntensity - b.emissiveIntensity) <= eps;
}

} // namespace

// ---------------------------------------------------------------------------
// SetNameCommand: Execute/Undo/Redo restores name; no-op suppression for
// identical names.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B2 SetNameCommand Execute/Undo/Redo restores name")
{
    SceneFixture f;
    const auto e = f.AddBox("Cube");
    REQUIRE(f.NameOf(e) == "Cube");

    auto cmd = MakeSetNameCommandIfEffective(e, "Cube", "Box");
    REQUIRE(cmd);
    auto r = f.history.Execute(std::move(cmd), f.manager);
    REQUIRE(r.success);
    REQUIRE(f.NameOf(e) == "Box");

    auto r1 = f.history.Undo(f.manager);
    REQUIRE(r1.success);
    REQUIRE(f.NameOf(e) == "Cube");

    auto r2 = f.history.Redo(f.manager);
    REQUIRE(r2.success);
    REQUIRE(f.NameOf(e) == "Box");
}

TEST_CASE("Phase 3B2 SetNameCommand no-op suppression for identical names")
{
    SceneFixture f;
    const auto e = f.AddBox("Cube");
    auto cmd = MakeSetNameCommandIfEffective(e, "Cube", "Cube");
    REQUIRE(!cmd);
}

TEST_CASE("Phase 3B2 SetNameCommand missing entity fails gracefully")
{
    SceneFixture f;
    rt2::core::UUID dead;
    auto cmd = MakeSetNameCommandIfEffective(dead, "A", "B");
    REQUIRE(cmd);
    auto r = f.history.Execute(std::move(cmd), f.manager);
    REQUIRE_FALSE(r.success);
    REQUIRE(r.error.code == rt2::core::Error::InvalidEntity);
}

// ---------------------------------------------------------------------------
// SetMaterialIndexCommand: Execute/Undo/Redo restores slot index; Material
// impact; resource stability (slot index unchanged because no compaction).
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B2 SetMaterialIndexCommand Execute/Undo/Redo restores index")
{
    SceneFixture f;
    f.manager.AddMaterial(SceneMaterial{}); // slot 1
    f.manager.AddMaterial(SceneMaterial{}); // slot 2
    const auto e = f.AddBox("Box");
    REQUIRE(f.MaterialIndexOf(e) == 0);

    const auto beforeOv = f.OverrideOf(e);
    auto cmd = MakeSetMaterialIndexCommandIfEffective(e, 0, 2, beforeOv, beforeOv);
    REQUIRE(cmd);
    auto r = f.history.Execute(std::move(cmd), f.manager);
    REQUIRE(r.success);
    REQUIRE(r.syncImpact == rt2::core::SyncImpact::Material);
    REQUIRE(f.MaterialIndexOf(e) == 2);

    auto r1 = f.history.Undo(f.manager);
    REQUIRE(r1.success);
    REQUIRE(f.MaterialIndexOf(e) == 0);

    auto r2 = f.history.Redo(f.manager);
    REQUIRE(r2.success);
    REQUIRE(f.MaterialIndexOf(e) == 2);
}

TEST_CASE("Phase 3B2 SetMaterialIndexCommand no-op suppression for identical index")
{
    SceneFixture f;
    const auto e = f.AddBox("Box");
    const auto ov = f.OverrideOf(e);
    auto cmd = MakeSetMaterialIndexCommandIfEffective(e, 0, 0, ov, ov);
    REQUIRE(!cmd);
}

// ---------------------------------------------------------------------------
// SetMaterialIndexCommand override restore: assign material on an imported
// entity → Undo → verify the MaterialOverrideComponent matches (or is absent
// as) before.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B2 SetMaterialIndexCommand override restore on imported entity")
{
    SceneFixture f;
    const auto uuid = f.AddBox("Imported");
    f.MarkImported(uuid);
    REQUIRE(!f.OverrideOf(uuid).has_value());

    f.manager.AddMaterial(SceneMaterial{}); // slot 1
    const auto beforeOv = f.OverrideOf(uuid); // nullopt
    // Apply the state API to capture the after-override the manager would
    // record (RecordMaterialOverride side effect).
    auto applyResult = f.manager.SetMaterialIndexState(uuid, 1);
    REQUIRE(applyResult.success);
    const auto afterOv = f.OverrideOf(uuid);
    REQUIRE(afterOv.has_value());

    // Build the command with the captured before/after overrides and record
    // it via RecordApplied (the mutation is already applied).
    auto cmd = MakeSetMaterialIndexCommandIfEffective(uuid, 0, 1, beforeOv, afterOv);
    REQUIRE(cmd);
    f.history.RecordApplied(std::move(cmd), f.manager, applyResult);
    REQUIRE(f.history.CanUndo());
    REQUIRE(f.MaterialIndexOf(uuid) == 1);
    REQUIRE(f.OverrideOf(uuid).has_value());

    // Undo restores index 0 AND removes the override (before was nullopt).
    auto r1 = f.history.Undo(f.manager);
    REQUIRE(r1.success);
    REQUIRE(f.MaterialIndexOf(uuid) == 0);
    REQUIRE(!f.OverrideOf(uuid).has_value());

    // Redo re-applies index 1 AND installs the after-override.
    auto r2 = f.history.Redo(f.manager);
    REQUIRE(r2.success);
    REQUIRE(f.MaterialIndexOf(uuid) == 1);
    REQUIRE(f.OverrideOf(uuid).has_value());
}

// ---------------------------------------------------------------------------
// SetMaterialPropertiesCommand: Execute/Undo/Redo restores full material
// value; Material impact; slot-keyed (not entity-keyed).
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B2 SetMaterialPropertiesCommand Execute/Undo/Redo restores material")
{
    SceneFixture f;
    SceneMaterial before = f.MaterialAt(0);
    SceneMaterial after = before;
    after.metallic = 0.75f;
    after.roughness = 0.25f;

    auto cmd = MakeSetMaterialPropertiesCommandIfEffective(0, before, after, {}, {});
    REQUIRE(cmd);
    auto r = f.history.Execute(std::move(cmd), f.manager);
    REQUIRE(r.success);
    REQUIRE(r.syncImpact == rt2::core::SyncImpact::Material);
    REQUIRE(MaterialEq(f.MaterialAt(0), after));

    auto r1 = f.history.Undo(f.manager);
    REQUIRE(r1.success);
    REQUIRE(MaterialEq(f.MaterialAt(0), before));

    auto r2 = f.history.Redo(f.manager);
    REQUIRE(r2.success);
    REQUIRE(MaterialEq(f.MaterialAt(0), after));
}

TEST_CASE("Phase 3B2 SetMaterialPropertiesCommand no-op suppression for equal materials")
{
    SceneFixture f;
    SceneMaterial m = f.MaterialAt(0);
    auto cmd = MakeSetMaterialPropertiesCommandIfEffective(0, m, m, {}, {});
    REQUIRE(!cmd);
}

// ---------------------------------------------------------------------------
// SetMaterialPropertiesCommand override restore: edit a shared slot → Undo
// → verify per-entity overrides of all imported entities referencing the slot
// are restored.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B2 SetMaterialPropertiesCommand override restore on imported entities")
{
    SceneFixture f;
    const auto a = f.AddBox("A");
    const auto b = f.AddBox("B");
    f.MarkImported(a);
    f.MarkImported(b);

    SceneMaterial before = f.MaterialAt(0);
    SceneMaterial after = before;
    after.metallic = 0.5f;

    // Phase 8 W3 S6-B: complete durable UUID + optional fan-out snapshots
    // (S5 shape, explicit nullopt = absent). Before is captured live; the
    // canonical after fan-out comes from the validate-only StageMaterialSlot
    // seam, so the command performs the first write inside Execute.
    const auto collectLive = [&]() {
        SetMaterialPropertiesCommand::OverrideList out;
        auto& reg = f.manager.GetECS().registry;
        for (auto e : reg.view<ImportedMeshSourceComponent>())
        {
            const auto* ref = reg.try_get<MeshRef>(e);
            if (!ref || ref->materialIndex != 0) continue;
            const auto* idc = reg.try_get<EntityIdComponent>(e);
            if (!idc) continue;
            const auto* ov = reg.try_get<MaterialOverrideComponent>(e);
            out.emplace_back(idc->id,
                ov ? std::optional<MaterialOverrideComponent>(*ov) : std::nullopt);
        }
        return out;
    };

    auto beforeOvs = collectLive();
    const auto staged = f.manager.StageMaterialSlot(0, after);
    REQUIRE(staged.IsOk());
    REQUIRE(staged.value.afterOverrides.size() == beforeOvs.size());

    auto cmd = MakeSetMaterialPropertiesCommandIfEffective(0, before, after,
        std::move(beforeOvs), staged.value.afterOverrides);
    REQUIRE(cmd);
    auto r = f.history.Execute(std::move(cmd), f.manager);
    REQUIRE(r.success);
    REQUIRE(f.OverrideOf(a).has_value());
    REQUIRE(f.OverrideOf(b).has_value());

    auto r1 = f.history.Undo(f.manager);
    REQUIRE(r1.success);
    REQUIRE(!f.OverrideOf(a).has_value());
    REQUIRE(!f.OverrideOf(b).has_value());
    REQUIRE(MaterialEq(f.MaterialAt(0), before));
}

// ---------------------------------------------------------------------------
// SetLightCommand: Execute/Undo/Redo restores color/intensity/type (full
// LightComponent); Material impact.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B2 SetLightCommand Execute/Undo/Redo restores light")
{
    SceneFixture f;
    const auto e = f.AddLight("Light");
    LightComponent before = f.GetLight(e);
    LightComponent after = before;
    after.color = {0.2f, 0.4f, 0.6f};
    after.intensity = 42.0f;
    after.type = LightType::Spot;

    auto cmd = MakeSetLightCommandIfEffective(e, before, after);
    REQUIRE(cmd);
    auto r = f.history.Execute(std::move(cmd), f.manager);
    REQUIRE(r.success);
    REQUIRE(r.syncImpact == rt2::core::SyncImpact::Material);
    REQUIRE(LightEq(f.GetLight(e), after));

    auto r1 = f.history.Undo(f.manager);
    REQUIRE(r1.success);
    REQUIRE(LightEq(f.GetLight(e), before));

    auto r2 = f.history.Redo(f.manager);
    REQUIRE(r2.success);
    REQUIRE(LightEq(f.GetLight(e), after));
}

// The Inspector gained Range and the spot cone angles, which reach the
// command through the same whole-component path. Covered separately because
// the case above only moves colour, intensity and type.
TEST_CASE("Phase 3B2 SetLightCommand carries range and spot cone angles")
{
    SceneFixture f;
    const auto e = f.AddLight("Spot");
    LightComponent before = f.GetLight(e);
    LightComponent after = before;
    after.type = LightType::Spot;
    after.range = 12.5f;
    after.innerConeAngle = 18.0f;
    after.outerConeAngle = 47.5f;

    auto cmd = MakeSetLightCommandIfEffective(e, before, after);
    REQUIRE(cmd);
    auto r = f.history.Execute(std::move(cmd), f.manager);
    REQUIRE(r.success);
    REQUIRE(LightEq(f.GetLight(e), after));
    CHECK(f.GetLight(e).innerConeAngle == doctest::Approx(18.0f));
    CHECK(f.GetLight(e).outerConeAngle == doctest::Approx(47.5f));
    CHECK(f.GetLight(e).range == doctest::Approx(12.5f));

    REQUIRE(f.history.Undo(f.manager).success);
    REQUIRE(LightEq(f.GetLight(e), before));
    REQUIRE(f.history.Redo(f.manager).success);
    REQUIRE(LightEq(f.GetLight(e), after));
}

// A cone-only edit must still produce a command. It would not if the no-op
// check ignored the angles, and the Inspector would then appear to accept a
// cone change that never reached the scene.
TEST_CASE("Phase 3B2 SetLightCommand is not suppressed for a cone-only change")
{
    SceneFixture f;
    const auto e = f.AddLight("Spot");
    LightComponent before = f.GetLight(e);

    LightComponent innerOnly = before;
    innerOnly.innerConeAngle = before.innerConeAngle + 5.0f;
    CHECK(MakeSetLightCommandIfEffective(e, before, innerOnly));

    LightComponent outerOnly = before;
    outerOnly.outerConeAngle = before.outerConeAngle + 5.0f;
    CHECK(MakeSetLightCommandIfEffective(e, before, outerOnly));

    LightComponent rangeOnly = before;
    rangeOnly.range = before.range + 5.0f;
    CHECK(MakeSetLightCommandIfEffective(e, before, rangeOnly));
}

TEST_CASE("Phase 3B2 SetLightCommand no-op suppression for equal values")
{
    SceneFixture f;
    const auto e = f.AddLight("Light");
    LightComponent v = f.GetLight(e);
    auto cmd = MakeSetLightCommandIfEffective(e, v, v);
    REQUIRE(!cmd);
}

// ---------------------------------------------------------------------------
// SetCameraCommand: Execute/Undo/Redo restores FOV/aperture/focusDistance;
// None impact.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B2 SetCameraCommand Execute/Undo/Redo restores camera")
{
    SceneFixture f;
    const auto e = f.AddCamera("Cam");
    CameraComponent before = f.GetCamera(e);
    CameraComponent after = before;
    after.verticalFOV = 90.0f;
    after.aperture = 0.5f;
    after.focusDistance = 10.0f;

    auto cmd = MakeSetCameraCommandIfEffective(e, before, after);
    REQUIRE(cmd);
    auto r = f.history.Execute(std::move(cmd), f.manager);
    REQUIRE(r.success);
    REQUIRE(r.syncImpact == rt2::core::SyncImpact::None);
    REQUIRE(CameraEq(f.GetCamera(e), after));

    auto r1 = f.history.Undo(f.manager);
    REQUIRE(r1.success);
    REQUIRE(CameraEq(f.GetCamera(e), before));

    auto r2 = f.history.Redo(f.manager);
    REQUIRE(r2.success);
    REQUIRE(CameraEq(f.GetCamera(e), after));
}

TEST_CASE("Phase 3B2 SetCameraCommand no-op suppression for equal values")
{
    SceneFixture f;
    const auto e = f.AddCamera("Cam");
    CameraComponent v = f.GetCamera(e);
    auto cmd = MakeSetCameraCommandIfEffective(e, v, v);
    REQUIRE(!cmd);
}

// ---------------------------------------------------------------------------
// SetMotionCommand: Add/Remove round trip; velocity edit round trip; Undo
// restores before-state. All three use cases via one command class.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B2 SetMotionCommand Add/Remove/velocity-edit round trips")
{
    SceneFixture f;
    const auto e = f.AddBox("Box");
    REQUIRE_FALSE(f.HasMotion(e));

    // Add: {nullopt, some}
    MotionComponent afterAdd; afterAdd.linearVelocity = {1, 2, 3};
    auto cmdAdd = MakeSetMotionCommandIfEffective(e, std::nullopt, afterAdd);
    REQUIRE(cmdAdd);
    auto r0 = f.history.Execute(std::move(cmdAdd), f.manager);
    REQUIRE(r0.success);
    REQUIRE(f.HasMotion(e));
    REQUIRE(MotionEq(f.GetMotion(e), afterAdd));

    // Velocity edit: {some, some}
    MotionComponent beforeEdit = afterAdd;
    MotionComponent afterEdit; afterEdit.linearVelocity = {4, 5, 6};
    auto cmdEdit = MakeSetMotionCommandIfEffective(e, beforeEdit, afterEdit);
    REQUIRE(cmdEdit);
    auto r1 = f.history.Execute(std::move(cmdEdit), f.manager);
    REQUIRE(r1.success);
    REQUIRE(MotionEq(f.GetMotion(e), afterEdit));

    // Undo velocity edit
    auto r2 = f.history.Undo(f.manager);
    REQUIRE(r2.success);
    REQUIRE(MotionEq(f.GetMotion(e), beforeEdit));

    // Remove: {some, nullopt}
    auto cmdRem = MakeSetMotionCommandIfEffective(e, beforeEdit, std::nullopt);
    REQUIRE(cmdRem);
    auto r3 = f.history.Execute(std::move(cmdRem), f.manager);
    REQUIRE(r3.success);
    REQUIRE_FALSE(f.HasMotion(e));

    // Undo remove
    auto r4 = f.history.Undo(f.manager);
    REQUIRE(r4.success);
    REQUIRE(f.HasMotion(e));
    REQUIRE(MotionEq(f.GetMotion(e), beforeEdit));
}

TEST_CASE("Phase 3B2 SetMotionCommand no-op suppression for both nullopt")
{
    SceneFixture f;
    const auto e = f.AddBox("Box");
    auto cmd = MakeSetMotionCommandIfEffective(e, std::nullopt, std::nullopt);
    REQUIRE(!cmd);
}

// ---------------------------------------------------------------------------
// AlignCameraCommand: RecordApplied records the composite after-state; Redo
// re-applies stored state (not re-align to current view); Undo restores
// before-localTRS + before-cameraProps; one revision bump; Transform impact.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B2 AlignCameraCommand RecordApplied/Undo/Redo composite state")
{
    SceneFixture f;
    const auto e = f.AddCamera("Cam");

    EditableTRS beforeLocal;
    REQUIRE(f.manager.GetLocalTransform(SceneManager::EntityId{ f.manager.FindEntityByUuid(e) }, beforeLocal));
    CameraComponent beforeCamera = f.GetCamera(e);

    EditableTRS afterLocal = beforeLocal;
    afterLocal.translation = {5, 5, 5};
    CameraComponent afterCamera = beforeCamera;
    afterCamera.verticalFOV = 80.0f;

    // Apply via the atomic state API (simulating AlignCameraEntityToView).
    auto applyResult = f.manager.SetCameraPoseState(e, afterLocal, afterCamera);
    REQUIRE(applyResult.success);
    REQUIRE(applyResult.syncImpact == rt2::core::SyncImpact::Transform);

    auto cmd = MakeAlignCameraCommandIfEffective(e, beforeLocal, afterLocal, beforeCamera, afterCamera);
    REQUIRE(cmd);
    f.history.RecordApplied(std::move(cmd), f.manager, applyResult);
    REQUIRE(f.history.CanUndo());

    // Undo restores the before-state.
    auto r1 = f.history.Undo(f.manager);
    REQUIRE(r1.success);
    REQUIRE(r1.syncImpact == rt2::core::SyncImpact::Transform);
    EditableTRS undoLocal;
    REQUIRE(f.manager.GetLocalTransform(SceneManager::EntityId{ f.manager.FindEntityByUuid(e) }, undoLocal));
    REQUIRE(TrsEq(undoLocal, beforeLocal));
    REQUIRE(CameraEq(f.GetCamera(e), beforeCamera));

    // Redo re-applies the stored after-state (NOT re-align to current view).
    auto r2 = f.history.Redo(f.manager);
    REQUIRE(r2.success);
    EditableTRS redoLocal;
    REQUIRE(f.manager.GetLocalTransform(SceneManager::EntityId{ f.manager.FindEntityByUuid(e) }, redoLocal));
    REQUIRE(TrsEq(redoLocal, afterLocal));
    REQUIRE(CameraEq(f.GetCamera(e), afterCamera));
}

TEST_CASE("Phase 3B2 AlignCameraCommand no-op suppression for equal state")
{
    SceneFixture f;
    const auto e = f.AddCamera("Cam");
    EditableTRS local;
    REQUIRE(f.manager.GetLocalTransform(SceneManager::EntityId{ f.manager.FindEntityByUuid(e) }, local));
    CameraComponent cam = f.GetCamera(e);
    auto cmd = MakeAlignCameraCommandIfEffective(e, local, local, cam, cam);
    REQUIRE(!cmd);
}

// ---------------------------------------------------------------------------
// PropertyEditSession<T> state machine: activation/deactivation lifecycle;
// deferred-close-after-mutation ordering; Escape-cancel discards;
// defensive guards all discard.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B2 PropertyEditSession lifecycle and deferred close")
{
    PropertyEditSession<int> session;
    REQUIRE_FALSE(session.IsOpen());

    session.OnActivated(rt2::core::UUID{}, 10);
    REQUIRE(session.IsOpen());
    REQUIRE(session.BeforeValue() == 10);

    // Close without a commit → no record (activation with no edit is a no-op).
    auto rec0 = session.CloseDeferred(10);
    REQUIRE_FALSE(rec0.has_value());
    REQUIRE_FALSE(session.IsOpen());

    // Activate + commit + close → record.
    session.OnActivated(rt2::core::UUID{}, 10);
    session.OnEditCommitted();
    auto rec1 = session.CloseDeferred(20);
    REQUIRE(rec1.has_value());
    REQUIRE(rec1->before == 10);
    REQUIRE(rec1->after == 20);
    REQUIRE_FALSE(session.IsOpen());

    // Escape cancel discards.
    session.OnActivated(rt2::core::UUID{}, 10);
    session.OnEditCommitted();
    session.OnCancelled();
    REQUIRE_FALSE(session.IsOpen());
}

TEST_CASE("Phase 3B2 PropertyEditSession defensive guards discard on failure")
{
    PropertyEditSession<int> session;
    session.OnActivated(rt2::core::UUID{}, 10);
    session.OnEditCommitted();

    bool guardPasses = false;
    auto rec = session.CloseDeferred(20, { [&]() { return guardPasses; } });
    REQUIRE_FALSE(rec.has_value());
    REQUIRE_FALSE(session.IsOpen());

    // Guard passes → record produced.
    session.OnActivated(rt2::core::UUID{}, 10);
    session.OnEditCommitted();
    guardPasses = true;
    auto rec2 = session.CloseDeferred(20, { [&]() { return guardPasses; } });
    REQUIRE(rec2.has_value());
}

TEST_CASE("Phase 3B2 PropertyEditSession slot-still-in-range guard")
{
    PropertyEditSession<int> session;
    session.OnActivated(rt2::core::UUID{}, 5);
    session.OnEditCommitted();

    int slot = 3;
    int slotLimit = 5;
    auto rec = session.CloseDeferred(7, { [&]() { return slot < slotLimit; } });
    REQUIRE(rec.has_value());

    session.OnActivated(rt2::core::UUID{}, 5);
    session.OnEditCommitted();
    slot = 10;
    auto rec2 = session.CloseDeferred(7, { [&]() { return slot < slotLimit; } });
    REQUIRE_FALSE(rec2.has_value());
}

// ---------------------------------------------------------------------------
// Generation guard covers the new commands: a document-generation mismatch
// clears both stacks (already covered for 3A/3B1; verify the new commands
// are also subject to it via a SetNameCommand).
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B2 generation guard clears history on document change")
{
    SceneFixture f;
    const auto e = f.AddBox("Box");
    auto cmd = MakeSetNameCommandIfEffective(e, "Box", "Renamed");
    REQUIRE(cmd);
    f.history.Execute(std::move(cmd), f.manager);
    REQUIRE(f.history.CanUndo());

    // Simulate a document reload by bumping the generation. The history's
    // generation guard fires on the next operation (Undo), clearing both
    // stacks. The Undo itself returns a no-op failure (empty result).
    f.manager.Clear(); // bumps DocumentGeneration
    auto r = f.history.Undo(f.manager);
    REQUIRE_FALSE(r.success);
    REQUIRE_FALSE(f.history.CanUndo());
    REQUIRE_FALSE(f.history.CanRedo());
}

// ---------------------------------------------------------------------------
// No-op suppression for every command: identical before/after records no
// entry. (Covered per-command above; this is a sanity aggregate.)
// ---------------------------------------------------------------------------
TEST_CASE("Phase 3B2 all property command factories suppress no-ops")
{
    SceneFixture f;
    const auto box = f.AddBox("Box");
    const auto light = f.AddLight("Light");
    const auto cam = f.AddCamera("Cam");

    REQUIRE(!MakeSetNameCommandIfEffective(box, "Box", "Box"));
    REQUIRE(!MakeSetMaterialIndexCommandIfEffective(box, 0, 0, std::nullopt, std::nullopt));
    SceneMaterial m = f.MaterialAt(0);
    REQUIRE(!MakeSetMaterialPropertiesCommandIfEffective(0, m, m, {}, {}));
    LightComponent lv = f.GetLight(light);
    REQUIRE(!MakeSetLightCommandIfEffective(light, lv, lv));
    CameraComponent cv = f.GetCamera(cam);
    REQUIRE(!MakeSetCameraCommandIfEffective(cam, cv, cv));
    REQUIRE(!MakeSetMotionCommandIfEffective(box, std::nullopt, std::nullopt));
    EditableTRS trs;
    f.manager.GetLocalTransform(SceneManager::EntityId{ f.manager.FindEntityByUuid(cam) }, trs);
    REQUIRE(!MakeAlignCameraCommandIfEffective(cam, trs, trs, cv, cv));
}