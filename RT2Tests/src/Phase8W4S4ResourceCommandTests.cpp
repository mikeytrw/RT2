#include <doctest/doctest.h>

#include "PrefabPropagationCommand.h"
#include "PrefabPropagationService.h"
#include "EditorCommandHistory.h"
#include "AssetIdentity.h"
#include "SceneManager.h"
#include "SceneSerializer.h"

#include <fstream>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <limits>

using namespace rt2::core;

namespace
{
const UUID kEntity = UUID::Parse("11111111-1111-4111-8111-111111111111");
const UUID kInstance = UUID::Parse("22222222-2222-4222-8222-222222222222");
const UUID kTemplate = UUID::Parse("33333333-3333-4333-8333-333333333333");

MeshData Triangle(const char* name)
{
    MeshData mesh;
    mesh.name = name;
    mesh.vertices = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    mesh.indices = {0, 1, 2};
    return mesh;
}

struct CheckedTempDir
{
    std::filesystem::path path;
    bool armed = true;
    ~CheckedTempDir()
    {
        if (!armed) return;
        std::error_code error;
        std::filesystem::remove_all(path, error);
        if (error) std::abort();
    }
};

void PopulateScene(SceneManager& scene, bool notify = true)
{
    auto& doc = scene.AuthoringDoc();
    const auto entity = doc.ecs.registry.create();
    REQUIRE(doc.AssignKnownUuid(entity, kEntity));
    doc.ecs.registry.emplace<NameComponent>(entity, NameComponent{"old"});
    doc.ecs.registry.emplace<Transform>(entity, Transform{});
    doc.ecs.registry.emplace<VisibleComponent>(entity, VisibleComponent{true});
    doc.ecs.registry.emplace<PrimitiveComponent>(entity,
        PrimitiveComponent{PrimitiveComponent::Cube, 1.0f, 24, 16});
    doc.ecs.registry.emplace<PrefabMemberComponent>(entity,
        PrefabMemberComponent{kInstance, kTemplate, {}});
    doc.ecs.registry.emplace<PrefabInstanceComponent>(entity,
        PrefabInstanceComponent{AssetReference{AssetKind::Prefab,
            "assets/source.rt2prefab", {}, {}, kInstance}, kInstance});
    const auto mesh = doc.ecs.meshRegistry.AddMesh(Triangle("before"));
    doc.ecs.registry.emplace<MeshRef>(entity, MeshRef{mesh, -1});
    if (notify) scene.NotifyAuthoringChanged();
}

void FinalizePlan(SceneManager& scene, PrefabPropagationPlan& plan)
{
    plan.authoringRevision = scene.AuthoringRevision();
    plan.authoringRevisionCaptured = true;
    plan.meshTableExtent = static_cast<std::uint32_t>(scene.GetMeshRegistryCount());
    plan.materialTableExtent = static_cast<std::uint32_t>(scene.GetMaterialCount());
    plan.textureTableExtent = static_cast<std::uint32_t>(scene.GetECS().textures.size());
    const auto root = scene.FindEntityByUuid(kEntity);
    const auto& registry = scene.AuthoringDoc().ecs.registry;
    if (const auto* link = registry.try_get<PrefabInstanceComponent>(root))
        if (std::none_of(plan.rootSnapshots.begin(), plan.rootSnapshots.end(),
                [](const auto& snapshot) { return snapshot.rootUuid == kEntity; }))
            plan.rootSnapshots.push_back({kEntity, link->instanceId, link->prefab});
}
}

TEST_CASE("Phase 8 W4 S4: propagation command owns append-only resources and reuses slots")
{
    SceneManager scene;
    PopulateScene(scene);
    auto& doc = scene.AuthoringDoc();
    auto& ecs = doc.ecs;
    const auto source = PrefabSourceFingerprint{
        "assets/source.rt2prefab", kInstance, "digest-a"};
    const auto beforeRevision = scene.AuthoringRevision();
    const auto beforeResourceGeneration = scene.ResourceGeneration();

    PrefabPropagationPlan plan;
    plan.source = source;
    plan.documentGeneration = scene.DocumentGeneration();
    plan.resourceGeneration = beforeResourceGeneration;
    plan.authoringRevision = beforeRevision;
    plan.componentOperations.push_back(PrefabPropagationComponentDelta::Make<NameComponent>(
        kEntity, kTemplate, NameComponent{"old"}, NameComponent{"new"}));
    plan.memberSnapshots.push_back({kEntity, kInstance, kTemplate, {}});
    const auto beforeRef = ecs.registry.get<MeshRef>(doc.FindByUuid(kEntity));
    const auto mesh = Triangle("after");
    PrefabPropagationResourceOwnership owned;
    owned.rebase.kind = PrefabPropagationResourceKind::Mesh;
    owned.rebase.sourceBeforeExtent = 1;
    owned.rebase.sceneBeforeExtent = 1;
    owned.rebase.sceneAppendBase = 1;
    owned.rebase.sceneAfterExtent = 2;
    owned.rebase.sourceSlots = {{0}};
    owned.rebase.sceneSlots = {{1}};
    owned.rebase.owned = PrefabPropagationResourceBlock::FromDecoded(
        PrefabPropagationResourceKind::Mesh,
        {PrefabPropagationResourcePayload{"mesh:after", "digest-m", mesh}});
    plan.resourceOwnership.push_back(owned);
    plan.meshRefOperations.push_back({kEntity, kTemplate, beforeRef, MeshRef{1, -1}});
    plan.affectedEntities = {kEntity};
    plan.syncImpact = SyncImpact::Structural;
    FinalizePlan(scene, plan);
    REQUIRE(plan.IsEffective());

    std::size_t fingerprintReads = 0;
    PrefabPropagationCommand command(plan, [&] {
        ++fingerprintReads;
        return Result<PrefabSourceFingerprint>::Ok(
            fingerprintReads == 1 ? source :
            PrefabSourceFingerprint{"assets/source.rt2prefab", kInstance, "changed-after-undo"});
    });
    const auto result = command.Execute(scene);
    REQUIRE(result.success);
    CHECK(result.effective);
    CHECK(scene.AuthoringDoc().ecs.registry.get<NameComponent>(
        scene.FindEntityByUuid(kEntity)).name == "new");
    CHECK(scene.AuthoringDoc().ecs.registry.get<MeshRef>(
        scene.FindEntityByUuid(kEntity)).meshIndex == 1);
    CHECK(ecs.meshRegistry.GetCount() == 2);
    CHECK(scene.ResourceGeneration() == beforeResourceGeneration + 1);

    const auto undo = command.Undo(scene);
    REQUIRE(undo.success);
    CHECK(scene.AuthoringDoc().ecs.registry.get<NameComponent>(
        scene.FindEntityByUuid(kEntity)).name == "old");
    CHECK(scene.AuthoringDoc().ecs.registry.get<MeshRef>(
        scene.FindEntityByUuid(kEntity)).meshIndex == beforeRef.meshIndex);
    CHECK(ecs.meshRegistry.GetCount() == 2);
    CHECK(scene.ResourceGeneration() == beforeResourceGeneration + 2);

    const auto redo = command.Execute(scene);
    REQUIRE(redo.success);
    CHECK(scene.AuthoringDoc().ecs.registry.get<MeshRef>(
        scene.FindEntityByUuid(kEntity)).meshIndex == 1);
    CHECK(ecs.meshRegistry.GetMesh(1).name == "after");
    CHECK(ecs.meshRegistry.GetCount() == 2);
    CHECK(fingerprintReads == 1);
}

TEST_CASE("Phase 8 W4 S4: stale value and fingerprint reject with zero mutation")
{
    SceneManager scene;
    PopulateScene(scene);
    auto& doc = scene.AuthoringDoc();
    const auto source = PrefabSourceFingerprint{
        "assets/source.rt2prefab", kInstance, "digest-a"};
    PrefabPropagationPlan plan;
    plan.source = source;
    plan.documentGeneration = scene.DocumentGeneration();
    plan.resourceGeneration = scene.ResourceGeneration();
    plan.authoringRevision = scene.AuthoringRevision();
    plan.componentOperations.push_back(PrefabPropagationComponentDelta::Make<NameComponent>(
        kEntity, kTemplate, NameComponent{"old"}, NameComponent{"new"}));
    plan.memberSnapshots.push_back({kEntity, kInstance, kTemplate, {}});
    plan.affectedEntities = {kEntity};
    plan.syncImpact = SyncImpact::None;
    FinalizePlan(scene, plan);
    REQUIRE(plan.IsEffective());
    doc.ecs.registry.get<NameComponent>(doc.FindByUuid(kEntity)).name = "changed";
    const auto beforeRevision = scene.AuthoringRevision();
    PrefabPropagationCommand command(plan, [source] {
        return Result<PrefabSourceFingerprint>::Ok(source);
    });
    const auto result = command.Execute(scene);
    CHECK_FALSE(result.success);
    CHECK(scene.AuthoringRevision() == beforeRevision);
    CHECK(scene.AuthoringDoc().ecs.registry.get<NameComponent>(
        scene.FindEntityByUuid(kEntity)).name == "changed");

    SceneManager fingerprintScene;
    PopulateScene(fingerprintScene);
    PrefabPropagationPlan fingerprintPlan;
    fingerprintPlan.source = source;
    fingerprintPlan.documentGeneration = fingerprintScene.DocumentGeneration();
    fingerprintPlan.resourceGeneration = fingerprintScene.ResourceGeneration();
    fingerprintPlan.authoringRevision = fingerprintScene.AuthoringRevision();
    fingerprintPlan.componentOperations.push_back(PrefabPropagationComponentDelta::Make<NameComponent>(
        kEntity, kTemplate, NameComponent{"old"}, NameComponent{"new"}));
    fingerprintPlan.memberSnapshots.push_back({kEntity, kInstance, kTemplate, {}});
    fingerprintPlan.affectedEntities = {kEntity};
    fingerprintPlan.syncImpact = SyncImpact::None;
    FinalizePlan(fingerprintScene, fingerprintPlan);
    REQUIRE(fingerprintPlan.IsEffective());
    const auto fingerprintResult = PrefabPropagationCommand(fingerprintPlan, [] {
        return Result<PrefabSourceFingerprint>::Ok(
            PrefabSourceFingerprint{"assets/source.rt2prefab", kInstance, "digest-b"});
    }).Execute(fingerprintScene);
    CHECK_FALSE(fingerprintResult.success);
    CHECK(fingerprintScene.AuthoringDoc().ecs.registry.get<NameComponent>(
        fingerprintScene.FindEntityByUuid(kEntity)).name == "old");

    SceneManager ioScene;
    PopulateScene(ioScene);
    auto ioPlan = fingerprintPlan;
    ioPlan.documentGeneration = ioScene.DocumentGeneration();
    ioPlan.resourceGeneration = ioScene.ResourceGeneration();
    ioPlan.authoringRevision = ioScene.AuthoringRevision();
    FinalizePlan(ioScene, ioPlan);
    const auto ioResult = PrefabPropagationCommand(ioPlan, [] {
        return Result<PrefabSourceFingerprint>::Fail(
            Error::Io, "locked.rt2prefab", "source bytes unavailable");
    }).Execute(ioScene);
    CHECK_FALSE(ioResult.success);
    CHECK(ioResult.error.code == Error::Io);
    CHECK(ioResult.error.path == "locked.rt2prefab");
    CHECK(ioResult.error.detail == "source bytes unavailable");
}

TEST_CASE("Phase 8 W4 S4: each commit precondition rejects its own stale mutation")
{
    const auto source = PrefabSourceFingerprint{
        "assets/source.rt2prefab", kInstance, "digest-a"};
    auto makePlan = [&](SceneManager& scene) {
        PrefabPropagationPlan plan;
        plan.source = source;
        plan.documentGeneration = scene.DocumentGeneration();
        plan.resourceGeneration = scene.ResourceGeneration();
        plan.authoringRevision = scene.AuthoringRevision();
        plan.componentOperations.push_back(PrefabPropagationComponentDelta::Make<NameComponent>(
            kEntity, kTemplate, NameComponent{"old"}, NameComponent{"new"}));
        plan.memberSnapshots.push_back({kEntity, kInstance, kTemplate, {}});
        plan.affectedEntities = {kEntity};
        plan.syncImpact = SyncImpact::None;
        FinalizePlan(scene, plan);
        REQUIRE(plan.IsEffective());
        return plan;
    };

    {
        SceneManager scene; PopulateScene(scene);
        auto plan = makePlan(scene);
        auto clone = SceneDocument{};
        Error error;
        REQUIRE(SceneSerializer::CloneInMemory(scene.AuthoringDoc(), clone, error));
        scene.ReplaceAuthoringDocument(std::move(clone));
        plan.resourceGeneration = scene.ResourceGeneration();
        const auto result = PrefabPropagationCommand(plan, [source] {
            return Result<PrefabSourceFingerprint>::Ok(source);
        }).Execute(scene);
        CHECK_FALSE(result.success);
    }
    {
        SceneManager scene; PopulateScene(scene);
        auto plan = makePlan(scene);
        scene.AuthoringDoc().ecs.meshRegistry.AddMesh(Triangle("orphan"));
        REQUIRE(scene.CompactMeshRegistry());
        const auto result = PrefabPropagationCommand(plan, [source] {
            return Result<PrefabSourceFingerprint>::Ok(source);
        }).Execute(scene);
        CHECK_FALSE(result.success);
    }
    {
        SceneManager scene; PopulateScene(scene);
        auto plan = makePlan(scene);
        scene.AuthoringDoc().ecs.registry.get<PrefabMemberComponent>(
            scene.FindEntityByUuid(kEntity)).instanceId = UUID::Parse(
                "44444444-4444-4444-8444-444444444444");
        CHECK_FALSE(PrefabPropagationCommand(plan, [source] {
            return Result<PrefabSourceFingerprint>::Ok(source);
        }).Execute(scene).success);
    }
    {
        SceneManager scene; PopulateScene(scene);
        auto plan = makePlan(scene);
        scene.AuthoringDoc().ecs.registry.get<PrefabMemberComponent>(
            scene.FindEntityByUuid(kEntity)).overrides.push_back(
                PrefabComponentKeyFor<NameComponent>::value);
        CHECK_FALSE(PrefabPropagationCommand(plan, [source] {
            return Result<PrefabSourceFingerprint>::Ok(source);
        }).Execute(scene).success);
    }
    {
        SceneManager scene; PopulateScene(scene);
        auto plan = makePlan(scene);
        scene.AuthoringDoc().ecs.registry.get<MeshRef>(
            scene.FindEntityByUuid(kEntity)).materialIndex = 4;
        plan.meshRefOperations.push_back({kEntity, kTemplate,
            MeshRef{0, -1}, MeshRef{0, -1}});
        plan.affectedEntities = {kEntity};
        plan.syncImpact = SyncImpact::None;
        CHECK_FALSE(PrefabPropagationCommand(plan, [source] {
            return Result<PrefabSourceFingerprint>::Ok(source);
        }).Execute(scene).success);
    }
}

TEST_CASE("Phase 8 W4 S4: local primitive recipe is one atomic append and marker edit")
{
    SceneManager scene;
    PopulateScene(scene);
    auto entity = scene.FindEntityByUuid(kEntity);
    auto& reg = scene.AuthoringDoc().ecs.registry;
    reg.emplace_or_replace<PrimitiveComponent>(entity,
        PrimitiveComponent{PrimitiveComponent::Cube, 1.0f, 24, 16});
    const auto beforeCount = scene.GetMeshRegistryCount();
    const auto beforeGeneration = scene.ResourceGeneration();
    const auto after = PrimitiveComponent{PrimitiveComponent::Sphere, 2.0f, 16, 8};
    auto prepared = PrefabPrimitiveRecipeCommand::Prepare(scene, kEntity, after);
    REQUIRE(prepared.IsOk());
    auto command = std::move(prepared.value);
    REQUIRE(command->Execute(scene).success);
    CHECK(scene.GetMeshRegistryCount() == beforeCount + 1);
    CHECK(reg.get<PrimitiveComponent>(entity).kind == PrimitiveComponent::Sphere);
    CHECK(reg.get<PrefabMemberComponent>(entity).overrides.size() == 1);
    CHECK(scene.ResourceGeneration() == beforeGeneration + 1);
    REQUIRE(command->Undo(scene).success);
    CHECK(scene.GetMeshRegistryCount() == beforeCount + 1);
    CHECK(reg.get<PrimitiveComponent>(entity).kind == PrimitiveComponent::Cube);
    CHECK(reg.get<PrefabMemberComponent>(entity).overrides.empty());
    REQUIRE(command->Execute(scene).success);
    CHECK(scene.GetMeshRegistryCount() == beforeCount + 1);
    CHECK(reg.get<PrefabMemberComponent>(entity).overrides.size() == 1);
}

TEST_CASE("Phase 8 W4 S4: resource staging is clone-only and derives MeshRef rebases")
{
    SceneManager scene;
    PopulateScene(scene);
    auto& doc = scene.AuthoringDoc();
    const auto entity = doc.FindByUuid(kEntity);
    doc.ecs.registry.emplace_or_replace<PrimitiveComponent>(entity,
        PrimitiveComponent{PrimitiveComponent::Cube, 1.0f, 24, 16});
    const auto beforeMeshes = scene.GetMeshRegistryCount();
    const auto beforeRevision = scene.AuthoringRevision();
    const auto source = PrefabSourceFingerprint{
        "assets/source.rt2prefab", kInstance, "digest-stage"};
    PrefabPropagationPlan durable;
    durable.source = source;
    durable.documentGeneration = scene.DocumentGeneration();
    durable.resourceGeneration = scene.ResourceGeneration();
    durable.authoringRevision = beforeRevision;
    durable.componentOperations.push_back(PrefabPropagationComponentDelta::Make<PrimitiveComponent>(
        kEntity, kTemplate,
        PrimitiveComponent{PrimitiveComponent::Cube, 1.0f, 24, 16},
        PrimitiveComponent{PrimitiveComponent::Sphere, 2.0f, 16, 8}));
    durable.memberSnapshots.push_back({kEntity, kInstance, kTemplate, {}});
    durable.instances.push_back({kInstance, kEntity,
        PrefabPropagationInstanceDisposition::Propagate, {kEntity}, {}});
    durable.affectedEntities = {kEntity};
    durable.syncImpact = SyncImpact::Structural;
    FinalizePlan(scene, durable);
    REQUIRE(durable.IsEffective());
    AssetResolutionContext assets;
    const auto staged = StagePrefabPropagationResources(durable, doc, assets);
    REQUIRE(staged.IsOk());
    CHECK(scene.GetMeshRegistryCount() == beforeMeshes);
    CHECK(scene.AuthoringRevision() == beforeRevision);
    REQUIRE(staged.value.resourceOwnership.size() == 1);
    REQUIRE(staged.value.meshRefOperations.size() == 1);
    CHECK(staged.value.meshRefOperations.front().after->meshIndex == beforeMeshes);
    CHECK(staged.value.resourceOwnership.front().rebase.sceneBeforeExtent == beforeMeshes);
    CHECK(staged.value.resourceOwnership.front().rebase.sceneAfterExtent == beforeMeshes + 1);
}

TEST_CASE("Phase 8 W4 S4: imported staging resolves source material then one authored layer")
{
    namespace fs = std::filesystem;
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    CheckedTempDir temp{fs::temp_directory_path() /
        ("rt2_w4_s4_imported_stage_" + std::to_string(suffix))};
    const auto& dir = temp.path;
    REQUIRE(fs::create_directories(dir));
    {
        std::ofstream tex(dir / "albedo.ppm");
        REQUIRE(tex.good());
        const char header[] = "P6\n1 1\n255\n";
        tex.write(header, sizeof(header) - 1);
        const unsigned char rgb[] = {255, 0, 0};
        tex.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
        Error textureIdentityError;
        REQUIRE(WriteSidecarId(AssetSidecarPath(dir / "albedo.ppm"),
                               kInstance, textureIdentityError));
        std::ofstream mtl(dir / "model.mtl");
        REQUIRE(mtl.good());
        mtl << "newmtl source\nKd 0.2 0.4 0.6\nmap_Kd albedo.ppm\n";
        std::ofstream obj(dir / "model.obj");
        REQUIRE(obj.good());
        obj << "mtllib model.mtl\no Shape\n"
               "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
               "usemtl source\nf 1 2 3\n";
    }

    SceneManager scene;
    auto& doc = scene.AuthoringDoc();
    const auto entity = doc.ecs.registry.create();
    REQUIRE(doc.AssignKnownUuid(entity, kEntity));
    doc.ecs.registry.emplace<NameComponent>(entity, NameComponent{"Imported"});
    doc.ecs.registry.emplace<Transform>(entity, Transform{});
    doc.ecs.registry.emplace<VisibleComponent>(entity, VisibleComponent{true});
    doc.ecs.registry.emplace<PrefabMemberComponent>(entity,
        PrefabMemberComponent{kInstance, kTemplate, {}});
    doc.ecs.registry.emplace<PrefabInstanceComponent>(entity,
        PrefabInstanceComponent{AssetReference{AssetKind::Prefab,
            "model.rt2prefab", {}, {}, kInstance}, kInstance});
    AssetReference oldSource;
    oldSource.kind = AssetKind::Model;
    oldSource.path = "old.obj";
    oldSource.sourceKey = "obj:whole-model";
    AssetReference newSource = oldSource;
    newSource.path = "model.obj";
    doc.ecs.registry.emplace<ImportedMeshSourceComponent>(entity,
        ImportedMeshSourceComponent{oldSource});
    doc.ecs.meshRegistry.AddMesh(Triangle("old"));
    doc.ecs.registry.emplace<MeshRef>(entity, MeshRef{0, 0});
    SceneMaterial oldMaterial;
    oldMaterial.sourceKey = "obj:material:index=0";
    doc.ecs.materials.push_back(oldMaterial);
    MaterialOverrideComponent authored;
    authored.authored = true;
    authored.material.baseColor = {0.9f, 0.1f, 0.2f};
    scene.NotifyAuthoringChanged();
    const auto beforeSource = ImportedMeshSourceComponent{oldSource};
    const auto afterSource = ImportedMeshSourceComponent{newSource};
    PrefabPropagationPlan durable;
    durable.source = {dir / "model.rt2prefab", kInstance, "digest-import"};
    durable.documentGeneration = scene.DocumentGeneration();
    durable.resourceGeneration = scene.ResourceGeneration();
    durable.authoringRevision = scene.AuthoringRevision();
    durable.componentOperations.push_back(PrefabPropagationComponentDelta::Make<ImportedMeshSourceComponent>(
        kEntity, kTemplate, beforeSource, afterSource));
    durable.componentOperations.push_back(PrefabPropagationComponentDelta::Make<MaterialOverrideComponent>(
        kEntity, kTemplate, std::nullopt, authored));
    durable.memberSnapshots.push_back({kEntity, kInstance, kTemplate, {}});
    durable.instances.push_back({kInstance, kEntity,
        PrefabPropagationInstanceDisposition::Propagate, {kEntity}, {}});
    durable.affectedEntities = {kEntity};
    durable.syncImpact = SyncImpact::Structural;
    FinalizePlan(scene, durable);
    REQUIRE(durable.IsEffective());
    const auto staged = StagePrefabPropagationResources(
        durable, doc, AssetResolutionContext{dir, nullptr});
    REQUIRE(staged.IsOk());
    CHECK(scene.GetMeshRegistryCount() == 1);
    CHECK(scene.GetMaterialCount() == 1);
    REQUIRE(staged.value.resourceOwnership.size() >= 2);
    REQUIRE(staged.value.meshRefOperations.size() == 1);
    CHECK(staged.value.meshRefOperations.front().after->meshIndex == 1);
    CHECK(staged.value.meshRefOperations.front().after->materialIndex >= 1);
    CHECK(staged.value.componentOperations.size() == 2);
    bool authoredMaterialStaged = false;
    for (const auto& ownership : staged.value.resourceOwnership)
        if (ownership.rebase.kind == PrefabPropagationResourceKind::Material)
            for (const auto& payload : ownership.rebase.owned.Entries())
                if (const auto* material = std::get_if<SceneMaterial>(&payload.decoded))
                    authoredMaterialStaged |= material->baseColor.x == doctest::Approx(0.9f);
    CHECK(authoredMaterialStaged);
    const auto repaired = std::find_if(staged.value.componentOperations.begin(),
        staged.value.componentOperations.end(), [](const auto& operation) {
            return operation.Key().wire() == PrefabWireKeys::kMaterialOverride;
        });
    REQUIRE(repaired != staged.value.componentOperations.end());
    REQUIRE(repaired->AfterValue().has_value());
    const auto repairedValueOptional = repaired->AfterValue();
    const auto& repairedValue = std::get<MaterialOverrideComponent>(*repairedValueOptional);
    CHECK(repairedValue.material.baseColor.x == doctest::Approx(0.9f));
    REQUIRE(staged.value.meshRefOperations.front().after.has_value());
    CHECK(repairedValue.materialIndex ==
          staged.value.meshRefOperations.front().after->materialIndex);
    bool repairedMaterialMatchesOwned = false;
    for (const auto& ownership : staged.value.resourceOwnership)
        if (ownership.rebase.kind == PrefabPropagationResourceKind::Material)
            for (const auto& payload : ownership.rebase.owned.Entries())
                if (const auto* material = std::get_if<SceneMaterial>(&payload.decoded))
                    repairedMaterialMatchesOwned |=
                        PrefabCanonicalMaterialEqual(*material, repairedValue.material);
    CHECK(repairedMaterialMatchesOwned);
}

TEST_CASE("Phase 8 W4 S4: candidate isolation omits unrelated imports")
{
    SceneManager scene;
    PopulateScene(scene);
    auto& doc = scene.AuthoringDoc();
    const UUID unrelated = UUID::Parse("55555555-5555-4555-8555-555555555555");
    const auto entity = doc.ecs.registry.create();
    REQUIRE(doc.AssignKnownUuid(entity, unrelated));
    AssetReference missing{AssetKind::Model, "unrelated.obj", {}, "obj:whole-model", kInstance};
    doc.ecs.registry.emplace<ImportedMeshSourceComponent>(entity,
        ImportedMeshSourceComponent{missing});
    scene.NotifyAuthoringChanged();

    PrefabPropagationPlan plan;
    plan.source = {"assets/source.rt2prefab", kInstance, "digest-isolation"};
    plan.documentGeneration = scene.DocumentGeneration();
    plan.resourceGeneration = scene.ResourceGeneration();
    plan.componentOperations.push_back(PrefabPropagationComponentDelta::Make<NameComponent>(
        kEntity, kTemplate, NameComponent{"old"}, NameComponent{"new"}));
    plan.memberSnapshots.push_back({kEntity, kInstance, kTemplate, {}});
    plan.instances.push_back({kInstance, kEntity,
        PrefabPropagationInstanceDisposition::Propagate, {kEntity}, {}});
    plan.affectedEntities = {kEntity};
    plan.syncImpact = SyncImpact::None;
    FinalizePlan(scene, plan);
    REQUIRE(plan.IsEffective());
    const auto staged = StagePrefabPropagationResources(plan, doc, AssetResolutionContext{});
    REQUIRE(staged.IsOk());
    CHECK(staged.value.resourceOwnership.empty());
    CHECK(staged.value.meshRefOperations.empty());
    CHECK(staged.value.componentOperations.size() == 1);
}

TEST_CASE("Phase 8 W4 S4: resolver failure quarantines the whole instance")
{
    SceneManager scene;
    PopulateScene(scene);
    auto& doc = scene.AuthoringDoc();
    const UUID childUuid = UUID::Parse("66666666-6666-4666-8666-666666666666");
    const auto child = doc.ecs.registry.create();
    REQUIRE(doc.AssignKnownUuid(child, childUuid));
    doc.ecs.registry.emplace<NameComponent>(child, NameComponent{"child"});
    doc.ecs.registry.emplace<PrimitiveComponent>(child,
        PrimitiveComponent{PrimitiveComponent::Cube, 1.0f, 24, 16});
    const auto childMesh = doc.ecs.meshRegistry.AddMesh(Triangle("child-before"));
    doc.ecs.registry.emplace<MeshRef>(child, MeshRef{childMesh, -1});
    doc.ecs.registry.emplace<PrefabMemberComponent>(child,
        PrefabMemberComponent{kInstance, UUID::Parse("77777777-7777-4777-8777-777777777777"), {}});
    scene.NotifyAuthoringChanged();

    AssetReference missing{AssetKind::Model, "missing.obj", {}, "obj:whole-model", kInstance};
    PrefabPropagationPlan plan;
    plan.source = {"assets/source.rt2prefab", kInstance, "digest-quarantine"};
    plan.documentGeneration = scene.DocumentGeneration();
    plan.resourceGeneration = scene.ResourceGeneration();
    plan.componentOperations.push_back(PrefabPropagationComponentDelta::Make<ImportedMeshSourceComponent>(
        kEntity, kTemplate, std::nullopt, ImportedMeshSourceComponent{missing}));
    plan.componentOperations.push_back(PrefabPropagationComponentDelta::Make<PrimitiveComponent>(
        childUuid, UUID::Parse("77777777-7777-4777-8777-777777777777"),
        PrimitiveComponent{PrimitiveComponent::Cube, 1.0f, 24, 16},
        PrimitiveComponent{PrimitiveComponent::Sphere, 2.0f, 16, 8}));
    plan.memberSnapshots.push_back({kEntity, kInstance, kTemplate, {}});
    plan.memberSnapshots.push_back({childUuid, kInstance,
        UUID::Parse("77777777-7777-4777-8777-777777777777"), {}});
    plan.instances.push_back({kInstance, kEntity,
        PrefabPropagationInstanceDisposition::Propagate, {kEntity, childUuid}, {}});
    plan.affectedEntities = {kEntity, childUuid};
    plan.syncImpact = SyncImpact::Structural;
    FinalizePlan(scene, plan);
    const UUID validRootUuid = UUID::Parse("88888888-8888-4888-8888-888888888888");
    const UUID validInstanceId = UUID::Parse("99999999-9999-4999-8999-999999999999");
    const UUID validTemplateId = UUID::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    const auto validRoot = doc.ecs.registry.create();
    REQUIRE(doc.AssignKnownUuid(validRoot, validRootUuid));
    doc.ecs.registry.emplace<PrimitiveComponent>(validRoot,
        PrimitiveComponent{PrimitiveComponent::Cube, 1.0f, 24, 16});
    const auto validMesh = doc.ecs.meshRegistry.AddMesh(Triangle("valid-before"));
    doc.ecs.registry.emplace<MeshRef>(validRoot, MeshRef{validMesh, -1});
    doc.ecs.registry.emplace<PrefabMemberComponent>(validRoot,
        PrefabMemberComponent{validInstanceId, validTemplateId, {}});
    doc.ecs.registry.emplace<PrefabInstanceComponent>(validRoot,
        PrefabInstanceComponent{AssetReference{AssetKind::Prefab,
            "valid.rt2prefab", {}, {}, kInstance}, validInstanceId});
    plan.componentOperations.push_back(PrefabPropagationComponentDelta::Make<PrimitiveComponent>(
        validRootUuid, validTemplateId,
        PrimitiveComponent{PrimitiveComponent::Cube, 1.0f, 24, 16},
        PrimitiveComponent{PrimitiveComponent::Sphere, 2.0f, 16, 8}));
    plan.memberSnapshots.push_back({validRootUuid, validInstanceId, validTemplateId, {}});
    plan.instances.push_back({validInstanceId, validRootUuid,
        PrefabPropagationInstanceDisposition::Propagate, {validRootUuid}, {}});
    plan.rootSnapshots.push_back({validRootUuid, validInstanceId,
        AssetReference{AssetKind::Prefab, "valid.rt2prefab", {}, {}, kInstance}});
    plan.affectedEntities.push_back(validRootUuid);
    plan.syncImpact = SyncImpact::Structural;
    scene.NotifyAuthoringChanged();
    plan.authoringRevision = scene.AuthoringRevision();
    plan.meshTableExtent = static_cast<std::uint32_t>(scene.GetMeshRegistryCount());
    REQUIRE(plan.IsEffective());
    const auto staged = StagePrefabPropagationResources(plan, doc,
        AssetResolutionContext{std::filesystem::temp_directory_path(), nullptr});
    REQUIRE(staged.IsOk());
    REQUIRE(staged.value.instances.size() == 2);
    CHECK(staged.value.instances.front().disposition ==
          PrefabPropagationInstanceDisposition::Quarantined);
    CHECK(staged.value.instances.back().disposition ==
          PrefabPropagationInstanceDisposition::Propagate);
    const auto badInstance = std::find_if(staged.value.instances.begin(),
        staged.value.instances.end(), [&](const auto& value) {
            return value.instanceId == kInstance;
        });
    REQUIRE(badInstance != staged.value.instances.end());
    REQUIRE_FALSE(badInstance->diagnostics.empty());
    CHECK(badInstance->diagnostics.front().instanceId == kInstance);
    REQUIRE(staged.value.componentOperations.size() == 1);
    CHECK(staged.value.componentOperations.front().EntityUuid() == validRootUuid);
    CHECK_FALSE(staged.value.resourceOwnership.empty());
}

TEST_CASE("Phase 8 W4 S4: history keeps immutable command ownership across undo and redo")
{
    SceneManager scene;
    PopulateScene(scene);
    auto& registry = scene.AuthoringDoc().ecs.registry;
    const auto source = PrefabSourceFingerprint{
        "assets/source.rt2prefab", kInstance, "history-digest"};
    PrefabPropagationPlan plan;
    plan.source = source;
    plan.documentGeneration = scene.DocumentGeneration();
    plan.resourceGeneration = scene.ResourceGeneration();
    plan.componentOperations.push_back(PrefabPropagationComponentDelta::Make<NameComponent>(
        kEntity, kTemplate, NameComponent{"old"}, NameComponent{"history"}));
    plan.memberSnapshots.push_back({kEntity, kInstance, kTemplate, {}});
    plan.instances.push_back({kInstance, kEntity,
        PrefabPropagationInstanceDisposition::Propagate, {kEntity}, {}});
    plan.affectedEntities = {kEntity};
    plan.syncImpact = SyncImpact::None;
    FinalizePlan(scene, plan);
    REQUIRE(plan.IsEffective());
    EditorCommandHistory history;
    auto command = std::make_unique<PrefabPropagationCommand>(plan,
        [source] { return Result<PrefabSourceFingerprint>::Ok(source); });
    REQUIRE(history.Execute(std::move(command), scene).success);
    CHECK(history.UndoDepthForTest() == 1);
    REQUIRE(history.Undo(scene).success);
    CHECK(history.RedoDepthForTest() == 1);
    CHECK(registry.get<NameComponent>(scene.FindEntityByUuid(kEntity)).name == "old");
    REQUIRE(history.Redo(scene).success);
    CHECK(history.UndoDepthForTest() == 1);
    CHECK(registry.get<NameComponent>(scene.FindEntityByUuid(kEntity)).name == "history");
}

TEST_CASE("Phase 8 W4 S4: commit rejects a plan without complete production evidence")
{
    SceneManager scene;
    PopulateScene(scene);
    const auto source = PrefabSourceFingerprint{
        "assets/source.rt2prefab", kInstance, "evidence-digest"};
    PrefabPropagationPlan plan;
    plan.source = source;
    plan.documentGeneration = scene.DocumentGeneration();
    plan.resourceGeneration = scene.ResourceGeneration();
    plan.componentOperations.push_back(PrefabPropagationComponentDelta::Make<NameComponent>(
        kEntity, kTemplate, NameComponent{"old"}, NameComponent{"new"}));
    plan.memberSnapshots.push_back({kEntity, kInstance, kTemplate, {}});
    plan.instances.push_back({kInstance, kEntity,
        PrefabPropagationInstanceDisposition::Propagate, {kEntity}, {}});
    plan.affectedEntities = {kEntity};
    plan.syncImpact = SyncImpact::None;
    FinalizePlan(scene, plan);
    plan.rootSnapshots.clear();
    CHECK(plan.IsValid());
    const auto result = PrefabPropagationCommand(plan,
        [source] { return Result<PrefabSourceFingerprint>::Ok(source); }).Execute(scene);
    CHECK_FALSE(result.success);
    CHECK(scene.AuthoringDoc().ecs.registry.get<NameComponent>(
        scene.FindEntityByUuid(kEntity)).name == "old");
}

TEST_CASE("Phase 8 W4 S4: forged owned and pre-existing MeshRef indices are rejected")
{
    SceneManager scene;
    PopulateScene(scene);
    const auto source = PrefabSourceFingerprint{
        "assets/source.rt2prefab", kInstance, "ownership-digest"};
    PrefabPropagationPlan plan;
    plan.source = source;
    plan.documentGeneration = scene.DocumentGeneration();
    plan.resourceGeneration = scene.ResourceGeneration();
    plan.componentOperations.push_back(PrefabPropagationComponentDelta::Make<NameComponent>(
        kEntity, kTemplate, NameComponent{"old"}, NameComponent{"new"}));
    plan.memberSnapshots.push_back({kEntity, kInstance, kTemplate, {}});
    plan.instances.push_back({kInstance, kEntity,
        PrefabPropagationInstanceDisposition::Propagate, {kEntity}, {}});
    plan.meshTableExtent = static_cast<std::uint32_t>(scene.GetMeshRegistryCount());
    plan.materialTableExtent = 0;
    plan.textureTableExtent = 0;
    plan.meshRefOperations.push_back({kEntity, kTemplate,
        MeshRef{0, -1}, MeshRef{99, -1}});
    plan.affectedEntities = {kEntity};
    plan.syncImpact = SyncImpact::Structural;
    FinalizePlan(scene, plan);
    CHECK_FALSE(plan.IsValid());
}

TEST_CASE("Phase 8 W4 S4: revision zero is captured and stale zero-to-one is rejected")
{
    SceneManager scene;
    PopulateScene(scene, false);
    REQUIRE(scene.AuthoringRevision() == 0);
    const auto source = PrefabSourceFingerprint{
        "assets/source.rt2prefab", kInstance, "revision-zero"};
    PrefabPropagationPlan plan;
    plan.source = source;
    plan.documentGeneration = scene.DocumentGeneration();
    plan.resourceGeneration = scene.ResourceGeneration();
    plan.componentOperations.push_back(PrefabPropagationComponentDelta::Make<NameComponent>(
        kEntity, kTemplate, NameComponent{"old"}, NameComponent{"zero"}));
    plan.memberSnapshots.push_back({kEntity, kInstance, kTemplate, {}});
    plan.instances.push_back({kInstance, kEntity,
        PrefabPropagationInstanceDisposition::Propagate, {kEntity}, {}});
    plan.affectedEntities = {kEntity};
    plan.syncImpact = SyncImpact::None;
    FinalizePlan(scene, plan);
    REQUIRE(plan.authoringRevision == 0);
    REQUIRE(plan.authoringRevisionCaptured);
    REQUIRE(plan.IsEffective());
    REQUIRE(PrefabPropagationCommand(plan, [source] {
        return Result<PrefabSourceFingerprint>::Ok(source);
    }).Execute(scene).success);
    CHECK(scene.AuthoringDoc().ecs.registry.get<NameComponent>(
        scene.FindEntityByUuid(kEntity)).name == "zero");

    SceneManager stale;
    PopulateScene(stale, false);
    auto stalePlan = plan;
    stalePlan.documentGeneration = stale.DocumentGeneration();
    stalePlan.resourceGeneration = stale.ResourceGeneration();
    stalePlan.rootSnapshots.clear();
    FinalizePlan(stale, stalePlan);
    stale.NotifyAuthoringChanged();
    const auto before = stale.AuthoringDoc().ecs.registry.get<NameComponent>(
        stale.FindEntityByUuid(kEntity)).name;
    const auto result = PrefabPropagationCommand(stalePlan, [source] {
        return Result<PrefabSourceFingerprint>::Ok(source);
    }).Execute(stale);
    CHECK_FALSE(result.success);
    CHECK(stale.AuthoringDoc().ecs.registry.get<NameComponent>(
        stale.FindEntityByUuid(kEntity)).name == before);
}

TEST_CASE("Phase 8 W4 S4: value-only propagation skips unchanged imported sibling in same instance")
{
    namespace fs = std::filesystem;
    SceneManager scene;
    PopulateScene(scene);
    auto& doc = scene.AuthoringDoc();
    const UUID siblingUuid = UUID::Parse("abababab-abab-4bab-8bab-abababababab");
    const auto sibling = doc.ecs.registry.create();
    REQUIRE(doc.AssignKnownUuid(sibling, siblingUuid));
    doc.ecs.registry.emplace<PrefabMemberComponent>(sibling,
        PrefabMemberComponent{kInstance,
            UUID::Parse("cdcdcdcd-cdcd-4dcd-8dcd-cdcdcdcdcdcd"), {}});
    AssetReference missing{AssetKind::Model, "same-instance-missing.obj", {},
                           "obj:whole-model", kInstance};
    doc.ecs.registry.emplace<ImportedMeshSourceComponent>(sibling,
        ImportedMeshSourceComponent{missing});
    scene.NotifyAuthoringChanged();

    PrefabPropagationPlan plan;
    plan.source = {"assets/source.rt2prefab", kInstance, "same-instance-value-only"};
    plan.documentGeneration = scene.DocumentGeneration();
    plan.resourceGeneration = scene.ResourceGeneration();
    plan.componentOperations.push_back(PrefabPropagationComponentDelta::Make<NameComponent>(
        kEntity, kTemplate, NameComponent{"old"}, NameComponent{"value-only"}));
    plan.memberSnapshots.push_back({kEntity, kInstance, kTemplate, {}});
    plan.memberSnapshots.push_back({siblingUuid, kInstance,
        UUID::Parse("cdcdcdcd-cdcd-4dcd-8dcd-cdcdcdcdcdcd"), {}});
    plan.instances.push_back({kInstance, kEntity,
        PrefabPropagationInstanceDisposition::Propagate, {kEntity}, {}});
    plan.affectedEntities = {kEntity};
    plan.syncImpact = SyncImpact::None;
    FinalizePlan(scene, plan);
    REQUIRE(plan.IsEffective());
    const auto staged = StagePrefabPropagationResources(plan, doc, AssetResolutionContext{});
    REQUIRE(staged.IsOk());
    CHECK(staged.value.resourceOwnership.empty());
    CHECK(staged.value.meshRefOperations.empty());
    CHECK(staged.value.syncImpact == SyncImpact::None);
    REQUIRE(staged.value.componentOperations.size() == 1);
    CHECK(staged.value.componentOperations.front().EntityUuid() == kEntity);
}

TEST_CASE("Phase 8 W4 S4: malformed quarantined root does not poison valid sibling")
{
    SceneManager scene;
    PopulateScene(scene);
    const UUID malformedInstance = UUID::Parse("dededede-dede-4ede-8ede-dededededede");
    const UUID validInstance = UUID::Parse("efefefef-efef-4fef-8fef-efefefefefef");
    const UUID validRoot = UUID::Parse("f0f0f0f0-f0f0-40f0-80f0-f0f0f0f0f0f0");
    auto& doc = scene.AuthoringDoc();
    const auto root = doc.ecs.registry.create();
    REQUIRE(doc.AssignKnownUuid(root, validRoot));
    doc.ecs.registry.emplace<PrefabMemberComponent>(root,
        PrefabMemberComponent{validInstance, kTemplate, {}});
    doc.ecs.registry.emplace<PrefabInstanceComponent>(root,
        PrefabInstanceComponent{AssetReference{AssetKind::Prefab,
            "valid.rt2prefab", {}, {}, validInstance}, validInstance});
    scene.NotifyAuthoringChanged();

    PrefabPropagationPlan plan;
    plan.source = {"assets/source.rt2prefab", kInstance, "malformed-root"};
    plan.documentGeneration = scene.DocumentGeneration();
    plan.resourceGeneration = scene.ResourceGeneration();
    plan.componentOperations.push_back(PrefabPropagationComponentDelta::Make<NameComponent>(
        validRoot, kTemplate, std::nullopt, NameComponent{"valid"}));
    plan.memberSnapshots.push_back({validRoot, validInstance, kTemplate, {}});
    plan.instances.push_back({malformedInstance, UUID::Nil(),
        PrefabPropagationInstanceDisposition::Quarantined, {},
        {PrefabPropagationDiagnostic{AssetDiagnostic::Malformed,
            plan.source.normalizedPath, plan.source.assetId, malformedInstance,
            UUID::Nil(), UUID::Nil(), "malformed root"}}});
    plan.instances.push_back({validInstance, validRoot,
        PrefabPropagationInstanceDisposition::Propagate, {validRoot}, {}});
    plan.rootSnapshots.push_back({validRoot, validInstance,
        AssetReference{AssetKind::Prefab, "valid.rt2prefab", {}, {}, validInstance}});
    plan.affectedEntities = {validRoot};
    plan.syncImpact = SyncImpact::None;
    FinalizePlan(scene, plan);
    REQUIRE(plan.IsEffective());
    const auto staged = StagePrefabPropagationResources(plan, doc, AssetResolutionContext{});
    REQUIRE(staged.IsOk());
    REQUIRE(staged.value.instances.size() == 2);
    CHECK(staged.value.instances.front().disposition ==
          PrefabPropagationInstanceDisposition::Quarantined);
    CHECK(staged.value.instances.back().disposition ==
          PrefabPropagationInstanceDisposition::Propagate);
    REQUIRE(staged.value.componentOperations.size() == 1);
    CHECK(staged.value.componentOperations.front().EntityUuid() == validRoot);
}

TEST_CASE("Phase 8 W4 S4: nested owned resource references and overflow are rejected")
{
    SceneManager scene;
    PopulateScene(scene);
    PrefabPropagationPlan materialPlan;
    materialPlan.source = {"assets/source.rt2prefab", kInstance, "nested-material"};
    materialPlan.documentGeneration = scene.DocumentGeneration();
    materialPlan.resourceGeneration = scene.ResourceGeneration();
    materialPlan.componentOperations.push_back(PrefabPropagationComponentDelta::Make<NameComponent>(
        kEntity, kTemplate, NameComponent{"old"}, NameComponent{"nested"}));
    materialPlan.memberSnapshots.push_back({kEntity, kInstance, kTemplate, {}});
    materialPlan.instances.push_back({kInstance, kEntity,
        PrefabPropagationInstanceDisposition::Propagate, {kEntity}, {}});
    materialPlan.affectedEntities = {kEntity};
    materialPlan.syncImpact = SyncImpact::Material;
    SceneMaterial invalidMaterial;
    invalidMaterial.baseColorTextureIndex = 99;
    PrefabPropagationResourceOwnership ownership;
    ownership.rebase.kind = PrefabPropagationResourceKind::Material;
    ownership.rebase.sourceBeforeExtent = 0;
    ownership.rebase.sceneBeforeExtent = 0;
    ownership.rebase.sceneAppendBase = 0;
    ownership.rebase.sceneAfterExtent = 1;
    ownership.rebase.sourceSlots = {{0}};
    ownership.rebase.sceneSlots = {{0}};
    ownership.rebase.owned = PrefabPropagationResourceBlock::FromDecoded(
        PrefabPropagationResourceKind::Material,
        {PrefabPropagationResourcePayload{"material:nested", "digest", invalidMaterial}});
    materialPlan.resourceOwnership.push_back(ownership);
    FinalizePlan(scene, materialPlan);
    CHECK_FALSE(materialPlan.IsValid());

    PrefabPropagationPlan invalidMesh = materialPlan;
    invalidMesh.resourceOwnership.clear();
    invalidMesh.syncImpact = SyncImpact::Structural;
    MeshData mesh = Triangle("overflow");
    mesh.materialIndices = {std::numeric_limits<std::uint32_t>::max()};
    PrefabPropagationResourceOwnership meshOwnership;
    meshOwnership.rebase.kind = PrefabPropagationResourceKind::Mesh;
    meshOwnership.rebase.sourceBeforeExtent = 1;
    meshOwnership.rebase.sceneBeforeExtent = 1;
    meshOwnership.rebase.sceneAppendBase = 1;
    meshOwnership.rebase.sceneAfterExtent = 2;
    meshOwnership.rebase.sourceSlots = {{0}};
    meshOwnership.rebase.sceneSlots = {{1}};
    meshOwnership.rebase.owned = PrefabPropagationResourceBlock::FromDecoded(
        PrefabPropagationResourceKind::Mesh,
        {PrefabPropagationResourcePayload{"mesh:overflow", "digest", mesh}});
    invalidMesh.resourceOwnership.push_back(meshOwnership);
    CHECK_FALSE(invalidMesh.IsValid());
}

TEST_CASE("Phase 8 W4 S4: staging rejects checked extent overflow before mutation")
{
    SceneManager scene;
    PopulateScene(scene);
    auto& doc = scene.AuthoringDoc();
    PrefabPropagationPlan plan;
    plan.source = {"assets/source.rt2prefab", kInstance, "extent-overflow"};
    plan.documentGeneration = scene.DocumentGeneration();
    plan.resourceGeneration = scene.ResourceGeneration();
    plan.componentOperations.push_back(PrefabPropagationComponentDelta::Make<PrimitiveComponent>(
        kEntity, kTemplate,
        PrimitiveComponent{PrimitiveComponent::Cube, 1.0f, 24, 16},
        PrimitiveComponent{PrimitiveComponent::Sphere, 2.0f, 16, 8}));
    plan.memberSnapshots.push_back({kEntity, kInstance, kTemplate, {}});
    plan.instances.push_back({kInstance, kEntity,
        PrefabPropagationInstanceDisposition::Propagate, {kEntity}, {}});
    plan.affectedEntities = {kEntity};
    plan.syncImpact = SyncImpact::Structural;
    FinalizePlan(scene, plan);
    plan.meshTableExtent = std::numeric_limits<std::uint32_t>::max();
    REQUIRE(plan.IsEffective());
    const auto staged = StagePrefabPropagationResources(plan, doc, AssetResolutionContext{});
    CHECK_FALSE(staged.IsOk());
    CHECK(doc.ecs.registry.get<NameComponent>(doc.FindByUuid(kEntity)).name == "old");
}
