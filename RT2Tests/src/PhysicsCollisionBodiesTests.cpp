// ============================================================================
// PhysicsCollisionBodiesTests — Bullet T4 collision + bodies + authoring.
//
// Permanent discriminating RED/GREEN coverage for ticket
// t4-collision-bodies-authoring: live host-owned collision provider/context;
// shared CPU OBJ/glTF decode and typed failures; immutable cache; sphere /
// box / convex / static-triangle shapes and bodies; declared units, inertia,
// margins, and CCD; Static/Kinematic/Dynamic authority with batched
// presentation sync; CPU authoring APIs with exact Undo/Redo; minimal authoring
// surface invariants; prefab-member refusal.
//
// CPU-only by design: no Vulkan, ImGui or Walnut (same guards as the T3 unit).
// Every Play-based case runs through RuntimeSceneController::Play with a real
// PhysicsCollisionAssetProvider over temp-dir assets; every refusal asserts
// the atomic candidate-commit contract (Edit, zero accumulator, no runtime,
// no world, no handles, no bridge traffic, UUID-named typed Error).
// ============================================================================

#include <doctest/doctest.h>
#include "AssetDatabase.h"
#include "PhysicsInspectorState.h"
#include "PhysicsWorld.h"
#include "IPhysicsCollisionAssetProvider.h"
#include "PhysicsCollisionAssetProvider.h"
#include "PhysicsCollisionGeometry.h"
#include "RuntimeSceneController.h"
#include "RuntimeLifecycleObserver.h"
#include "SceneGraph.h"
#include "SceneManager.h"
#include "ScriptSystem.h"
#include "TransformEditing.h"
#include "EditorCommandHistory.h"
#include "EditorPropertyCommands.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "ISceneRenderBridge.h"
#include "GPUSceneData.h"
#include "core/Error.h"
#include "core/UUID.h"

#ifdef IMGUI_VERSION
#error "T4 boundary: physics collision tests must not import ImGui"
#endif

#ifdef VK_HEADER_VERSION
#error "T4 boundary: physics collision tests must not import Vulkan"
#endif

#if __has_include("imgui.h")
#error "T4 boundary: imgui.h must not be reachable from physics collision units"
#endif

#if __has_include("vulkan/vulkan.h")
#error "T4 boundary: vulkan headers must not be reachable from physics collision units"
#endif

#if __has_include("Walnut/Application.h")
#error "T4 boundary: Walnut headers must not be reachable from physics collision units"
#endif

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <string>

using namespace rt2::core;

namespace {

class T4NullBridge final : public ISceneRenderBridge
{
public:
    int fullSyncCalls      = 0;
    int transformSyncCalls = 0;
    int resetTemporalCalls = 0;
    int renderRequests     = 0;

    void FullSync(GPUSceneData&) override        { ++fullSyncCalls; }
    void MaterialSync(GPUSceneData&) override    {}
    void TransformSync(GPUSceneData&) override   { ++transformSyncCalls; }
    void ResetTemporalState() override           { ++resetTemporalCalls; }
    void RequestRender() override                { ++renderRequests; }

    void Reset()
    {
        fullSyncCalls = 0;
        transformSyncCalls = 0;
        resetTemporalCalls = 0;
        renderRequests = 0;
    }

    bool Quiet() const
    {
        return fullSyncCalls == 0 && transformSyncCalls == 0 &&
               resetTemporalCalls == 0 && renderRequests == 0;
    }
};

class T4NoopObserver final : public IRuntimeLifecycleObserver
{
public:
    int starts = 0;
    int stops  = 0;
    void OnSceneStart(const SceneDocument&) override { ++starts; }
    void OnSceneStop(const SceneDocument&) override  { ++stops; }
};

struct T4Fixture
{
    DeterministicUuidProvider ids;
    SceneManager manager;

    T4Fixture() { manager.SetUuidProvider(&ids); }

    UUID Create(const char* name)
    {
        return manager.CreateEmpty(name).affectedEntities.front();
    }

    entt::entity Handle(const UUID& uuid) const
    {
        return manager.FindEntityByUuid(uuid);
    }

    entt::registry& Registry() { return manager.GetECS().registry; }
    const SceneDocument& Authoring() const { return manager.AuthoringDoc(); }
};

// Shared refusal contract (mirrors the T3 helper): failed Play leaves Edit,
// zero accumulator, no runtime, no world, no handles, no bridge traffic, no
// script callbacks, and a typed Error naming the entity UUID.
void T4CheckCleanRefusal(RuntimeSceneController& ctrl, const T4NullBridge& bridge,
                         const T4NoopObserver& obs, const Error& err,
                         const UUID& uuid)
{
    CHECK_FALSE(err.IsOk());
    CHECK(err.path == uuid.ToString());
    CHECK(err.detail.find(uuid.ToString()) != std::string::npos);
    CHECK(ctrl.GetState() == SceneRunState::Edit);
    CHECK(ctrl.TryGetRuntimeScene() == nullptr);
    CHECK(ctrl.TryGetPhysicsWorld() == nullptr);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
    CHECK(ctrl.DebugAccumulator() == doctest::Approx(0.0f));
    CHECK(bridge.Quiet());
    CHECK(obs.starts == 0);
    CHECK(obs.stops == 0);
}

PhysicsBodyComponent T4DynamicBody(float mass = 1.0f)
{
    PhysicsBodyComponent body;
    body.kind = PhysicsBodyKind::Dynamic;
    body.mass = mass;
    body.layer = PhysicsLayer::Dynamic;
    body.mask = PhysicsLayer::WorldStatic | PhysicsLayer::Mechanism;
    return body;
}

PhysicsBodyComponent T4StaticBody()
{
    PhysicsBodyComponent body;
    body.kind = PhysicsBodyKind::Static;
    body.mass = 0.0f;
    body.layer = PhysicsLayer::WorldStatic;
    body.mask = PhysicsLayer::Dynamic | PhysicsLayer::WorldStatic |
                PhysicsLayer::Mechanism;
    return body;
}

PhysicsShapeComponent T4SphereShape(float radius = 0.5f)
{
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::Sphere;
    shape.radius = radius;
    return shape;
}

PhysicsShapeComponent T4BoxShape(float half = 0.5f)
{
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::Box;
    shape.halfExtents = {half, half, half};
    return shape;
}

AssetReference T4ModelRef(const std::string& path, const std::string& key)
{
    AssetReference ref;
    ref.kind = AssetKind::Model;
    ref.path = path;
    ref.sourceKey = key;
    return ref;
}

void T4WriteText(const std::filesystem::path& path, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    REQUIRE_MESSAGE(!ec, "T4 fixture directory creation failed");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE_MESSAGE(out.is_open(), "T4 fixture file open failed");
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    REQUIRE_MESSAGE(out.good(), "T4 fixture file write failed");
}

// Unit cube OBJ (8 verts, 12 tris): convex-hull and static-tri source.
std::string T4CubeObj(float s = 1.0f)
{
    char buf[2048];
    std::snprintf(buf, sizeof(buf),
        "v %g %g %g\nv %g %g %g\nv %g %g %g\nv %g %g %g\n"
        "v %g %g %g\nv %g %g %g\nv %g %g %g\nv %g %g %g\n",
        -s,-s,-s, s,-s,-s, s,s,-s, -s,s,-s,
        -s,-s,s, s,-s,s, s,s,s, -s,s,s);
    std::string obj(buf);
    obj += "f 1 2 3\nf 1 3 4\nf 5 6 7\nf 5 7 8\n"
           "f 1 2 6\nf 1 6 5\nf 2 3 7\nf 2 7 6\n"
           "f 3 4 8\nf 3 8 7\nf 4 1 5\nf 4 5 8\n";
    return obj;
}

struct T4TempAssets
{
    std::filesystem::path dir;
    T4TempAssets()
    {
        dir = std::filesystem::temp_directory_path() / "t4_collision_tests";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        REQUIRE_MESSAGE(!ec, "T4 fixture cleanup failed");
        std::filesystem::create_directories(dir, ec);
        REQUIRE_MESSAGE(!ec, "T4 fixture directory creation failed");
    }
    ~T4TempAssets()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::string MakeCube(const std::string& name, float s = 1.0f)
    {
        T4WriteText(dir / name, T4CubeObj(s));
        return name;
    }
};

// Minimal two-primitive glTF: tri0 at the origin, tri1 offset +10 on X.
// Separate POSITION/INDEX accessors per primitive so the two sourceKeys
// address isolated subresources of the SAME file.
std::string T4Base64(const unsigned char* data, size_t bytes)
{
    static const char* k =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < bytes; i += 3)
    {
        const size_t n = bytes - i < 3 ? bytes - i : 3;
        unsigned v = 0;
        for (size_t j = 0; j < n; ++j)
            v |= (unsigned)data[i + j] << (8 * (2 - j));
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += n > 1 ? k[(v >> 6) & 63] : '=';
        out += n > 2 ? k[v & 63] : '=';
    }
    return out;
}

std::string T4TwoTriGltf()
{
    // 2 tris x (3 verts x 3 floats) = 72 bytes positions, then 12 bytes idx.
    // Each primitive's indices are local to its own POSITION accessor. The
    // raw buffer interleaves per primitive (pos0, idx0, pos1, idx1) to match
    // the bufferViews below.
    float pos0[9] = {0,0,0, 1,0,0, 0,1,0};
    float pos1[9] = {10,0,0, 11,0,0, 10,1,0};
    uint16_t idx0[3] = {0,1,2};
    uint16_t idx1[3] = {0,1,2};
    const unsigned char pad[2] = {0, 0};
    std::string raw;
    raw.append((const char*)pos0, sizeof(pos0));
    raw.append((const char*)idx0, sizeof(idx0));
    raw.append((const char*)pad, sizeof(pad)); // keep pos1 4-aligned per spec
    raw.append((const char*)pos1, sizeof(pos1));
    raw.append((const char*)idx1, sizeof(idx1));
    const std::string b64 =
        T4Base64((const unsigned char*)raw.data(), raw.size());
    char json[4096];
    std::snprintf(json, sizeof(json),
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":0},\"indices\":1},"
        "{\"attributes\":{\"POSITION\":2},\"indices\":3}]} ],"
        "\"buffers\":[{\"byteLength\":86,\"uri\":\"data:application/octet-stream;base64,%s\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":44,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":80,\"byteLength\":6}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"max\":[1,1,0],\"min\":[0,0,0]},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"max\":[11,1,0],\"min\":[10,0,0]},"
        "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}]}",
        b64.c_str());
    return std::string(json);
}

} // namespace

TEST_CASE("T4 GREEN_CollisionCacheDedup: identical keys decode once and sourceKeys stay isolated")
{
    T4TempAssets assets;
    assets.MakeCube("cube.obj");
    T4WriteText(assets.dir / "two_tris.gltf", T4TwoTriGltf());

    PhysicsCollisionAssetProvider provider;
    provider.SetContext(AssetResolutionContext{assets.dir, nullptr});
    DeterministicUuidProvider ids;
    const UUID entity = ids.CreateV4();

    // Identical keys decode once: same pointer, one decode, one entry.
    AssetReference cube = T4ModelRef("cube.obj", "obj:whole-model");
    auto first = provider.GetCollisionGeometry(cube, entity, "Cube");
    REQUIRE(first.IsOk());
    auto second = provider.GetCollisionGeometry(cube, entity, "Cube");
    REQUIRE(second.IsOk());
    CHECK(first.value == second.value);
    CHECK(first.value->vertices.size() == 24);
    CHECK(first.value->indices.size() == 36);
    CHECK(provider.DecodeCount() == 1);
    CHECK(provider.CacheEntryCount() == 1);

    // Same file, two sourceKeys: isolated entries with isolated contents.
    AssetReference tri0 = T4ModelRef("two_tris.gltf",
        "gltf:scene=0:node=0:mesh=0:primitive=0");
    AssetReference tri1 = T4ModelRef("two_tris.gltf",
        "gltf:scene=0:node=0:mesh=0:primitive=1");
    auto got0 = provider.GetCollisionGeometry(tri0, entity, "Tri");
    auto got1 = provider.GetCollisionGeometry(tri1, entity, "Tri");
    REQUIRE(got0.IsOk());
    REQUIRE(got1.IsOk());
    CHECK(got0.value != got1.value);
    REQUIRE(got0.value->vertices.size() == 9);
    REQUIRE(got1.value->vertices.size() == 9);
    CHECK(got0.value->vertices[0] == doctest::Approx(0.0f));
    CHECK(got1.value->vertices[0] == doctest::Approx(10.0f));
    CHECK(provider.DecodeCount() == 3);
    CHECK(provider.CacheEntryCount() == 3);
}

TEST_CASE("T4 RED_HostileGltfRefused: forged relationships and hostile accessor metadata fail loudly")
{
    // Each hostile document is decoded directly (no provider): every case
    // must return a typed error quickly — never allocate from hostile counts,
    // never read outside the buffer, and never select forged geometry.
    T4TempAssets assets;
    const std::string valid = T4TwoTriGltf();
    auto writeVariant = [&](const std::string& name, const std::string& json) {
        T4WriteText(assets.dir / name, json);
        return assets.dir / name;
    };
    auto replace = [](std::string doc, const std::string& from,
                      const std::string& to) {
        const size_t at = doc.find(from);
        REQUIRE(at != std::string::npos);
        return doc.substr(0, at) + to + doc.substr(at + from.size());
    };
    ImportSettings settings;
    auto expectRefusal = [&](const std::filesystem::path& path,
                             const std::string& key, Error::Code code) {
        const auto r =
            DecodeCollisionGeometry(path, key, settings);
        CHECK_FALSE(r.IsOk());
        if (!r.IsOk())
            CHECK(r.error.code == code);
    };
    const std::string key0 = "gltf:scene=0:node=0:mesh=0:primitive=0";
    const std::string key1 = "gltf:scene=0:node=0:mesh=0:primitive=1";

    // Forged scene relationship: node exists but is not in the named scene.
    expectRefusal(writeVariant("noroot.gltf",
                    replace(valid, "\"scenes\":[{\"nodes\":[0]}]",
                            "\"scenes\":[{\"nodes\":[]}]")),
                  key0, Error::InvalidArgument);
    // Forged mesh relationship: node carries mesh 0, key names mesh 1 (which
    // exists, so only the identity check can refuse).
    std::string twoMesh = replace(
        valid, "\"meshes\":[{\"primitives\":[",
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}],\"extra\":0},{\"primitives\":[");
    expectRefusal(writeVariant("wrongmesh.gltf", twoMesh),
                  "gltf:scene=0:node=0:mesh=1:primitive=0",
                  Error::InvalidArgument);
    // Non-triangle primitive mode.
    expectRefusal(writeVariant("points.gltf",
                    replace(valid, "{\"attributes\":{\"POSITION\":0},\"indices\":1}",
                            "{\"attributes\":{\"POSITION\":0},\"indices\":1,\"mode\":0}")),
                  key0, Error::InvalidArgument);
    // Sparse accessor (unsupported, never silently ignored).
    expectRefusal(writeVariant("sparse.gltf",
                    replace(valid, "\"max\":[1,1,0],\"min\":[0,0,0]}",
                            "\"max\":[1,1,0],\"min\":[0,0,0],\"sparse\":{\"count\":1,\"indices\":{\"bufferView\":1,\"byteOffset\":0,\"componentType\":5123},\"values\":{\"bufferView\":1,\"byteOffset\":0}}}")),
                  key0, Error::InvalidArgument);
    // Overflowed count: capped before allocation (returns fast, no giant vector).
    expectRefusal(writeVariant("hugecount.gltf",
                    replace(valid, "{\"bufferView\":0,\"componentType\":5126,\"count\":3,",
                            "{\"bufferView\":0,\"componentType\":5126,\"count\":1000000000,")),
                  key0, Error::InvalidArgument);
    // Short buffer view: accessor span escapes the view.
    expectRefusal(writeVariant("shortview.gltf",
                    replace(valid, "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}",
                            "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":10}")),
                  key0, Error::Parse);
    // Undersized stride: byteStride smaller than one element.
    expectRefusal(writeVariant("shortstride.gltf",
                    replace(valid, "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}",
                            "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,\"byteStride\":4}")),
                  key0, Error::Parse);
    // Misaligned accessor base.
    expectRefusal(writeVariant("misaligned.gltf",
                    replace(valid, "{\"buffer\":0,\"byteOffset\":44,\"byteLength\":36}",
                            "{\"buffer\":0,\"byteOffset\":46,\"byteLength\":36}")),
                  key1, Error::Parse);
    // Index cardinality: 4 indices are not triangle soup (dedicated
    // builder: prim1 carries four uint16 indices over a matching buffer).
    {
        float pos0[9] = {0,0,0, 1,0,0, 0,1,0};
        float pos1[9] = {10,0,0, 11,0,0, 10,1,0};
        uint16_t idx0[3] = {0,1,2};
        uint16_t idx1[4] = {0,1,2,0};
        const unsigned char pad[2] = {0, 0};
        std::string raw;
        raw.append((const char*)pos0, sizeof(pos0));
        raw.append((const char*)idx0, sizeof(idx0));
        raw.append((const char*)pad, sizeof(pad));
        raw.append((const char*)pos1, sizeof(pos1));
        raw.append((const char*)idx1, sizeof(idx1));
        REQUIRE(raw.size() == 88);
        const std::string b64 =
            T4Base64((const unsigned char*)raw.data(), raw.size());
        char json[4096];
        std::snprintf(json, sizeof(json),
            "{\"asset\":{\"version\":\"2.0\"},"
            "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
            "\"meshes\":[{\"primitives\":["
            "{\"attributes\":{\"POSITION\":0},\"indices\":1},"
            "{\"attributes\":{\"POSITION\":2},\"indices\":3}]} ],"
            "\"buffers\":[{\"byteLength\":88,\"uri\":\"data:application/octet-stream;base64,%s\"}],"
            "\"bufferViews\":["
            "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
            "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":8},"
            "{\"buffer\":0,\"byteOffset\":44,\"byteLength\":36},"
            "{\"buffer\":0,\"byteOffset\":80,\"byteLength\":8}],"
            "\"accessors\":["
            "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
            "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
            "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
            "{\"bufferView\":3,\"componentType\":5123,\"count\":4,\"type\":\"SCALAR\"}]}",
            b64.c_str());
        expectRefusal(writeVariant("cardinality.gltf", json), key1,
                      Error::Parse);
    }
}

TEST_CASE("T4 GREEN_CollisionCacheRebuilds: changed files rebuild for the next Play")
{
    T4TempAssets assets;
    assets.MakeCube("morph.obj", 1.0f);

    PhysicsCollisionAssetProvider provider;
    provider.SetContext(AssetResolutionContext{assets.dir, nullptr});
    DeterministicUuidProvider ids;
    const UUID entity = ids.CreateV4();
    AssetReference ref = T4ModelRef("morph.obj", "obj:whole-model");

    auto before = provider.GetCollisionGeometry(ref, entity, "Morph");
    REQUIRE(before.IsOk());
    const float v0 = before.value->vertices[0];
    CHECK(provider.DecodeCount() == 1);

    // Same-size rewrite (single-digit coordinates keep every line width, so
    // the byte size is identical) with the mtime forced back to the original
    // stamp: mtime/size comparison alone would serve the stale entry. The
    // raw-byte fingerprint still observes the change and rebuilds.
    std::error_code ec;
    const auto originalMtime = std::filesystem::last_write_time(
        assets.dir / "morph.obj", ec);
    REQUIRE(!ec);
    const auto originalSize = std::filesystem::file_size(
        assets.dir / "morph.obj", ec);
    REQUIRE(!ec);
    T4WriteText(assets.dir / "morph.obj", T4CubeObj(2.0f));
    // Same byte size, same timestamp: only the content fingerprint observes
    // the rewrite.
    REQUIRE(std::filesystem::file_size(assets.dir / "morph.obj", ec) ==
            originalSize);
    REQUIRE(!ec);
    std::filesystem::last_write_time(assets.dir / "morph.obj", originalMtime, ec);
    REQUIRE(!ec);

    auto after = provider.GetCollisionGeometry(ref, entity, "Morph");
    REQUIRE(after.IsOk());
    CHECK(provider.DecodeCount() == 2);
    CHECK(provider.CacheEntryCount() == 1);
    CHECK(after.value->vertices[0] == doctest::Approx(2.0f * v0));
}

TEST_CASE("T4 GREEN_CollisionCacheRetarget: same asset ID on a new path decodes anew")
{
    T4TempAssets assets;
    assets.MakeCube("a.obj", 1.0f);
    assets.MakeCube("b.obj", 3.0f);

    DeterministicUuidProvider ids;
    const UUID assetId = ids.CreateV4();
    const UUID entity = ids.CreateV4();
    // Two database snapshots: the same ID first identifies a.obj, then (as
    // after a host database refresh) b.obj. Swapping the provider context
    // mirrors the host refreshing its value-held context before the next
    // Play; the canonical path in the key misses the old entry instead of
    // reusing stale geometry.
    AssetDatabase dbA, dbB;
    std::vector<AssetDatabaseDiagnostic> dbDiags;
    AssetRecord recordA;
    recordA.assetId = assetId;
    recordA.sourcePath = "a.obj";
    recordA.identityAuthority = AssetIdentityAuthority::Reference;
    dbA.AddOrUpdate(recordA, dbDiags);
    AssetRecord recordB;
    recordB.assetId = assetId;
    recordB.sourcePath = "b.obj";
    recordB.identityAuthority = AssetIdentityAuthority::Reference;
    dbB.AddOrUpdate(recordB, dbDiags);

    PhysicsCollisionAssetProvider provider;
    provider.SetContext(AssetResolutionContext{assets.dir, &dbA});
    AssetReference ref = T4ModelRef("a.obj", "obj:whole-model");
    ref.assetId = assetId;

    auto first = provider.GetCollisionGeometry(ref, entity, "Retarget");
    REQUIRE(first.IsOk());
    CHECK(first.value->vertices[0] == doctest::Approx(-1.0f));
    CHECK(provider.DecodeCount() == 1);

    provider.SetContext(AssetResolutionContext{assets.dir, &dbB});
    auto second = provider.GetCollisionGeometry(ref, entity, "Retarget");
    REQUIRE(second.IsOk());
    CHECK(second.value->vertices[0] == doctest::Approx(-3.0f));
    CHECK(second.value != first.value);
    CHECK(provider.DecodeCount() == 2);
    CHECK(provider.CacheEntryCount() == 2);
}

TEST_CASE("T4 GREEN_CcdFastSphereStops: authored CCD stops the fast sphere")
{
    // Thin static wall (0.2 thick on X); a 100 u/s sphere advances ~1.667
    // units per tick, so a discrete sphere jumps the overlap window while the
    // CCD sphere (threshold 0.25 = 0.5r, swept 0.4 = 0.8r via the preset)
    // stops at the face. Five ticks, restitution zero, gravity sag ~3cm.
    auto runCase = [](bool ccdEnabled) {
        T4Fixture f;
        const UUID wall = f.Create("Wall");
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(wall), T4StaticBody());
        f.Registry().emplace<PhysicsShapeComponent>(f.Handle(wall), T4BoxShape(0.5f));
        // Thin the wall on X only: half extents (0.1, 5, 5).
        f.Registry().get<PhysicsShapeComponent>(f.Handle(wall)).halfExtents =
            {0.1f, 5.0f, 5.0f};

        const UUID ball = f.Create("Ball");
        PhysicsBodyComponent body = T4DynamicBody(1.0f);
        if (ccdEnabled)
        {
            float threshold = 0.0f, swept = 0.0f;
            PhysicsWorld::FastSphereCcdPreset(0.5f, threshold, swept);
            CHECK(threshold == doctest::Approx(0.25f));
            CHECK(swept == doctest::Approx(0.4f));
            body.ccdEnabled = true;
            body.ccdMotionThreshold = threshold;
            body.ccdSweptRadius = swept;
        }
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(ball), body);
        f.Registry().emplace<PhysicsShapeComponent>(f.Handle(ball), T4SphereShape(0.5f));
        f.Registry().get<Transform>(f.Handle(ball)).translation = {-4.0f, 0.0f, 0.0f};

        T4NullBridge bridge;
        Error err;
        RuntimeSceneController ctrl;
        REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

        PhysicsWorld* world = ctrl.TryGetPhysicsWorldMut();
        REQUIRE(world != nullptr);
        REQUIRE(world->SetBodyLinearVelocity(ball, {100.0f, 0.0f, 0.0f}));
        for (int i = 0; i < 5; ++i)
            ctrl.Update(kFixedDt, bridge);

        const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
        REQUIRE(runtime != nullptr);
        const auto e = runtime->FindByUuid(ball);
        const bool resolved = (e != entt::null);
        REQUIRE(resolved);
        const float x =
            runtime->ecs.registry.get<Transform>(e).translation.x;
        ctrl.Stop(f.Authoring(), bridge);
        return x;
    };

    const float discreteX = runCase(false);
    // Discrete tunnels past the thin wall (spike anchor: tunnels to x=3.0).
    CHECK(discreteX > 0.5f);
    const float ccdX = runCase(true);
    // CCD stops at the face (spike anchor: stops at x=-0.055 scale).
    CHECK(ccdX < -0.3f);
    CHECK(ccdX > -2.0f);
}

TEST_CASE("T4 GREEN_KinematicPlatformDrags: commanded platform motion derives velocity and drags contact")
{
    // Discriminator for derived kinematic velocity (not just final pose):
    // the platform pose is published through the motion state so Bullet's
    // saveKinematicState derives linear velocity, which friction then
    // transfers to a resting Dynamic crate. Zeroing the interpolation
    // transform at push time would drag nothing.
    T4Fixture f;
    const UUID platform = f.Create("Platform");
    PhysicsBodyComponent pBody;
    pBody.kind = PhysicsBodyKind::Kinematic;
    pBody.mass = 0.0f;
    pBody.friction = 1.0f;
    pBody.layer = PhysicsLayer::Mechanism;
    pBody.mask = PhysicsLayer::Dynamic;
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(platform), pBody);
    PhysicsShapeComponent pShape = T4BoxShape(0.5f);
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(platform), pShape);
    f.Registry().get<Transform>(f.Handle(platform)).translation = {0.0f, -0.25f, 0.0f};
    // Widen the platform on X/Z only (uniform scale must stay 1).
    f.Registry().get<PhysicsShapeComponent>(f.Handle(platform)).halfExtents =
        {2.0f, 0.25f, 2.0f};

    const UUID crate = f.Create("Crate");
    PhysicsBodyComponent cBody = T4DynamicBody(1.0f);
    cBody.friction = 1.0f;
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(crate), cBody);
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(crate), T4BoxShape(0.5f));
    f.Registry().get<Transform>(f.Handle(crate)).translation = {0.0f, 0.6f, 0.0f};

    T4NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

    // Settle the crate onto the platform first (commanded motion starts after).
    for (int i = 0; i < 30; ++i)
        ctrl.Update(kFixedDt, bridge);

    // Drive the platform +X at 2 u/s through the ECS (dirty-marked, exactly
    // like every ECS write path) for one simulated second.
    float platformX = 0.0f;
    for (int i = 0; i < 60; ++i)
    {
        platformX += 2.0f * kFixedDt;
        SceneDocument* runtime = ctrl.TryGetRuntimeSceneMut();
        REQUIRE(runtime != nullptr);
        const auto e = runtime->FindByUuid(platform);
        const bool resolved = (e != entt::null);
        REQUIRE(resolved);
        runtime->ecs.registry.get<Transform>(e).translation =
            {platformX, -0.25f, 0.0f};
        SceneGraph::SetLocalDirty(runtime->ecs.registry, e);
        ctrl.Update(kFixedDt, bridge);
    }

    // Derived platform velocity is the commanded 2 u/s (not zero).
    const PhysicsWorld* world = ctrl.TryGetPhysicsWorld();
    REQUIRE(world != nullptr);
    const PhysicsBodyRecord* platformRec = world->FindBody(platform);
    REQUIRE(platformRec != nullptr);
    REQUIRE(platformRec->body != nullptr);
    CHECK(platformRec->body->getLinearVelocity().x() ==
          doctest::Approx(2.0f).epsilon(0.05));
    // Contact friction dragged the crate along (it started at x = 0).
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    const auto ce = runtime->FindByUuid(crate);
    const bool crateResolved = (ce != entt::null);
    REQUIRE(crateResolved);
    const float crateX =
        runtime->ecs.registry.get<Transform>(ce).translation.x;
    CHECK(crateX > 1.0f);
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("T4 GREEN_KinematicEcsToBullet: kinematic pose pushes before the step")
{
    T4Fixture f;
    const UUID id = f.Create("Platform");
    PhysicsBodyComponent body;
    body.kind = PhysicsBodyKind::Kinematic;
    body.mass = 0.0f;
    body.layer = PhysicsLayer::Mechanism;
    body.mask = PhysicsLayer::Dynamic;
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), body);
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(id), T4BoxShape(0.5f));

    T4NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

    // Script-side pose write lands in the ECS first (marked dirty, exactly
    // like every ECS write path: MotionSystem, SetLocalTransformStates);
    // the pre-step hook refreshes world matrices and pushes the pose into
    // Bullet before the step runs.
    SceneDocument* runtime = ctrl.TryGetRuntimeSceneMut();
    REQUIRE(runtime != nullptr);
    {
        const auto e = runtime->FindByUuid(id);
        const bool resolved = (e != entt::null);
        REQUIRE(resolved);
        runtime->ecs.registry.get<Transform>(e).translation =
            {3.0f, 1.0f, 2.0f};
        SceneGraph::SetLocalDirty(runtime->ecs.registry, e);
    }
    ctrl.Update(kFixedDt, bridge);

    glm::vec3 bulletPos{0.0f, 0.0f, 0.0f};
    REQUIRE(ctrl.TryGetPhysicsWorld()->BodyWorldPosition(id, bulletPos));
    CHECK(bulletPos.x == doctest::Approx(3.0f));
    CHECK(bulletPos.y == doctest::Approx(1.0f));
    CHECK(bulletPos.z == doctest::Approx(2.0f));
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("T4 GREEN_DynamicBulletToEcs: dynamic pose writes back after the step")
{
    T4Fixture f;
    const UUID faller = f.Create("Faller");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(faller), T4DynamicBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(faller), T4SphereShape());
    f.Registry().get<Transform>(f.Handle(faller)).translation = {0.0f, 5.0f, 0.0f};
    const UUID anchor = f.Create("Anchor");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(anchor), T4StaticBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(anchor), T4BoxShape());

    T4NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

    // One second of fixed ticks: the dynamic sphere falls ~4.9m under the
    // declared (0,-9.81,0) gravity while the static anchor never moves.
    for (int i = 0; i < 60; ++i)
        ctrl.Update(kFixedDt, bridge);

    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    const float y = runtime->ecs.registry
                        .get<Transform>(runtime->FindByUuid(faller))
                        .translation.y;
    CHECK(y < 4.5f);
    const float anchorY = runtime->ecs.registry
                              .get<Transform>(runtime->FindByUuid(anchor))
                              .translation.y;
    CHECK(anchorY == doctest::Approx(0.0f));
    glm::vec3 bulletPos{0.0f, 0.0f, 0.0f};
    REQUIRE(ctrl.TryGetPhysicsWorld()->BodyWorldPosition(faller, bulletPos));
    CHECK(bulletPos.y == doctest::Approx(y));
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("T4 GREEN_BodiesCollideUnderUnits: sphere and convex hull settle on static ground")
{
    T4TempAssets assets;
    assets.MakeCube("hullbox.obj", 0.5f);

    T4Fixture f;
    const UUID ground = f.Create("Ground");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(ground), T4StaticBody());
    PhysicsShapeComponent groundShape = T4BoxShape(5.0f);
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(ground), groundShape);
    // Ground slab top at y=0: center the 10-unit box at y=-5.
    f.Registry().get<Transform>(f.Handle(ground)).translation = {0.0f, -5.0f, 0.0f};

    const UUID sphere = f.Create("Sphere");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(sphere), T4DynamicBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(sphere), T4SphereShape(0.5f));
    f.Registry().get<Transform>(f.Handle(sphere)).translation = {0.0f, 4.0f, 0.0f};

    const UUID hull = f.Create("Hull");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(hull), T4DynamicBody());
    PhysicsShapeComponent hullShape;
    hullShape.shape = PhysicsShapeKind::ConvexHull;
    hullShape.hull = T4ModelRef("hullbox.obj", "obj:whole-model");
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(hull), hullShape);
    f.Registry().get<Transform>(f.Handle(hull)).translation = {3.0f, 4.0f, 0.0f};

    T4NullBridge bridge;
    Error err;
    PhysicsCollisionAssetProvider provider;
    provider.SetContext(AssetResolutionContext{assets.dir, nullptr});
    RuntimeSceneController ctrl;
    ctrl.SetCollisionProvider(&provider);
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

    CHECK(ctrl.PhysicsBodyCount() == 3);
    CHECK(ctrl.PhysicsShapeCount() == 3);

    // Four simulated seconds: both droppers rest on the slab (sphere center
    // at radius height, hull cube at half-extent height, within tolerance).
    for (int i = 0; i < 240; ++i)
        ctrl.Update(kFixedDt, bridge);
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    const float sphereY = runtime->ecs.registry
                              .get<Transform>(runtime->FindByUuid(sphere))
                              .translation.y;
    const float hullY = runtime->ecs.registry
                            .get<Transform>(runtime->FindByUuid(hull))
                            .translation.y;
    CHECK(sphereY == doctest::Approx(0.5f).epsilon(0.08));
    CHECK(hullY == doctest::Approx(0.5f).epsilon(0.12));
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
}

TEST_CASE("T4 GREEN_TriggerAuthority: Static baked, Kinematic pushed, Dynamic refused")
{
    T4Fixture f;
    // Static ghost: stages one ghost, never simulates, never writes back.
    const UUID statik = f.Create("StaticGhost");
    PhysicsBodyComponent sBody = T4StaticBody();
    sBody.layer = PhysicsLayer::Trigger;
    sBody.mask = PhysicsLayer::Dynamic;
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(statik), sBody);
    PhysicsShapeComponent sShape = T4SphereShape();
    sShape.isTrigger = true;
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(statik), sShape);
    // Kinematic ghost: pushed ECS -> Bullet before the step.
    const UUID kinematic = f.Create("KinematicGhost");
    PhysicsBodyComponent kBody;
    kBody.kind = PhysicsBodyKind::Kinematic;
    kBody.mass = 0.0f;
    kBody.layer = PhysicsLayer::Trigger;
    kBody.mask = PhysicsLayer::Dynamic;
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(kinematic), kBody);
    PhysicsShapeComponent kShape = T4SphereShape();
    kShape.isTrigger = true;
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(kinematic), kShape);

    T4NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    CHECK(ctrl.PhysicsBodyCount() == 0);
    CHECK(ctrl.PhysicsGhostCount() == 2);
    CHECK(ctrl.PhysicsShapeCount() == 2);

    SceneDocument* runtime = ctrl.TryGetRuntimeSceneMut();
    REQUIRE(runtime != nullptr);
    {
        const auto e = runtime->FindByUuid(kinematic);
        const bool resolved = (e != entt::null);
        REQUIRE(resolved);
        runtime->ecs.registry.get<Transform>(e).translation = {2.0f, 0.0f, 0.0f};
        SceneGraph::SetLocalDirty(runtime->ecs.registry, e);
    }
    for (int i = 0; i < 5; ++i)
        ctrl.Update(kFixedDt, bridge);

    glm::vec3 ghostPos{0.0f, 0.0f, 0.0f};
    REQUIRE(ctrl.TryGetPhysicsWorld()->BodyWorldPosition(kinematic, ghostPos));
    CHECK(ghostPos.x == doctest::Approx(2.0f));
    // Static ghost keeps its baked pose; the ECS write-back never touches
    // ghosts of any kind.
    glm::vec3 staticPos{0.0f, 0.0f, 0.0f};
    REQUIRE(ctrl.TryGetPhysicsWorld()->BodyWorldPosition(statik, staticPos));
    CHECK(staticPos.x == doctest::Approx(0.0f));
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
}

TEST_CASE("T4 RED_DynamicTriggerRefused: dynamic triggers refuse Play atomically")
{
    T4Fixture f;
    const UUID id = f.Create("DynamicGhost");
    // Forged past the authoring API (which refuses the same combination):
    // proves the Play staging boundary independently.
    PhysicsBodyComponent body = T4DynamicBody();
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), body);
    PhysicsShapeComponent shape = T4SphereShape();
    shape.isTrigger = true;
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(id), shape);
    // Trigger layer agreement holds, so only the host-kind rule can refuse.
    f.Registry().get<PhysicsBodyComponent>(f.Handle(id)).layer =
        PhysicsLayer::Trigger;
    f.Registry().get<PhysicsBodyComponent>(f.Handle(id)).mask =
        PhysicsLayer::Dynamic;

    T4NullBridge bridge;
    T4NoopObserver obs;
    Error err;
    RuntimeSceneController ctrl;
    ctrl.SetLifecycleObserver(&obs);
    CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
    CHECK(err.code == Error::InvalidArgument);
    T4CheckCleanRefusal(ctrl, bridge, obs, err, id);
}

TEST_CASE("T4 RED_InvalidPhysicsEnumRefused: forged body/shape kinds refuse Play atomically")
{
    // Forged past the CPU mutation boundary (which refuses the same values):
    // proves Play staging re-validates enums with UUID-bearing errors and
    // never dereferences a null shape.
    for (int variant = 0; variant < 2; ++variant)
    {
        T4Fixture f;
        const UUID id = f.Create(variant == 0 ? "BadKind" : "BadShape");
        PhysicsBodyComponent body = T4StaticBody();
        PhysicsShapeComponent shape = T4SphereShape();
        if (variant == 0)
            body.kind = (PhysicsBodyKind)99;
        else
            shape.shape = (PhysicsShapeKind)99;
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), body);
        f.Registry().emplace<PhysicsShapeComponent>(f.Handle(id), shape);

        T4NullBridge bridge;
        T4NoopObserver obs;
        Error err;
        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        CHECK(err.code == Error::InvalidArgument);
        T4CheckCleanRefusal(ctrl, bridge, obs, err, id);
    }
}

TEST_CASE("T4 GREEN_InspectorWorkPolicy: clean resync, dirty conflict, reset, assetId rebind")
{
    // CPU proof of the Inspector working-copy policy (the ImGui layer only
    // renders this state): apply -> undo -> same-selection edit can never
    // overwrite fields Undo restored, and same-UUID document replacement
    // cannot inherit values.
    DeterministicUuidProvider ids;
    const UUID target = ids.CreateV4();
    const UUID other = ids.CreateV4();
    PhysicsBodyComponent live = T4DynamicBody();
    PhysicsShapeComponent liveShape = T4SphereShape();

    PhysicsInspectorWork work;
    CHECK_FALSE(work.HasTarget());

    // Selection seeds clean copies.
    work.Sync(target, live, liveShape);
    CHECK(work.HasTarget());
    CHECK(work.body == live);
    CHECK_FALSE(work.bodyDirty);
    CHECK_FALSE(work.bodyConflict);

    // Clean copy resyncs when live drifts (Undo of an earlier edit while
    // selected): the next edit starts from restored values.
    PhysicsBodyComponent undone = live;
    undone.friction = 0.1f;
    work.Sync(target, undone, liveShape);
    CHECK_FALSE(work.bodyDirty);
    CHECK_FALSE(work.bodyConflict);
    CHECK(work.body == undone);

    // Dirty copy + live drift (Undo of another command) raises conflict and
    // keeps the user's edits (nothing silently discarded).
    work.body->mass = 9.0f;
    work.bodyDirty = true;
    work.Sync(target, live, liveShape);
    CHECK(work.bodyDirty);
    CHECK(work.bodyConflict);
    CHECK(work.body->mass == doctest::Approx(9.0f));

    // Revert reseeds from live and clears the conflict.
    work.RevertBody(live);
    CHECK_FALSE(work.bodyDirty);
    CHECK_FALSE(work.bodyConflict);
    CHECK(work.body == live);

    // Dirty copy edited back to live values is not dirty (return-to-start
    // needs no history entry).
    work.body->friction = 0.9f;
    work.bodyDirty = true;
    work.Sync(target, live, liveShape);
    work.body->friction = live.friction;
    work.Sync(target, live, liveShape);
    CHECK_FALSE(work.bodyDirty);

    // A dirty copy survives a live presence change and conflicts until the
    // user explicitly reverts; no unapplied edit is silently discarded.
    work.body->mass = 9.0f;
    work.bodyDirty = true;
    work.Sync(target, std::nullopt, liveShape);
    REQUIRE(work.body.has_value());
    CHECK(work.body->mass == doctest::Approx(9.0f));
    CHECK(work.bodyDirty);
    CHECK(work.bodyConflict);
    work.RevertBody(std::nullopt);
    CHECK_FALSE(work.body.has_value());
    CHECK_FALSE(work.bodyDirty);
    CHECK_FALSE(work.bodyConflict);

    // Applying one side advances only that seed and preserves pending edits
    // to the other component.
    work.Sync(target, live, liveShape);
    work.body->mass = 9.0f;
    work.bodyDirty = true;
    work.shape->radius = 2.0f;
    work.shapeDirty = true;
    work.AppliedBody(live);
    CHECK_FALSE(work.bodyDirty);
    CHECK(work.body == live);
    CHECK(work.shapeDirty);
    CHECK(work.shape->radius == doctest::Approx(2.0f));
    work.AppliedShape(liveShape);
    CHECK_FALSE(work.shapeDirty);
    CHECK(work.shape == liveShape);

    // Selection change still reseeds everything.
    work.body->mass = 9.0f;
    work.bodyDirty = true;
    work.Sync(other, live, liveShape);
    CHECK(work.target == other);
    CHECK_FALSE(work.bodyDirty);
    CHECK(work.body == live);

    // Document reset drops all state (same-UUID replacement is safe).
    work.body->mass = 9.0f;
    work.bodyDirty = true;
    work.Clear();
    CHECK_FALSE(work.HasTarget());
    CHECK_FALSE(work.body.has_value());
    CHECK_FALSE(work.shape.has_value());
    CHECK_FALSE(work.bodyDirty);
    CHECK_FALSE(work.shapeDirty);

    // Collision path edits clear the stale asset ID; unchanged paths and
    // sourceKey edits preserve identity.
    AssetReference ref = T4ModelRef("old.obj", "obj:whole-model");
    DeterministicUuidProvider ids2;
    ref.assetId = ids2.CreateV4();
    CHECK(PhysicsInspectorWork::NoteCollisionPathChanged(ref, "new.obj"));
    CHECK(ref.path == "new.obj");
    CHECK(ref.kind == AssetKind::Model);
    CHECK(ref.assetId.IsNull());
    ref.assetId = ids2.CreateV4();
    CHECK_FALSE(
        PhysicsInspectorWork::NoteCollisionPathChanged(ref, "new.obj"));
    CHECK_FALSE(ref.assetId.IsNull());
}

TEST_CASE("T4 GREEN_StaticTriMeshRamp: sphere deflects along a static triangle ramp")
{
    // Production StaticTriMesh success path: a sloped static triangle ramp
    // (both windings, so contact never depends on triangle sidedness).
    // A dropped sphere lands, rolls down-slope (+X), and settles on it.
    T4TempAssets assets;
    T4WriteText(assets.dir / "ramp.obj",
        "v 0 2 -1\nv 4 0 -1\nv 4 0 1\nv 0 2 1\n"
        "f 1 2 3\nf 1 3 4\nf 1 4 3\nf 1 3 2\n");

    T4Fixture f;
    const UUID ramp = f.Create("Ramp");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(ramp), T4StaticBody());
    PhysicsShapeComponent rampShape;
    rampShape.shape = PhysicsShapeKind::StaticTriMesh;
    rampShape.triMesh = T4ModelRef("ramp.obj", "obj:whole-model");
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(ramp), rampShape);

    const UUID ball = f.Create("Ball");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(ball), T4DynamicBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(ball), T4SphereShape(0.5f));
    f.Registry().get<Transform>(f.Handle(ball)).translation = {0.5f, 4.0f, 0.0f};
    // Catch floor below (gravity-only motion cannot move +X on its own, so
    // any +X travel proves ramp contact and deflection; the floor proves the
    // ball never tunneled through the triangle mesh).
    const UUID floor = f.Create("Floor");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(floor), T4StaticBody());
    PhysicsShapeComponent floorShape = T4BoxShape(5.0f);
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(floor), floorShape);
    f.Registry().get<PhysicsShapeComponent>(f.Handle(floor)).halfExtents =
        {20.0f, 5.0f, 20.0f};
    f.Registry().get<Transform>(f.Handle(floor)).translation = {4.0f, -6.0f, 0.0f};

    T4NullBridge bridge;
    Error err;
    PhysicsCollisionAssetProvider provider;
    provider.SetContext(AssetResolutionContext{assets.dir, nullptr});
    RuntimeSceneController ctrl;
    ctrl.SetCollisionProvider(&provider);
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    CHECK(ctrl.PhysicsBodyCount() == 3);
    CHECK(ctrl.PhysicsShapeCount() == 3);

    for (int i = 0; i < 90; ++i)
        ctrl.Update(kFixedDt, bridge);
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    const auto e = runtime->FindByUuid(ball);
    const bool resolved = (e != entt::null);
    REQUIRE(resolved);
    const glm::vec3 p =
        runtime->ecs.registry.get<Transform>(e).translation;
    // Deflected down-slope (gravity alone cannot move +X) and caught by the
    // ramp or the floor (never tunneled through the triangle mesh).
    CHECK(p.x > 0.8f);
    CHECK(p.y > -0.9f);
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
}

TEST_CASE("T4 GREEN_NonUnitScaleMarginCcdInertia: uniform scale composes once with Bullet-level proofs")
{
    // Single scale owner: uniform world scale 2 composes once at build for
    // sphere/box/hull/tri shapes. Bullet-boundary proofs: baked scale,
    // sphere margin == radius (policy), box margin round-trip, CCD values,
    // and post-scale inertia (box half 1.0, mass 12 -> I = 8 per axis).
    T4TempAssets assets;
    assets.MakeCube("hullbox.obj", 0.5f);
    T4WriteText(assets.dir / "slab.obj",
        "v -5 0 -5\nv 5 0 -5\nv 5 0 5\nv -5 0 5\n"
        "f 1 2 3\nf 1 3 4\nf 1 4 3\nf 1 3 2\n");

    T4Fixture f;
    const UUID ground = f.Create("Ground");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(ground), T4StaticBody());
    PhysicsShapeComponent groundShape;
    groundShape.shape = PhysicsShapeKind::StaticTriMesh;
    groundShape.triMesh = T4ModelRef("slab.obj", "obj:whole-model");
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(ground), groundShape);
    f.Registry().get<Transform>(f.Handle(ground)).scale = {2.0f, 2.0f, 2.0f};

    const UUID sphere = f.Create("Sphere");
    PhysicsBodyComponent sphereBody = T4DynamicBody(1.0f);
    sphereBody.ccdEnabled = true;
    sphereBody.ccdMotionThreshold = 0.5f;
    sphereBody.ccdSweptRadius = 0.8f;
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(sphere), sphereBody);
    PhysicsShapeComponent sphereShape = T4SphereShape(0.5f);
    sphereShape.collisionMargin = 0.3f; // reserved for spheres (policy proof)
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(sphere), sphereShape);
    f.Registry().get<Transform>(f.Handle(sphere)).translation = {0.0f, 5.0f, 0.0f};
    f.Registry().get<Transform>(f.Handle(sphere)).scale = {2.0f, 2.0f, 2.0f};

    const UUID box = f.Create("Box");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(box), T4DynamicBody(12.0f));
    PhysicsShapeComponent boxShape = T4BoxShape(0.5f);
    boxShape.collisionMargin = 0.07f;
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(box), boxShape);
    f.Registry().get<Transform>(f.Handle(box)).translation = {4.0f, 5.0f, 0.0f};
    f.Registry().get<Transform>(f.Handle(box)).scale = {2.0f, 2.0f, 2.0f};

    const UUID hull = f.Create("Hull");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(hull), T4DynamicBody(8.0f));
    PhysicsShapeComponent hullShape;
    hullShape.shape = PhysicsShapeKind::ConvexHull;
    hullShape.hull = T4ModelRef("hullbox.obj", "obj:whole-model");
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(hull), hullShape);
    f.Registry().get<Transform>(f.Handle(hull)).translation = {-4.0f, 5.0f, 0.0f};
    f.Registry().get<Transform>(f.Handle(hull)).scale = {2.0f, 2.0f, 2.0f};

    T4NullBridge bridge;
    Error err;
    PhysicsCollisionAssetProvider provider;
    provider.SetContext(AssetResolutionContext{assets.dir, nullptr});
    RuntimeSceneController ctrl;
    ctrl.SetCollisionProvider(&provider);
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    REQUIRE(ctrl.TryGetPhysicsWorld() != nullptr);
    const PhysicsWorld* world = ctrl.TryGetPhysicsWorld();

    // Bullet-boundary proofs at staging time.
    for (const UUID id : {ground, sphere, box, hull})
    {
        const PhysicsBodyRecord* rec = world->FindBody(id);
        REQUIRE(rec != nullptr);
        CHECK(rec->bakedScale == doctest::Approx(2.0f));
    }
    const btCollisionShape* sphereStaged = world->FindBodyShape(sphere);
    REQUIRE(sphereStaged != nullptr);
    CHECK(sphereStaged->getMargin() == doctest::Approx(1.0f));
    const btCollisionShape* boxStaged = world->FindBodyShape(box);
    REQUIRE(boxStaged != nullptr);
    CHECK(boxStaged->getMargin() == doctest::Approx(0.07f));
    const PhysicsBodyRecord* sphereRec = world->FindBody(sphere);
    REQUIRE(sphereRec != nullptr);
    REQUIRE(sphereRec->body != nullptr);
    CHECK(sphereRec->body->getCcdMotionThreshold() == doctest::Approx(0.5f));
    CHECK(sphereRec->body->getCcdSweptSphereRadius() == doctest::Approx(0.8f));
    const PhysicsBodyRecord* boxRec = world->FindBody(box);
    REQUIRE(boxRec != nullptr);
    REQUIRE(boxRec->body != nullptr);
    CHECK(boxRec->body->getInvMass() == doctest::Approx(1.0f / 12.0f));
    const btVector3& invI = boxRec->body->getInvInertiaDiagLocal();
    CHECK(invI.x() == doctest::Approx(1.0f / 8.0f).epsilon(0.02));
    CHECK(invI.y() == doctest::Approx(1.0f / 8.0f).epsilon(0.02));
    CHECK(invI.z() == doctest::Approx(1.0f / 8.0f).epsilon(0.02));
    CHECK(world->FindBodyShape(UUID::Nil()) == nullptr);

    // All droppers rest at scaled-extent height on the scaled tri slab.
    for (int i = 0; i < 240; ++i)
        ctrl.Update(kFixedDt, bridge);
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    auto readY = [&](const UUID& id) {
        const auto e = runtime->FindByUuid(id);
        const bool resolved = (e != entt::null);
        REQUIRE(resolved);
        return runtime->ecs.registry.get<Transform>(e).translation.y;
    };
    CHECK(readY(sphere) == doctest::Approx(1.0f).epsilon(0.1));
    CHECK(readY(box) == doctest::Approx(1.0f).epsilon(0.1));
    CHECK(readY(hull) == doctest::Approx(1.0f).epsilon(0.15));
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
}

TEST_CASE("T4 GREEN_ProviderOutlivesSession: destroying the provider mid-Play leaves the session intact")
{
    // Lifetime proof for the host-owned borrow rule: the committed world
    // owns its Bullet shapes, so the provider is only touched during
    // candidate construction. Destroying it mid-Play must not disturb
    // stepping or Stop (mirrors host shutdown ordering, where the provider
    // member is now declared before the controller and destroyed after it).
    T4TempAssets assets;
    assets.MakeCube("hullbox.obj", 0.5f);

    T4Fixture f;
    const UUID hull = f.Create("Hull");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(hull), T4DynamicBody());
    PhysicsShapeComponent hullShape;
    hullShape.shape = PhysicsShapeKind::ConvexHull;
    hullShape.hull = T4ModelRef("hullbox.obj", "obj:whole-model");
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(hull), hullShape);

    T4NullBridge bridge;
    Error err;
    auto provider = std::make_unique<PhysicsCollisionAssetProvider>();
    provider->SetContext(AssetResolutionContext{assets.dir, nullptr});
    RuntimeSceneController ctrl;
    ctrl.SetCollisionProvider(provider.get());
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    REQUIRE(ctrl.PhysicsBodyCount() == 1);

    // The session borrow ends at commit: destroy the provider mid-Play.
    provider.reset();
    ctrl.SetCollisionProvider(nullptr);
    for (int i = 0; i < 30; ++i)
        ctrl.Update(kFixedDt, bridge);
    CHECK(ctrl.PhysicsStepCount() == 30);
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.GetState() == SceneRunState::Edit);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
    CHECK(PhysicsWorld::LiveWorldCount() == 0);
}

TEST_CASE("T4 RED_HostileProviderPayloadRefused: injected payloads are validated before Bullet reads")
{
    // A buggy/test/alternate provider can return success with hostile
    // payloads. Staging must refuse each with a typed UUID-bearing error and
    // roll the candidate back atomically — never null-deref or over-read.
    struct HostileProvider final : public IPhysicsCollisionAssetProvider
    {
        CollisionGeometry payload;
        bool nullIt = false;
        void SetContext(const AssetResolutionContext&) override {}
        Result<const CollisionGeometry*> GetCollisionGeometry(
            const AssetReference&, const UUID&, const std::string&) override
        {
            if (nullIt)
                return Result<const CollisionGeometry*>::Ok(nullptr);
            return Result<const CollisionGeometry*>::Ok(&payload);
        }
        size_t CacheEntryCount() const override { return 0; }
        size_t DecodeCount() const override { return 0; }
    };
    auto tetra = []() {
        CollisionGeometry g;
        g.vertices = {0,0,0, 1,0,0, 0,1,0, 0,0,1};
        g.indices = {0,1,2, 0,1,3, 0,2,3, 1,2,3};
        return g;
    };
    struct Variant
    {
        const char* name;
        std::function<void(CollisionGeometry&, bool&)> arm;
        Error::Code code;
    };
    const Variant variants[] = {
        {"null", [](CollisionGeometry&, bool& nullIt) { nullIt = true; },
         Error::Parse},
        {"truncated", [tetra](CollisionGeometry& g, bool&) {
             g = tetra();
             g.vertices.resize(4);
         }, Error::Parse},
        {"nonfinite", [tetra](CollisionGeometry& g, bool&) {
             g = tetra();
             g.vertices[0] = std::numeric_limits<float>::quiet_NaN();
         }, Error::Parse},
        {"outofrange", [tetra](CollisionGeometry& g, bool&) {
             g = tetra();
             g.indices[0] = 99;
         }, Error::Parse},
    };
    for (const auto& variant : variants)
    {
        T4Fixture f;
        const UUID id = f.Create(variant.name);
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), T4StaticBody());
        PhysicsShapeComponent shape;
        shape.shape = PhysicsShapeKind::ConvexHull;
        shape.hull = T4ModelRef("hostile.obj", "obj:whole-model");
        f.Registry().emplace<PhysicsShapeComponent>(f.Handle(id), shape);

        T4NullBridge bridge;
        T4NoopObserver obs;
        Error err;
        HostileProvider provider;
        variant.arm(provider.payload, provider.nullIt);
        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        ctrl.SetCollisionProvider(&provider);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        CHECK(err.code == variant.code);
        T4CheckCleanRefusal(ctrl, bridge, obs, err, id);
    }

    // Valid injected payload stages (control: the boundary accepts).
    {
        T4Fixture f;
        const UUID id = f.Create("Honest");
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), T4StaticBody());
        PhysicsShapeComponent shape;
        shape.shape = PhysicsShapeKind::ConvexHull;
        shape.hull = T4ModelRef("honest.obj", "obj:whole-model");
        f.Registry().emplace<PhysicsShapeComponent>(f.Handle(id), shape);

        T4NullBridge bridge;
        Error err;
        HostileProvider provider;
        provider.payload = tetra();
        RuntimeSceneController ctrl;
        ctrl.SetCollisionProvider(&provider);
        REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
        CHECK(ctrl.PhysicsBodyCount() == 1);
        ctrl.Stop(f.Authoring(), bridge);
    }
}

TEST_CASE("T4 RED_UnsafeMarginRefused: margins at/above the scaled half-extent refuse Play atomically")
{
    // Forged past the authoring API (which refuses the same pairs): proves
    // Play staging enforces the Bullet core-dimension invariant for boxes
    // and hulls alike.
    T4TempAssets assets;
    assets.MakeCube("tinyhull.obj", 0.01f);
    struct MarginVariant
    {
        const char* name;
        PhysicsShapeComponent shape;
    };
    PhysicsShapeComponent tinyBox = T4BoxShape(0.01f);
    tinyBox.collisionMargin = 1.0f;
    PhysicsShapeComponent tinyHull;
    tinyHull.shape = PhysicsShapeKind::ConvexHull;
    tinyHull.hull = T4ModelRef("tinyhull.obj", "obj:whole-model");
    tinyHull.collisionMargin = 0.04f; // hull min-half is 0.01
    const MarginVariant variants[] = {
        {"TinyBox", tinyBox},
        {"TinyHull", tinyHull},
    };
    for (const auto& variant : variants)
    {
        T4Fixture f;
        const UUID id = f.Create(variant.name);
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), T4StaticBody());
        f.Registry().emplace<PhysicsShapeComponent>(f.Handle(id), variant.shape);

        T4NullBridge bridge;
        T4NoopObserver obs;
        Error err;
        PhysicsCollisionAssetProvider provider;
        provider.SetContext(AssetResolutionContext{assets.dir, nullptr});
        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        ctrl.SetCollisionProvider(&provider);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        CHECK(err.code == Error::InvalidArgument);
        T4CheckCleanRefusal(ctrl, bridge, obs, err, id);
    }
}

TEST_CASE("T4 GREEN_SmallMarginStages: a valid small margin keeps support, AABB, and contact")
{
    T4Fixture f;
    const UUID pedestal = f.Create("Pedestal");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(pedestal), T4StaticBody());
    PhysicsShapeComponent validSmall = T4BoxShape(0.05f);
    validSmall.collisionMargin = 0.04f;
    REQUIRE(f.manager.SetPhysicsShapeState(pedestal, validSmall).success);
    const UUID ball = f.Create("Ball");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(ball), T4DynamicBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(ball), T4SphereShape(0.5f));
    f.Registry().get<Transform>(f.Handle(ball)).translation = {0.0f, 3.0f, 0.0f};

    T4NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    const PhysicsWorld* world = ctrl.TryGetPhysicsWorld();
    REQUIRE(world != nullptr);
    // Support/AABB stay valid: margin applied, outer dims preserved, and the
    // box AABB has strictly positive extent on every axis.
    const btCollisionShape* staged = world->FindBodyShape(pedestal);
    REQUIRE(staged != nullptr);
    CHECK(staged->getMargin() == doctest::Approx(0.04f));
    btVector3 aabbMin, aabbMax;
    const PhysicsBodyRecord* rec = world->FindBody(pedestal);
    REQUIRE(rec != nullptr);
    REQUIRE(rec->body != nullptr);
    staged->getAabb(rec->body->getWorldTransform(), aabbMin, aabbMax);
    CHECK((aabbMax.x() - aabbMin.x()) > 0.0f);
    CHECK((aabbMax.y() - aabbMin.y()) > 0.0f);
    CHECK((aabbMax.z() - aabbMin.z()) > 0.0f);
    // Collision behavior: the ball rests on the pedestal top (y = 0.05).
    for (int i = 0; i < 180; ++i)
        ctrl.Update(kFixedDt, bridge);
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    const auto e = runtime->FindByUuid(ball);
    const bool resolved = (e != entt::null);
    REQUIRE(resolved);
    CHECK(runtime->ecs.registry.get<Transform>(e).translation.y ==
          doctest::Approx(0.55f).epsilon(0.1));
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("T4 GREEN_PhysicsPairAuthoring: atomic trigger-pair create, convert, undo, redo")
{
    // The production pair path: mutually dependent body-layer and shape
    // transitions commit atomically through history, so a valid trigger pair
    // is creatable and convertible without ever persisting an invalid half.
    T4Fixture f;
    const UUID id = f.Create("Pair");
    EditorCommandHistory history;

    PhysicsBodyComponent solidBody = T4StaticBody();
    PhysicsShapeComponent solidShape = T4SphereShape();
    PhysicsBodyComponent triggerBody = T4StaticBody();
    triggerBody.layer = PhysicsLayer::Trigger;
    triggerBody.mask = PhysicsLayer::Dynamic;
    PhysicsShapeComponent triggerShape = T4SphereShape();
    triggerShape.isTrigger = true;

    // No-op suppression across both sides.
    CHECK(MakeSetPhysicsBodyShapeCommandIfEffective(
              id, solidBody, solidBody, solidShape, solidShape) == nullptr);

    // Neither single command can build the trigger pair from the solid one:
    // each half alone conflicts with the live other half (the deadlock the
    // atomic command exists to resolve).
    REQUIRE(f.manager.SetPhysicsBodyState(id, solidBody).success);
    REQUIRE(f.manager.SetPhysicsShapeState(id, solidShape).success);
    CHECK_FALSE(f.manager.SetPhysicsBodyState(id, triggerBody).success);
    CHECK_FALSE(f.manager.SetPhysicsShapeState(id, triggerShape).success);
    CHECK(f.manager.GetPhysicsBody(id) == solidBody);
    CHECK(f.manager.GetPhysicsShape(id) == solidShape);

    // Atomic solid -> trigger through history: one entry, exact values.
    auto toTrigger = MakeSetPhysicsBodyShapeCommandIfEffective(
        id, solidBody, triggerBody, solidShape, triggerShape);
    REQUIRE(toTrigger != nullptr);
    REQUIRE(history.Execute(std::move(toTrigger), f.manager).success);
    CHECK(f.manager.GetPhysicsBody(id) == triggerBody);
    CHECK(f.manager.GetPhysicsShape(id) == triggerShape);
    REQUIRE(history.CanUndo());
    // Undo restores the exact solid pair; redo restores the trigger pair.
    REQUIRE(history.Undo(f.manager).success);
    CHECK(f.manager.GetPhysicsBody(id) == solidBody);
    CHECK(f.manager.GetPhysicsShape(id) == solidShape);
    REQUIRE(history.Redo(f.manager).success);
    CHECK(f.manager.GetPhysicsBody(id) == triggerBody);
    CHECK(f.manager.GetPhysicsShape(id) == triggerShape);

    // Atomic trigger -> solid converts back through the same seam.
    auto toSolid = MakeSetPhysicsBodyShapeCommandIfEffective(
        id, triggerBody, solidBody, triggerShape, solidShape);
    REQUIRE(toSolid != nullptr);
    REQUIRE(history.Execute(std::move(toSolid), f.manager).success);
    CHECK(f.manager.GetPhysicsBody(id) == solidBody);

    // An invalid after-pair surfaces the failure without recording.
    const size_t depth = history.UndoDepthForTest();
    PhysicsBodyComponent dynamicBody = T4DynamicBody();
    auto badPair = MakeSetPhysicsBodyShapeCommandIfEffective(
        id, solidBody, dynamicBody, solidShape, triggerShape);
    REQUIRE(badPair != nullptr);
    CHECK_FALSE(history.Execute(std::move(badPair), f.manager).success);
    CHECK(history.UndoDepthForTest() == depth);
    CHECK(f.manager.GetPhysicsBody(id) == solidBody);
    CHECK(f.manager.GetPhysicsShape(id) == solidShape);

    // The converted trigger pair Plays as one ghost plus one shape.
    auto reTrigger = MakeSetPhysicsBodyShapeCommandIfEffective(
        id, solidBody, triggerBody, solidShape, triggerShape);
    REQUIRE(history.Execute(std::move(reTrigger), f.manager).success);
    T4NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    CHECK(ctrl.PhysicsBodyCount() == 0);
    CHECK(ctrl.PhysicsGhostCount() == 1);
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("T4 RED_PhysicsPairPrefabMemberRejected: linked members refuse the atomic pair")
{
    T4Fixture f;
    const UUID member = f.Create("Member");
    PrefabMemberComponent link;
    link.instanceId = f.ids.CreateV4();
    link.templateId = f.ids.CreateV4();
    f.Registry().emplace<PrefabMemberComponent>(f.Handle(member), link);
    const uint64_t rev0 = f.manager.AuthoringRevision();

    EditorCommandHistory history;
    auto cmd = MakeSetPhysicsBodyShapeCommandIfEffective(
        member, std::nullopt, T4StaticBody(), std::nullopt, T4SphereShape());
    REQUIRE(cmd != nullptr);
    CHECK_FALSE(history.Execute(std::move(cmd), f.manager).success);
    CHECK_FALSE(history.CanUndo());
    CHECK_FALSE(f.manager.GetPhysicsBody(member).has_value());
    CHECK_FALSE(f.manager.GetPhysicsShape(member).has_value());
    CHECK(f.manager.AuthoringRevision() == rev0);
}

TEST_CASE("T4 RED_MissingColliderRefusesPlay: body without shape refuses Play atomically")
{
    T4Fixture f;
    const UUID id = f.Create("NoShape");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), T4DynamicBody());
    T4NullBridge bridge;
    T4NoopObserver obs;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.SetLifecycleObserver(&obs);
    CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
    T4CheckCleanRefusal(ctrl, bridge, obs, err, id);
}

TEST_CASE("T4 RED_MissingCollisionAssetRefusesPlay: dangling hull path refuses Play atomically")
{
    T4TempAssets assets;
    T4Fixture f;
    const UUID id = f.Create("Dangling");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), T4StaticBody());
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::ConvexHull;
    shape.hull = T4ModelRef("no-such-file.obj", "obj:whole-model");
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(id), shape);

    T4NullBridge bridge;
    T4NoopObserver obs;
    Error err;
    PhysicsCollisionAssetProvider provider;
    provider.SetContext(AssetResolutionContext{assets.dir, nullptr});
    RuntimeSceneController ctrl;
    ctrl.SetLifecycleObserver(&obs);
    ctrl.SetCollisionProvider(&provider);
    CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
    CHECK(err.code == Error::MissingAsset);
    T4CheckCleanRefusal(ctrl, bridge, obs, err, id);
}

TEST_CASE("T4 RED_MalformedCollisionAssetRefusesPlay: corrupt collision file refuses Play atomically")
{
    T4TempAssets assets;
    T4WriteText(assets.dir / "garbage.obj", "this is not an obj file\n{{{");
    T4Fixture f;
    const UUID id = f.Create("Garbage");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), T4StaticBody());
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::ConvexHull;
    shape.hull = T4ModelRef("garbage.obj", "obj:whole-model");
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(id), shape);

    T4NullBridge bridge;
    T4NoopObserver obs;
    Error err;
    PhysicsCollisionAssetProvider provider;
    provider.SetContext(AssetResolutionContext{assets.dir, nullptr});
    RuntimeSceneController ctrl;
    ctrl.SetLifecycleObserver(&obs);
    ctrl.SetCollisionProvider(&provider);
    CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
    CHECK(err.code == Error::Parse);
    T4CheckCleanRefusal(ctrl, bridge, obs, err, id);
}

TEST_CASE("T4 RED_SourceKeyMismatchRefusesPlay: wrong sourceKey for the file refuses Play atomically")
{
    T4TempAssets assets;
    assets.MakeCube("cube.obj");
    T4Fixture f;
    const UUID id = f.Create("Mismatch");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), T4StaticBody());
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::ConvexHull;
    // glTF key against an OBJ file: same file, different sourceKey isolation.
    shape.hull = T4ModelRef("cube.obj", "gltf:scene=0:node=0:mesh=0:primitive=0");
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(id), shape);

    T4NullBridge bridge;
    T4NoopObserver obs;
    Error err;
    PhysicsCollisionAssetProvider provider;
    provider.SetContext(AssetResolutionContext{assets.dir, nullptr});
    RuntimeSceneController ctrl;
    ctrl.SetLifecycleObserver(&obs);
    ctrl.SetCollisionProvider(&provider);
    CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
    CHECK(err.code == Error::InvalidArgument);
    T4CheckCleanRefusal(ctrl, bridge, obs, err, id);
}

TEST_CASE("T4 RED_OversizeCollisionAssetRefusesPlay: oversize geometry refuses Play atomically")
{
    T4TempAssets assets;
    {
        // 260k vertices over the 250k cap (positions only; one triangle).
        std::ofstream out(assets.dir / "huge.obj", std::ios::binary);
        REQUIRE_MESSAGE(out.is_open(), "T4 huge OBJ fixture open failed");
        for (int i = 0; i < 260000; ++i)
            out << "v 0 0 " << (i % 1000) << "\n";
        out << "f 1 2 3\n";
        out.flush();
        REQUIRE_MESSAGE(out.good(), "T4 huge OBJ fixture write failed");
    }
    T4Fixture f;
    const UUID id = f.Create("Huge");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), T4StaticBody());
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::ConvexHull;
    shape.hull = T4ModelRef("huge.obj", "obj:whole-model");
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(id), shape);

    T4NullBridge bridge;
    T4NoopObserver obs;
    Error err;
    PhysicsCollisionAssetProvider provider;
    provider.SetContext(AssetResolutionContext{assets.dir, nullptr});
    RuntimeSceneController ctrl;
    ctrl.SetLifecycleObserver(&obs);
    ctrl.SetCollisionProvider(&provider);
    CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
    CHECK(err.code == Error::InvalidArgument);
    T4CheckCleanRefusal(ctrl, bridge, obs, err, id);
}

TEST_CASE("T4 RED_StaticSetPositionRefused: runtime position setter on Static/Dynamic returns false; Kinematic succeeds")
{
    // Approved contract (plan:613): the production RuntimeCommandSink —
    // the exact entry point Lua entity:set_position calls — refuses pose
    // writes on Static and Dynamic without mutation, accepts Kinematic pose
    // writes, and refuses scale writes on every physics-owned body (shape
    // extents are baked once at Play).
    T4Fixture f;
    const UUID statik = f.Create("Static");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(statik), T4StaticBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(statik), T4SphereShape());
    const UUID dynamic = f.Create("Dynamic");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(dynamic), T4DynamicBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(dynamic), T4SphereShape());
    const UUID kinematic = f.Create("Kinematic");
    PhysicsBodyComponent kBody;
    kBody.kind = PhysicsBodyKind::Kinematic;
    kBody.mass = 0.0f;
    kBody.layer = PhysicsLayer::Mechanism;
    kBody.mask = PhysicsLayer::Dynamic;
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(kinematic), kBody);
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(kinematic), T4BoxShape());
    const UUID plain = f.Create("Plain");

    T4NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    RuntimeCommandSink sink(ctrl);

    // Static refuses position and full-transform writes; ECS is untouched.
    CHECK_FALSE(sink.SetPosition(statik, {9.0f, 9.0f, 9.0f}));
    EditableTRS moved;
    moved.translation = {9.0f, 9.0f, 9.0f};
    moved.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    moved.scale = {1.0f, 1.0f, 1.0f};
    CHECK_FALSE(sink.SetLocalTransform(statik, moved));
    glm::vec3 pos{0.0f, 0.0f, 0.0f};
    REQUIRE(sink.GetPosition(statik, pos));
    CHECK(pos.x == doctest::Approx(0.0f));
    CHECK(pos.y == doctest::Approx(0.0f));
    CHECK(pos.z == doctest::Approx(0.0f));

    // Dynamic refuses the same way (Bullet owns Dynamic -> ECS).
    CHECK_FALSE(sink.SetPosition(dynamic, {9.0f, 9.0f, 9.0f}));
    CHECK_FALSE(sink.SetLocalTransform(dynamic, moved));
    REQUIRE(sink.GetPosition(dynamic, pos));
    CHECK(pos.x == doctest::Approx(0.0f));

    // Kinematic accepts pose writes...
    CHECK(sink.SetPosition(kinematic, {1.0f, 2.0f, 3.0f}));
    REQUIRE(sink.GetPosition(kinematic, pos));
    CHECK(pos.x == doctest::Approx(1.0f));
    CHECK(pos.y == doctest::Approx(2.0f));
    CHECK(pos.z == doctest::Approx(3.0f));
    // ...but refuses scale writes without mutating anything else.
    EditableTRS scaled = moved;
    scaled.translation = {1.0f, 2.0f, 3.0f};
    scaled.scale = {2.0f, 2.0f, 2.0f};
    CHECK_FALSE(sink.SetLocalTransform(kinematic, scaled));
    EditableTRS live;
    REQUIRE(sink.GetLocalTransform(kinematic, live));
    CHECK(live.translation.x == doctest::Approx(1.0f));
    CHECK(live.scale.x == doctest::Approx(1.0f));

    // Non-physics entities are unaffected; unknown UUIDs still fail.
    CHECK(sink.SetPosition(plain, {4.0f, 5.0f, 6.0f}));
    DeterministicUuidProvider ids;
    CHECK_FALSE(sink.SetPosition(ids.CreateV4(), {0.0f, 0.0f, 0.0f}));

    // Velocity remains a legal Dynamic control; Static refuses it.
    PhysicsWorld* world = ctrl.TryGetPhysicsWorldMut();
    REQUIRE(world != nullptr);
    CHECK(world->SetBodyLinearVelocity(dynamic, {1.0f, 0.0f, 0.0f}));
    CHECK_FALSE(world->SetBodyLinearVelocity(statik, {1.0f, 0.0f, 0.0f}));

    // Steps never move the baked Static pose.
    for (int i = 0; i < 10; ++i)
        ctrl.Update(kFixedDt, bridge);
    glm::vec3 staticPos{0.0f, 0.0f, 0.0f};
    REQUIRE(world->BodyWorldPosition(statik, staticPos));
    CHECK(staticPos.x == doctest::Approx(0.0f));
    CHECK(staticPos.y == doctest::Approx(0.0f));
    CHECK(staticPos.z == doctest::Approx(0.0f));
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("T4 GREEN_LuaSetPositionAuthority: Lua set_position obeys per-kind authority")
{
    // End-to-end Lua proof of the same contract: every scripted body attempts
    // entity:set_position({7,0,0}) in on_update. Static never moves, Dynamic
    // never teleports (it only falls), Kinematic arrives.
    const auto scriptDir =
        std::filesystem::temp_directory_path() / "t4_lua_authority";
    std::error_code ec;
    std::filesystem::remove_all(scriptDir, ec);
    REQUIRE_MESSAGE(!ec, "T4 Lua fixture cleanup failed");
    std::filesystem::create_directories(scriptDir, ec);
    REQUIRE_MESSAGE(!ec, "T4 Lua fixture directory creation failed");
    T4WriteText(scriptDir / "authority.lua", R"LUA(
function on_update(entity, dt, input, world)
    entity:set_position({7, 0, 0})
end
)LUA");

    DeterministicUuidProvider uuidProv;
    SceneDocument doc;
    doc.SetUuidProvider(&uuidProv);
    doc.metadata.sourcePath = scriptDir / "fixture.rt2scene";
    UUID ids[3];
    const char* names[3] = {"S", "D", "K"};
    // Separated spawns: co-located spheres would depenetrate on the first
    // tick and pollute the x assertions below.
    const glm::vec3 spawns[3] = {{-3.0f, 0.0f, 0.0f}, {0.0f, 3.0f, 0.0f}, {3.0f, 0.0f, 0.0f}};
    for (int i = 0; i < 3; ++i)
    {
        entt::entity e = doc.ecs.registry.create();
        doc.ecs.registry.emplace<NameComponent>(e, names[i]);
        Transform& tf = doc.ecs.registry.emplace<Transform>(e);
        tf.translation = spawns[i];
        tf.dirty = true;
        doc.ecs.registry.emplace<VisibleComponent>(e);
        PhysicsBodyComponent body = i == 0 ? T4StaticBody()
            : i == 1         ? T4DynamicBody()
                             : PhysicsBodyComponent{};
        if (i == 2)
        {
            body.kind = PhysicsBodyKind::Kinematic;
            body.mass = 0.0f;
            body.layer = PhysicsLayer::Mechanism;
            body.mask = PhysicsLayer::Dynamic;
        }
        doc.ecs.registry.emplace<PhysicsBodyComponent>(e, body);
        doc.ecs.registry.emplace<PhysicsShapeComponent>(e, T4SphereShape());
        ScriptComponent sc;
        sc.asset.kind = AssetKind::Script;
        sc.asset.path = "authority.lua";
        sc.asset.sourceKey = "lua:asset=authority.lua";
        doc.ecs.registry.emplace<ScriptComponent>(e, sc);
        doc.AssignNewUuid(e);
        ids[i] = doc.ecs.registry.get<EntityIdComponent>(e).id;
    }

    T4NullBridge bridge;
    AssetResolutionContext assetContext;
    std::vector<AssetDiagnostic> assetDiagnostics;
    ScriptSystem scriptSys(uuidProv, assetContext, assetDiagnostics);
    RuntimeSceneController ctrl;
    RuntimeCommandSink sink(ctrl);
    ctrl.SetRuntimeUuidProvider(&uuidProv);
    ctrl.SetLifecycleObserver(&scriptSys);
    ctrl.SetScriptDispatch(&scriptSys);
    ctrl.SetInputService(nullptr);
    ctrl.SetRuntimeCommandSink(&sink);
    assetContext.assetRoot = doc.metadata.sourcePath.parent_path();
    assetContext.database = nullptr;
    Error err;
    REQUIRE(ctrl.Play(doc, bridge, err));

    for (int i = 0; i < 3; ++i)
        ctrl.Update(kFixedDt, bridge);

    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    auto readX = [&](const UUID& id) {
        const auto e = runtime->FindByUuid(id);
        const bool resolved = (e != entt::null);
        REQUIRE(resolved);
        return runtime->ecs.registry.get<Transform>(e).translation.x;
    };
    CHECK(readX(ids[0]) == doctest::Approx(-3.0f));
    CHECK(readX(ids[1]) == doctest::Approx(0.0f));
    CHECK(readX(ids[2]) == doctest::Approx(7.0f));
    ctrl.Stop(doc, bridge);
    std::filesystem::remove_all(scriptDir, ec);
}

TEST_CASE("T4 RED_PhysicsBodyValidationRejects: out-of-range authoring values fail atomically")
{
    T4Fixture f;
    const UUID id = f.Create("Victim");
    const uint64_t rev0 = f.manager.AuthoringRevision();

    auto expectBodyReject = [&](PhysicsBodyComponent bad) {
        const auto r = f.manager.SetPhysicsBodyState(id, bad);
        CHECK_FALSE(r.success);
        CHECK(r.error.code == Error::InvalidArgument);
        CHECK_FALSE(f.manager.GetPhysicsBody(id).has_value());
    };
    PhysicsBodyComponent negative = T4DynamicBody();
    negative.mass = -1.0f;
    expectBodyReject(negative);
    PhysicsBodyComponent bouncy = T4DynamicBody();
    bouncy.restitution = 2.0f;
    expectBodyReject(bouncy);
    PhysicsBodyComponent grouped = T4DynamicBody();
    grouped.layer = PhysicsLayer::Dynamic | PhysicsLayer::Mechanism;
    expectBodyReject(grouped);
    PhysicsBodyComponent deaf = T4DynamicBody();
    deaf.mask = 0;
    expectBodyReject(deaf);
    PhysicsBodyComponent ccd = T4DynamicBody();
    ccd.ccdEnabled = true;
    ccd.ccdMotionThreshold = 0.0f;
    expectBodyReject(ccd);
    PhysicsBodyComponent unknownKind = T4DynamicBody();
    unknownKind.kind = (PhysicsBodyKind)99;
    expectBodyReject(unknownKind);
    // Unknown entity is InvalidEntity, not InvalidArgument.
    const auto missing =
        f.manager.SetPhysicsBodyState(f.ids.CreateV4(), T4DynamicBody());
    CHECK_FALSE(missing.success);
    CHECK(missing.error.code == Error::InvalidEntity);

    auto expectShapeReject = [&](PhysicsShapeComponent bad) {
        const auto r = f.manager.SetPhysicsShapeState(id, bad);
        CHECK_FALSE(r.success);
        CHECK(r.error.code == Error::InvalidArgument);
        CHECK_FALSE(f.manager.GetPhysicsShape(id).has_value());
    };
    PhysicsShapeComponent flat = T4SphereShape();
    flat.radius = 0.0f;
    expectShapeReject(flat);
    PhysicsShapeComponent unknownShape = T4SphereShape();
    unknownShape.shape = (PhysicsShapeKind)99;
    expectShapeReject(unknownShape);
    // Safe-margin rule at authoring: margin must be strictly below the
    // smallest final half-extent (entity scale is 1 here).
    PhysicsShapeComponent tinyBox = T4BoxShape(0.01f);
    tinyBox.collisionMargin = 0.04f;
    expectShapeReject(tinyBox);
    PhysicsShapeComponent equalBox = T4BoxShape(0.04f);
    equalBox.collisionMargin = 0.04f;
    expectShapeReject(equalBox);
    PhysicsShapeComponent validSmallBox = T4BoxShape(0.05f);
    validSmallBox.collisionMargin = 0.04f;
    CHECK(f.manager.SetPhysicsShapeState(id, validSmallBox).success);
    REQUIRE(f.manager.SetPhysicsShapeState(id, std::nullopt).success);
    // Hull size rule at authoring (resolvable asset): the 0.01 hull refuses
    // a 0.04 margin before Play is ever involved.
    T4TempAssets hullAssets;
    hullAssets.MakeCube("tiny.obj", 0.01f);
    f.manager.SetAssetResolutionContext(
        AssetResolutionContext{hullAssets.dir, nullptr});
    REQUIRE(f.manager.SetPhysicsBodyState(id, T4StaticBody()).success);
    PhysicsShapeComponent tinyHull;
    tinyHull.shape = PhysicsShapeKind::ConvexHull;
    tinyHull.hull = T4ModelRef("tiny.obj", "obj:whole-model");
    tinyHull.collisionMargin = 0.04f;
    {
        const auto r = f.manager.SetPhysicsShapeState(id, tinyHull);
        CHECK_FALSE(r.success);
        CHECK(r.error.code == Error::InvalidArgument);
        CHECK_FALSE(f.manager.GetPhysicsShape(id).has_value());
    }
    REQUIRE(f.manager.SetPhysicsBodyState(id, std::nullopt).success);
    PhysicsShapeComponent textured = T4SphereShape();
    textured.shape = PhysicsShapeKind::ConvexHull;
    textured.hull.kind = AssetKind::Texture;
    textured.hull.path = "x.obj";
    textured.hull.sourceKey = "obj:whole-model";
    expectShapeReject(textured);
    // Trigger/layer agreement at authoring time.
    REQUIRE(f.manager.SetPhysicsBodyState(id, T4DynamicBody()).success);
    PhysicsShapeComponent ghost = T4SphereShape();
    ghost.isTrigger = true;
    expectShapeReject(ghost);
    REQUIRE(f.manager.SetPhysicsBodyState(id, std::nullopt).success);

    // Zero mutation, zero revision drift across every refusal above.
    CHECK(f.manager.AuthoringRevision() > rev0);
    const uint64_t revQuiet = f.manager.AuthoringRevision();
    CHECK_FALSE(f.manager.SetPhysicsBodyState(id, negative).success);
    CHECK(f.manager.AuthoringRevision() == revQuiet);
}

TEST_CASE("T4 RED_PhysicsPrefabMemberEditRejected: linked members refuse every physics edit")
{
    T4Fixture f;
    const UUID member = f.Create("Member");
    PrefabMemberComponent link;
    link.instanceId = f.ids.CreateV4();
    link.templateId = f.ids.CreateV4();
    f.Registry().emplace<PrefabMemberComponent>(f.Handle(member), link);
    const UUID plain = f.Create("Plain");
    const uint64_t rev0 = f.manager.AuthoringRevision();

    // All attach/edit/remove operations on the linked member refuse loudly
    // before mutation, for both component types.
    const PhysicsBodyComponent bodyA = T4DynamicBody();
    PhysicsBodyComponent bodyB = T4DynamicBody(2.0f);
    const PhysicsShapeComponent shapeA = T4SphereShape();
    PhysicsShapeComponent shapeB = T4SphereShape(0.25f);
    CHECK_FALSE(f.manager.SetPhysicsBodyState(member, bodyA).success);
    CHECK_FALSE(f.manager.SetPhysicsShapeState(member, shapeA).success);
    // Seed ordinary-entity state directly (bypasses history) so member edits
    // below exercise the edit/remove paths rather than attach.
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(member), bodyA);
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(member), shapeA);
    CHECK_FALSE(f.manager.SetPhysicsBodyState(member, bodyB).success);
    CHECK_FALSE(f.manager.SetPhysicsShapeState(member, shapeB).success);
    CHECK_FALSE(
        f.manager.SetPhysicsBodyState(member, std::nullopt).success);
    CHECK_FALSE(
        f.manager.SetPhysicsShapeState(member, std::nullopt).success);
    // Zero mutation: the directly seeded values are byte-identical.
    CHECK(f.Registry().get<PhysicsBodyComponent>(f.Handle(member)) == bodyA);
    CHECK(f.Registry().get<PhysicsShapeComponent>(f.Handle(member)) == shapeA);
    CHECK(f.manager.AuthoringRevision() == rev0);

    // Commands through history refuse the same way and record nothing.
    EditorCommandHistory history;
    auto refused = MakeSetPhysicsBodyCommandIfEffective(member, bodyA, bodyB);
    REQUIRE(refused != nullptr);
    CHECK_FALSE(history.Execute(std::move(refused), f.manager).success);
    CHECK_FALSE(history.CanUndo());
    CHECK(f.manager.AuthoringRevision() == rev0);

    // Ordinary entities succeed through the same seams.
    const auto ok = f.manager.SetPhysicsBodyState(plain, bodyA);
    CHECK(ok.success);
    CHECK(ok.syncImpact == SyncImpact::None);
    CHECK(f.manager.GetPhysicsBody(plain) == bodyA);
    CHECK(f.manager.AuthoringRevision() > rev0);
}

TEST_CASE("T4 GREEN_PhysicsAuthoringUndoRedo: body and shape edits are exact with revision and no GPU sync")
{
    T4Fixture f;
    const UUID id = f.Create("Authored");
    EditorCommandHistory history;
    const uint64_t rev0 = f.manager.AuthoringRevision();

    const PhysicsBodyComponent bodyA = T4DynamicBody(1.0f);
    PhysicsBodyComponent bodyB = T4DynamicBody(2.5f);
    bodyB.friction = 0.9f;
    const PhysicsShapeComponent shapeA = T4SphereShape(0.5f);
    const PhysicsShapeComponent shapeB = T4BoxShape(0.25f);

    // No-op factory suppression: identical states produce no command.
    CHECK(MakeSetPhysicsBodyCommandIfEffective(id, bodyA, bodyA) == nullptr);
    CHECK(MakeSetPhysicsBodyCommandIfEffective(id, std::nullopt, std::nullopt) ==
          nullptr);

    // Attach body: one revision bump, SyncImpact None, undoable.
    auto addBody = MakeSetPhysicsBodyCommandIfEffective(id, std::nullopt, bodyA);
    REQUIRE(addBody != nullptr);
    CHECK(addBody->Description() == "Add Physics Body");
    const auto applied = history.Execute(std::move(addBody), f.manager);
    REQUIRE(applied.success);
    CHECK(applied.syncImpact == SyncImpact::None);
    CHECK(f.manager.GetPhysicsBody(id) == bodyA);
    CHECK(f.manager.AuthoringRevision() > rev0);
    REQUIRE(history.CanUndo());

    // Edit body: exact new value; Undo restores the exact old value.
    auto editBody = MakeSetPhysicsBodyCommandIfEffective(id, bodyA, bodyB);
    REQUIRE(editBody != nullptr);
    REQUIRE(history.Execute(std::move(editBody), f.manager).success);
    CHECK(f.manager.GetPhysicsBody(id) == bodyB);
    REQUIRE(history.Undo(f.manager).success);
    CHECK(f.manager.GetPhysicsBody(id) == bodyA);
    REQUIRE(history.Redo(f.manager).success);
    CHECK(f.manager.GetPhysicsBody(id) == bodyB);

    // Attach + remove shape around the body: exact presence tracking.
    auto addShape =
        MakeSetPhysicsShapeCommandIfEffective(id, std::nullopt, shapeA);
    REQUIRE(addShape != nullptr);
    REQUIRE(history.Execute(std::move(addShape), f.manager).success);
    CHECK(f.manager.GetPhysicsShape(id) == shapeA);
    auto swapShape =
        MakeSetPhysicsShapeCommandIfEffective(id, shapeA, shapeB);
    REQUIRE(history.Execute(std::move(swapShape), f.manager).success);
    CHECK(f.manager.GetPhysicsShape(id) == shapeB);
    REQUIRE(history.Undo(f.manager).success);
    CHECK(f.manager.GetPhysicsShape(id) == shapeA);
    auto removeShape =
        MakeSetPhysicsShapeCommandIfEffective(id, shapeA, std::nullopt);
    REQUIRE(removeShape != nullptr);
    CHECK(removeShape->Description() == "Remove Physics Shape");
    REQUIRE(history.Execute(std::move(removeShape), f.manager).success);
    CHECK_FALSE(f.manager.GetPhysicsShape(id).has_value());
    REQUIRE(history.Undo(f.manager).success);
    CHECK(f.manager.GetPhysicsShape(id) == shapeA);

    // An invalid after-state surfaces the manager failure without recording.
    const size_t depth = history.UndoDepthForTest();
    PhysicsBodyComponent invalid = bodyB;
    invalid.restitution = 5.0f;
    auto badEdit = MakeSetPhysicsBodyCommandIfEffective(id, bodyB, invalid);
    REQUIRE(badEdit != nullptr);
    CHECK_FALSE(history.Execute(std::move(badEdit), f.manager).success);
    CHECK(history.UndoDepthForTest() == depth);
    CHECK(f.manager.GetPhysicsBody(id) == bodyB);

    // The authored pair Plays: authoring flows into the candidate world.
    auto useShape =
        MakeSetPhysicsShapeCommandIfEffective(id, shapeA, shapeA);
    CHECK(useShape == nullptr);
    T4NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    CHECK(ctrl.PhysicsBodyCount() == 1);
    CHECK(ctrl.PhysicsShapeCount() == 1);
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
}
