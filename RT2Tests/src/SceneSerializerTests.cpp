#include <doctest/doctest.h>

#include "SceneSerializer.h"
#include "SceneDocument.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "SceneManager.h"
#include "PrimitiveGeometry.h"
#include "core/UUID.h"
#include "core/Error.h"
#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <fstream>
#include <functional>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace rt2::core;

// ============================================================================
// Helpers for building slice fixture scenes in memory
// ============================================================================

namespace {

// Build a minimal slice fixture: a cube with motion, a light, and a camera.
SceneDocument BuildSliceFixture(IUuidProvider* provider)
{
    SceneDocument doc;
    doc.SetUuidProvider(provider);

    // Material
    SceneMaterial mat;
    mat.baseColor = { 0.8f, 0.2f, 0.2f };
    mat.roughness = 0.5f;
    doc.ecs.materials.push_back(mat);

    // Cube entity with PrimitiveComponent + MeshRef + MotionComponent
    MeshData cubeMesh = PrimitiveGeometry::CreateCube(1.0f);
    cubeMesh.name = "cube";
    uint32_t meshIdx = doc.ecs.meshRegistry.AddMesh(std::move(cubeMesh));

    entt::entity cube = doc.ecs.registry.create();
    doc.ecs.registry.emplace<NameComponent>(cube, "Cube");
    Transform& tf = doc.ecs.registry.emplace<Transform>(cube);
    tf.translation = { 0.0f, 0.0f, 0.0f };
    tf.dirty = true;
    doc.ecs.registry.emplace<VisibleComponent>(cube);
    doc.ecs.registry.emplace<MeshRef>(cube, meshIdx, 0);
    doc.ecs.registry.emplace<PrimitiveComponent>(cube, PrimitiveComponent{ PrimitiveComponent::Cube, 1.0f, 24, 16 });
    doc.ecs.registry.emplace<MotionComponent>(cube, MotionComponent{ { 1.0f, 0.0f, 0.0f } });
    doc.AssignNewUuid(cube); // emplaces EntityIdComponent

    // Light entity
    entt::entity light = doc.ecs.registry.create();
    doc.ecs.registry.emplace<NameComponent>(light, "Light");
    Transform& ltf = doc.ecs.registry.emplace<Transform>(light);
    ltf.translation = { 5.0f, 10.0f, 5.0f };
    ltf.dirty = true;
    doc.ecs.registry.emplace<VisibleComponent>(light);
    doc.ecs.registry.emplace<LightComponent>(light, LightComponent{ { 1, 1, 1 }, 50.0f, 50.0f, 30.0f, 45.0f, false });
    doc.AssignNewUuid(light);

    // Camera entity
    entt::entity cam = doc.ecs.registry.create();
    doc.ecs.registry.emplace<NameComponent>(cam, "Camera");
    Transform& ctf = doc.ecs.registry.emplace<Transform>(cam);
    ctf.translation = { 0.0f, 1.0f, 10.0f };
    ctf.dirty = true;
    doc.ecs.registry.emplace<VisibleComponent>(cam);
    doc.ecs.registry.emplace<CameraComponent>(cam, CameraComponent{ 45.0f, 0.0f, 1.0f, { 0, 0, -1 } });
    doc.AssignNewUuid(cam);

    // Scene camera
    doc.ecs.camera.position = { 0.0f, 1.0f, 10.0f };
    doc.ecs.camera.forwardDirection = { 0.0f, 0.0f, -1.0f };
    doc.ecs.camera.verticalFOV = 45.0f;

    return doc;
}

// Write a string to a temp file and return its path.
std::filesystem::path WriteTempFile(const std::string& content, const std::string& suffix = ".rt2scene")
{
    auto dir = std::filesystem::temp_directory_path();
    auto path = dir / ("rt2_test_" + std::to_string(std::rand()) + suffix);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    return path;
}

} // anonymous namespace

// ============================================================================
// Round-trip tests
// ============================================================================

TEST_CASE("VS-2 Serializer: empty scene round-trips")
{
    DeterministicUuidProvider provider;
    SceneDocument src;
    src.SetUuidProvider(&provider);

    auto path = std::filesystem::temp_directory_path() / "rt2_empty_test.rt2scene";
    Error err;
    CHECK(SceneSerializer::Save(src, path, err));
    CHECK(err.IsOk());

    SceneDocument loaded;
    loaded.SetUuidProvider(&provider);
    CHECK(SceneSerializer::Load(loaded, path, err));
    CHECK(err.IsOk());

    CHECK(loaded.ecs.registry.view<EntityIdComponent>().size() == 0);
    CHECK(loaded.ecs.materials.empty());

    std::filesystem::remove(path);
}

TEST_CASE("VS-2 Serializer: slice fixture round-trips structurally")
{
    DeterministicUuidProvider provider;
    SceneDocument src = BuildSliceFixture(&provider);

    auto path = std::filesystem::temp_directory_path() / "rt2_slice_test.rt2scene";
    Error err;
    CHECK(SceneSerializer::Save(src, path, err));
    CHECK(err.IsOk());

    // Load into a fresh document with a fresh provider.
    DeterministicUuidProvider loadProvider;
    SceneDocument loaded;
    loaded.SetUuidProvider(&loadProvider);
    CHECK(SceneSerializer::Load(loaded, path, err));
    CHECK(err.IsOk());

    // Entity count
    CHECK(loaded.ecs.registry.view<EntityIdComponent>().size() == 3);

    // Materials
    CHECK(loaded.ecs.materials.size() == 1);
    CHECK(loaded.ecs.materials[0].baseColor.x == doctest::Approx(0.8f));

    // Mesh registry should have one mesh (the cube)
    CHECK(loaded.ecs.meshRegistry.GetCount() == 1);

    // Find the cube by checking for MotionComponent
    bool foundCube = false;
    bool foundLight = false;
    bool foundCamera = false;
    auto view = loaded.ecs.registry.view<EntityIdComponent>();
    for (auto e : view)
    {
        auto& idc = view.get<EntityIdComponent>(e);
        if (loaded.ecs.registry.all_of<MotionComponent>(e))
        {
            foundCube = true;
            auto& mc = loaded.ecs.registry.get<MotionComponent>(e);
            CHECK(mc.linearVelocity.x == doctest::Approx(1.0f));

            auto* nc = loaded.ecs.registry.try_get<NameComponent>(e);
            CHECK(nc != nullptr);
            CHECK(nc->name == "Cube");

            CHECK(loaded.ecs.registry.all_of<PrimitiveComponent>(e));
            CHECK(loaded.ecs.registry.all_of<MeshRef>(e));

            auto& pc = loaded.ecs.registry.get<PrimitiveComponent>(e);
            CHECK(pc.kind == PrimitiveComponent::Cube);
        }
        if (loaded.ecs.registry.all_of<LightComponent>(e))
        {
            foundLight = true;
            auto& lc = loaded.ecs.registry.get<LightComponent>(e);
            CHECK(lc.intensity == doctest::Approx(50.0f));
        }
        if (loaded.ecs.registry.all_of<CameraComponent>(e))
        {
            foundCamera = true;
            auto& cc = loaded.ecs.registry.get<CameraComponent>(e);
            CHECK(cc.verticalFOV == doctest::Approx(45.0f));
        }
    }
    CHECK(foundCube);
    CHECK(foundLight);
    CHECK(foundCamera);

    // UUIDs should be preserved
    auto srcView = src.ecs.registry.view<EntityIdComponent>();
    auto loadView = loaded.ecs.registry.view<EntityIdComponent>();
    CHECK(srcView.size() == loadView.size());

    std::set<UUID> srcUuids, loadUuids;
    for (auto e : srcView)  srcUuids.insert(srcView.get<EntityIdComponent>(e).id);
    for (auto e : loadView) loadUuids.insert(loadView.get<EntityIdComponent>(e).id);
    CHECK(srcUuids == loadUuids);

    std::filesystem::remove(path);
}

TEST_CASE("VS-2 Serializer: save is deterministic (byte-identical with same provider)")
{
    DeterministicUuidProvider provider;
    SceneDocument src = BuildSliceFixture(&provider);

    auto path1 = std::filesystem::temp_directory_path() / "rt2_det1.rt2scene";
    auto path2 = std::filesystem::temp_directory_path() / "rt2_det2.rt2scene";
    Error err;

    CHECK(SceneSerializer::Save(src, path1, err));
    // Same document, same provider — should be byte-identical.
    CHECK(SceneSerializer::Save(src, path2, err));

    std::ifstream f1(path1, std::ios::binary), f2(path2, std::ios::binary);
    std::string c1((std::istreambuf_iterator<char>(f1)), std::istreambuf_iterator<char>());
    std::string c2((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
    f1.close();
    f2.close();
    CHECK(c1 == c2);

    std::error_code ec1, ec2;
    std::filesystem::remove(path1, ec1);
    std::filesystem::remove(path2, ec2);
}

TEST_CASE("VS-2 Serializer: failed save leaves existing file intact")
{
    DeterministicUuidProvider provider;
    SceneDocument src = BuildSliceFixture(&provider);

    auto path = std::filesystem::temp_directory_path() / "rt2_intact_test.rt2scene";
    Error err;

    // Save once to create the file.
    CHECK(SceneSerializer::Save(src, path, err));
    CHECK(err.IsOk());

    // Read the original content.
    std::ifstream in1(path, std::ios::binary);
    std::string original((std::istreambuf_iterator<char>(in1)), std::istreambuf_iterator<char>());
    in1.close();

    // Make the target read-only and try to save again.
#ifdef _WIN32
    DWORD attrs = GetFileAttributesW(path.wstring().c_str());
    SetFileAttributesW(path.wstring().c_str(), attrs | FILE_ATTRIBUTE_READONLY);
#endif

    SceneDocument src2;
    src2.SetUuidProvider(&provider);
    bool result = SceneSerializer::Save(src2, path, err);

#ifdef _WIN32
    // Restore attributes.
    SetFileAttributesW(path.wstring().c_str(), attrs);
#endif

    // On Windows with ReplaceFileW, a read-only target should cause failure.
    // The original file must be intact.
    std::ifstream in2(path, std::ios::binary);
    std::string afterSave((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());
    in2.close();

    CHECK(afterSave == original);

    std::filesystem::remove(path);
}

TEST_CASE("VS-2 Serializer: malformed JSON fails with Parse error")
{
    auto path = WriteTempFile("{ this is not json }");
    Error err;
    SceneDocument doc;
    DeterministicUuidProvider provider;
    doc.SetUuidProvider(&provider);
    CHECK_FALSE(SceneSerializer::Load(doc, path, err));
    CHECK(err.code == Error::Parse);
    std::filesystem::remove(path);
}

TEST_CASE("VS-2 Serializer: unsupported schema version fails clearly")
{
    auto path = WriteTempFile(R"({"version":99,"entities":[]})");
    Error err;
    SceneDocument doc;
    DeterministicUuidProvider provider;
    doc.SetUuidProvider(&provider);
    CHECK_FALSE(SceneSerializer::Load(doc, path, err));
    CHECK(err.code == Error::SchemaVersion);
    std::filesystem::remove(path);
}

TEST_CASE("VS-2 Serializer: duplicate UUID fails with DuplicateUuid error")
{
    auto path = WriteTempFile(R"({
        "version": 3,
        "entities": [
            {"uuid":"550e8400-e29b-41d4-a716-446655440000","name":"A","parent":"","visible":true,"transform":{"translation":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}},
            {"uuid":"550e8400-e29b-41d4-a716-446655440000","name":"B","parent":"","visible":true,"transform":{"translation":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}}
        ],
        "materials": [],
        "textures": [],
        "camera": {"position":[0,0,10],"forward":[0,0,-1],"fov":45,"aperture":0,"focusDist":1},
        "envMap": {"path":"","width":0,"height":0}
    })");
    Error err;
    SceneDocument doc;
    DeterministicUuidProvider provider;
    doc.SetUuidProvider(&provider);
    CHECK_FALSE(SceneSerializer::Load(doc, path, err));
    CHECK(err.code == Error::DuplicateUuid);
    std::filesystem::remove(path);
}

TEST_CASE("VS-2 Serializer: missing parent fails with MissingParent error")
{
    auto path = WriteTempFile(R"({
        "version": 3,
        "entities": [
            {"uuid":"550e8400-e29b-41d4-a716-446655440000","name":"Child","parent":"660e8400-e29b-41d4-a716-446655440001","visible":true,"transform":{"translation":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}}
        ],
        "materials": [],
        "textures": [],
        "camera": {"position":[0,0,10],"forward":[0,0,-1],"fov":45,"aperture":0,"focusDist":1},
        "envMap": {"path":"","width":0,"height":0}
    })");
    Error err;
    SceneDocument doc;
    DeterministicUuidProvider provider;
    doc.SetUuidProvider(&provider);
    CHECK_FALSE(SceneSerializer::Load(doc, path, err));
    CHECK(err.code == Error::MissingParent);
    std::filesystem::remove(path);
}

TEST_CASE("VS-2 Serializer: hierarchy resolves independent of serialized order")
{
    DeterministicUuidProvider provider;
    SceneDocument src;
    src.SetUuidProvider(&provider);

    // Create parent first, then child.
    entt::entity parent = src.ecs.registry.create();
    UUID parentUuid = src.AssignNewUuid(parent);
    src.ecs.registry.emplace<NameComponent>(parent, "Parent");
    Transform& ptf = src.ecs.registry.emplace<Transform>(parent);
    ptf.translation = { 10.0f, 0.0f, 0.0f };
    ptf.dirty = true;
    src.ecs.registry.emplace<VisibleComponent>(parent);

    entt::entity child = src.ecs.registry.create();
    UUID childUuid = src.AssignNewUuid(child);
    src.ecs.registry.emplace<NameComponent>(child, "Child");
    Transform& ctf = src.ecs.registry.emplace<Transform>(child);
    ctf.dirty = true;
    src.ecs.registry.emplace<VisibleComponent>(child);

    Hierarchy& ph = src.ecs.registry.emplace<Hierarchy>(parent);
    ph.children.push_back(child);
    Hierarchy& ch = src.ecs.registry.emplace<Hierarchy>(child);
    ch.parent = parent;

    auto path = std::filesystem::temp_directory_path() / "rt2_hier_test.rt2scene";
    Error err;
    CHECK(SceneSerializer::Save(src, path, err));

    // Load and verify hierarchy.
    DeterministicUuidProvider loadProvider;
    SceneDocument loaded;
    loaded.SetUuidProvider(&loadProvider);
    CHECK(SceneSerializer::Load(loaded, path, err));
    CHECK(err.IsOk());

    entt::entity loadedChild = loaded.FindByUuid(childUuid);
    entt::entity loadedParent = loaded.FindByUuid(parentUuid);
    CHECK(loadedChild != static_cast<entt::entity>(entt::null));
    CHECK(loadedParent != static_cast<entt::entity>(entt::null));

    auto* childHier = loaded.ecs.registry.try_get<Hierarchy>(loadedChild);
    CHECK(childHier != nullptr);
    CHECK(childHier->parent == loadedParent);

    auto* parentHier = loaded.ecs.registry.try_get<Hierarchy>(loadedParent);
    CHECK(parentHier != nullptr);
    CHECK(parentHier->children.size() == 1);
    CHECK(parentHier->children[0] == loadedChild);

    // Verify the parent's transform survived
    auto& ptf2 = loaded.ecs.registry.get<Transform>(loadedParent);
    CHECK(ptf2.translation.x == doctest::Approx(10.0f));

    std::filesystem::remove(path);
}

TEST_CASE("VS-2 Serializer: unknown optional fields are ignored")
{
    auto path = WriteTempFile(R"({
        "version": 3,
        "entities": [],
        "materials": [],
        "textures": [],
        "camera": {"position":[0,0,10],"forward":[0,0,-1],"fov":45,"aperture":0,"focusDist":1},
        "envMap": {"path":"","width":0,"height":0},
        "unknownTopLevelField": "should be ignored",
        "futureExtension": {"data": 42}
    })");
    Error err;
    SceneDocument doc;
    DeterministicUuidProvider provider;
    doc.SetUuidProvider(&provider);
    CHECK(SceneSerializer::Load(doc, path, err));
    CHECK(err.IsOk());
    std::filesystem::remove(path);
}

TEST_CASE("VS-2 Serializer: non-primitive mesh entity rejected with UnknownPrimitive")
{
    // An entity with a MeshRef but no PrimitiveComponent should be rejected.
    auto path = WriteTempFile(R"({
        "version": 3,
        "entities": [
            {"uuid":"550e8400-e29b-41d4-a716-446655440000","name":"ImportedMesh","parent":"","visible":true,
             "transform":{"translation":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
             "meshRef":{"materialIndex":0}}
        ],
        "materials": [],
        "textures": [],
        "camera": {"position":[0,0,10],"forward":[0,0,-1],"fov":45,"aperture":0,"focusDist":1},
        "envMap": {"path":"","width":0,"height":0}
    })");
    Error err;
    SceneDocument doc;
    DeterministicUuidProvider provider;
    doc.SetUuidProvider(&provider);
    CHECK_FALSE(SceneSerializer::Load(doc, path, err));
    CHECK(err.code == Error::UnknownPrimitive);
    std::filesystem::remove(path);
}

TEST_CASE("VS-2 Serializer: materials round-trip")
{
    DeterministicUuidProvider provider;
    SceneDocument src;
    src.SetUuidProvider(&provider);

    SceneMaterial m1;
    m1.baseColor = { 0.5f, 0.6f, 0.7f };
    m1.metallic = 0.8f;
    m1.roughness = 0.2f;
    m1.emissiveColor = { 1.0f, 0.0f, 0.0f };
    m1.emissiveIntensity = 5.0f;
    src.ecs.materials.push_back(m1);

    SceneMaterial m2;
    m2.type = MaterialType::Metal;
    m2.baseColor = { 0.1f, 0.1f, 0.1f };
    m2.metallic = 1.0f;
    m2.roughness = 0.1f;
    src.ecs.materials.push_back(m2);

    auto path = std::filesystem::temp_directory_path() / "rt2_mat_test.rt2scene";
    Error err;
    CHECK(SceneSerializer::Save(src, path, err));

    DeterministicUuidProvider loadProvider;
    SceneDocument loaded;
    loaded.SetUuidProvider(&loadProvider);
    CHECK(SceneSerializer::Load(loaded, path, err));
    CHECK(err.IsOk());

    CHECK(loaded.ecs.materials.size() == 2);
    CHECK(loaded.ecs.materials[0].baseColor.x == doctest::Approx(0.5f));
    CHECK(loaded.ecs.materials[0].metallic == doctest::Approx(0.8f));
    CHECK(loaded.ecs.materials[0].emissiveIntensity == doctest::Approx(5.0f));
    CHECK(loaded.ecs.materials[1].type == MaterialType::Metal);
    CHECK(loaded.ecs.materials[1].metallic == doctest::Approx(1.0f));

    std::filesystem::remove(path);
}

TEST_CASE("VS-2 Serializer: camera round-trips")
{
    DeterministicUuidProvider provider;
    SceneDocument src;
    src.SetUuidProvider(&provider);

    src.ecs.camera.position = { 3.0f, 5.0f, -7.0f };
    src.ecs.camera.forwardDirection = { 0.5f, -0.3f, 0.8f };
    src.ecs.camera.verticalFOV = 60.0f;
    src.ecs.camera.aperture = 0.05f;
    src.ecs.camera.focusDistance = 3.0f;

    auto path = std::filesystem::temp_directory_path() / "rt2_cam_test.rt2scene";
    Error err;
    CHECK(SceneSerializer::Save(src, path, err));

    DeterministicUuidProvider loadProvider;
    SceneDocument loaded;
    loaded.SetUuidProvider(&loadProvider);
    CHECK(SceneSerializer::Load(loaded, path, err));
    CHECK(err.IsOk());

    CHECK(loaded.ecs.camera.position.x == doctest::Approx(3.0f));
    CHECK(loaded.ecs.camera.position.y == doctest::Approx(5.0f));
    CHECK(loaded.ecs.camera.position.z == doctest::Approx(-7.0f));
    CHECK(loaded.ecs.camera.forwardDirection.x == doctest::Approx(0.5f));
    CHECK(loaded.ecs.camera.verticalFOV == doctest::Approx(60.0f));
    CHECK(loaded.ecs.camera.aperture == doctest::Approx(0.05f));
    CHECK(loaded.ecs.camera.focusDistance == doctest::Approx(3.0f));

    std::filesystem::remove(path);
}

// ============================================================================
// CloneInMemory tests
// ============================================================================

TEST_CASE("VS-2 CloneInMemory: clone preserves UUIDs and data")
{
    DeterministicUuidProvider provider;
    SceneDocument src = BuildSliceFixture(&provider);

    SceneDocument dst;
    DeterministicUuidProvider dstProvider;
    dst.SetUuidProvider(&dstProvider);

    Error err;
    CHECK(SceneSerializer::CloneInMemory(src, dst, err));
    CHECK(err.IsOk());

    // Same UUIDs
    auto srcView = src.ecs.registry.view<EntityIdComponent>();
    auto dstView = dst.ecs.registry.view<EntityIdComponent>();
    CHECK(srcView.size() == dstView.size());

    std::set<UUID> srcUuids, dstUuids;
    for (auto e : srcView) srcUuids.insert(srcView.get<EntityIdComponent>(e).id);
    for (auto e : dstView) dstUuids.insert(dstView.get<EntityIdComponent>(e).id);
    CHECK(srcUuids == dstUuids);

    // Same material count
    CHECK(dst.ecs.materials.size() == src.ecs.materials.size());

    // Same mesh count
    CHECK(dst.ecs.meshRegistry.GetCount() == src.ecs.meshRegistry.GetCount());
}

TEST_CASE("VS-2 CloneInMemory: clone has independent storage")
{
    DeterministicUuidProvider provider;
    SceneDocument src = BuildSliceFixture(&provider);

    SceneDocument dst;
    DeterministicUuidProvider dstProvider;
    dst.SetUuidProvider(&dstProvider);

    Error err;
    CHECK(SceneSerializer::CloneInMemory(src, dst, err));

    // Mutate the clone's cube transform.
    auto dstView = dst.ecs.registry.view<MotionComponent>();
    for (auto e : dstView)
    {
        auto& tf = dst.ecs.registry.get<Transform>(e);
        tf.translation = { 99.0f, 99.0f, 99.0f };
    }

    // Source should be unchanged.
    auto srcView = src.ecs.registry.view<MotionComponent>();
    for (auto e : srcView)
    {
        auto& tf = src.ecs.registry.get<Transform>(e);
        CHECK(tf.translation.x == doctest::Approx(0.0f));
    }
}

TEST_CASE("VS-2 CloneInMemory: does not clone transient state")
{
    DeterministicUuidProvider provider;
    SceneDocument src = BuildSliceFixture(&provider);

    // Set some transient state on src.
    src.metadata.dirty = true;
    src.gpuCache.envMapIndex = 42; // pretend GPU cache has data

    SceneDocument dst;
    DeterministicUuidProvider dstProvider;
    dst.SetUuidProvider(&dstProvider);

    Error err;
    CHECK(SceneSerializer::CloneInMemory(src, dst, err));

    // dst should NOT have the dirty flag or gpuCache from src.
    CHECK_FALSE(dst.metadata.dirty);
    CHECK(dst.gpuCache.envMapIndex == -1); // default from GPUSceneData{}, not copied from src
}

TEST_CASE("VS-2 CloneInMemory: clone preserves hierarchy")
{
    DeterministicUuidProvider provider;
    SceneDocument src;
    src.SetUuidProvider(&provider);

    // Parent + child
    entt::entity parent = src.ecs.registry.create();
    UUID parentUuid = src.AssignNewUuid(parent);
    src.ecs.registry.emplace<Transform>(parent).dirty = true;
    src.ecs.registry.emplace<VisibleComponent>(parent);

    entt::entity child = src.ecs.registry.create();
    UUID childUuid = src.AssignNewUuid(child);
    src.ecs.registry.emplace<Transform>(child).dirty = true;
    src.ecs.registry.emplace<VisibleComponent>(child);

    Hierarchy& ph = src.ecs.registry.emplace<Hierarchy>(parent);
    ph.children.push_back(child);
    Hierarchy& ch = src.ecs.registry.emplace<Hierarchy>(child);
    ch.parent = parent;

    SceneDocument dst;
    DeterministicUuidProvider dstProvider;
    dst.SetUuidProvider(&dstProvider);

    Error err;
    CHECK(SceneSerializer::CloneInMemory(src, dst, err));
    CHECK(err.IsOk());

    entt::entity dstParent = dst.FindByUuid(parentUuid);
    entt::entity dstChild  = dst.FindByUuid(childUuid);
    CHECK(dstParent != static_cast<entt::entity>(entt::null));
    CHECK(dstChild  != static_cast<entt::entity>(entt::null));

    auto* dh = dst.ecs.registry.try_get<Hierarchy>(dstChild);
    CHECK(dh != nullptr);
    CHECK(dh->parent == dstParent);
}

// ============================================================================
// P1.4: Save rejects non-primitive meshes (imported glTF/OBJ geometry)
// ============================================================================

TEST_CASE("VS-2 Serializer: save rejects entity with MeshRef but no PrimitiveComponent")
{
    DeterministicUuidProvider provider;
    SceneDocument doc;
    doc.SetUuidProvider(&provider);

    // Create an entity with a MeshRef (simulating imported geometry) but no
    // PrimitiveComponent. This is what a glTF-imported scene looks like.
    MeshData meshData;
    meshData.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    meshData.indices = {0, 1, 2};
    meshData.name = "imported_triangle";
    uint32_t meshIdx = doc.ecs.meshRegistry.AddMesh(std::move(meshData));

    entt::entity e = doc.ecs.registry.create();
    doc.ecs.registry.emplace<NameComponent>(e, "Imported");
    Transform& tf = doc.ecs.registry.emplace<Transform>(e);
    tf.dirty = true;
    doc.ecs.registry.emplace<VisibleComponent>(e);
    doc.ecs.registry.emplace<MeshRef>(e, meshIdx, 0);
    // No PrimitiveComponent — this is imported geometry.
    doc.AssignNewUuid(e);

    auto path = std::filesystem::temp_directory_path() / "rt2_reject_test.rt2scene";
    Error err;
    CHECK_FALSE(SceneSerializer::Save(doc, path, err));
    CHECK(err.code == Error::UnknownPrimitive);
    CHECK(err.detail.find("Imported") != std::string::npos);

    // The file should NOT exist (save was rejected before writing).
    CHECK_FALSE(std::filesystem::exists(path));
    std::filesystem::remove(path);
}

TEST_CASE("VS-2 Serializer: save succeeds when all mesh entities have PrimitiveComponent")
{
    DeterministicUuidProvider provider;
    SceneDocument doc = BuildSliceFixture(&provider);

    auto path = std::filesystem::temp_directory_path() / "rt2_accept_test.rt2scene";
    Error err;
    CHECK(SceneSerializer::Save(doc, path, err));
    CHECK(err.IsOk());
    CHECK(std::filesystem::exists(path));
    std::filesystem::remove(path);
}

TEST_CASE("Phase6B W3 Serializer: every typed script field round-trips")
{
    DeterministicUuidProvider provider;
    SceneDocument doc;
    doc.SetUuidProvider(&provider);
    const entt::entity entity = doc.ecs.registry.create();
    const UUID entityId = doc.AssignNewUuid(entity);
    doc.ecs.registry.emplace<Transform>(entity);
    doc.ecs.registry.emplace<VisibleComponent>(entity);

    ScriptComponent script;
    script.asset.kind = AssetKind::Script;
    script.asset.path = "scripts/all-fields.lua";
    script.asset.sourceKey = "stale-key";
    script.fieldValues["enabled"] = { ScriptFieldType::Bool, true };
    script.fieldValues["count"] = { ScriptFieldType::Int, int64_t{42} };
    script.fieldValues["speed"] = { ScriptFieldType::Float, 7.5 };
    script.fieldValues["label"] = { ScriptFieldType::String, std::string("runner") };
    script.fieldValues["target"] = { ScriptFieldType::Uuid, UUID::Nil() };
    script.fieldValues["offset"] = { ScriptFieldType::Vec3, glm::vec3(1, 2, 3) };
    script.fieldValues["tint"] = { ScriptFieldType::Color, glm::vec3(1, 0.5f, 0.2f) };
    doc.ecs.registry.emplace<ScriptComponent>(entity, script);
    doc.metadata.sourcePath = "C:/project/scenes/source.rt2scene";

    const auto path = std::filesystem::temp_directory_path() / "rt2_w3_all_fields.rt2scene";
    Error err;
    REQUIRE(SceneSerializer::Save(doc, path, err));
    CHECK(doc.ecs.registry.get<ScriptComponent>(entity).asset.sourceKey == "stale-key");

    nlohmann::json saved;
    { std::ifstream in(path); in >> saved; }
    CHECK(saved["version"].get<uint32_t>() == 3);
    CHECK_FALSE(saved["metadata"].contains("sourcePath"));
    CHECK_FALSE(saved["entities"][0]["script"]["asset"].contains("importSettings"));

    SceneDocument loaded;
    loaded.SetUuidProvider(&provider);
    SceneLoadReport report;
    REQUIRE(SceneSerializer::Load(loaded, path, report, err));
    CHECK_FALSE(report.normalizedScriptMetadata);
    CHECK_FALSE(report.droppedScriptFieldData);
    CHECK(loaded.metadata.sourcePath == path);

    const auto loadedEntity = loaded.FindByUuid(entityId);
    REQUIRE(loadedEntity != static_cast<entt::entity>(entt::null));
    const auto& roundTrip = loaded.ecs.registry.get<ScriptComponent>(loadedEntity);
    CHECK(roundTrip.asset.kind == AssetKind::Script);
    CHECK(roundTrip.asset.sourceKey == "lua:asset=" + roundTrip.asset.path);
    REQUIRE(roundTrip.fieldValues.size() == 7);
    CHECK(std::get<bool>(roundTrip.fieldValues.at("enabled").value));
    CHECK(std::get<int64_t>(roundTrip.fieldValues.at("count").value) == 42);
    CHECK(std::get<double>(roundTrip.fieldValues.at("speed").value) == doctest::Approx(7.5));
    CHECK(std::get<std::string>(roundTrip.fieldValues.at("label").value) == "runner");
    CHECK(std::get<UUID>(roundTrip.fieldValues.at("target").value).IsNull());
    CHECK(roundTrip.fieldValues.at("offset").type == ScriptFieldType::Vec3);
    CHECK(roundTrip.fieldValues.at("tint").type == ScriptFieldType::Color);
    std::filesystem::remove(path);
}

TEST_CASE("Phase6B W3 Serializer: bad fields are isolated and reported")
{
    const auto path = WriteTempFile(R"({
      "version":3,
      "metadata":{"name":"repair"},
      "entities":[{
        "uuid":"550e8400-e29b-41d4-a716-446655440000",
        "name":"Scripted","parent":"","visible":true,
        "transform":{"translation":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
        "script":{
          "asset":{"kind":"script","path":"scripts/test.lua","sourceKey":"lua:asset=old.lua"},
          "fields":{
            "good":{"type":"float","value":2.5},
            "unknown":{"type":"matrix","value":[]},
            "wrong":{"type":"vec3","value":[1,2]}
          }
        }
      }],"materials":[],"textures":[],
      "camera":{"position":[0,0,10],"forward":[0,0,-1],"fov":45,"aperture":0,"focusDist":1},
      "envMap":{"path":"","width":0,"height":0}
    })");

    SceneDocument loaded;
    SceneLoadReport report;
    Error err;
    REQUIRE(SceneSerializer::Load(loaded, path, report, err));
    CHECK(report.normalizedScriptMetadata);
    CHECK(report.droppedScriptFieldData);
    REQUIRE(report.fieldDiagnostics.size() == 3);
    CHECK(report.fieldDiagnostics[0].kind == FieldDiagnostic::Kind::NormalizedScriptSourceKey);
    CHECK(report.fieldDiagnostics[1].kind == FieldDiagnostic::Kind::UnknownSerializedType);
    CHECK(report.fieldDiagnostics[2].kind == FieldDiagnostic::Kind::MalformedSerializedValue);

    const auto view = loaded.ecs.registry.view<ScriptComponent>();
    REQUIRE(view.size() == 1);
    const auto& script = view.get<ScriptComponent>(*view.begin());
    REQUIRE(script.fieldValues.size() == 1);
    CHECK(script.fieldValues.count("good") == 1);
    CHECK(script.asset.sourceKey == "lua:asset=scripts/test.lua");

    const auto repairedPath = std::filesystem::temp_directory_path() /
        "rt2_w3_repaired_save.rt2scene";
    REQUIRE(SceneSerializer::Save(loaded, repairedPath, err));
    std::filesystem::remove(path);
    std::filesystem::remove(repairedPath);
}

TEST_CASE("Phase6B W3 Serializer: invalid live script fails atomically")
{
    DeterministicUuidProvider provider;
    SceneDocument doc;
    doc.SetUuidProvider(&provider);
    const entt::entity entity = doc.ecs.registry.create();
    doc.AssignNewUuid(entity);
    doc.ecs.registry.emplace<Transform>(entity);
    doc.ecs.registry.emplace<VisibleComponent>(entity);
    ScriptComponent invalid;
    invalid.asset.kind = AssetKind::Script;
    invalid.asset.path = "scripts/bad.lua";
    invalid.fieldValues["bad"] = { ScriptFieldType::Bool, 1.0 };
    doc.ecs.registry.emplace<ScriptComponent>(entity, invalid);

    const auto path = std::filesystem::temp_directory_path() / "rt2_w3_atomic.rt2scene";
    { std::ofstream out(path); out << "sentinel"; }
    Error err;
    CHECK_FALSE(SceneSerializer::Save(doc, path, err));
    CHECK(err.code == Error::InvalidArgument);
    {
        std::ifstream in(path);
        CHECK(std::string((std::istreambuf_iterator<char>(in)), {}) == "sentinel");
    }
    CHECK(doc.ecs.registry.get<ScriptComponent>(entity).fieldValues.count("bad") == 1);
    std::filesystem::remove(path);
}

TEST_CASE("Phase6B W3 Serializer: invalid UTF-8 and empty field names reject save")
{
    DeterministicUuidProvider provider;
    SceneDocument doc;
    doc.SetUuidProvider(&provider);
    const entt::entity entity = doc.ecs.registry.create();
    doc.AssignNewUuid(entity);
    doc.ecs.registry.emplace<Transform>(entity);
    doc.ecs.registry.emplace<VisibleComponent>(entity);
    ScriptComponent script;
    script.asset.kind = AssetKind::Script;
    script.asset.path = "scripts/bad-text.lua";
    script.fieldValues["label"] = {
        ScriptFieldType::String, std::string(1, static_cast<char>(0xff)) };
    doc.ecs.registry.emplace<ScriptComponent>(entity, script);

    const auto path = std::filesystem::temp_directory_path() /
        "rt2_w3_invalid_utf8.rt2scene";
    { std::ofstream out(path); out << "sentinel"; }
    Error err;
    CHECK_FALSE(SceneSerializer::Save(doc, path, err));
    CHECK(err.code == Error::InvalidArgument);
    CHECK(err.detail.find("UTF-8") != std::string::npos);
    { std::ifstream in(path); CHECK(std::string((std::istreambuf_iterator<char>(in)), {}) == "sentinel"); }

    auto& fields = doc.ecs.registry.get<ScriptComponent>(entity).fieldValues;
    fields.clear();
    fields[""] = { ScriptFieldType::Bool, true };
    err = Error{};
    CHECK_FALSE(SceneSerializer::Save(doc, path, err));
    CHECK(err.code == Error::InvalidArgument);
    CHECK(err.detail.find("must not be empty") != std::string::npos);
    { std::ifstream in(path); CHECK(std::string((std::istreambuf_iterator<char>(in)), {}) == "sentinel"); }
    std::filesystem::remove(path);
}

TEST_CASE("Phase6B W3 Serializer: empty field name is isolated on load")
{
    const auto path = WriteTempFile(R"({
      "version":3,"metadata":{"name":"empty-name"},
      "entities":[{"uuid":"550e8400-e29b-41d4-a716-446655440000",
        "name":"Scripted","parent":"","visible":true,
        "transform":{"translation":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
        "script":{"asset":{"kind":"script","path":"test.lua","sourceKey":"lua:asset=test.lua"},
                  "fields":{"":{"type":"bool","value":true}}}}],
      "materials":[],"textures":[],
      "camera":{"position":[0,0,10],"forward":[0,0,-1],"fov":45,"aperture":0,"focusDist":1},
      "envMap":{"path":"","width":0,"height":0}})");

    SceneDocument loaded;
    SceneLoadReport report;
    Error err;
    REQUIRE(SceneSerializer::Load(loaded, path, report, err));
    CHECK(report.droppedScriptFieldData);
    REQUIRE(report.fieldDiagnostics.size() == 1);
    CHECK(report.fieldDiagnostics[0].kind ==
          FieldDiagnostic::Kind::MalformedSerializedValue);
    const auto view = loaded.ecs.registry.view<ScriptComponent>();
    REQUIRE(view.size() == 1);
    CHECK(view.get<ScriptComponent>(*view.begin()).fieldValues.empty());
    std::filesystem::remove(path);
}

TEST_CASE("Phase6B W3 Serializer: uppercase UUID text normalizes without loss")
{
    const auto path = WriteTempFile(R"({
      "version":3,"metadata":{"name":"uuid-normalize"},
      "entities":[{"uuid":"550e8400-e29b-41d4-a716-446655440000",
        "name":"Scripted","parent":"","visible":true,
        "transform":{"translation":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
        "script":{"asset":{"kind":"script","path":"test.lua","sourceKey":"lua:asset=test.lua"},
                  "fields":{"target":{"type":"uuid","value":"550E8400-E29B-41D4-A716-446655440000"}}}}],
      "materials":[],"textures":[],
      "camera":{"position":[0,0,10],"forward":[0,0,-1],"fov":45,"aperture":0,"focusDist":1},
      "envMap":{"path":"","width":0,"height":0}})");

    SceneDocument loaded;
    SceneLoadReport report;
    Error err;
    REQUIRE(SceneSerializer::Load(loaded, path, report, err));
    CHECK(report.normalizedScriptFieldData);
    CHECK_FALSE(report.droppedScriptFieldData);
    REQUIRE(report.fieldDiagnostics.size() == 1);
    CHECK(report.fieldDiagnostics[0].kind ==
          FieldDiagnostic::Kind::NormalizedSerializedUuid);
    const auto view = loaded.ecs.registry.view<ScriptComponent>();
    REQUIRE(view.size() == 1);
    const auto& target = view.get<ScriptComponent>(*view.begin()).fieldValues.at("target");
    CHECK(std::get<UUID>(target.value).ToString() ==
          "550e8400-e29b-41d4-a716-446655440000");
    std::filesystem::remove(path);
}

TEST_CASE("Phase6B W3 Serializer: unbound script round-trips")
{
    DeterministicUuidProvider provider;
    SceneDocument doc;
    doc.SetUuidProvider(&provider);
    const entt::entity entity = doc.ecs.registry.create();
    const UUID id = doc.AssignNewUuid(entity);
    doc.ecs.registry.emplace<Transform>(entity);
    doc.ecs.registry.emplace<VisibleComponent>(entity);
    ScriptComponent unbound;
    unbound.asset.kind = AssetKind::Script;
    doc.ecs.registry.emplace<ScriptComponent>(entity, unbound);

    const auto path = std::filesystem::temp_directory_path() / "rt2_w3_unbound.rt2scene";
    Error err;
    REQUIRE(SceneSerializer::Save(doc, path, err));
    SceneDocument loaded;
    REQUIRE(SceneSerializer::Load(loaded, path, err));
    const auto loadedEntity = loaded.FindByUuid(id);
    REQUIRE(loadedEntity != static_cast<entt::entity>(entt::null));
    const auto& script = loaded.ecs.registry.get<ScriptComponent>(loadedEntity);
    CHECK(script.asset.path.empty());
    CHECK(script.asset.sourceKey.empty());
    CHECK(script.fieldValues.empty());
    std::filesystem::remove(path);
}

TEST_CASE("Phase6B W3 Serializer: Save As rebases every durable path")
{
    const auto root = std::filesystem::temp_directory_path() / "rt2_w3_rebase";
    const auto oldRoot = root / "old";
    const auto newRoot = root / "new";
    std::filesystem::create_directories(oldRoot);
    std::filesystem::create_directories(newRoot);

    DeterministicUuidProvider provider;
    SceneDocument doc;
    doc.SetUuidProvider(&provider);
    doc.metadata.sourcePath = oldRoot / "source.rt2scene";
    doc.environment.ref.path = "env/night.exr";
    const auto entity = doc.ecs.registry.create();
    doc.AssignNewUuid(entity);
    doc.ecs.registry.emplace<Transform>(entity);
    doc.ecs.registry.emplace<VisibleComponent>(entity);
    ImportedMeshSourceComponent imported;
    imported.model.kind = AssetKind::Model;
    imported.model.path = "assets/model.glb";
    imported.model.sourceKey = "gltf:scene=0:node=0:mesh=0:prim=0";
    doc.ecs.registry.emplace<ImportedMeshSourceComponent>(entity, imported);
    ScriptComponent script;
    script.asset.kind = AssetKind::Script;
    script.asset.path = "scripts/move.lua";
    doc.ecs.registry.emplace<ScriptComponent>(entity, script);

    const auto target = newRoot / "saved.rt2scene";
    Error err;
    REQUIRE(SceneSerializer::Save(doc, target, err));
    nlohmann::json saved;
    { std::ifstream in(target); in >> saved; }
    CHECK(saved["envMap"]["path"] == "../old/env/night.exr");
    CHECK(saved["entities"][0]["importedSource"]["path"] ==
          "../old/assets/model.glb");
    CHECK(saved["entities"][0]["script"]["asset"]["path"] ==
          "../old/scripts/move.lua");
    CHECK(saved["entities"][0]["script"]["asset"]["sourceKey"] ==
          "lua:asset=../old/scripts/move.lua");
    std::filesystem::remove_all(root);
}

TEST_CASE("Phase6B W3 Serializer: field insertion order does not affect bytes")
{
    auto build = [](bool reverse) {
        SceneDocument doc;
        const auto entity = doc.ecs.registry.create();
        const UUID id = UUID::Parse("550e8400-e29b-41d4-a716-446655440000");
        const bool assigned = doc.AssignKnownUuid(entity, id);
        (void)assigned;
        doc.ecs.registry.emplace<Transform>(entity);
        doc.ecs.registry.emplace<VisibleComponent>(entity);
        ScriptComponent script;
        script.asset.kind = AssetKind::Script;
        script.asset.path = "scripts/order.lua";
        if (reverse)
        {
            script.fieldValues["zeta"] = { ScriptFieldType::Int, int64_t{2} };
            script.fieldValues["alpha"] = { ScriptFieldType::Bool, true };
        }
        else
        {
            script.fieldValues["alpha"] = { ScriptFieldType::Bool, true };
            script.fieldValues["zeta"] = { ScriptFieldType::Int, int64_t{2} };
        }
        doc.ecs.registry.emplace<ScriptComponent>(entity, script);
        return doc;
    };

    auto first = build(false);
    auto second = build(true);
    const auto dir = std::filesystem::temp_directory_path();
    const auto a = dir / "rt2_w3_order_a.rt2scene";
    const auto b = dir / "rt2_w3_order_b.rt2scene";
    Error err;
    REQUIRE(SceneSerializer::Save(first, a, err));
    REQUIRE(SceneSerializer::Save(second, b, err));
    std::ifstream inA(a, std::ios::binary), inB(b, std::ios::binary);
    const std::string bytesA((std::istreambuf_iterator<char>(inA)), {});
    const std::string bytesB((std::istreambuf_iterator<char>(inB)), {});
    CHECK(bytesA == bytesB);
    inA.close();
    inB.close();
    std::filesystem::remove(a);
    std::filesystem::remove(b);
}

#ifdef _WIN32
TEST_CASE("Phase6B W3 Serializer: cross-volume absolute path remains absolute")
{
    SceneDocument doc;
    const auto entity = doc.ecs.registry.create();
    REQUIRE(doc.AssignKnownUuid(
        entity, UUID::Parse("550e8400-e29b-41d4-a716-446655440000")));
    doc.ecs.registry.emplace<Transform>(entity);
    doc.ecs.registry.emplace<VisibleComponent>(entity);
    ScriptComponent script;
    script.asset.kind = AssetKind::Script;
    script.asset.path = "Z:/external/move.lua";
    doc.ecs.registry.emplace<ScriptComponent>(entity, script);

    const auto path = std::filesystem::temp_directory_path() /
        "rt2_w3_cross_volume.rt2scene";
    Error err;
    REQUIRE(SceneSerializer::Save(doc, path, err));
    nlohmann::json saved;
    { std::ifstream in(path); in >> saved; }
    CHECK(saved["entities"][0]["script"]["asset"]["path"] ==
          "Z:/external/move.lua");
    std::filesystem::remove(path);
}
#endif

// ============================================================================
// Phase 7 W0: no relativizable absolute path may leak into a saved .rt2scene.
//
// The cross-volume test above documents a deliberate escape hatch: an asset on
// a different Windows drive cannot be made relative, so it is stored absolute
// on purpose. This test guards the *other* case — assets that live under the
// scene file's directory tree MUST be relativized. It walks EVERY string value
// in the saved JSON recursively and asserts none is an absolute-looking path,
// so a future change that adds a new path-bearing serialized field and forgets
// to route it through RebasePath is caught without having to enumerate fields.
//
// Why a recursive walk is safe (no false positives):
//   - sourceKey forms are "gltf:scene=0:...", "obj:whole-model",
//     "lua:asset=<rel-path>", "obj:shape=...". Their ':' is at index 4, 3, 4,
//     3 — never index 1 — so the drive-letter test does not trip. They do not
//     start with '/' or '\\'.
//   - sourceMaterialKey is "<sourceKey>:material" — same property.
//   - UUIDs are hyphenated hex, no drive letter, no leading slash.
//   - AssetKind names ("model","texture","environment","script","unknown"),
//     PrimitiveKind names, field type names, field names: none look absolute.
//
// Audit (grounded against SceneSerializer.cpp at 2ebf621):
//   - importedSource.model.path   -> rebased at SceneSerializer.cpp:586
//   - script.asset.path           -> rebased at SceneSerializer.cpp:643
//   - envMap.path                 -> rebased at SceneSerializer.cpp:1262
//   - metadata                    -> only "name" (SceneSerializer.cpp:1226-1230)
//   - textures                    -> always an empty array (SceneSerializer.cpp:1254)
//   - sourceKey / sourceMaterialKey -> not filesystem paths (see above)
// These three path fields are the complete current set; the walker makes the
// test independent of that enumeration so it stays correct as fields are added.
//
// The fixture places every asset under the scene directory, so nothing in it is
// legitimately absolute. If a field that MUST stay absolute is ever added,
// exempt it explicitly by JSON path here with a comment, rather than weakening
// the predicate.
// ============================================================================
TEST_CASE("Phase7 W0: relativizable asset paths are never stored absolute")
{
    // Build a scene whose assets all live under the scene file's own directory,
    // but store them as absolute paths in the document (simulating an import
    // that produced absolute refs). Save must relativize every one.
    const auto root = std::filesystem::temp_directory_path() / "rt2_w0_audit";
    const auto sceneDir = root / "scene";
    const auto assetsDir = sceneDir / "assets";
    const auto scriptsDir = sceneDir / "scripts";
    const auto envDir = sceneDir / "env";
    std::filesystem::create_directories(assetsDir);
    std::filesystem::create_directories(scriptsDir);
    std::filesystem::create_directories(envDir);

    DeterministicUuidProvider provider;
    SceneDocument doc;
    doc.SetUuidProvider(&provider);

    const auto entity = doc.ecs.registry.create();
    doc.AssignNewUuid(entity);
    doc.ecs.registry.emplace<Transform>(entity);
    doc.ecs.registry.emplace<VisibleComponent>(entity);

    // Imported model: absolute path under the scene directory.
    ImportedMeshSourceComponent imported;
    imported.model.kind = AssetKind::Model;
    imported.model.path = (assetsDir / "cube.glb").generic_string();
    imported.model.sourceKey = "gltf:scene=0:node=0:mesh=0:prim=0";
    doc.ecs.registry.emplace<ImportedMeshSourceComponent>(entity, imported);

    // Script: absolute path under the scene directory.
    ScriptComponent script;
    script.asset.kind = AssetKind::Script;
    script.asset.path = (scriptsDir / "move.lua").generic_string();
    doc.ecs.registry.emplace<ScriptComponent>(entity, script);

    // Environment: absolute path under the scene directory.
    doc.environment.ref.path = (envDir / "night.exr").generic_string();

    const auto target = sceneDir / "w0_audit.rt2scene";
    Error err;
    REQUIRE(SceneSerializer::Save(doc, target, err));
    REQUIRE(err.IsOk());

    nlohmann::json saved;
    { std::ifstream in(target); in >> saved; }

    // A path string is "portable relative" iff it has no drive letter (e.g.
    // "C:") and does not start with a slash. This is exactly what RebasePath
    // produces when relativization succeeds.
    auto isPortableRelative = [](const std::string& s) -> bool
    {
        if (s.empty()) return true; // empty is fine (no asset)
        if (s.size() >= 2 && s[1] == ':') return false; // drive letter
        if (s.front() == '/' || s.front() == '\\') return false; // rooted
        return true;
    };

    // Recursively walk every string in the saved JSON. Collect the JSON path
    // of any absolute-looking string so the failure message points at the
    // offending field, not just its value.
    std::vector<std::string> offenders;
    std::function<void(const nlohmann::json&, const std::string&)> walk =
        [&](const nlohmann::json& node, const std::string& path)
    {
        switch (node.type())
        {
        case nlohmann::json::value_t::object:
            for (auto it = node.begin(); it != node.end(); ++it)
                walk(it.value(), path.empty() ? it.key() : path + "." + it.key());
            break;
        case nlohmann::json::value_t::array:
            for (size_t i = 0; i < node.size(); ++i)
                walk(node[i], path + "[" + std::to_string(i) + "]");
            break;
        case nlohmann::json::value_t::string:
            if (!isPortableRelative(node.get<std::string>()))
                offenders.push_back(path + " = " + node.get<std::string>());
            break;
        default:
            break;
        }
    };
    walk(saved, "");

    for (const auto& o : offenders)
        CHECK_MESSAGE(false, "absolute path leaked into saved scene: " << o);
    CHECK_MESSAGE(offenders.empty(),
        "no absolute paths expected; saw " << offenders.size());

    // Defensive: metadata must not carry a sourcePath (the Phase 6 leak). The
    // walker would already catch it, but this names the regression explicitly.
    CHECK_FALSE(saved["metadata"].contains("sourcePath"));

    std::filesystem::remove_all(root);
}
