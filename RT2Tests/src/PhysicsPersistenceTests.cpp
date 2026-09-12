// ============================================================================
// T2: physics persistence foundation.
//
// Authored physics data (PhysicsBody/Shape/Hinge/SliderComponent) must
// survive every scene, clone, snapshot, duplicate, paste and prefab path
// before runtime physics consumes it. No Bullet world, no simulation, no
// Inspector, no Lua bindings here — plain data and its codecs only.
//
// Boundaries pinned by this file:
// - GREEN_V8Roundtrip: all four components survive save/load with exact
//   values; the file carries schema version 8.
// - GREEN_V3V7Migration: v3-v7 input without physics blocks loads with "no
//   body" and keeps its core data byte-semantically compatible.
// - GREEN_PhysicsCodecCoverage: one entity carrying all persisted components
//   round-trips through scene JSON, CloneInMemory, subtree snapshot
//   capture/remove-exact/restore, and duplicate with exact equality. This is
//   the dedicated tripwire: PersistedComponents::Count alone cannot cover the
//   manual codecs (EntityRecord, BuildDocumentFromRecords, SubtreeEntityRecord
//   capture/apply/exact-match, prefab record conversions), so each of those
//   paths is exercised here with all four physics components present.
//   Named mutants deleting any one manual codec path turn this red.
// - GREEN_ConstraintUuidRemapInternal / GREEN_ExternalOtherBodyPreserved:
//   intra-copy otherBody refs rebase to the copy; external refs and nil world
//   anchors survive verbatim.
// - RED_BadConstraintUuidRefused: unknown/self/missing-owner references fail
//   loudly with both UUIDs in the diagnostic.
// - Prefab wires: the four physics keys resolve non-overridable, the
//   overridable total stays 9, propagation has no physics adapter, and both
//   the scene-load and IsOverridden paths reject physics override attempts.
// ============================================================================

#include <doctest/doctest.h>

#include "ECSComponents.h"
#include "EntityReferenceRemapper.h"
#include "PrefabComponentValueEquality.h"
#include "PrefabPropagationComponentAdapter.h"
#include "PrefabSerializer.h"
#include "SceneAssetReferenceVisitor.h"
#include "SceneHierarchy.h"
#include "SceneManager.h"
#include "SceneSerializer.h"
#include "SceneSerializerTestSupport.h"
#include "core/Error.h"
#include "json.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace rt2::core;
using json = nlohmann::json;

namespace
{

std::filesystem::path UniqueTempDir(const std::string& tag)
{
    auto dir = std::filesystem::temp_directory_path() / ("rt2_" + tag);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

struct PhysicsFixture
{
    DeterministicUuidProvider ids;
    SceneManager manager;

    PhysicsFixture()
    {
        manager.SetUuidProvider(&ids);
    }

    UUID CreateEmpty(const char* name)
    {
        return manager.CreateEmpty(name).affectedEntities.front();
    }

    UUID CreateChild(const char* name, const UUID& parent)
    {
        return manager.CreateEmpty(name, parent).affectedEntities.front();
    }

    entt::entity Handle(const UUID& uuid) const
    {
        return manager.FindEntityByUuid(uuid);
    }

    bool Alive(const UUID& uuid) const
    {
        return Handle(uuid) != entt::null;
    }
};

// Non-default body: dynamic, CCD on, non-trivial damping and mask.
PhysicsBodyComponent MakeBody()
{
    PhysicsBodyComponent body;
    body.kind = PhysicsBodyKind::Dynamic;
    body.mass = 1.35f;
    body.friction = 0.42f;
    body.restitution = 0.61f;
    body.linearDamping = 0.05f;
    body.angularDamping = 0.11f;
    body.ccdEnabled = true;
    body.ccdMotionThreshold = 0.025f;
    body.ccdSweptRadius = 0.04f;
    body.startAsleep = true;
    body.layer = PhysicsLayer::Dynamic;
    body.mask = PhysicsLayer::WorldStatic | PhysicsLayer::Mechanism;
    return body;
}

// Non-default convex-hull shape with a durable hull reference.
PhysicsShapeComponent MakeHullShape()
{
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::ConvexHull;
    shape.radius = 0.25f;
    shape.halfExtents = {0.3f, 0.2f, 0.1f};
    shape.hull.kind = AssetKind::Model;
    shape.hull.path = "colliders/hull.obj";
    shape.hull.sourceKey = "obj:whole-model";
    shape.triMesh.kind = AssetKind::Model;
    shape.triMesh.path = "colliders/ramp.obj";
    shape.triMesh.sourceKey = "obj:whole-model";
    shape.isTrigger = false;
    shape.collisionMargin = 0.015f;
    return shape;
}

PhysicsHingeComponent MakeHinge(const UUID& other)
{
    PhysicsHingeComponent hinge;
    hinge.otherBody = other;
    hinge.ownerPivot = {0.1f, 0.2f, 0.3f};
    hinge.ownerAxis = {0.0f, 0.0f, 1.0f};
    hinge.otherPivot = {-0.1f, 0.0f, 0.05f};
    hinge.otherAxis = {0.0f, 1.0f, 0.0f};
    hinge.minAngleLimit = -0.96f;
    hinge.maxAngleLimit = 0.96f;
    hinge.driveMode = 0;
    hinge.motorTargetVelocity = 18.0f;
    hinge.motorMaxImpulse = 8.5f;
    hinge.motorEnabled = true;
    hinge.restAngle = 0.12f;
    return hinge;
}

PhysicsSliderComponent MakeSlider(const UUID& other)
{
    PhysicsSliderComponent slider;
    slider.otherBody = other;
    slider.axis = {1.0f, 0.0f, 0.0f};
    slider.lowerLimit = 0.0f;
    slider.upperLimit = 0.3f;
    slider.targetPosition = 0.18f;
    slider.motorTargetVelocity = 8.0f;
    slider.motorMaxForce = 80.0f;
    slider.motorEnabled = true;
    return slider;
}

void AttachAllPhysics(SceneManager& manager, const UUID& uuid,
                      const UUID& hingeOther, const UUID& sliderOther)
{
    auto handle = manager.FindEntityByUuid(uuid);
    REQUIRE(static_cast<uint32_t>(handle) !=
            static_cast<uint32_t>(entt::null));
    auto& registry = manager.GetECS().registry;
    registry.emplace_or_replace<PhysicsBodyComponent>(handle, MakeBody());
    registry.emplace_or_replace<PhysicsShapeComponent>(handle, MakeHullShape());
    registry.emplace_or_replace<PhysicsHingeComponent>(
        handle, MakeHinge(hingeOther));
    registry.emplace_or_replace<PhysicsSliderComponent>(
        handle, MakeSlider(sliderOther));
}

const PhysicsBodyComponent* BodyOf(SceneManager& manager, const UUID& uuid)
{
    return manager.GetECS().registry.try_get<PhysicsBodyComponent>(
        manager.FindEntityByUuid(uuid));
}

const PhysicsShapeComponent* ShapeOf(SceneManager& manager, const UUID& uuid)
{
    return manager.GetECS().registry.try_get<PhysicsShapeComponent>(
        manager.FindEntityByUuid(uuid));
}

const PhysicsHingeComponent* HingeOf(SceneManager& manager, const UUID& uuid)
{
    return manager.GetECS().registry.try_get<PhysicsHingeComponent>(
        manager.FindEntityByUuid(uuid));
}

const PhysicsSliderComponent* SliderOf(SceneManager& manager, const UUID& uuid)
{
    return manager.GetECS().registry.try_get<PhysicsSliderComponent>(
        manager.FindEntityByUuid(uuid));
}

void RequireAllPhysicsEqual(SceneManager& manager, const UUID& uuid,
                            const UUID& hingeOther, const UUID& sliderOther)
{
    const auto* body = BodyOf(manager, uuid);
    const auto* shape = ShapeOf(manager, uuid);
    const auto* hinge = HingeOf(manager, uuid);
    const auto* slider = SliderOf(manager, uuid);
    REQUIRE(body);
    REQUIRE(shape);
    REQUIRE(hinge);
    REQUIRE(slider);
    CHECK(*body == MakeBody());
    CHECK(*shape == MakeHullShape());
    CHECK(*hinge == MakeHinge(hingeOther));
    CHECK(*slider == MakeSlider(sliderOther));
    CHECK(PrefabCanonicalComponentEqual(*body, MakeBody()));
    CHECK(PrefabCanonicalComponentEqual(*shape, MakeHullShape()));
    CHECK(PrefabCanonicalComponentEqual(*hinge, MakeHinge(hingeOther)));
    CHECK(PrefabCanonicalComponentEqual(*slider, MakeSlider(sliderOther)));
}

std::string ReadFileBinary(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void WriteFileBinary(const std::filesystem::path& path,
                     const std::string& content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

} // namespace

TEST_CASE("T2 GREEN_V8Roundtrip: all four physics components survive save and load exactly")
{
    PhysicsFixture f;
    const auto ball = f.CreateEmpty("Ball");
    const auto anchor = f.CreateEmpty("Anchor");
    AttachAllPhysics(f.manager, ball, anchor, UUID::Nil());

    const auto dir = UniqueTempDir("t2_v8_roundtrip");
    const auto path = dir / "physics.rt2scene";
    Error err;
    REQUIRE(SaveSceneForTest(f.manager.AuthoringDoc(), path, err));

    // The file carries schema version 8.
    const json saved = json::parse(ReadFileBinary(path));
    REQUIRE(saved.contains("version"));
    CHECK(saved["version"].get<uint32_t>() == 8u);
    CHECK(saved["version"].get<uint32_t>() ==
          SceneSerializer::SchemaVersion);

    SceneDocument loaded;
    REQUIRE(SceneSerializer::Load(loaded, path, err));
    CHECK(loaded.metadata.schemaVersion == 8u);

    const auto loadedBall = loaded.FindByUuid(ball);
    const auto loadedAnchor = loaded.FindByUuid(anchor);
    REQUIRE(static_cast<uint32_t>(loadedBall) !=
            static_cast<uint32_t>(entt::null));
    REQUIRE(static_cast<uint32_t>(loadedAnchor) !=
            static_cast<uint32_t>(entt::null));
    const auto* body =
        loaded.ecs.registry.try_get<PhysicsBodyComponent>(loadedBall);
    const auto* shape =
        loaded.ecs.registry.try_get<PhysicsShapeComponent>(loadedBall);
    const auto* hinge =
        loaded.ecs.registry.try_get<PhysicsHingeComponent>(loadedBall);
    const auto* slider =
        loaded.ecs.registry.try_get<PhysicsSliderComponent>(loadedBall);
    REQUIRE(body);
    REQUIRE(shape);
    REQUIRE(hinge);
    REQUIRE(slider);
    CHECK(*body == MakeBody());
    CHECK(*shape == MakeHullShape());
    CHECK(*hinge == MakeHinge(anchor));
    CHECK(*slider == MakeSlider(UUID::Nil()));

    std::filesystem::remove_all(dir);
}

TEST_CASE("T2 GREEN_V3V7Migration: scenes without physics load as no-body with core data intact")
{
    PhysicsFixture f;
    const auto ball = f.CreateEmpty("Ball");
    AttachAllPhysics(f.manager, ball, UUID::Nil(), UUID::Nil());

    const auto dir = UniqueTempDir("t2_v3v7_migration");
    const auto path = dir / "physics.rt2scene";
    Error err;
    REQUIRE(SaveSceneForTest(f.manager.AuthoringDoc(), path, err));
    const json saved = json::parse(ReadFileBinary(path));
    REQUIRE(saved["version"].get<uint32_t>() == 8u);

    for (uint32_t version = 3; version <= 7; ++version)
    {
        json relabeled = saved;
        relabeled["version"] = version;
        // v3-v7 input carries no physics blocks: strip them.
        for (auto& entity : relabeled["entities"])
        {
            entity.erase("physicsBody");
            entity.erase("physicsShape");
            entity.erase("physicsHinge");
            entity.erase("physicsSlider");
        }
        const auto versionPath =
            dir / ("physics_v" + std::to_string(version) + ".rt2scene");
        WriteFileBinary(versionPath, relabeled.dump(2));

        SceneDocument loaded;
        Error loadErr;
        REQUIRE_MESSAGE(SceneSerializer::Load(loaded, versionPath, loadErr),
                        "v" << version << ": " << loadErr.detail);
        CHECK(loaded.metadata.schemaVersion == version);

        // Absent physics means no body — and the core data is untouched.
        const auto loadedBall = loaded.FindByUuid(ball);
        REQUIRE(static_cast<uint32_t>(loadedBall) !=
            static_cast<uint32_t>(entt::null));
        CHECK_FALSE(
            loaded.ecs.registry.all_of<PhysicsBodyComponent>(loadedBall));
        CHECK_FALSE(
            loaded.ecs.registry.all_of<PhysicsShapeComponent>(loadedBall));
        CHECK_FALSE(
            loaded.ecs.registry.all_of<PhysicsHingeComponent>(loadedBall));
        CHECK_FALSE(
            loaded.ecs.registry.all_of<PhysicsSliderComponent>(loadedBall));
        const auto* name =
            loaded.ecs.registry.try_get<NameComponent>(loadedBall);
        REQUIRE(name);
        CHECK(name->name == "Ball");
    }

    std::filesystem::remove_all(dir);
}

TEST_CASE("T2 GREEN_PhysicsCodecCoverage: all persisted components survive every manual codec")
{
    // Tripwire: one entity carrying every persisted component (including all
    // four physics components) round-trips through scene JSON, CloneInMemory,
    // subtree snapshot capture/remove-exact/restore, and duplicate with exact
    // equality. Deleting any one manual codec path (EntityRecord collect,
    // scene JSON read/write, BuildDocumentFromRecords, CloneInMemory,
    // SubtreeEntityRecord capture/apply/exact-match, prefab record
    // conversions) turns this red; PersistedComponents::Count alone cannot.
    CHECK(PersistedComponents::Count == 17);

    PhysicsFixture f;
    const auto covered = f.CreateEmpty("Covered");
    AttachAllPhysics(f.manager, covered, UUID::Nil(), UUID::Nil());
    auto handle = f.manager.FindEntityByUuid(covered);
    REQUIRE(static_cast<uint32_t>(handle) !=
            static_cast<uint32_t>(entt::null));
    auto& registry = f.manager.GetECS().registry;
    registry.emplace_or_replace<PrimitiveComponent>(
        handle, PrimitiveComponent{PrimitiveComponent::Cube, 1.0f, 24, 16});
    registry.emplace_or_replace<LightComponent>(
        handle, LightComponent{{1.0f, 0.9f, 0.8f}, 50.0f, 60.0f, 30.0f,
                               45.0f, LightType::Point});
    registry.emplace_or_replace<CameraComponent>(
        handle, CameraComponent{45.0f, 0.0f, 1.0f, {0.0f, 0.0f, -1.0f}});
    registry.emplace_or_replace<MotionComponent>(
        handle, MotionComponent{{1.0f, 0.0f, 0.0f}});
    ScriptComponent script;
    script.asset.kind = AssetKind::Script;
    script.asset.path = "scripts/cover.lua";
    script.asset.sourceKey = "lua:asset=scripts/cover.lua";
    registry.emplace_or_replace<ScriptComponent>(handle, script);
    ImportedMeshSourceComponent imported;
    imported.model.kind = AssetKind::Model;
    imported.model.path = "models/cover.obj";
    imported.model.sourceKey = "obj:whole-model";
    registry.emplace_or_replace<ImportedMeshSourceComponent>(handle, imported);
    MaterialOverrideComponent overrideComp;
    overrideComp.authored = true;
    overrideComp.sourceMaterialKey = "obj:material:name=cover";
    registry.emplace_or_replace<MaterialOverrideComponent>(
        handle, overrideComp);
    PrefabInstanceComponent pic;
    pic.prefab.kind = AssetKind::Prefab;
    pic.prefab.path = "prefabs/cover.rt2prefab";
    pic.instanceId = f.ids.CreateV4();
    registry.emplace_or_replace<PrefabInstanceComponent>(handle, pic);
    PrefabMemberComponent pmc;
    pmc.instanceId = pic.instanceId;
    pmc.templateId = f.ids.CreateV4();
    registry.emplace_or_replace<PrefabMemberComponent>(handle, pmc);

    auto requireCovered = [&](SceneManager& manager, const UUID& uuid) {
        RequireAllPhysicsEqual(manager, uuid, UUID::Nil(), UUID::Nil());
        const auto h = manager.FindEntityByUuid(uuid);
        REQUIRE(static_cast<uint32_t>(h) !=
                static_cast<uint32_t>(entt::null));
        const auto& reg = manager.GetECS().registry;
        REQUIRE(reg.all_of<PrimitiveComponent>(h));
        REQUIRE(reg.all_of<LightComponent>(h));
        REQUIRE(reg.all_of<CameraComponent>(h));
        REQUIRE(reg.all_of<MotionComponent>(h));
        REQUIRE(reg.all_of<ScriptComponent>(h));
        REQUIRE(reg.all_of<ImportedMeshSourceComponent>(h));
        REQUIRE(reg.all_of<MaterialOverrideComponent>(h));
        REQUIRE(reg.all_of<PrefabInstanceComponent>(h));
        REQUIRE(reg.all_of<PrefabMemberComponent>(h));
    };

    // Path 1: scene JSON save/load.
    const auto dir = UniqueTempDir("t2_codec_coverage");
    const auto path = dir / "coverage.rt2scene";
    Error err;
    REQUIRE(SaveSceneForTest(f.manager.AuthoringDoc(), path, err));
    SceneDocument fromFile;
    REQUIRE(SceneSerializer::Load(fromFile, path, err));
    REQUIRE(static_cast<uint32_t>(fromFile.FindByUuid(covered)) !=
            static_cast<uint32_t>(entt::null));
    {
        // try_get + REQUIRE (not direct get): a codec mutant that drops a
        // component must turn this red as a clean failure, never as an
        // entt abort that kills the whole test process.
        const auto e = fromFile.FindByUuid(covered);
        const auto* body =
            fromFile.ecs.registry.try_get<PhysicsBodyComponent>(e);
        const auto* shape =
            fromFile.ecs.registry.try_get<PhysicsShapeComponent>(e);
        const auto* hinge =
            fromFile.ecs.registry.try_get<PhysicsHingeComponent>(e);
        const auto* slider =
            fromFile.ecs.registry.try_get<PhysicsSliderComponent>(e);
        REQUIRE(body);
        REQUIRE(shape);
        REQUIRE(hinge);
        REQUIRE(slider);
        CHECK(*body == MakeBody());
        CHECK(*shape == MakeHullShape());
        CHECK(*hinge == MakeHinge(UUID::Nil()));
        CHECK(*slider == MakeSlider(UUID::Nil()));
    }

    // Path 2: CloneInMemory (the Play-clone path).
    {
        SceneDocument clone;
        Error cloneErr;
        REQUIRE(SceneSerializer::CloneInMemory(
            f.manager.AuthoringDoc(), clone, cloneErr));
        const auto e = clone.FindByUuid(covered);
        REQUIRE(static_cast<uint32_t>(e) !=
                static_cast<uint32_t>(entt::null));
        const auto* body =
            clone.ecs.registry.try_get<PhysicsBodyComponent>(e);
        const auto* shape =
            clone.ecs.registry.try_get<PhysicsShapeComponent>(e);
        const auto* hinge =
            clone.ecs.registry.try_get<PhysicsHingeComponent>(e);
        const auto* slider =
            clone.ecs.registry.try_get<PhysicsSliderComponent>(e);
        REQUIRE(body);
        REQUIRE(shape);
        REQUIRE(hinge);
        REQUIRE(slider);
        CHECK(*body == MakeBody());
        CHECK(*shape == MakeHullShape());
        CHECK(*hinge == MakeHinge(UUID::Nil()));
        CHECK(*slider == MakeSlider(UUID::Nil()));
    }

    // Path 3: subtree snapshot capture, exact removal, verbatim restore.
    // Fixup P2.1: the exact-match comparisons are proven live per physics
    // component. Mutating any one live component after capture must make
    // RemoveSubtreesExact fail atomically (entity retained); restoring the
    // captured value re-arms success. Deleting any one of the four
    // EntityMatchesRecord physics comparisons therefore turns this red.
    {
        const auto snapshot =
            f.manager.CaptureSubtreeSnapshot({covered});
        REQUIRE(snapshot.entities.size() == 1);
        CHECK(snapshot.entities.front().hasPhysicsBody);
        CHECK(snapshot.entities.front().hasPhysicsShape);
        CHECK(snapshot.entities.front().hasPhysicsHinge);
        CHECK(snapshot.entities.front().hasPhysicsSlider);
        auto live = f.manager.FindEntityByUuid(covered);
        REQUIRE(static_cast<uint32_t>(live) !=
                static_cast<uint32_t>(entt::null));
        auto& liveRegistry = f.manager.GetECS().registry;

        auto mutateOne = [&](const char* which) {
            if (std::strcmp(which, "body") == 0)
                liveRegistry.get<PhysicsBodyComponent>(live).mass += 1.0f;
            else if (std::strcmp(which, "shape") == 0)
                liveRegistry.get<PhysicsShapeComponent>(live).radius += 1.0f;
            else if (std::strcmp(which, "hinge") == 0)
                liveRegistry.get<PhysicsHingeComponent>(live).restAngle += 1.0f;
            else
                liveRegistry.get<PhysicsSliderComponent>(live).upperLimit +=
                    1.0f;
        };
        auto restoreOne = [&](const char* which) {
            const auto& recorded = snapshot.entities.front();
            if (std::strcmp(which, "body") == 0)
                liveRegistry.emplace_or_replace<PhysicsBodyComponent>(
                    live, recorded.physicsBody);
            else if (std::strcmp(which, "shape") == 0)
                liveRegistry.emplace_or_replace<PhysicsShapeComponent>(
                    live, recorded.physicsShape);
            else if (std::strcmp(which, "hinge") == 0)
                liveRegistry.emplace_or_replace<PhysicsHingeComponent>(
                    live, recorded.physicsHinge);
            else
                liveRegistry.emplace_or_replace<PhysicsSliderComponent>(
                    live, recorded.physicsSlider);
        };
        for (const char* which : {"body", "shape", "hinge", "slider"})
        {
            mutateOne(which);
            CHECK_FALSE(f.manager.RemoveSubtreesExact(snapshot).success);
            CHECK(f.Alive(covered));
            restoreOne(which);
        }
        REQUIRE(f.manager.RemoveSubtreesExact(snapshot).success);
        CHECK_FALSE(f.Alive(covered));
        REQUIRE(f.manager.RestoreSubtrees(snapshot).success);
        REQUIRE(f.Alive(covered));
        requireCovered(f.manager, covered);
        // The link components restore verbatim (instance identity, not fresh).
        const auto h = f.manager.FindEntityByUuid(covered);
        CHECK(f.manager.GetECS()
                  .registry.get<PrefabMemberComponent>(h)
                  .templateId == pmc.templateId);
    }

    // Path 4: duplicate with fresh UUIDs (verbatim values, verbatim anchors).
    {
        const auto uuids = f.manager.ReserveKnownUuids(1);
        const auto duplicated =
            f.manager.DuplicateSubtreesWithUuids({covered}, uuids);
        REQUIRE(duplicated.mutation.success);
        REQUIRE(uuids.size() == 1);
        RequireAllPhysicsEqual(f.manager, uuids.front(), UUID::Nil(),
                               UUID::Nil());
        // Prefab link survives as a reminted instance (W3-D8), never dropped.
        const auto h = f.manager.FindEntityByUuid(uuids.front());
        REQUIRE(f.manager.GetECS().registry.all_of<PrefabMemberComponent>(h));
        CHECK(f.manager.GetECS()
                  .registry.get<PrefabMemberComponent>(h)
                  .templateId == pmc.templateId);
    }

    // Path 5: prefab record conversion (SubtreeEntityRecord payload). Prefab
    // files never carry scene-side link components (loud refusal), so the
    // conversion runs on a link-free sibling carrying all four physics
    // components.
    {
        const auto sibling = f.CreateEmpty("RecordSibling");
        AttachAllPhysics(f.manager, sibling, UUID::Nil(), UUID::Nil());
        const auto snapshot =
            f.manager.CaptureSubtreeSnapshot({sibling});
        REQUIRE(snapshot.entities.size() == 1);
        PrefabEntityRecord record;
        record.templateId = f.ids.CreateV4();
        record.record = snapshot.entities.front();
        std::vector<AssetDiagnostic> diags;
        Error recordErr;
        json out;
        REQUIRE(PrefabRecordToJson(record, diags, recordErr, out));
        PrefabEntityRecord parsed;
        REQUIRE(JsonToPrefabRecord(out, recordErr, parsed));
        CHECK(parsed.templateId == record.templateId);
        CHECK(parsed.record.hasPhysicsBody);
        CHECK(parsed.record.hasPhysicsShape);
        CHECK(parsed.record.hasPhysicsHinge);
        CHECK(parsed.record.hasPhysicsSlider);
        CHECK(parsed.record.physicsBody == MakeBody());
        CHECK(parsed.record.physicsShape == MakeHullShape());
        CHECK(parsed.record.physicsHinge == MakeHinge(UUID::Nil()));
        CHECK(parsed.record.physicsSlider == MakeSlider(UUID::Nil()));
    }

    std::filesystem::remove_all(dir);
}

TEST_CASE("T2 GREEN_ConstraintUuidRemapInternal: duplicate rebases intra-copy otherBody refs")
{
    PhysicsFixture f;
    const auto root = f.CreateEmpty("Root");
    const auto bodyA = f.CreateChild("BodyA", root);
    const auto bodyB = f.CreateChild("BodyB", root);
    auto& registry = f.manager.GetECS().registry;
    registry.emplace_or_replace<PhysicsBodyComponent>(
        f.Handle(bodyA), MakeBody());
    registry.emplace_or_replace<PhysicsBodyComponent>(
        f.Handle(bodyB), MakeBody());
    registry.emplace_or_replace<PhysicsHingeComponent>(
        f.Handle(bodyB), MakeHinge(bodyA));
    registry.emplace_or_replace<PhysicsSliderComponent>(
        f.Handle(bodyA), MakeSlider(bodyB));

    const auto uuids = f.manager.ReserveKnownUuids(3);
    const auto duplicated =
        f.manager.DuplicateSubtreesWithUuids({root}, uuids);
    REQUIRE(duplicated.mutation.success);

    UUID copyA, copyB;
    for (const auto& [source, copy] : duplicated.sourceToDuplicate)
    {
        if (source == bodyA) copyA = copy;
        if (source == bodyB) copyB = copy;
    }
    REQUIRE_FALSE(copyA.IsNull());
    REQUIRE_FALSE(copyB.IsNull());

    const auto* copyHinge = HingeOf(f.manager, copyB);
    const auto* copySlider = SliderOf(f.manager, copyA);
    REQUIRE(copyHinge);
    REQUIRE(copySlider);
    // Internal references point at the copies, never back at the sources.
    CHECK(copyHinge->otherBody == copyA);
    CHECK(copyHinge->otherBody != bodyA);
    CHECK(copySlider->otherBody == copyB);
    CHECK(copySlider->otherBody != bodyB);
    // Non-reference payload is verbatim.
    CHECK(*copyHinge == MakeHinge(copyA));
    CHECK(*copySlider == MakeSlider(copyB));

    // The rebased copy validates loudly-clean.
    Error err;
    CHECK(ValidatePhysicsConstraintReferences(f.manager.AuthoringDoc(), err));
}

TEST_CASE("T2 GREEN_ExternalOtherBodyPreserved: duplicate keeps external refs and world anchors")
{
    PhysicsFixture f;
    const auto root = f.CreateEmpty("Root");
    const auto bodyA = f.CreateChild("BodyA", root);
    const auto external = f.CreateEmpty("External");
    auto& registry = f.manager.GetECS().registry;
    registry.emplace_or_replace<PhysicsBodyComponent>(
        f.Handle(bodyA), MakeBody());
    registry.emplace_or_replace<PhysicsBodyComponent>(
        f.Handle(external), MakeBody());
    registry.emplace_or_replace<PhysicsHingeComponent>(
        f.Handle(bodyA), MakeHinge(external));
    const auto anchored = f.CreateChild("Anchored", root);
    registry.emplace_or_replace<PhysicsBodyComponent>(
        f.Handle(anchored), MakeBody());
    registry.emplace_or_replace<PhysicsSliderComponent>(
        f.Handle(anchored), MakeSlider(UUID::Nil()));

    const auto uuids = f.manager.ReserveKnownUuids(3);
    const auto duplicated =
        f.manager.DuplicateSubtreesWithUuids({root}, uuids);
    REQUIRE(duplicated.mutation.success);

    UUID copyA, copyAnchored;
    for (const auto& [source, copy] : duplicated.sourceToDuplicate)
    {
        if (source == bodyA) copyA = copy;
        if (source == anchored) copyAnchored = copy;
    }
    REQUIRE_FALSE(copyA.IsNull());
    REQUIRE_FALSE(copyAnchored.IsNull());
    const auto* copyHinge = HingeOf(f.manager, copyA);
    const auto* copySlider = SliderOf(f.manager, copyAnchored);
    REQUIRE(copyHinge);
    REQUIRE(copySlider);
    // External reference still names the original body; the world anchor
    // stays nil (it keeps the original world anchor until re-authored).
    CHECK(copyHinge->otherBody == external);
    CHECK(copySlider->otherBody.IsNull());

    Error err;
    CHECK(ValidatePhysicsConstraintReferences(f.manager.AuthoringDoc(), err));
}

TEST_CASE("T2 paste preserves physics and rebases internal otherBody refs")
{
    PhysicsFixture f;
    const auto root = f.CreateEmpty("Root");
    const auto bodyA = f.CreateChild("BodyA", root);
    const auto bodyB = f.CreateChild("BodyB", root);
    auto& registry = f.manager.GetECS().registry;
    registry.emplace_or_replace<PhysicsBodyComponent>(
        f.Handle(bodyA), MakeBody());
    registry.emplace_or_replace<PhysicsBodyComponent>(
        f.Handle(bodyB), MakeBody());
    registry.emplace_or_replace<PhysicsHingeComponent>(
        f.Handle(bodyB), MakeHinge(bodyA));
    registry.emplace_or_replace<PhysicsShapeComponent>(
        f.Handle(bodyA), MakeHullShape());

    // Clipboard = whole-scene clone, exactly as EditorSceneState::Copy does.
    SceneDocument clipboard;
    DeterministicUuidProvider idsClip;
    clipboard.SetUuidProvider(&idsClip);
    Error cloneErr;
    REQUIRE(SceneSerializer::CloneInMemory(
        f.manager.AuthoringDoc(), clipboard, cloneErr));

    const auto pasted = f.manager.PasteSubtreesFrom(clipboard, {root});
    REQUIRE(pasted.success);
    REQUIRE(pasted.affectedEntities.size() == 1);

    // Resolve pasted entities: the pasted root plus its two children.
    const UUID pastedRoot = pasted.affectedEntities.front();
    REQUIRE(pastedRoot != root);
    std::vector<entt::entity> subtree;
    SceneHierarchy::CollectSubtreePreOrder(
        f.manager.GetECS().registry, f.manager.FindEntityByUuid(pastedRoot),
        subtree);
    REQUIRE(subtree.size() == 3);

    // Only pasted ROOTS gain the " Copy" suffix; children keep their names
    // but carry fresh UUIDs, so match by name + not-a-source-UUID.
    UUID pastedA, pastedB;
    for (const auto e : subtree)
    {
        const auto* name =
            f.manager.GetECS().registry.try_get<NameComponent>(e);
        REQUIRE(name);
        const auto id =
            f.manager.GetECS().registry.get<EntityIdComponent>(e).id;
        if (name->name == "BodyA" && id != bodyA) pastedA = id;
        if (name->name == "BodyB" && id != bodyB) pastedB = id;
    }
    REQUIRE_FALSE(pastedA.IsNull());
    REQUIRE_FALSE(pastedB.IsNull());
    const auto* pastedHinge = HingeOf(f.manager, pastedB);
    REQUIRE(pastedHinge);
    CHECK(pastedHinge->otherBody == pastedA);
    const auto* pastedShape = ShapeOf(f.manager, pastedA);
    REQUIRE(pastedShape);
    CHECK(*pastedShape == MakeHullShape());
}

TEST_CASE("T2 prefab instantiate preserves physics and remaps template-internal refs")
{
    PhysicsFixture f;
    const auto root = f.CreateEmpty("Rig");
    const auto bodyA = f.CreateChild("BodyA", root);
    const auto bodyB = f.CreateChild("BodyB", root);
    auto& registry = f.manager.GetECS().registry;
    registry.emplace_or_replace<PhysicsBodyComponent>(
        f.Handle(bodyA), MakeBody());
    registry.emplace_or_replace<PhysicsBodyComponent>(
        f.Handle(bodyB), MakeBody());
    registry.emplace_or_replace<PhysicsHingeComponent>(
        f.Handle(bodyB), MakeHinge(bodyA));
    registry.emplace_or_replace<PhysicsSliderComponent>(
        f.Handle(bodyB), MakeSlider(UUID::Nil()));

    const auto dir = UniqueTempDir("t2_prefab_physics");
    const auto prefabPath = dir / "rig.rt2prefab";
    const auto created = f.manager.CreatePrefabFromSubtree({root}, prefabPath);
    REQUIRE(created.ok);

    std::vector<AssetDiagnostic> diags;
    const auto uuids = f.manager.ReserveKnownUuids(3);
    const auto inst = f.manager.InstantiatePrefabWithUuids(
        prefabPath, uuids, diags);
    REQUIRE(inst.mutation.success);
    REQUIRE(inst.createdRoots.size() == 1);

    // Pre-order: root first, then children.
    const UUID instA = uuids[1];
    const UUID instB = uuids[2];
    const auto* instHinge = HingeOf(f.manager, instB);
    const auto* instSlider = SliderOf(f.manager, instB);
    REQUIRE(instHinge);
    REQUIRE(instSlider);
    CHECK(instHinge->otherBody == instA);
    CHECK(instHinge->otherBody != bodyA);
    CHECK(instSlider->otherBody.IsNull());
    CHECK(*instHinge == MakeHinge(instA));
    const auto* instBody = BodyOf(f.manager, instA);
    REQUIRE(instBody);
    CHECK(*instBody == MakeBody());

    std::filesystem::remove_all(dir);
}

TEST_CASE("T2 RED_BadConstraintUuidRefused: dangling, self, and missing-owner refs fail loudly")
{
    // Unknown otherBody: both UUIDs in the diagnostic.
    {
        PhysicsFixture f;
        const auto owner = f.CreateEmpty("Owner");
        const auto missing = f.ids.CreateV4();
        auto& registry = f.manager.GetECS().registry;
        registry.emplace_or_replace<PhysicsBodyComponent>(
            f.Handle(owner), MakeBody());
        registry.emplace_or_replace<PhysicsHingeComponent>(
            f.Handle(owner), MakeHinge(missing));
        Error err;
        CHECK_FALSE(
            ValidatePhysicsConstraintReferences(f.manager.AuthoringDoc(), err));
        CHECK(err.path == owner.ToString());
        CHECK(err.detail.find(owner.ToString()) != std::string::npos);
        CHECK(err.detail.find(missing.ToString()) != std::string::npos);
    }
    // Self-constraint: both UUIDs in the diagnostic (identical here).
    {
        PhysicsFixture f;
        const auto owner = f.CreateEmpty("Owner");
        auto& registry = f.manager.GetECS().registry;
        registry.emplace_or_replace<PhysicsBodyComponent>(
            f.Handle(owner), MakeBody());
        registry.emplace_or_replace<PhysicsSliderComponent>(
            f.Handle(owner), MakeSlider(owner));
        Error err;
        CHECK_FALSE(
            ValidatePhysicsConstraintReferences(f.manager.AuthoringDoc(), err));
        CHECK(err.path == owner.ToString());
        CHECK(err.detail.find(owner.ToString()) != std::string::npos);
    }
    // Missing owner body: the owner UUID is named.
    {
        PhysicsFixture f;
        const auto owner = f.CreateEmpty("Owner");
        auto& registry = f.manager.GetECS().registry;
        registry.emplace_or_replace<PhysicsHingeComponent>(
            f.Handle(owner), MakeHinge(UUID::Nil()));
        Error err;
        CHECK_FALSE(
            ValidatePhysicsConstraintReferences(f.manager.AuthoringDoc(), err));
        CHECK(err.path == owner.ToString());
        CHECK(err.detail.find(owner.ToString()) != std::string::npos);
    }
    // otherBody naming an entity without a body: both UUIDs named.
    {
        PhysicsFixture f;
        const auto owner = f.CreateEmpty("Owner");
        const auto plain = f.CreateEmpty("Plain");
        auto& registry = f.manager.GetECS().registry;
        registry.emplace_or_replace<PhysicsBodyComponent>(
            f.Handle(owner), MakeBody());
        registry.emplace_or_replace<PhysicsHingeComponent>(
            f.Handle(owner), MakeHinge(plain));
        Error err;
        CHECK_FALSE(
            ValidatePhysicsConstraintReferences(f.manager.AuthoringDoc(), err));
        CHECK(err.path == owner.ToString());
        CHECK(err.detail.find(owner.ToString()) != std::string::npos);
        CHECK(err.detail.find(plain.ToString()) != std::string::npos);
    }
}

TEST_CASE("T2 physics prefab wires are non-overridable and carry no propagation adapter")
{
    CHECK(PersistedComponents::Count == 17);
    CHECK(CountOverridableEntries() == 9);

    CHECK_FALSE(IsOverridable<PhysicsBodyComponent>());
    CHECK_FALSE(IsOverridable<PhysicsShapeComponent>());
    CHECK_FALSE(IsOverridable<PhysicsHingeComponent>());
    CHECK_FALSE(IsOverridable<PhysicsSliderComponent>());

    const char* wires[] = {"physicsBody", "physicsShape", "physicsHinge",
                           "physicsSlider"};
    for (const char* wire : wires)
    {
        const auto key = FindComponentByWire(wire);
        REQUIRE(key.has_value());
        CHECK_FALSE(key->overridable());
    }

    // PrefabPropagationComponentAdapter is a closed set: physics has no wire
    // adapter in this slice (deferred), so source edits never flow.
    CHECK_FALSE(IsPropagationComponentV<PhysicsBodyComponent>);
    CHECK_FALSE(IsPropagationComponentV<PhysicsShapeComponent>);
    CHECK_FALSE(IsPropagationComponentV<PhysicsHingeComponent>);
    CHECK_FALSE(IsPropagationComponentV<PhysicsSliderComponent>);

    // Ordinary-entity success plus loud member rejection through the
    // CPU mutation-layer guard (IsOverridden): a physics wire on a linked
    // member fails InvalidArgument and leaves state unchanged.
    PhysicsFixture f;
    const auto root = f.CreateEmpty("Rig");
    const auto dir = UniqueTempDir("t2_prefab_wire");
    const auto prefabPath = dir / "rig.rt2prefab";
    REQUIRE(f.manager.CreatePrefabFromSubtree({root}, prefabPath).ok);
    const auto uuids = f.manager.ReserveKnownUuids(1);
    std::vector<AssetDiagnostic> diags;
    const auto inst = f.manager.InstantiatePrefabWithUuids(
        prefabPath, uuids, diags);
    REQUIRE(inst.mutation.success);
    const UUID member = uuids.front();

    const auto before = f.manager.GetOverrides(member);
    REQUIRE(before.IsOk());

    const auto rejected = f.manager.IsOverridden(
        member, PrefabComponentKeyFor<PhysicsBodyComponent>::value);
    CHECK_FALSE(rejected.IsOk());
    CHECK(rejected.error.code == Error::InvalidArgument);
    CHECK(rejected.error.detail.find("physicsBody") != std::string::npos);

    // State unchanged: the override set is exactly what it was.
    const auto after = f.manager.GetOverrides(member);
    REQUIRE(after.IsOk());
    CHECK(after.value.size() == before.value.size());

    std::filesystem::remove_all(dir);
}

TEST_CASE("T2 scene load rejects a prefab override naming a physics wire")
{
    // The frozen table forbids physics overrides; a file claiming one is a
    // loud transactional failure, never a silent drop (which would convert
    // "diverged" into "tracks the source").
    const std::string uuid = "11111111-1111-4111-8111-111111111111";
    const std::string instance = "22222222-2222-4222-8222-222222222222";
    const std::string templ = "33333333-3333-4333-8333-333333333333";
    const std::string content = std::string(R"({
  "version": 8,
  "metadata": {"name": "wire-reject"},
  "entities": [{
    "uuid": ")") + uuid + R"(",
    "name": "Member",
    "parent": "",
    "visible": true,
    "transform": {"translation": [0,0,0], "rotation": [0,0,0,1],
                  "scale": [1,1,1]},
    "prefabMember": {"instanceId": ")" + instance + R"(",
                     "templateId": ")" + templ + R"(",
                     "overrides": ["physicsBody"]}
  }],
  "materials": [], "textures": [],
  "camera": {"position": [0,0,0], "forward": [0,0,-1], "fov": 45},
  "envMap": {"kind": "unknown", "path": "", "sourceKey": ""}
})";
    const auto dir = UniqueTempDir("t2_wire_reject");
    const auto path = dir / "wire.rt2scene";
    WriteFileBinary(path, content);
    SceneDocument loaded;
    Error err;
    CHECK_FALSE(SceneSerializer::Load(loaded, path, err));
    CHECK(err.code == Error::Parse);
    CHECK(err.detail.find("physicsBody") != std::string::npos);
    std::filesystem::remove_all(dir);
}

namespace
{

// Minimal v8 entity carrying one raw physics fragment. The fragment is
// spliced verbatim so malformed JSON shapes reach the codec exactly as a
// hostile/hand-edited file would carry them.
std::string MalformedPhysicsDoc(const std::string& uuid,
                                const std::string& physicsFragment)
{
    return std::string(R"({
  "version": 8,
  "metadata": {"name": "malformed-physics"},
  "entities": [{
    "uuid": ")") + uuid + R"(",
    "name": "Suspicious",
    "parent": "",
    "visible": true,
    "transform": {"translation": [0,0,0], "rotation": [0,0,0,1],
                  "scale": [1,1,1]},
    )" + physicsFragment + R"(
  }],
  "materials": [], "textures": [],
  "camera": {"position": [0,0,0], "forward": [0,0,-1], "fov": 45},
  "envMap": {"kind": "unknown", "path": "", "sourceKey": ""}
})";
}

void ExpectPhysicsParseFail(const std::string& tag,
                            const std::string& physicsFragment,
                            const std::string& componentWire)
{
    const std::string uuid = "11111111-1111-4111-8111-111111111111";
    const auto dir = UniqueTempDir("t2_malformed_" + tag);
    const auto path = dir / "malformed.rt2scene";
    WriteFileBinary(path, MalformedPhysicsDoc(uuid, physicsFragment));
    SceneDocument loaded;
    Error err;
    CHECK_FALSE_MESSAGE(SceneSerializer::Load(loaded, path, err),
                        "fragment " << tag << " loaded but must be rejected");
    CHECK(err.code == Error::Parse);
    // The entity UUID survives in the diagnostic even though Load reports
    // the file path in err.path.
    CHECK_MESSAGE(err.detail.find(uuid) != std::string::npos,
                  "fragment " << tag << " diagnostic names no entity: "
                              << err.detail);
    CHECK_MESSAGE(err.detail.find(componentWire) != std::string::npos,
                  "fragment " << tag << " diagnostic names no block: "
                              << err.detail);
    std::filesystem::remove_all(dir);
}

} // namespace

TEST_CASE("T2 malformed v8 physics blocks fail loudly with entity identity")
{
    // Fixup P1.1: every present block must be an object; every scalar, enum,
    // vector, and asset field is type- and range-checked before conversion,
    // so malformed physics can neither silently default nor escape Load as
    // a JSON exception. One case per defect class per component.
    ExpectPhysicsParseFail("body-string", R"("physicsBody": "bad")",
                           "physicsBody");
    ExpectPhysicsParseFail("body-scalar-type",
                           R"("physicsBody": {"mass": "heavy"})",
                           "physicsBody");
    ExpectPhysicsParseFail("body-bool-type",
                           R"("physicsBody": {"ccdEnabled": "yes"})",
                           "physicsBody");
    ExpectPhysicsParseFail("body-layer-range",
                           R"("physicsBody": {"layer": 70000})",
                           "physicsBody");
    ExpectPhysicsParseFail("body-mask-negative",
                           R"("physicsBody": {"mask": -1})",
                           "physicsBody");
    ExpectPhysicsParseFail("body-kind-unknown",
                           R"("physicsBody": {"kind": "ethereal"})",
                           "physicsBody");
    ExpectPhysicsParseFail("shape-array", R"("physicsShape": [1, 2])",
                           "physicsShape");
    ExpectPhysicsParseFail("shape-short-vector",
                           R"("physicsShape": {"halfExtents": [1, 2]})",
                           "physicsShape");
    ExpectPhysicsParseFail("shape-nonnumeric-vector",
                           R"("physicsShape": {"halfExtents": [1, "x", 3]})",
                           "physicsShape");
    ExpectPhysicsParseFail("shape-hull-string",
                           R"("physicsShape": {"hull": "nope"})",
                           "physicsShape");
    ExpectPhysicsParseFail("hinge-drivemode-range",
                           R"("physicsHinge": {"driveMode": 300})",
                           "physicsHinge");
    ExpectPhysicsParseFail("hinge-long-vector",
                           R"("physicsHinge": {"ownerPivot": [0, 0, 0, 1]})",
                           "physicsHinge");
    ExpectPhysicsParseFail("hinge-other-malformed",
                           R"("physicsHinge": {"otherBody": "not-a-uuid"})",
                           "physicsHinge");
    ExpectPhysicsParseFail("slider-number", R"("physicsSlider": 42)",
                           "physicsSlider");
    ExpectPhysicsParseFail("slider-nonnumeric-axis",
                           R"("physicsSlider": {"axis": [1, "y", 0]})",
                           "physicsSlider");
}

TEST_CASE("T2 nested asset fields and float overflow fail loudly with wire path")
{
    // Fixup P1.1 (narrowed): the shared asset decoder silently discards a
    // present kind/path/sourceKey unless it is a string, and finite doubles
    // outside the float domain narrow to infinity. Both gaps reproduced as
    // successful SliceRunner loads; both are now transactional Parse
    // failures naming the entity UUID and the complete physics wire path.
    ExpectPhysicsParseFail("shape-path-number",
                           R"("physicsShape": {"shape": "convexHull", "hull": {"kind": "model", "path": 123, "sourceKey": "obj:whole-model"}})",
                           "physicsShape.hull.path");
    ExpectPhysicsParseFail("shape-sourcekey-number",
                           R"("physicsShape": {"triMesh": {"kind": "model", "path": "colliders/ramp.obj", "sourceKey": 7}})",
                           "physicsShape.triMesh.sourceKey");
    ExpectPhysicsParseFail("shape-kind-number",
                           R"("physicsShape": {"hull": {"kind": 42, "path": "colliders/hull.obj"}})",
                           "physicsShape.hull.kind");
    ExpectPhysicsParseFail("shape-assetid-number",
                           R"("physicsShape": {"hull": {"kind": "model", "path": "colliders/hull.obj", "assetId": 9}})",
                           "physicsShape.hull.assetId");
    ExpectPhysicsParseFail("shape-importsettings-flag",
                           R"("physicsShape": {"hull": {"kind": "model", "path": "colliders/hull.obj", "importSettings": {"triangulate": "yes"}}})",
                           "physicsShape.hull");
    ExpectPhysicsParseFail("shape-unknown-kind",
                           R"("physicsShape": {"hull": {"kind": "unknown", "path": "colliders/orphan.obj"}})",
                           "physicsShape.hull");
    ExpectPhysicsParseFail("body-mass-overflow",
                           R"("physicsBody": {"mass": 1e39})",
                           "physicsBody");
    ExpectPhysicsParseFail("hinge-pivot-overflow",
                           R"("physicsHinge": {"ownerPivot": [1e39, 0, 0]})",
                           "physicsHinge");
}

TEST_CASE("T2 both persisted collision refs are visited unconditionally")
{
    // Fixup P1.2: the visitor emits BOTH stored AssetReferences of every
    // physics shape — active or inactive, valid or empty — matching
    // imported/script/prefab reference behavior. Both fields are exact
    // authored payload and both are serialized, so save validation,
    // migration, and dependency protection must see them. A malformed
    // stored ref (non-empty path, unknown kind) is therefore caught by the
    // save gate instead of sailing into a v8 file that Load refuses.
    PhysicsFixture f;
    const auto hullEntity = f.CreateEmpty("Hull");
    const auto rampEntity = f.CreateEmpty("Ramp");
    const auto plainEntity = f.CreateEmpty("Plain");
    auto& registry = f.manager.GetECS().registry;
    PhysicsShapeComponent hullShape;
    hullShape.shape = PhysicsShapeKind::ConvexHull;
    hullShape.hull.kind = AssetKind::Model;
    hullShape.hull.path = "colliders/hull.obj";
    hullShape.hull.sourceKey = "obj:whole-model";
    // Inactive side is still visited (stale refs stay protected).
    hullShape.triMesh.kind = AssetKind::Model;
    hullShape.triMesh.path = "colliders/stale.obj";
    hullShape.triMesh.sourceKey = "obj:whole-model";
    registry.emplace_or_replace<PhysicsShapeComponent>(
        f.Handle(hullEntity), hullShape);
    PhysicsShapeComponent rampShape;
    rampShape.shape = PhysicsShapeKind::StaticTriMesh;
    rampShape.triMesh.kind = AssetKind::Model;
    rampShape.triMesh.path = "colliders/ramp.obj";
    rampShape.triMesh.sourceKey = "obj:whole-model";
    registry.emplace_or_replace<PhysicsShapeComponent>(
        f.Handle(rampEntity), rampShape);
    // Sphere with an inactive hull path set: also visited.
    PhysicsShapeComponent sphereShape;
    sphereShape.hull.kind = AssetKind::Model;
    sphereShape.hull.path = "colliders/unused.obj";
    sphereShape.hull.sourceKey = "obj:whole-model";
    registry.emplace_or_replace<PhysicsShapeComponent>(
        f.Handle(plainEntity), sphereShape);

    const auto slots =
        CollectSceneAssetReferences(f.manager.AuthoringDoc());
    bool sawHull = false;
    bool sawRamp = false;
    bool sawStale = false;
    bool sawUnused = false;
    std::size_t physicsSlots = 0;
    for (const auto& slot : slots)
    {
        REQUIRE(slot.reference);
        if (slot.reference->path == "colliders/hull.obj") sawHull = true;
        if (slot.reference->path == "colliders/ramp.obj") sawRamp = true;
        if (slot.reference->path == "colliders/stale.obj") sawStale = true;
        if (slot.reference->path == "colliders/unused.obj") sawUnused = true;
    }
    CHECK(sawHull);
    CHECK(sawRamp);
    CHECK(sawStale);
    CHECK(sawUnused);
    // Exactly two slots per physics shape (3 shapes x 2 refs), including
    // the empty default refs.
    for (const auto& slot : slots)
    {
        if (slot.entityUuid == hullEntity || slot.entityUuid == rampEntity ||
            slot.entityUuid == plainEntity)
            ++physicsSlots;
    }
    CHECK(physicsSlots == 6);
}

TEST_CASE("T2 save rejects a physics ref with a path but no asset kind")
{
    // Fixup P1.2 companion: the unconditional visitor feeds SaveInternal's
    // existing unknown-kind gate, so a malformed stored physics ref fails
    // the save loudly instead of producing a v8 file Load would refuse.
    PhysicsFixture f;
    const auto bad = f.CreateEmpty("Bad");
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::ConvexHull;
    shape.hull.kind = AssetKind::Unknown;
    shape.hull.path = "colliders/orphan.obj";
    shape.hull.sourceKey = "obj:whole-model";
    f.manager.GetECS().registry.emplace_or_replace<PhysicsShapeComponent>(
        f.Handle(bad), shape);

    const auto dir = UniqueTempDir("t2_save_reject_physics_ref");
    const auto path = dir / "bad.rt2scene";
    Error err;
    std::vector<AssetDiagnostic> diagnostics;
    CHECK_FALSE(
        SceneSerializer::Save(f.manager.AuthoringDoc(), path, diagnostics,
                              err));
    CHECK(err.code == Error::InvalidArgument);
    std::filesystem::remove_all(dir);
}
