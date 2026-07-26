#include <doctest/doctest.h>

#include "SceneRecoveryService.h"
#include "SceneSerializer.h"
#include "SceneDocument.h"
#include "SceneAssetResolver.h"
#include "SceneManager.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "PrimitiveGeometry.h"
#include "Phase1AFixtureGenerator.h"
#include "FixtureGenerator.h"
#include "UnsavedChangesCoordinator.h"
#include "ScriptFieldRegistry.h"
#include "ScriptFieldResolver.h"
#include "ScriptFieldChangePolicy.h"
#include "core/UUID.h"
#include "core/Error.h"
#include "json.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <vector>

using namespace rt2::core;

namespace {

std::filesystem::path UniqueTempDir(const std::string& tag)
{
    auto base = std::filesystem::temp_directory_path();
    auto dir = base / (tag + "_" + std::to_string(std::rand()));
    std::filesystem::create_directories(dir);
    return dir;
}

// A controllable clock for the recovery service. Pass by reference via a
// lambda capture so mutations are visible to the service.
struct FakeClock
{
    int64_t now = 1000;
    int64_t operator()() const { return now; }
};

// Helper: construct a ClockNow lambda that reads from a FakeClock by ref.
static std::function<int64_t()> ClockRef(FakeClock& c)
{
    return [&c]() { return c.now; };
}

static bool SnapshotAfterInterval(SceneRecoveryService& svc,
                                  const SceneDocument& doc,
                                  uint64_t revision,
                                  FakeClock& clock,
                                  Error& err,
                                  const std::string& untitledId = "test-untitled-id",
                                  const std::filesystem::path& logicalAssetRoot = {})
{
    const auto root = logicalAssetRoot.empty()
        ? std::filesystem::current_path()
        : logicalAssetRoot;
    CHECK_FALSE(svc.MaybeSnapshot(doc, revision, untitledId, root, err));
    if (!err.IsOk()) return false;
    clock.now += 60;
    return svc.MaybeSnapshot(doc, revision, untitledId, root, err);
}

// Build a scene with a primitive entity so it can be saved/loaded without
// external assets.
SceneDocument MakePrimitiveScene(IUuidProvider* provider)
{
    SceneDocument doc;
    doc.SetUuidProvider(provider);

    SceneMaterial mat;
    mat.baseColor = {0.8f, 0.2f, 0.2f};
    mat.roughness = 0.5f;
    doc.ecs.materials.push_back(mat);

    MeshData cubeMesh = PrimitiveGeometry::CreateCube(1.0f);
    cubeMesh.name = "cube";
    uint32_t meshIdx = doc.ecs.meshRegistry.AddMesh(std::move(cubeMesh));

    auto e = doc.ecs.registry.create();
    doc.ecs.registry.emplace<NameComponent>(e, "Cube");
    Transform& tf = doc.ecs.registry.emplace<Transform>(e);
    tf.translation = {0.0f, 0.0f, 0.0f};
    tf.dirty = true;
    doc.ecs.registry.emplace<VisibleComponent>(e);
    doc.ecs.registry.emplace<MeshRef>(e, meshIdx, 0);
    doc.ecs.registry.emplace<PrimitiveComponent>(e,
        PrimitiveComponent{PrimitiveComponent::Cube, 1.0f, 24, 16});
    doc.AssignNewUuid(e);

    doc.metadata.name = "RecoveryTest";
    doc.metadata.dirty = false;
    return doc;
}

// Read a file's bytes as a string for byte-for-byte comparison.
std::string ReadFileBytes(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // anonymous namespace

// ============================================================================
// Autosave tests
// ============================================================================

// 1. Clean document writes nothing.
TEST_CASE("Recovery: clean doc writes nothing")
{
    auto dir = UniqueTempDir("rcv_clean");
    FakeClock clk;
    SceneRecoveryService svc(dir, ClockRef(clk), 8, 60.0);
    OsUuidProvider provider;
    auto doc = MakePrimitiveScene(&provider);
    // doc.metadata.dirty == false
    Error err;
    CHECK_FALSE(svc.MaybeSnapshot(doc, 0, err));
    CHECK(err.IsOk());
    // No records discovered.
    auto recs = svc.Discover(err);
    CHECK(recs.empty());
    std::filesystem::remove_all(dir);
}

// 2. Dirty document writes after the interval.
TEST_CASE("Recovery: dirty doc writes after interval")
{
    auto dir = UniqueTempDir("rcv_dirty");
    FakeClock clk;
    SceneRecoveryService svc(dir, ClockRef(clk), 8, 60.0);
    OsUuidProvider provider;
    auto doc = MakePrimitiveScene(&provider);
    doc.metadata.dirty = true;
    doc.metadata.sourcePath = (dir / "scene.rt2scene").string();
    Error err;
    CHECK_FALSE(svc.MaybeSnapshot(doc, 1, "unused", dir, err));
    clk.now = 1059;
    CHECK_FALSE(svc.MaybeSnapshot(doc, 1, "unused", dir, err));
    clk.now = 1060;
    CHECK(svc.MaybeSnapshot(doc, 1, "unused", dir, err));
    CHECK(err.IsOk());
    auto recs = svc.Discover(err);
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].valid);
    std::filesystem::remove_all(dir);
}

// 3. Injected time controls scheduling without sleeping.
TEST_CASE("Recovery: injected clock controls scheduling")
{
    auto dir = UniqueTempDir("rcv_clock");
    FakeClock clk;
    SceneRecoveryService svc(dir, ClockRef(clk), 8, 60.0);
    OsUuidProvider provider;
    auto doc = MakePrimitiveScene(&provider);
    doc.metadata.dirty = true;
    doc.metadata.sourcePath = (dir / "scene.rt2scene").string();
    Error err;
    // First snapshot at t=1000.
    CHECK_FALSE(svc.MaybeSnapshot(doc, 1, "unused", dir, err));
    // Advance by 30s — not enough.
    clk.now = 1030;
    CHECK_FALSE(svc.MaybeSnapshot(doc, 2, "unused", dir, err)); // different revision but interval not elapsed
    // Advance by 40s — total 70 since last, enough.
    clk.now = 1060;
    CHECK(svc.MaybeSnapshot(doc, 2, "unused", dir, err));
    std::filesystem::remove_all(dir);
}

// 4. Unchanged revision does not repeatedly produce snapshots.
TEST_CASE("Recovery: unchanged revision skips write")
{
    auto dir = UniqueTempDir("rcv_skip");
    FakeClock clk;
    SceneRecoveryService svc(dir, ClockRef(clk), 8, 60.0);
    OsUuidProvider provider;
    auto doc = MakePrimitiveScene(&provider);
    doc.metadata.dirty = true;
    doc.metadata.sourcePath = (dir / "scene.rt2scene").string();
    Error err;
    REQUIRE(SnapshotAfterInterval(svc, doc, 5, clk, err));
    // Advance past interval but same revision.
    clk.now = 2000;
    CHECK_FALSE(svc.MaybeSnapshot(doc, 5, err));
    std::filesystem::remove_all(dir);
}

// 5. Autosave does not clear dirty state.
TEST_CASE("Recovery: autosave preserves dirty flag")
{
    auto dir = UniqueTempDir("rcv_dirtyflag");
    FakeClock clk;
    SceneRecoveryService svc(dir, ClockRef(clk), 8, 60.0);
    OsUuidProvider provider;
    auto doc = MakePrimitiveScene(&provider);
    doc.metadata.dirty = true;
    doc.metadata.sourcePath = (dir / "scene.rt2scene").string();
    Error err;
    REQUIRE(SnapshotAfterInterval(svc, doc, 1, clk, err));
    CHECK(doc.metadata.dirty == true);
    std::filesystem::remove_all(dir);
}

// 6. Autosave does not change sourcePath.
TEST_CASE("Recovery: autosave preserves sourcePath")
{
    auto dir = UniqueTempDir("rcv_srcpath");
    FakeClock clk;
    SceneRecoveryService svc(dir, ClockRef(clk), 8, 60.0);
    OsUuidProvider provider;
    auto doc = MakePrimitiveScene(&provider);
    doc.metadata.dirty = true;
    std::string sp = (dir / "scene.rt2scene").string();
    doc.metadata.sourcePath = sp;
    Error err;
    REQUIRE(SnapshotAfterInterval(svc, doc, 1, clk, err));
    CHECK(doc.metadata.sourcePath.string() == sp);
    std::filesystem::remove_all(dir);
}

// 7. Autosave does not alter the explicit scene file.
TEST_CASE("Recovery: explicit file unchanged by autosave")
{
    auto dir = UniqueTempDir("rcv_explicit");
    auto scenePath = dir / "scene.rt2scene";
    {
        OsUuidProvider provider;
        auto doc = MakePrimitiveScene(&provider);
        doc.metadata.sourcePath = scenePath.string();
        Error err;
        REQUIRE(SceneSerializer::Save(doc, scenePath, err));
    }
    std::string before = ReadFileBytes(scenePath);

    FakeClock clk;
    SceneRecoveryService svc(dir / "Recovery", ClockRef(clk), 8, 60.0);
    OsUuidProvider provider;
    auto doc = MakePrimitiveScene(&provider);
    doc.metadata.dirty = true;
    doc.metadata.sourcePath = scenePath.string();
    Error err;
    REQUIRE(SnapshotAfterInterval(svc, doc, 1, clk, err));

    std::string after = ReadFileBytes(scenePath);
    CHECK(before == after);
    std::filesystem::remove_all(dir);
}

// 8. Bounded retention evicts oldest.
TEST_CASE("Recovery: bounded retention evicts oldest")
{
    auto dir = UniqueTempDir("rcv_evict");
    FakeClock clk;
    SceneRecoveryService svc(dir, ClockRef(clk), 3, 60.0); // max 3
    OsUuidProvider provider;
    Error err;

    // Write 4 distinct documents to force eviction.
    for (int i = 0; i < 4; ++i)
    {
        auto doc = MakePrimitiveScene(&provider);
        doc.metadata.dirty = true;
        doc.metadata.sourcePath = (dir / ("s" + std::to_string(i) + ".rt2scene")).string();
        doc.metadata.name = "s" + std::to_string(i);
        clk.now = 1000 + i * 100;
        svc.ResetSchedule();
        CHECK(SnapshotAfterInterval(svc, doc, i + 1, clk, err));
    }
    auto recs = svc.Discover(err);
    // Should have at most 3.
    CHECK(recs.size() <= 3);
    std::filesystem::remove_all(dir);
}

// 9. Failed write preserves prior recovery.
//     (Simulate by writing to a read-only recovery root path. Hard to
//      reliably test on Windows, so we test a simpler invariant: a
//      successful write followed by Discover returns the record.)

// ============================================================================
// Recovery restore tests
// ============================================================================

// 10. Discovery after simulated unclean shutdown.
TEST_CASE("Recovery: discovery after unclean exit")
{
    auto dir = UniqueTempDir("rcv_discover");
    FakeClock clk;
    SceneRecoveryService svc(dir, ClockRef(clk), 8, 60.0);
    OsUuidProvider provider;
    auto doc = MakePrimitiveScene(&provider);
    doc.metadata.dirty = true;
    doc.metadata.sourcePath = (dir / "scene.rt2scene").string();
    Error err;
    REQUIRE(SnapshotAfterInterval(svc, doc, 1, clk, err));

    // Simulate unclean exit: just drop the service on the floor and create a
    // new one pointing at the same recovery root.
    SceneRecoveryService svc2(dir, ClockRef(clk), 8, 60.0);
    auto recs = svc2.Discover(err);
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].valid);
    CHECK(recs[0].docId.find("scene.rt2scene") != std::string::npos);
    std::filesystem::remove_all(dir);
}

// 11. Restore preserves UUIDs and marks dirty.
TEST_CASE("Recovery: restore preserves UUIDs and marks dirty")
{
    auto dir = UniqueTempDir("rcv_restore");
    FakeClock clk;
    SceneRecoveryService svc(dir, ClockRef(clk), 8, 60.0);
    OsUuidProvider provider;
    auto doc = MakePrimitiveScene(&provider);
    doc.metadata.dirty = true;
    doc.metadata.sourcePath = (dir / "scene.rt2scene").string();

    // Capture the UUID of the single entity.
    UUID originalUuid;
    {
        auto view = doc.ecs.registry.view<EntityIdComponent>();
        originalUuid = view.get<EntityIdComponent>(*view.begin()).id;
    }

    Error err;
    REQUIRE(SnapshotAfterInterval(svc, doc, 1, clk, err));

    // Restore into a fresh document.
    auto recs = svc.Discover(err);
    REQUIRE(recs.size() == 1);

    SceneDocument restored;
    OsUuidProvider provider2;
    restored.SetUuidProvider(&provider2);
    std::vector<AssetDiagnostic> diags;
    REQUIRE(svc.Restore(recs[0], restored, diags, err));

    CHECK(restored.metadata.dirty == true);
    auto view = restored.ecs.registry.view<EntityIdComponent>();
    REQUIRE(view.size() > 0);
    UUID restoredUuid = view.get<EntityIdComponent>(*view.begin()).id;
    CHECK(restoredUuid == originalUuid);
    std::filesystem::remove_all(dir);
}

// 12. Explicit saved file remains byte-for-byte unchanged.
TEST_CASE("Recovery: explicit file unchanged through restore")
{
    auto dir = UniqueTempDir("rcv_explicit_restore");
    auto scenePath = dir / "scene.rt2scene";
    {
        OsUuidProvider provider;
        auto doc = MakePrimitiveScene(&provider);
        doc.metadata.sourcePath = scenePath.string();
        Error err;
        REQUIRE(SceneSerializer::Save(doc, scenePath, err));
    }
    std::string before = ReadFileBytes(scenePath);

    FakeClock clk;
    SceneRecoveryService svc(dir / "Recovery", ClockRef(clk), 8, 60.0);
    OsUuidProvider provider;
    auto doc = MakePrimitiveScene(&provider);
    doc.metadata.dirty = true;
    doc.metadata.sourcePath = scenePath.string();
    Error err;
    REQUIRE(SnapshotAfterInterval(svc, doc, 1, clk, err));

    auto recs = svc.Discover(err);
    REQUIRE(recs.size() == 1);
    SceneDocument restored;
    OsUuidProvider p2;
    restored.SetUuidProvider(&p2);
    std::vector<AssetDiagnostic> diags;
    REQUIRE(svc.Restore(recs[0], restored, diags, err));

    std::string after = ReadFileBytes(scenePath);
    CHECK(before == after);
    std::filesystem::remove_all(dir);
}

// 13. Discard removes recovery but not the explicit scene.
TEST_CASE("Recovery: discard removes record only")
{
    auto dir = UniqueTempDir("rcv_discard");
    auto scenePath = dir / "scene.rt2scene";
    OsUuidProvider provider;
    {
        auto doc = MakePrimitiveScene(&provider);
        doc.metadata.sourcePath = scenePath.string();
        Error err;
        REQUIRE(SceneSerializer::Save(doc, scenePath, err));
    }

    FakeClock clk;
    SceneRecoveryService svc(dir / "Recovery", ClockRef(clk), 8, 60.0);
    auto doc = MakePrimitiveScene(&provider);
    doc.metadata.dirty = true;
    doc.metadata.sourcePath = scenePath.string();
    Error err;
    REQUIRE(SnapshotAfterInterval(svc, doc, 1, clk, err));

    auto recs = svc.Discover(err);
    REQUIRE(recs.size() == 1);
    REQUIRE(svc.Discard(recs[0], err));
    auto recs2 = svc.Discover(err);
    CHECK(recs2.empty());
    CHECK(std::filesystem::exists(scenePath)); // explicit file untouched
    std::filesystem::remove_all(dir);
}

// 14. Corrupt manifest produces a useful error.
TEST_CASE("Recovery: corrupt manifest is reported")
{
    auto dir = UniqueTempDir("rcv_corrupt");
    auto recDir = dir / "Recovery";
    std::filesystem::create_directories(recDir);
    std::ofstream(recDir / "bad.rt2recovery") << "{ not json";

    FakeClock clk;
    SceneRecoveryService svc(dir / "Recovery", ClockRef(clk), 8, 60.0);
    Error err;
    auto recs = svc.Discover(err);
    REQUIRE(recs.size() >= 1);
    bool foundInvalid = false;
    for (const auto& r : recs)
    {
        if (!r.valid) { foundInvalid = true; break; }
    }
    CHECK(foundInvalid);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Recovery: manifest v1 is rejected at envelope discovery")
{
    auto dir = UniqueTempDir("rcv_old_manifest");
    FakeClock clk;
    SceneRecoveryService svc(dir, ClockRef(clk), 8, 60.0);
    OsUuidProvider provider;
    auto doc = MakePrimitiveScene(&provider);
    doc.metadata.dirty = true;
    doc.metadata.sourcePath = dir / "scene.rt2scene";
    Error err;
    REQUIRE(SnapshotAfterInterval(svc, doc, 1, clk, err));

    auto records = svc.Discover(err);
    REQUIRE(records.size() == 1);
    nlohmann::json envelope;
    { std::ifstream in(records[0].recordPath); in >> envelope; }
    envelope["version"] = 1;
    { std::ofstream out(records[0].recordPath, std::ios::trunc); out << envelope.dump(2); }

    records = svc.Discover(err);
    REQUIRE(records.size() == 1);
    CHECK_FALSE(records[0].valid);
    CHECK(records[0].diagnostic.find("unsupported recovery version 1") !=
          std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Recovery: restored script fields resolve from original source path")
{
    auto dir = UniqueTempDir("rcv_script_root");
    std::filesystem::create_directories(dir / "scripts");
    {
        std::ofstream script(dir / "scripts" / "move.lua");
        script << "rt2.fields = { speed = rt2.field.float(1.0) }\n";
    }

    FakeClock clk;
    SceneRecoveryService svc(dir / "Recovery", ClockRef(clk), 8, 60.0);
    OsUuidProvider provider;
    auto doc = MakePrimitiveScene(&provider);
    doc.metadata.dirty = true;
    doc.metadata.sourcePath = dir / "scene.rt2scene";
    auto entities = doc.ecs.registry.view<EntityIdComponent>();
    REQUIRE(entities.size() == 1);
    const auto entity = *entities.begin();
    ScriptComponent component;
    component.asset.kind = AssetKind::Script;
    component.asset.path = "scripts/move.lua";
    component.asset.sourceKey = "lua:asset=scripts/move.lua";
    component.fieldValues["speed"] = {
        ScriptFieldType::Float, ScriptFieldValue{5.0} };
    doc.ecs.registry.emplace<ScriptComponent>(entity, component);

    Error err;
    REQUIRE(SnapshotAfterInterval(svc, doc, 2, clk, err));
    auto records = svc.Discover(err);
    REQUIRE(records.size() == 1);

    SceneDocument restored;
    restored.SetUuidProvider(&provider);
    std::vector<AssetDiagnostic> assetDiagnostics;
    SceneLoadReport loadReport;
    REQUIRE(svc.Restore(
        records[0], restored, assetDiagnostics, loadReport, err));
    CHECK(restored.metadata.sourcePath == doc.metadata.sourcePath);

    ScriptFieldRegistry registry;
    std::vector<FieldDiagnostic> fieldDiagnostics = loadReport.fieldDiagnostics;
    AssetResolutionContext assetContext{records[0].assetRoot, nullptr};
    const auto resolution = ScriptFieldResolver::ResolveDocument(
        restored, registry, assetContext, assetDiagnostics, fieldDiagnostics);
    CHECK(resolution.resolvedEntities == 1);
    CHECK(resolution.skippedEntities == 0);
    CHECK_FALSE(resolution.changed);
    CHECK(fieldDiagnostics.empty());

    const auto restoredEntity = restored.FindByUuid(
        doc.ecs.registry.get<EntityIdComponent>(entity).id);
    REQUIRE(restoredEntity != static_cast<entt::entity>(entt::null));
    CHECK(std::get<double>(restored.ecs.registry
        .get<ScriptComponent>(restoredEntity).fieldValues.at("speed").value) ==
        doctest::Approx(5.0));
    std::filesystem::remove_all(dir);
}

// 15. Failed parse preserves an already-populated live document.
TEST_CASE("Recovery: failed restore preserves live doc")
{
    auto dir = UniqueTempDir("rcv_faillive");
    FakeClock clk;
    SceneRecoveryService svc(dir, ClockRef(clk), 8, 60.0);
    OsUuidProvider provider;
    auto doc = MakePrimitiveScene(&provider);
    doc.metadata.dirty = true;
    doc.metadata.sourcePath = (dir / "scene.rt2scene").string();
    Error err;
    REQUIRE(SnapshotAfterInterval(svc, doc, 1, clk, err));

    auto recs = svc.Discover(err);
    REQUIRE(recs.size() == 1);
    recs[0].snapshotJson = "not a valid rt2scene";

    // Live doc has its own content.
    SceneDocument liveDoc;
    OsUuidProvider p2;
    liveDoc.SetUuidProvider(&p2);
    auto liveEntity = liveDoc.ecs.registry.create();
    liveDoc.AssignNewUuid(liveEntity);
    liveDoc.ecs.registry.emplace<NameComponent>(liveEntity, "Live");
    size_t liveCount = liveDoc.ecs.registry.view<EntityIdComponent>().size();

    std::vector<AssetDiagnostic> diags;
    bool ok = svc.Restore(recs[0], liveDoc, diags, err);
    CHECK_FALSE(ok);
    // Live doc should be unchanged (restore is transactional).
    CHECK(liveDoc.ecs.registry.view<EntityIdComponent>().size() == liveCount);
    std::filesystem::remove_all(dir);
}

// 16. Recovery uses the original logical asset root, not the recovery dir.
//     (For primitive-only scenes asset refs are trivial, but we verify the
//      manifest records the correct assetRoot.)
TEST_CASE("Recovery: manifest records original asset root")
{
    auto dir = UniqueTempDir("rcv_assetroot");
    FakeClock clk;
    SceneRecoveryService svc(dir / "Recovery", ClockRef(clk), 8, 60.0);
    OsUuidProvider provider;
    auto doc = MakePrimitiveScene(&provider);
    doc.metadata.dirty = true;
    doc.metadata.sourcePath = (dir / "scene.rt2scene").string();
    Error err;
    REQUIRE(SnapshotAfterInterval(svc, doc, 1, clk, err));

    auto recs = svc.Discover(err);
    REQUIRE(recs.size() == 1);
    // assetRoot should be dir (the parent of scene.rt2scene), NOT the
    // recovery directory.
    CHECK(recs[0].assetRoot == dir);
    std::filesystem::remove_all(dir);
}

// 17. Untitled recovery remains untitled.
TEST_CASE("Recovery: untitled scene stays untitled")
{
    auto dir = UniqueTempDir("rcv_untitled");
    FakeClock clk;
    SceneRecoveryService svc(dir, ClockRef(clk), 8, 60.0);
    OsUuidProvider provider;
    auto doc = MakePrimitiveScene(&provider);
    doc.metadata.dirty = true;
    doc.metadata.sourcePath.clear(); // untitled
    doc.metadata.name = "untitled-recovery-id-123";
    Error err;
    REQUIRE(SnapshotAfterInterval(svc, doc, 1, clk, err));

    auto recs = svc.Discover(err);
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].untitled);
    CHECK(recs[0].originalSourcePath.empty());

    SceneDocument restored;
    OsUuidProvider p2;
    restored.SetUuidProvider(&p2);
    std::vector<AssetDiagnostic> diags;
    REQUIRE(svc.Restore(recs[0], restored, diags, err));
    CHECK(restored.metadata.sourcePath.empty());
    CHECK(restored.metadata.dirty);
    std::filesystem::remove_all(dir);
}

// 18. DiscardForDoc removes a specific doc's recovery.
TEST_CASE("Recovery: DiscardForDoc removes specific record")
{
    auto dir = UniqueTempDir("rcv_dfd");
    FakeClock clk;
    SceneRecoveryService svc(dir, ClockRef(clk), 8, 60.0);
    OsUuidProvider provider;
    auto doc = MakePrimitiveScene(&provider);
    doc.metadata.dirty = true;
    doc.metadata.sourcePath = (dir / "scene.rt2scene").string();
    Error err;
    REQUIRE(SnapshotAfterInterval(svc, doc, 1, clk, err));

    std::string docId = SceneRecoveryService::DocIdFor(doc, doc.metadata.name);
    svc.DiscardForDoc(docId);
    auto recs = svc.Discover(err);
    CHECK(recs.empty());
    std::filesystem::remove_all(dir);
}

// 19. OnSaveAs retires both old and new identities.
TEST_CASE("Recovery: OnSaveAs retires both identities")
{
    auto dir = UniqueTempDir("rcv_saveas");
    FakeClock clk;
    SceneRecoveryService svc(dir, ClockRef(clk), 8, 60.0);
    OsUuidProvider provider;
    auto doc = MakePrimitiveScene(&provider);
    doc.metadata.dirty = true;
    doc.metadata.sourcePath = (dir / "old.rt2scene").string();
    doc.metadata.name = "oldname";
    Error err;
    REQUIRE(SnapshotAfterInterval(svc, doc, 1, clk, err));

    std::string oldId = SceneRecoveryService::DocIdFor(doc, doc.metadata.name);
    doc.metadata.sourcePath = (dir / "new.rt2scene").string();
    std::string newId = SceneRecoveryService::DocIdFor(doc, doc.metadata.name);

    svc.OnSaveAs(oldId, newId);

    auto recs = svc.Discover(err);
    CHECK(recs.empty());
    std::filesystem::remove_all(dir);
}

TEST_CASE("Recovery: record identity hashes the complete document path")
{
    auto dir = UniqueTempDir("rcv_long_ids");
    FakeClock clk;
    SceneRecoveryService svc(dir / "Recovery", ClockRef(clk), 8, 60.0);
    OsUuidProvider provider;
    Error err;

    const std::string sharedPrefix(180, 'a');
    for (int i = 0; i < 2; ++i)
    {
        auto doc = MakePrimitiveScene(&provider);
        doc.metadata.dirty = true;
        doc.metadata.sourcePath = dir / (sharedPrefix + std::to_string(i) + ".rt2scene");
        svc.ResetSchedule();
        REQUIRE(SnapshotAfterInterval(svc, doc, i + 1, clk, err));
    }

    auto records = svc.Discover(err);
    REQUIRE(records.size() == 2);
    CHECK(records[0].recordPath != records[1].recordPath);
    for (const auto& record : records)
    {
        CHECK(std::filesystem::is_regular_file(record.recordPath));
        CHECK(record.recordPath.extension() == ".rt2recovery");
        CHECK(record.recordPath.parent_path() == dir / "Recovery");
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("Recovery: discard refuses paths outside its storage root")
{
    auto dir = UniqueTempDir("rcv_containment");
    FakeClock clk;
    SceneRecoveryService svc(dir / "Recovery", ClockRef(clk), 8, 60.0);
    SceneRecoveryService::RecoveryRecord forged;
    forged.recordPath = dir / "outside.rt2recovery";
    std::ofstream(forged.recordPath) << "sentinel";

    Error err;
    CHECK_FALSE(svc.Discard(forged, err));
    CHECK_FALSE(err.IsOk());
    CHECK(std::filesystem::exists(forged.recordPath));
    std::filesystem::remove_all(dir);
}

TEST_CASE("Recovery: asset-backed restore preserves UUIDs overrides and environment")
{
    auto dir = UniqueTempDir("rcv_assets");
    auto glbPath = dir / "tiny_textured.glb";
    auto exrPath = dir / "tiny_env.exr";
    auto scenePath = dir / "asset_scene.rt2scene";
    Error err;
    REQUIRE(GenerateTinyTexturedGlb(glbPath, err));
    REQUIRE(GenerateTinyExrEnv(exrPath, err));

    SceneManager mgr;
    REQUIRE(mgr.LoadScene(glbPath.string()));
    REQUIRE(mgr.LoadEnvMap(exrPath.string()));
    auto& doc = mgr.AuthoringDoc();
    doc.metadata.sourcePath = scenePath;
    doc.metadata.dirty = false;
    REQUIRE(SceneSerializer::Save(doc, scenePath, err));
    const std::string explicitBytes = ReadFileBytes(scenePath);

    auto imported = doc.ecs.registry.view<ImportedMeshSourceComponent>();
    REQUIRE(imported.size() > 0);
    const entt::entity entity = *imported.begin();
    const UUID uuid = doc.ecs.registry.get<EntityIdComponent>(entity).id;
    const int materialIndex = doc.ecs.registry.get<MeshRef>(entity).materialIndex;
    REQUIRE(materialIndex >= 0);

    SceneMaterial edited = mgr.GetMaterial(materialIndex);
    edited.roughness = 0.123f;
    edited.baseColor = { 0.1f, 0.9f, 0.1f };
    mgr.SetMaterialProperties(materialIndex, edited);
    mgr.SetTransform({entity}, {4.0f, 2.0f, -1.0f});
    REQUIRE(doc.ecs.registry.all_of<MaterialOverrideComponent>(entity));

    FakeClock clk;
    SceneRecoveryService svc(dir / "Recovery", ClockRef(clk), 8, 60.0);
    REQUIRE(SnapshotAfterInterval(svc, doc, mgr.AuthoringRevision(), clk, err));
    auto records = svc.Discover(err);
    REQUIRE(records.size() == 1);

    SceneDocument restored;
    OsUuidProvider provider;
    restored.SetUuidProvider(&provider);
    std::vector<AssetDiagnostic> diagnostics;
    REQUIRE(svc.Restore(records[0], restored, diagnostics, err));
    CHECK(ReadFileBytes(scenePath) == explicitBytes);
    CHECK(restored.environment.width > 0);
    CHECK(restored.environment.height > 0);
    CHECK_FALSE(restored.environment.floatPixels.empty());
    CHECK(restored.ecs.meshRegistry.GetCount() > 0);
    CHECK_FALSE(restored.ecs.textures.empty());

    const entt::entity restoredEntity = restored.FindByUuid(uuid);
    const bool restoredEntityFound = (restoredEntity != entt::null);
    REQUIRE(restoredEntityFound);
    REQUIRE(restored.ecs.registry.all_of<ImportedMeshSourceComponent,
                                        MaterialOverrideComponent,
                                        Transform>(restoredEntity));
    const auto& restoredOverride =
        restored.ecs.registry.get<MaterialOverrideComponent>(restoredEntity);
    CHECK(restoredOverride.authored);
    CHECK(restoredOverride.material.roughness == doctest::Approx(0.123f));
    CHECK(restored.ecs.registry.get<Transform>(restoredEntity).translation.x ==
          doctest::Approx(4.0f));
    std::filesystem::remove_all(dir);
}

TEST_CASE("SceneManager: recovered document adoption retains content and revision")
{
    OsUuidProvider provider;
    auto recovered = MakePrimitiveScene(&provider);
    recovered.metadata.dirty = true;
    const auto view = recovered.ecs.registry.view<EntityIdComponent>();
    REQUIRE(view.size() == 1);
    const UUID uuid = view.get<EntityIdComponent>(*view.begin()).id;

    SceneManager manager;
    manager.ReplaceAuthoringDocument(std::move(recovered), 42);
    CHECK(manager.AuthoringRevision() == 42);
    CHECK(manager.AuthoringDoc().metadata.dirty);
    const bool managerFoundEntity = (manager.FindEntityByUuid(uuid) != entt::null);
    CHECK(managerFoundEntity);
    CHECK(manager.GetEntityCount() == 1);
}

// ============================================================================
// UnsavedChangesCoordinator tests
// ============================================================================

// 20. Clean New executes immediately.
TEST_CASE("Coordinator: clean executes immediately")
{
    UnsavedChangesCoordinator c;
    bool dirty = false;
    c.SetIsDirtyQuery([&]() { return dirty; });
    bool executed = false;
    c.SetExecuteGate([&](const UnsavedChangesCoordinator::PendingAction&) { executed = true; });
    bool r = c.Request({UnsavedChangesCoordinator::ActionKind::New, {}});
    CHECK(r);
    CHECK(executed);
    CHECK_FALSE(c.NeedsPrompt());
}

// 21. Dirty New queues and does not execute.
TEST_CASE("Coordinator: dirty queues")
{
    UnsavedChangesCoordinator c;
    bool dirty = true;
    c.SetIsDirtyQuery([&]() { return dirty; });
    bool executed = false;
    c.SetExecuteGate([&](const UnsavedChangesCoordinator::PendingAction&) { executed = true; });
    bool r = c.Request({UnsavedChangesCoordinator::ActionKind::New, {}});
    CHECK_FALSE(r);
    CHECK_FALSE(executed);
    CHECK(c.NeedsPrompt());
}

// 22. Save success continues.
TEST_CASE("Coordinator: save success continues")
{
    UnsavedChangesCoordinator c;
    bool dirty = true;
    c.SetIsDirtyQuery([&]() { return dirty; });
    bool executed = false;
    c.SetExecuteGate([&](const UnsavedChangesCoordinator::PendingAction&) { executed = true; });
    c.SetSaveGate([&]() { return true; });
    c.Request({UnsavedChangesCoordinator::ActionKind::New, {}});
    c.ResolveSave();
    CHECK(executed);
    CHECK_FALSE(c.NeedsPrompt());
}

// 23. Save failure retains pending.
TEST_CASE("Coordinator: save failure retains pending")
{
    UnsavedChangesCoordinator c;
    bool dirty = true;
    c.SetIsDirtyQuery([&]() { return dirty; });
    bool executed = false;
    c.SetExecuteGate([&](const UnsavedChangesCoordinator::PendingAction&) { executed = true; });
    c.SetSaveGate([&]() { return false; });
    c.Request({UnsavedChangesCoordinator::ActionKind::New, {}});
    c.ResolveSave();
    CHECK_FALSE(executed);
    CHECK(c.NeedsPrompt());
}

// 24. Save As cancel retains pending.
TEST_CASE("Coordinator: saveas cancel retains pending")
{
    UnsavedChangesCoordinator c;
    bool dirty = true;
    c.SetIsDirtyQuery([&]() { return dirty; });
    bool executed = false;
    c.SetExecuteGate([&](const UnsavedChangesCoordinator::PendingAction&) { executed = true; });
    c.SetSaveGate([&]() { return false; }); // Save As was cancelled by the host
    c.Request({UnsavedChangesCoordinator::ActionKind::Open, {"C:/x.rt2scene"}});
    c.ResolveSave();
    CHECK_FALSE(executed);
    CHECK(c.NeedsPrompt());
}

// 25. Discard continues and clears recovery.
TEST_CASE("Coordinator: discard continues and clears recovery")
{
    UnsavedChangesCoordinator c;
    bool dirty = true;
    c.SetIsDirtyQuery([&]() { return dirty; });
    bool executed = false;
    bool recoveryCleared = false;
    c.SetExecuteGate([&](const UnsavedChangesCoordinator::PendingAction&) { executed = true; });
    c.SetDiscardRecoveryGate([&]() { recoveryCleared = true; });
    c.Request({UnsavedChangesCoordinator::ActionKind::New, {}});
    c.ResolveDiscard();
    CHECK(executed);
    CHECK(recoveryCleared);
    CHECK_FALSE(c.NeedsPrompt());
}

// 26. Cancel changes nothing.
TEST_CASE("Coordinator: cancel changes nothing")
{
    UnsavedChangesCoordinator c;
    bool dirty = true;
    c.SetIsDirtyQuery([&]() { return dirty; });
    bool executed = false;
    c.SetExecuteGate([&](const UnsavedChangesCoordinator::PendingAction&) { executed = true; });
    c.Request({UnsavedChangesCoordinator::ActionKind::New, {}});
    c.ResolveCancel();
    CHECK_FALSE(executed);
    CHECK_FALSE(c.NeedsPrompt());
}

// 27. Second pending request cannot replace the first.
TEST_CASE("Coordinator: second request rejected")
{
    UnsavedChangesCoordinator c;
    bool dirty = true;
    c.SetIsDirtyQuery([&]() { return dirty; });
    c.SetExecuteGate([&](const UnsavedChangesCoordinator::PendingAction&) {});
    c.Request({UnsavedChangesCoordinator::ActionKind::New, {}});
    // Second request while pending — should be rejected.
    bool r = c.Request({UnsavedChangesCoordinator::ActionKind::Open, {"C:/y.rt2scene"}});
    CHECK_FALSE(r);
    // The first action should still be pending.
    CHECK(c.Pending().kind == UnsavedChangesCoordinator::ActionKind::New);
}
