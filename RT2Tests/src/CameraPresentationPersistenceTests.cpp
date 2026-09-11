#include <doctest/doctest.h>

#include "CameraPresentation.h"
#include "CompositePreviewSession.h"
#include "EditorCommandHistory.h"
#include "EditorPropertyCommands.h"
#include "PrefabComponentValueEquality.h"
#include "PrefabSerializer.h"
#include "PrimitiveGeometry.h"
#include "SceneLoader.h"
#include "SceneLoaderTestSupport.h"
#include "SceneManager.h"
#include "SceneRecoveryService.h"
#include "SceneSerializer.h"
#include "SceneSerializerTestSupport.h"
#include "SubtreeSnapshot.h"
#include "core/UUID.h"
#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace rt2::core;
namespace fs = std::filesystem;

namespace
{
using json = nlohmann::json;

fs::path UniqueTempDir(const std::string& tag)
{
    static int counter = 0;
    auto dir = fs::temp_directory_path() / ("rt2_campersist_" + tag + "_" + std::to_string(++counter));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

std::string ReadFileBytes(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void WriteFileBytes(const fs::path& p, const std::string& content)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

struct SceneFixture
{
    DeterministicUuidProvider ids;
    SceneManager manager;
    EditorCommandHistory history;

    SceneFixture()
    {
        manager.SetUuidProvider(&ids);
        manager.AddMaterial(SceneMaterial{});
    }

    UUID AddCamera(const char* name = "Cam")
    {
        const auto uuid = manager.CreateEmpty(name).affectedEntities.front();
        const entt::entity e = manager.FindEntityByUuid(uuid);
        manager.GetECS().registry.emplace<CameraComponent>(e);
        return uuid;
    }

    CameraComponent GetCamera(const UUID& uuid)
    {
        const entt::entity e = manager.FindEntityByUuid(uuid);
        REQUIRE((e != entt::null));
        return *manager.GetECS().registry.try_get<CameraComponent>(e);
    }

    void SetCamera(const UUID& uuid, const CameraComponent& value)
    {
        const entt::entity e = manager.FindEntityByUuid(uuid);
        REQUIRE((e != entt::null));
        manager.GetECS().registry.emplace_or_replace<CameraComponent>(e, value);
    }
};

bool CameraEqStrict(const CameraComponent& a, const CameraComponent& b)
{
    constexpr float eps = 1e-5f;
    return std::fabs(a.verticalFOV - b.verticalFOV) <= eps &&
           std::fabs(a.aperture - b.aperture) <= eps &&
           std::fabs(a.focusDistance - b.focusDistance) <= eps &&
           a.forwardDirection == b.forwardDirection &&
           a.presentation == b.presentation;
}

PrefabValuePayload CameraReader(SceneManager& manager, const UUID& uuid,
                                const PrefabValuePayload& raw)
{
    const auto entity = manager.FindEntityByUuid(uuid);
    if (entity == entt::null) return raw;
    const auto* cc = manager.GetECS().registry.try_get<CameraComponent>(entity);
    if (!cc) return raw;
    return PrefabValuePayload{ *cc };
}

// Save via production Save, then rewrite the file's JSON with `mutate` so
// old-file shapes (absent keys) and corrupt shapes can be authored exactly.
fs::path SaveThenRewrite(SceneManager& manager, const fs::path& dir,
                         const std::string& name,
                         const std::function<void(json&)>& mutate)
{
    const auto path = dir / name;
    Error err;
    REQUIRE(SaveSceneForTest(manager.AuthoringDoc(), path, err));
    json root = json::parse(ReadFileBytes(path));
    mutate(root);
    WriteFileBytes(path, root.dump(2));
    return path;
}

} // namespace

TEST_CASE("camera presentation round-trips through native save and load")
{
    const auto dir = UniqueTempDir("roundtrip");
    SceneFixture f;
    f.manager.AuthoringDoc().ecs.camera.presentation.toneMap = ToneMapOperator::ACESFitted;
    f.manager.AuthoringDoc().ecs.camera.presentation.exposureEV = 2.0f;
    const auto cam = f.AddCamera();
    CameraComponent authored = f.GetCamera(cam);
    authored.presentation.toneMap = ToneMapOperator::Reinhard;
    authored.presentation.exposureEV = -1.5f;
    authored.verticalFOV = 60.0f;
    f.SetCamera(cam, authored);

    const auto path = dir / "cameras.rt2scene";
    Error err;
    REQUIRE(SaveSceneForTest(f.manager.AuthoringDoc(), path, err));

    SceneDocument loaded;
    DeterministicUuidProvider ids;
    loaded.SetUuidProvider(&ids);
    REQUIRE(SceneSerializer::Load(loaded, path, err));
    CHECK(loaded.ecs.camera.presentation.toneMap == ToneMapOperator::ACESFitted);
    CHECK(loaded.ecs.camera.presentation.exposureEV == doctest::Approx(2.0f));

    const entt::entity e = loaded.FindByUuid(cam);
    REQUIRE((e != entt::null));
    const auto* cc = loaded.ecs.registry.try_get<CameraComponent>(e);
    REQUIRE(cc != nullptr);
    CHECK(cc->presentation.toneMap == ToneMapOperator::Reinhard);
    CHECK(cc->presentation.exposureEV == doctest::Approx(-1.5f));
    CHECK(cc->verticalFOV == doctest::Approx(60.0f));
    fs::remove_all(dir);
}

TEST_CASE("camera files without presentation migrate to AgX at 0 EV")
{
    const auto dir = UniqueTempDir("migration");
    SceneFixture f;
    f.manager.AuthoringDoc().ecs.camera.presentation.toneMap = ToneMapOperator::Reinhard;
    f.manager.AuthoringDoc().ecs.camera.presentation.exposureEV = -3.0f;
    const auto cam = f.AddCamera();
    CameraComponent authored = f.GetCamera(cam);
    authored.presentation.toneMap = ToneMapOperator::ACESFitted;
    authored.presentation.exposureEV = 1.0f;
    f.SetCamera(cam, authored);

    const auto path = SaveThenRewrite(f.manager, dir, "old.rt2scene", [](json& root) {
        root["camera"].erase("toneMap");
        root["camera"].erase("exposureEV");
        for (auto& e : root["entities"])
            if (e.contains("camera"))
            {
                e["camera"].erase("toneMap");
                e["camera"].erase("exposureEV");
            }
    });

    SceneDocument loaded;
    DeterministicUuidProvider ids;
    loaded.SetUuidProvider(&ids);
    Error err;
    REQUIRE(SceneSerializer::Load(loaded, path, err));
    CHECK(loaded.ecs.camera.presentation == DefaultCameraPresentation());
    const entt::entity e = loaded.FindByUuid(cam);
    REQUIRE((e != entt::null));
    const auto* cc = loaded.ecs.registry.try_get<CameraComponent>(e);
    REQUIRE(cc != nullptr);
    CHECK(cc->presentation == DefaultCameraPresentation());
    fs::remove_all(dir);
}

TEST_CASE("explicit-invalid global presentation fails the whole load without touching the destination")
{
    const auto dir = UniqueTempDir("badglobal");
    SceneFixture f;
    f.manager.AuthoringDoc().ecs.camera.presentation.toneMap = ToneMapOperator::ACESFitted;
    f.manager.AuthoringDoc().ecs.camera.presentation.exposureEV = 2.0f;
    const auto cam = f.AddCamera();

    const auto goodPath = dir / "good.rt2scene";
    Error err;
    REQUIRE(SaveSceneForTest(f.manager.AuthoringDoc(), goodPath, err));

    SceneDocument doc;
    DeterministicUuidProvider ids;
    doc.SetUuidProvider(&ids);
    REQUIRE(SceneSerializer::Load(doc, goodPath, err));

    // Unknown operator: the whole load fails and the populated destination
    // keeps its exact prior state.
    const auto badOp = SaveThenRewrite(f.manager, dir, "badop.rt2scene", [](json& root) {
        root["camera"]["toneMap"] = "hdr";
    });
    Error badErr;
    CHECK_FALSE(SceneSerializer::Load(doc, badOp, badErr));
    CHECK(badErr.code == Error::Parse);
    CHECK(doc.ecs.camera.presentation.toneMap == ToneMapOperator::ACESFitted);
    CHECK(doc.ecs.camera.presentation.exposureEV == doctest::Approx(2.0f));
    CHECK((doc.FindByUuid(cam) != entt::null));

    // Out-of-range EV with a valid operator sibling: no partial adoption.
    const auto badEv = SaveThenRewrite(f.manager, dir, "badev.rt2scene", [](json& root) {
        root["camera"]["toneMap"] = "reinhard";
        root["camera"]["exposureEV"] = 20.0;
    });
    Error badEvErr;
    CHECK_FALSE(SceneSerializer::Load(doc, badEv, badEvErr));
    CHECK(badEvErr.code == Error::Parse);
    CHECK(doc.ecs.camera.presentation.toneMap == ToneMapOperator::ACESFitted);
    CHECK(doc.ecs.camera.presentation.exposureEV == doctest::Approx(2.0f));

    // Non-string operator is malformed, not a migration case.
    const auto numOp = SaveThenRewrite(f.manager, dir, "numop.rt2scene", [](json& root) {
        root["camera"]["toneMap"] = 1;
    });
    Error numOpErr;
    CHECK_FALSE(SceneSerializer::Load(doc, numOp, numOpErr));
    CHECK(numOpErr.code == Error::Parse);
    CHECK(doc.ecs.camera.presentation.toneMap == ToneMapOperator::ACESFitted);
    fs::remove_all(dir);
}

TEST_CASE("explicit-invalid entity presentation fails the load without partial adoption")
{
    const auto dir = UniqueTempDir("badentity");
    SceneFixture f;
    const auto cam = f.AddCamera();
    CameraComponent authored = f.GetCamera(cam);
    authored.presentation.toneMap = ToneMapOperator::Reinhard;
    authored.presentation.exposureEV = -1.0f;
    f.SetCamera(cam, authored);

    const auto goodPath = dir / "good.rt2scene";
    Error err;
    REQUIRE(SaveSceneForTest(f.manager.AuthoringDoc(), goodPath, err));
    SceneDocument doc;
    DeterministicUuidProvider ids;
    doc.SetUuidProvider(&ids);
    REQUIRE(SceneSerializer::Load(doc, goodPath, err));

    const auto badPath = SaveThenRewrite(f.manager, dir, "bad.rt2scene", [](json& root) {
        for (auto& e : root["entities"])
            if (e.contains("camera"))
            {
                e["camera"]["toneMap"] = "aces";
                e["camera"]["exposureEV"] = -20.0;
            }
    });
    Error badErr;
    CHECK_FALSE(SceneSerializer::Load(doc, badPath, badErr));
    CHECK(badErr.code == Error::Parse);
    const entt::entity e = doc.FindByUuid(cam);
    REQUIRE((e != entt::null));
    const auto* cc = doc.ecs.registry.try_get<CameraComponent>(e);
    REQUIRE(cc != nullptr);
    CHECK(cc->presentation.toneMap == ToneMapOperator::Reinhard);
    CHECK(cc->presentation.exposureEV == doctest::Approx(-1.0f));
    fs::remove_all(dir);
}

TEST_CASE("prefab codec preserves presentation and rejects invalid loudly")
{
    const auto dir = UniqueTempDir("prefab");
    const auto fixedUuid = UUID::Parse("33333333-3333-4333-8333-333333333333");
    const auto fixedTemplate = UUID::Parse("44444444-4444-4444-8444-444444444444");
    REQUIRE_FALSE(fixedUuid.IsNull());
    REQUIRE_FALSE(fixedTemplate.IsNull());

    PrefabDocument prefabDoc;
    prefabDoc.version = PrefabSerializer::FormatVersion;
    PrefabEntityRecord rec;
    rec.templateId = fixedTemplate;
    rec.record.uuid = fixedUuid;
    rec.record.hasCamera = true;
    rec.record.camera.verticalFOV = 60.0f;
    rec.record.camera.presentation.toneMap = ToneMapOperator::ACESFitted;
    rec.record.camera.presentation.exposureEV = 2.0f;
    prefabDoc.entities.push_back(rec);

    Error err;
    std::string content;
    REQUIRE(PrefabSerializer::Serialize(prefabDoc, content, err));
    PrefabDocument loaded;
    REQUIRE(PrefabSerializer::LoadBytes(loaded, content, dir / "cam.rt2prefab", err));
    REQUIRE(loaded.entities.size() == 1);
    CHECK(loaded.entities[0].record.camera.presentation.toneMap == ToneMapOperator::ACESFitted);
    CHECK(loaded.entities[0].record.camera.presentation.exposureEV == doctest::Approx(2.0f));

    // Prefab records without presentation migrate to AgX/0 like scenes.
    json stripped = json::parse(content);
    stripped["entities"][0]["record"]["camera"].erase("toneMap");
    stripped["entities"][0]["record"]["camera"].erase("exposureEV");
    PrefabDocument migrated;
    REQUIRE(PrefabSerializer::LoadBytes(migrated, stripped.dump(2), dir / "old.rt2prefab", err));
    REQUIRE(migrated.entities.size() == 1);
    CHECK(migrated.entities[0].record.camera.presentation == DefaultCameraPresentation());

    // Corrupt operator: whole prefab load fails, destination untouched.
    json badOp = json::parse(content);
    badOp["entities"][0]["record"]["camera"]["toneMap"] = "hdr";
    Error badOpErr;
    CHECK_FALSE(PrefabSerializer::LoadBytes(loaded, badOp.dump(2), dir / "bad.rt2prefab", badOpErr));
    CHECK(badOpErr.code == Error::Parse);
    REQUIRE(loaded.entities.size() == 1);
    CHECK(loaded.entities[0].record.camera.presentation.toneMap == ToneMapOperator::ACESFitted);

    // Out-of-range EV: same loud failure, no partial adoption.
    json badEv = json::parse(content);
    badEv["entities"][0]["record"]["camera"]["exposureEV"] = 99.0;
    Error badEvErr;
    CHECK_FALSE(PrefabSerializer::LoadBytes(loaded, badEv.dump(2), dir / "badev.rt2prefab", badEvErr));
    CHECK(badEvErr.code == Error::Parse);
    CHECK(loaded.entities[0].record.camera.presentation.exposureEV == doctest::Approx(2.0f));
    fs::remove_all(dir);
}

TEST_CASE("presentation-only edit is an effective camera command with None impact")
{
    SceneFixture f;
    const auto cam = f.AddCamera();
    const CameraComponent before = f.GetCamera(cam);
    CameraComponent after = before;
    after.presentation.toneMap = ToneMapOperator::ACESFitted;
    after.presentation.exposureEV = -1.0f;

    // Before the equality fix this was suppressed as a no-op.
    auto cmd = MakeSetCameraCommandIfEffective(cam, before, after);
    REQUIRE(cmd);
    auto r = f.history.Execute(std::move(cmd), f.manager);
    REQUIRE(r.success);
    CHECK(r.syncImpact == SyncImpact::None);
    CHECK(CameraEqStrict(f.GetCamera(cam), after));

    auto r1 = f.history.Undo(f.manager);
    REQUIRE(r1.success);
    CHECK(CameraEqStrict(f.GetCamera(cam), before));

    auto r2 = f.history.Redo(f.manager);
    REQUIRE(r2.success);
    CHECK(CameraEqStrict(f.GetCamera(cam), after));

    // -0.0f vs +0.0f EV is still silence, not history churn.
    CameraComponent negZero = after;
    negZero.presentation.exposureEV = -0.0f;
    CameraComponent posZero = after;
    posZero.presentation.exposureEV = 0.0f;
    CHECK(!MakeSetCameraCommandIfEffective(cam, negZero, posZero));
}

TEST_CASE("align command carries presentation-only changes with exact undo")
{
    SceneFixture f;
    const auto cam = f.AddCamera();
    EditableTRS local;
    REQUIRE(f.manager.GetLocalTransform(SceneManager::EntityId{ f.manager.FindEntityByUuid(cam) }, local));
    const CameraComponent before = f.GetCamera(cam);
    CameraComponent after = before;
    after.presentation.toneMap = ToneMapOperator::Reinhard;
    after.presentation.exposureEV = 3.0f;

    auto cmd = MakeAlignCameraCommandIfEffective(cam, local, local, before, after);
    REQUIRE(cmd);
    auto r = f.history.Execute(std::move(cmd), f.manager);
    REQUIRE(r.success);
    CHECK(CameraEqStrict(f.GetCamera(cam), after));

    REQUIRE(f.history.Undo(f.manager).success);
    CHECK(CameraEqStrict(f.GetCamera(cam), before));
    REQUIRE(f.history.Redo(f.manager).success);
    CHECK(CameraEqStrict(f.GetCamera(cam), after));
}

TEST_CASE("invalid presentation payload fails loudly at command execute")
{
    SceneFixture f;
    const auto cam = f.AddCamera();
    const CameraComponent before = f.GetCamera(cam);
    CameraComponent after = before;
    after.presentation.exposureEV = 20.0f; // outside [-8,+8]

    auto cmd = MakeSetCameraCommandIfEffective(cam, before, after);
    REQUIRE(cmd);
    const auto r = f.history.Execute(std::move(cmd), f.manager);
    CHECK_FALSE(r.success);
    // The rejected command changed nothing.
    CHECK(CameraEqStrict(f.GetCamera(cam), before));
}

TEST_CASE("prefab canonical equality and payload equality cover presentation")
{
    CameraComponent a;
    CameraComponent b = a;
    CHECK(PrefabCanonicalComponentEqual(a, b));
    CHECK(PrefabValuePayloadEqual(PrefabValuePayload{ a }, PrefabValuePayload{ b }));

    CameraComponent opDiff = a;
    opDiff.presentation.toneMap = ToneMapOperator::Reinhard;
    CHECK_FALSE(PrefabCanonicalComponentEqual(a, opDiff));
    CHECK_FALSE(PrefabValuePayloadEqual(PrefabValuePayload{ a }, PrefabValuePayload{ opDiff }));

    CameraComponent evDiff = a;
    evDiff.presentation.exposureEV = 1.0f;
    CHECK_FALSE(PrefabCanonicalComponentEqual(a, evDiff));
    CHECK_FALSE(PrefabValuePayloadEqual(PrefabValuePayload{ a }, PrefabValuePayload{ evDiff }));

    // Signed zero is one value, not a divergence.
    CameraComponent negZero = a;
    negZero.presentation.exposureEV = -0.0f;
    CHECK(PrefabCanonicalComponentEqual(a, negZero));

    // NaN never compares equal, even to itself.
    CameraComponent nanEv = a;
    nanEv.presentation.exposureEV = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(PrefabCanonicalComponentEqual(nanEv, nanEv));
}

TEST_CASE("presentation preview on a prefab instance marks the whole-camera override")
{
    const auto dir = UniqueTempDir("instance");
    SceneFixture f;
    const auto root = f.manager.CreateEmpty("Root").affectedEntities.front();
    const auto child = f.manager.CreateEmpty("Child", root).affectedEntities.front();
    const entt::entity rootHandle0 = f.manager.FindEntityByUuid(root);
    CameraComponent templateCam;
    templateCam.verticalFOV = 60.0f;
    f.manager.GetECS().registry.emplace<CameraComponent>(rootHandle0, templateCam);

    const auto prefabPath = dir / "cam.rt2prefab";
    REQUIRE(f.manager.CreatePrefabFromSubtree({ root }, prefabPath).ok);
    const auto uuids = f.manager.ReserveKnownUuids(2);
    std::vector<AssetDiagnostic> diags;
    REQUIRE(f.manager.InstantiatePrefabWithUuids(prefabPath, uuids, diags).mutation.success);

    const UUID member = uuids[0];
    const auto camKey = PrefabComponentKeyFor<CameraComponent>::value;
    REQUIRE_FALSE(f.manager.IsOverridden(member, camKey).value);
    const CameraComponent origin = f.GetCamera(member);

    CompositePreviewSession session;
    auto reader = [&](const UUID& uuid, const PrefabValuePayload& raw) {
        return CameraReader(f.manager, uuid, raw);
    };
    REQUIRE(session.Begin(f.manager, member, PrefabValueKind::CameraProperties,
                          camKey, PrefabValuePayload{ origin }, reader));

    // Presentation-only preview frame: effective, live, and overridden.
    // Before the equality fix this frame was a silent no-op.
    CameraComponent framed = origin;
    framed.presentation.toneMap = ToneMapOperator::ACESFitted;
    framed.presentation.exposureEV = 1.5f;
    const auto r1 = session.Preview(f.manager, PrefabValuePayload{ framed });
    REQUIRE(r1.success);
    REQUIRE(r1.effective);
    CHECK(f.GetCamera(member).presentation == framed.presentation);
    CHECK(f.manager.IsOverridden(member, camKey).value);

    // Rolling back to the origin is a zero-churn close: no command.
    const auto r2 = session.Preview(f.manager, PrefabValuePayload{ origin });
    REQUIRE(r2.success);
    REQUIRE(r2.effective);
    CHECK(f.GetCamera(member).presentation == origin.presentation);
    CHECK(!MakeSetCameraCommandIfEffective(member, origin,
        std::get<CameraComponent>(session.RollingValue()), &session.Origin()));

    // A committed presentation edit records one history entry and undoes
    // to the exact origin, clearing the override.
    const auto r3 = session.Preview(f.manager, PrefabValuePayload{ framed });
    REQUIRE(r3.success);
    REQUIRE(r3.effective);
    auto close = MakeSetCameraCommandIfEffective(member, origin,
        std::get<CameraComponent>(session.RollingValue()), &session.Origin());
    REQUIRE(close);
    REQUIRE(f.history.RecordApplied(std::move(close), f.manager,
                                    session.LastEffectiveResult()).effective);
    CHECK(f.manager.IsOverridden(member, camKey).value);
    REQUIRE(f.history.Undo(f.manager).success);
    CHECK(f.GetCamera(member).presentation == origin.presentation);
    CHECK_FALSE(f.manager.IsOverridden(member, camKey).value);
    (void)child;
    fs::remove_all(dir);
}

TEST_CASE("second scene adoption cannot retain the first scene's look")
{
    const auto dir = UniqueTempDir("stale");
    SceneFixture fa;
    fa.manager.AuthoringDoc().ecs.camera.presentation.toneMap = ToneMapOperator::ACESFitted;
    fa.manager.AuthoringDoc().ecs.camera.presentation.exposureEV = 2.0f;
    const auto camA = fa.AddCamera();
    CameraComponent authoredA = fa.GetCamera(camA);
    authoredA.presentation.toneMap = ToneMapOperator::Reinhard;
    authoredA.presentation.exposureEV = -1.0f;
    fa.SetCamera(camA, authoredA);
    const auto pathA = dir / "a.rt2scene";
    Error err;
    REQUIRE(SaveSceneForTest(fa.manager.AuthoringDoc(), pathA, err));

    SceneFixture fb;
    const auto pathB = dir / "b.rt2scene";
    REQUIRE(SaveSceneForTest(fb.manager.AuthoringDoc(), pathB, err));

    // Native-open seam: load A, adopt, then load B and adopt. B's defaults
    // must replace A's look everywhere.
    SceneFixture host;
    SceneDocument loadedA;
    loadedA.SetUuidProvider(&host.ids);
    REQUIRE(SceneSerializer::Load(loadedA, pathA, err));
    host.manager.AdoptLoadedDocument(std::move(loadedA), false);
    CHECK(host.manager.AuthoringDoc().ecs.camera.presentation.toneMap == ToneMapOperator::ACESFitted);

    SceneDocument loadedB;
    loadedB.SetUuidProvider(&host.ids);
    REQUIRE(SceneSerializer::Load(loadedB, pathB, err));
    host.manager.AdoptLoadedDocument(std::move(loadedB), false);
    CHECK(host.manager.AuthoringDoc().ecs.camera.presentation == DefaultCameraPresentation());

    auto view = host.manager.AuthoringDoc().ecs.registry.view<CameraComponent>();
    for (auto e : view)
        CHECK(view.get<CameraComponent>(e).presentation == DefaultCameraPresentation());
    fs::remove_all(dir);
}

TEST_CASE("clone in memory preserves presentation for the play path")
{
    SceneFixture f;
    f.manager.AuthoringDoc().ecs.camera.presentation.toneMap = ToneMapOperator::ACESFitted;
    f.manager.AuthoringDoc().ecs.camera.presentation.exposureEV = -2.0f;
    const auto cam = f.AddCamera();
    CameraComponent authored = f.GetCamera(cam);
    authored.presentation.toneMap = ToneMapOperator::Reinhard;
    authored.presentation.exposureEV = 1.0f;
    f.SetCamera(cam, authored);

    SceneDocument clone;
    DeterministicUuidProvider ids;
    clone.SetUuidProvider(&ids);
    Error err;
    REQUIRE(SceneSerializer::CloneInMemory(f.manager.AuthoringDoc(), clone, err));
    CHECK(clone.ecs.camera.presentation.toneMap == ToneMapOperator::ACESFitted);
    CHECK(clone.ecs.camera.presentation.exposureEV == doctest::Approx(-2.0f));
    const entt::entity e = clone.FindByUuid(cam);
    REQUIRE((e != entt::null));
    const auto* cc = clone.ecs.registry.try_get<CameraComponent>(e);
    REQUIRE(cc != nullptr);
    CHECK(cc->presentation.toneMap == ToneMapOperator::Reinhard);
    CHECK(cc->presentation.exposureEV == doctest::Approx(1.0f));
}

TEST_CASE("recovery snapshot and restore preserve the camera look")
{
    const auto dir = UniqueTempDir("recovery");
    SceneFixture f;
    f.manager.AuthoringDoc().ecs.camera.presentation.toneMap = ToneMapOperator::ACESFitted;
    f.manager.AuthoringDoc().ecs.camera.presentation.exposureEV = 2.0f;
    const auto cam = f.AddCamera();
    CameraComponent authored = f.GetCamera(cam);
    authored.presentation.toneMap = ToneMapOperator::Reinhard;
    authored.presentation.exposureEV = -2.0f;
    f.SetCamera(cam, authored);

    // The recovery write path (SaveTo) followed by the restore read path.
    const auto snapshotPath = dir / "snapshot.rt2scene";
    const auto logicalPath = dir / "scene.rt2scene";
    std::vector<AssetDiagnostic> diags;
    Error err;
    REQUIRE(SceneSerializer::SaveTo(f.manager.AuthoringDoc(), snapshotPath,
                                    logicalPath, diags, err));
    SceneDocument restored;
    DeterministicUuidProvider ids;
    restored.SetUuidProvider(&ids);
    REQUIRE(SceneSerializer::Load(restored, snapshotPath, err));
    CHECK(restored.ecs.camera.presentation.toneMap == ToneMapOperator::ACESFitted);
    CHECK(restored.ecs.camera.presentation.exposureEV == doctest::Approx(2.0f));
    const entt::entity e = restored.FindByUuid(cam);
    REQUIRE((e != entt::null));
    const auto* cc = restored.ecs.registry.try_get<CameraComponent>(e);
    REQUIRE(cc != nullptr);
    CHECK(cc->presentation.toneMap == ToneMapOperator::Reinhard);
    CHECK(cc->presentation.exposureEV == doctest::Approx(-2.0f));
    fs::remove_all(dir);
}

TEST_CASE("imported gltf cameras receive AgX at 0 EV")
{
    const auto dir = UniqueTempDir("gltfimport");
    // Author a glTF through the production exporter with a non-default lens.
    // glTF carries no presentation channel, so the import must yield AgX/0.
    ECSScene authored;
    authored.camera.verticalFOV = 60.0f;
    authored.camera.aperture = 0.5f;
    authored.camera.presentation.toneMap = ToneMapOperator::Reinhard;
    authored.camera.presentation.exposureEV = 3.0f;
    const auto target = dir / "cam.gltf";
    REQUIRE(SceneLoader::Save(authored, target.string()));

    ECSScene imported;
    imported.camera.presentation.toneMap = ToneMapOperator::ACESFitted;
    imported.camera.presentation.exposureEV = -4.0f;
    std::vector<AssetDiagnostic> diagnostics;
    const auto context = MakeSceneLoaderTestContext(dir, target);
    REQUIRE((SceneLoader::ImportIntoECS(imported, context, diagnostics) != entt::null));

    auto view = imported.registry.view<CameraComponent>();
    REQUIRE(view.size() == 1);
    const auto* cc = imported.registry.try_get<CameraComponent>(*view.begin());
    REQUIRE(cc != nullptr);
    CHECK(cc->presentation == DefaultCameraPresentation());
    CHECK(IsValidCameraPresentation(cc->presentation));
    // Lens fields survive the import; the file has no presentation channel.
    CHECK(cc->verticalFOV == doctest::Approx(60.0f));
    CHECK(cc->aperture == doctest::Approx(0.5f));
    // Import is additive: the pre-existing scene-global look is preserved.
    CHECK(imported.camera.presentation.toneMap == ToneMapOperator::ACESFitted);
    CHECK(imported.camera.presentation.exposureEV == doctest::Approx(-4.0f));
    fs::remove_all(dir);
}
