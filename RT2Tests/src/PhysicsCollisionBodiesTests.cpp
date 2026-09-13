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
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
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
        std::filesystem::create_directories(dir, ec);
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
        for (int i = 0; i < 260000; ++i)
            out << "v 0 0 " << (i % 1000) << "\n";
        out << "f 1 2 3\n";
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
    std::filesystem::create_directories(scriptDir, ec);
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
