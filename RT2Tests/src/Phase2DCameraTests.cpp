#include <doctest/doctest.h>

#include "EditorCameraWorkflow.h"
#include "EditorSceneState.h"
#include "ISceneRenderBridge.h"
#include "PrimitiveGeometry.h"
#include "SceneGraph.h"
#include "SceneManager.h"
#include "SceneSerializer.h"

#include <cmath>
#include <filesystem>

namespace
{
class RecordingBridge final : public rt2::core::ISceneRenderBridge
{
public:
    int fullSync = 0;
    int materialSync = 0;
    int transformSync = 0;
    int temporalReset = 0;
    void FullSync(GPUSceneData&) override { ++fullSync; }
    void MaterialSync(GPUSceneData&) override { ++materialSync; }
    void TransformSync(GPUSceneData&) override { ++transformSync; }
    void ResetTemporalState() override { ++temporalReset; }
    void RequestRender() override {}
};

void CheckFinite(const EditorCameraPose& pose)
{
    CHECK(std::isfinite(pose.position.x));
    CHECK(std::isfinite(pose.position.y));
    CHECK(std::isfinite(pose.position.z));
    CHECK(std::isfinite(pose.focusDistance));
    CHECK(pose.focusDistance > 0.0f);
}

TEST_CASE("Phase 2D editor camera cut applies atomically and only resets temporal state")
{
    RecordingBridge bridge;
    EditorCameraPose applied;
    EditorCameraPose requested;
    requested.position = { 2.0f, 3.0f, 4.0f };
    requested.forward = { 0.0f, 0.0f, -3.0f };
    requested.aperture = 0.4f;
    requested.focusDistance = 9.0f;
    REQUIRE(ApplyEditorCameraCut(requested, bridge,
        [&applied](const EditorCameraPose& normalized) {
            applied = normalized;
            return true;
        }));
    CHECK(applied.position == requested.position);
    CHECK(applied.forward == glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(applied.aperture == doctest::Approx(0.4f));
    CHECK(applied.focusDistance == doctest::Approx(9.0f));
    CHECK(bridge.temporalReset == 1);
    CHECK(bridge.fullSync == 0);
    CHECK(bridge.materialSync == 0);
    CHECK(bridge.transformSync == 0);

    requested.forward = glm::vec3(0.0f);
    CHECK_FALSE(ApplyEditorCameraCut(requested, bridge,
        [](const EditorCameraPose&) { return true; }));
    CHECK(bridge.temporalReset == 1);
}
}

TEST_CASE("Phase 2D mesh registration caches finite object bounds")
{
    MeshRegistry registry;
    MeshData mesh;
    mesh.vertices = { -2.0f, 3.0f, 1.0f, 4.0f, -1.0f, 7.0f,
                       0.0f, 2.0f, -5.0f };
    const uint32_t index = registry.AddMesh(std::move(mesh));
    const auto& cached = registry.GetMesh(index);
    REQUIRE(cached.boundsValid);
    CHECK(cached.boundsMin == glm::vec3(-2.0f, -1.0f, -5.0f));
    CHECK(cached.boundsMax == glm::vec3(4.0f, 3.0f, 7.0f));
}

TEST_CASE("Phase 2D selection bounds include hidden hierarchy descendants")
{
    rt2::core::DeterministicUuidProvider ids;
    SceneManager manager;
    manager.SetUuidProvider(&ids);
    manager.AddMaterial(SceneMaterial{});
    const auto root = manager.CreateEmpty("Root").affectedEntities.front();
    const auto childEntity = manager.AddObjectWithGeometry(
        "Cube", PrimitiveGeometry::CreateCube(2.0f),
        { 10.0f, 0.0f, 0.0f }, {}, 1.0f, 0);
    const auto child = manager.GetEntityUuid(childEntity);
    REQUIRE(manager.Reparent({ child }, root, ReparentMode::PreserveLocal).success);
    REQUIRE(manager.SetVisibility({ root }, false).success);

    EditorSelectionBounds bounds;
    REQUIRE(ComputeEditorSelectionBounds(manager.AuthoringDoc(), { root }, bounds));
    CHECK(bounds.minimum.x == doctest::Approx(-0.25f)); // empty root fallback
    CHECK(bounds.maximum.x == doctest::Approx(11.0f));
}

TEST_CASE("Phase 2D empty-only selection has deterministic non-zero bounds")
{
    rt2::core::DeterministicUuidProvider ids;
    SceneManager manager;
    manager.SetUuidProvider(&ids);
    const auto empty = manager.CreateEmpty("Empty").affectedEntities.front();
    EditorSelectionBounds bounds;
    REQUIRE(ComputeEditorSelectionBounds(manager.AuthoringDoc(), { empty }, bounds));
    CHECK(bounds.minimum == glm::vec3(-0.25f));
    CHECK(bounds.maximum == glm::vec3(0.25f));

    EditorCameraPose current;
    EditorCameraPose framed;
    REQUIRE(TryFrameEditorCamera(current, bounds, {}, framed));
    CheckFinite(framed);
}

TEST_CASE("Phase 2D frame selected fits both axes and handles camera inside bounds")
{
    EditorSelectionBounds bounds;
    bounds.minimum = glm::vec3(-1.0f);
    bounds.maximum = glm::vec3(1.0f);
    bounds.valid = true;
    EditorCameraPose current;
    current.position = glm::vec3(0.0f); // exact centre fallback keeps -Z

    EditorCameraPose wide;
    EditorFrameSettings wideSettings;
    wideSettings.viewportAspect = 2.0f;
    REQUIRE(TryFrameEditorCamera(current, bounds, wideSettings, wide));
    CheckFinite(wide);
    CHECK(glm::dot(wide.forward, glm::vec3(0.0f, 0.0f, -1.0f)) > 0.999f);
    CHECK(wide.position.z > bounds.maximum.z);

    EditorCameraPose tall;
    EditorFrameSettings tallSettings;
    tallSettings.viewportAspect = 0.5f;
    REQUIRE(TryFrameEditorCamera(current, bounds, tallSettings, tall));
    CheckFinite(tall);
    CHECK(tall.focusDistance > wide.focusDistance);
}

TEST_CASE("Phase 2D focus selected rotates without translating")
{
    EditorSelectionBounds bounds;
    bounds.minimum = { 4.5f, -0.5f, -0.5f };
    bounds.maximum = { 5.5f, 0.5f, 0.5f };
    bounds.valid = true;
    EditorCameraPose current;
    current.position = { 0.0f, 0.0f, 0.0f };
    EditorCameraPose focused;
    REQUIRE(TryFocusEditorCamera(current, bounds, 0.1f, focused));
    CHECK(focused.position == current.position);
    CHECK(glm::dot(focused.forward, glm::vec3(1.0f, 0.0f, 0.0f)) > 0.999f);
    CHECK(focused.focusDistance == doctest::Approx(5.0f));
}

TEST_CASE("Phase 2D bookmarks are normalized document-session state")
{
    rt2::core::DeterministicUuidProvider ids;
    SceneManager manager;
    manager.SetUuidProvider(&ids);
    const uint64_t revision = manager.AuthoringRevision();
    const bool dirty = manager.IsDirty();

    EditorSceneState state;
    EditorCameraPose pose;
    pose.position = { 3.0f, 4.0f, 5.0f };
    pose.forward = { 0.0f, 0.0f, -4.0f };
    pose.verticalFOV = 60.0f;
    pose.aperture = 0.25f;
    pose.focusDistance = 8.0f;
    pose.farClip = 5000.0f;
    REQUIRE(state.CaptureCameraBookmark(0, pose));
    const EditorCameraPose* stored = state.CameraBookmark(0);
    REQUIRE(stored != nullptr);
    CHECK(stored->position == pose.position);
    CHECK(stored->forward == glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(stored->aperture == doctest::Approx(0.25f));
    CHECK(stored->farClip == doctest::Approx(5000.0f));
    CHECK(manager.AuthoringRevision() == revision);
    CHECK(manager.IsDirty() == dirty);
    state.ResetForDocument();
    CHECK(state.CameraBookmark(0) == nullptr);
}

TEST_CASE("Phase 2D runtime camera choice is UUID deterministic")
{
    rt2::core::DeterministicUuidProvider ids;
    rt2::core::SceneDocument document;
    document.SetUuidProvider(&ids);
    const entt::entity first = document.ecs.registry.create();
    document.ecs.registry.emplace<Transform>(first);
    document.ecs.registry.emplace<CameraComponent>(first);
    const auto firstUuid = document.AssignNewUuid(first);
    const entt::entity second = document.ecs.registry.create();
    document.ecs.registry.emplace<Transform>(second);
    document.ecs.registry.emplace<CameraComponent>(second);
    const auto secondUuid = document.AssignNewUuid(second);
    CHECK(FindDeterministicCameraEntity(document) ==
          std::min(firstUuid, secondUuid));
}

TEST_CASE("Phase 2D align camera is atomic and refreshes derived forward")
{
    rt2::core::DeterministicUuidProvider ids;
    SceneManager manager;
    manager.SetUuidProvider(&ids);
    const auto cameraUuid = manager.CreateEmpty("Camera").affectedEntities.front();
    const entt::entity entity = manager.FindEntityByUuid(cameraUuid);
    manager.GetECS().registry.emplace<CameraComponent>(entity);
    const uint64_t revision = manager.AuthoringRevision();

    EditorCameraPose pose;
    pose.position = { 8.0f, 3.0f, -2.0f };
    pose.forward = glm::normalize(glm::vec3(-1.0f, 0.25f, -0.5f));
    pose.verticalFOV = 72.0f;
    const auto result = manager.AlignCameraEntityToView(cameraUuid, pose);
    REQUIRE(result.success);
    CHECK(result.syncImpact == rt2::core::SyncImpact::Transform);
    CHECK(manager.AuthoringRevision() == revision + 1);
    const auto& component = manager.GetECS().registry.get<CameraComponent>(entity);
    CHECK(glm::dot(component.forwardDirection, pose.forward) > 0.999f);

    EditorCameraPose readBack;
    REQUIRE(TryGetCameraEntityPose(manager.AuthoringDoc(), cameraUuid, pose, readBack));
    CHECK(readBack.position.x == doctest::Approx(8.0f));
    CHECK(glm::dot(readBack.forward, pose.forward) > 0.999f);
}

TEST_CASE("Phase 2D camera alignment rejects sheared parent conversion")
{
    rt2::core::DeterministicUuidProvider ids;
    SceneManager manager;
    manager.SetUuidProvider(&ids);
    const auto parent = manager.CreateEmpty("Scaled Parent").affectedEntities.front();
    const auto camera = manager.CreateEmpty("Camera", parent).affectedEntities.front();
    const entt::entity parentEntity = manager.FindEntityByUuid(parent);
    const entt::entity cameraEntity = manager.FindEntityByUuid(camera);
    manager.GetECS().registry.emplace<CameraComponent>(cameraEntity);
    EditableTRS parentTransform;
    parentTransform.scale = { 2.0f, 1.0f, 1.0f };
    manager.SetLocalTransform({ parentEntity }, parentTransform);
    EditableTRS before;
    REQUIRE(manager.GetLocalTransform({ cameraEntity }, before));
    const uint64_t revision = manager.AuthoringRevision();

    EditorCameraPose pose;
    pose.forward = glm::normalize(glm::vec3(-1.0f, 0.0f, -1.0f));
    const auto result = manager.AlignCameraEntityToView(camera, pose);
    CHECK_FALSE(result.success);
    CHECK(manager.AuthoringRevision() == revision);
    EditableTRS after;
    REQUIRE(manager.GetLocalTransform({ cameraEntity }, after));
    CHECK(after.translation == before.translation);
    CHECK(after.rotation == before.rotation);
}

TEST_CASE("Phase 2D camera transform authority survives native save and reload")
{
    rt2::core::DeterministicUuidProvider ids;
    SceneManager source;
    source.SetUuidProvider(&ids);
    const auto cameraUuid = source.CreateEmpty("Persisted Camera").affectedEntities.front();
    const entt::entity cameraEntity = source.FindEntityByUuid(cameraUuid);
    source.GetECS().registry.emplace<CameraComponent>(cameraEntity);

    EditorCameraPose expected;
    expected.position = { -7.0f, 2.5f, 11.0f };
    expected.forward = glm::normalize(glm::vec3(0.35f, -0.2f, -1.0f));
    expected.verticalFOV = 63.0f;
    expected.aperture = 0.12f;
    expected.focusDistance = 14.0f;
    REQUIRE(source.AlignCameraEntityToView(cameraUuid, expected).success);

    const auto path = std::filesystem::temp_directory_path() /
        "rt2_phase2d_camera_roundtrip.rt2scene";
    rt2::core::Error error;
    REQUIRE(rt2::core::SceneSerializer::Save(source.AuthoringDoc(), path, error));

    rt2::core::SceneDocument loaded;
    loaded.SetUuidProvider(&ids);
    REQUIRE(rt2::core::SceneSerializer::Load(loaded, path, error));
    SceneManager adopted;
    adopted.SetUuidProvider(&ids);
    adopted.ReplaceAuthoringDocument(std::move(loaded));

    EditorCameraPose actual;
    REQUIRE(TryGetCameraEntityPose(adopted.AuthoringDoc(), cameraUuid,
        expected, actual));
    CHECK(actual.position.x == doctest::Approx(expected.position.x));
    CHECK(actual.position.y == doctest::Approx(expected.position.y));
    CHECK(actual.position.z == doctest::Approx(expected.position.z));
    CHECK(glm::dot(actual.forward, expected.forward) > 0.999f);
    CHECK(actual.verticalFOV == doctest::Approx(expected.verticalFOV));
    CHECK(actual.aperture == doctest::Approx(expected.aperture));
    CHECK(actual.focusDistance == doctest::Approx(expected.focusDistance));

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}
