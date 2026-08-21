#include <doctest/doctest.h>

#include "PrefabPropagationCommand.h"
#include "PrefabPropagationService.h"
#include "SceneManager.h"
#include "SceneSerializer.h"

#include <fstream>
#include <filesystem>
#include <chrono>

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
    ~CheckedTempDir()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void PopulateScene(SceneManager& scene)
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
    const auto mesh = doc.ecs.meshRegistry.AddMesh(Triangle("before"));
    doc.ecs.registry.emplace<MeshRef>(entity, MeshRef{mesh, -1});
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
    plan.componentOperations.push_back({
        kEntity, kTemplate, PrefabComponentKeyFor<NameComponent>::value,
        PrefabPropagationComponentValue{NameComponent{"old"}},
        PrefabPropagationComponentValue{NameComponent{"new"}}});
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
    REQUIRE(plan.IsEffective());

    PrefabPropagationCommand command(plan, [source] { return source; });
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
    CHECK(ecs.meshRegistry.GetCount() == 1);
    CHECK(scene.ResourceGeneration() == beforeResourceGeneration);

    const auto redo = command.Execute(scene);
    REQUIRE(redo.success);
    CHECK(scene.AuthoringDoc().ecs.registry.get<MeshRef>(
        scene.FindEntityByUuid(kEntity)).meshIndex == 1);
    CHECK(ecs.meshRegistry.GetMesh(1).name == "after");
    CHECK(ecs.meshRegistry.GetCount() == 2);
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
    plan.componentOperations.push_back({
        kEntity, kTemplate, PrefabComponentKeyFor<NameComponent>::value,
        PrefabPropagationComponentValue{NameComponent{"old"}},
        PrefabPropagationComponentValue{NameComponent{"new"}}});
    plan.memberSnapshots.push_back({kEntity, kInstance, kTemplate, {}});
    plan.affectedEntities = {kEntity};
    plan.syncImpact = SyncImpact::None;
    REQUIRE(plan.IsEffective());
    doc.ecs.registry.get<NameComponent>(doc.FindByUuid(kEntity)).name = "changed";
    const auto beforeRevision = scene.AuthoringRevision();
    PrefabPropagationCommand command(plan);
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
    fingerprintPlan.componentOperations.push_back({
        kEntity, kTemplate, PrefabComponentKeyFor<NameComponent>::value,
        PrefabPropagationComponentValue{NameComponent{"old"}},
        PrefabPropagationComponentValue{NameComponent{"new"}}});
    fingerprintPlan.memberSnapshots.push_back({kEntity, kInstance, kTemplate, {}});
    fingerprintPlan.affectedEntities = {kEntity};
    fingerprintPlan.syncImpact = SyncImpact::None;
    REQUIRE(fingerprintPlan.IsEffective());
    const auto fingerprintResult = PrefabPropagationCommand(fingerprintPlan, [] {
        return PrefabSourceFingerprint{"assets/source.rt2prefab", kInstance, "digest-b"};
    }).Execute(fingerprintScene);
    CHECK_FALSE(fingerprintResult.success);
    CHECK(fingerprintScene.AuthoringDoc().ecs.registry.get<NameComponent>(
        fingerprintScene.FindEntityByUuid(kEntity)).name == "old");
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
        plan.componentOperations.push_back({kEntity, kTemplate,
            PrefabComponentKeyFor<NameComponent>::value,
            PrefabPropagationComponentValue{NameComponent{"old"}},
            PrefabPropagationComponentValue{NameComponent{"new"}}});
        plan.memberSnapshots.push_back({kEntity, kInstance, kTemplate, {}});
        plan.affectedEntities = {kEntity};
        plan.syncImpact = SyncImpact::None;
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
        const auto result = PrefabPropagationCommand(plan).Execute(scene);
        CHECK_FALSE(result.success);
    }
    {
        SceneManager scene; PopulateScene(scene);
        auto plan = makePlan(scene);
        scene.AuthoringDoc().ecs.meshRegistry.AddMesh(Triangle("orphan"));
        REQUIRE(scene.CompactMeshRegistry());
        const auto result = PrefabPropagationCommand(plan).Execute(scene);
        CHECK_FALSE(result.success);
    }
    {
        SceneManager scene; PopulateScene(scene);
        auto plan = makePlan(scene);
        scene.AuthoringDoc().ecs.registry.get<PrefabMemberComponent>(
            scene.FindEntityByUuid(kEntity)).instanceId = UUID::Parse(
                "44444444-4444-4444-8444-444444444444");
        CHECK_FALSE(PrefabPropagationCommand(plan).Execute(scene).success);
    }
    {
        SceneManager scene; PopulateScene(scene);
        auto plan = makePlan(scene);
        scene.AuthoringDoc().ecs.registry.get<PrefabMemberComponent>(
            scene.FindEntityByUuid(kEntity)).overrides.push_back(
                PrefabComponentKeyFor<NameComponent>::value);
        CHECK_FALSE(PrefabPropagationCommand(plan).Execute(scene).success);
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
        CHECK_FALSE(PrefabPropagationCommand(plan).Execute(scene).success);
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
    CHECK(scene.GetMeshRegistryCount() == beforeCount);
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
    durable.componentOperations.push_back({
        kEntity, kTemplate, PrefabComponentKeyFor<PrimitiveComponent>::value,
        PrefabPropagationComponentValue{PrimitiveComponent{PrimitiveComponent::Cube, 1.0f, 24, 16}},
        PrefabPropagationComponentValue{PrimitiveComponent{PrimitiveComponent::Sphere, 2.0f, 16, 8}}});
    durable.memberSnapshots.push_back({kEntity, kInstance, kTemplate, {}});
    durable.affectedEntities = {kEntity};
    durable.syncImpact = SyncImpact::Structural;
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
        std::ofstream mtl(dir / "model.mtl");
        REQUIRE(mtl.good());
        mtl << "newmtl source\nKd 0.2 0.4 0.6\n";
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
    const auto beforeSource = PrefabPropagationComponentValue{
        ImportedMeshSourceComponent{oldSource}};
    const auto afterSource = PrefabPropagationComponentValue{
        ImportedMeshSourceComponent{newSource}};
    PrefabPropagationPlan durable;
    durable.source = {dir / "model.rt2prefab", kInstance, "digest-import"};
    durable.documentGeneration = scene.DocumentGeneration();
    durable.resourceGeneration = scene.ResourceGeneration();
    durable.authoringRevision = scene.AuthoringRevision();
    durable.componentOperations.push_back({kEntity, kTemplate,
        PrefabComponentKeyFor<ImportedMeshSourceComponent>::value,
        beforeSource, afterSource});
    durable.componentOperations.push_back({kEntity, kTemplate,
        PrefabComponentKeyFor<MaterialOverrideComponent>::value, std::nullopt,
        PrefabPropagationComponentValue{authored}});
    durable.memberSnapshots.push_back({kEntity, kInstance, kTemplate, {}});
    durable.affectedEntities = {kEntity};
    durable.syncImpact = SyncImpact::Structural;
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
}
