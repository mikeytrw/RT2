#include <doctest/doctest.h>

#include "AssetIdentity.h"
#include "EditorCommandHistory.h"
#include "EditorPropertyCommands.h"
#include "Phase1AFixtureGenerator.h"
#include "PrefabPropagationCommand.h"
#include "PrefabPropagationDiscovery.h"
#include "PrefabPropagationService.h"
#include "PrefabPropagationLive.h"
#include "SceneAssetResolver.h"
#include "SceneManager.h"
#include "SceneSerializer.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace rt2::core;

namespace
{
ExecutablePropagationPlan AsExecutable(
    const SceneDocument& document,
    DiscoveredPropagationPlan* plan)
{
    REQUIRE(plan != nullptr);
    DiscoveredPropagationPlan& stagePlan = *plan;
    stagePlan.syncImpact = stagePlan.DerivedSyncImpact();
    stagePlan.affectedEntities = stagePlan.DerivedAffectedEntities();
    auto staged = StagePrefabPropagationResources(stagePlan, document, {});
    REQUIRE(staged.IsOk());
    REQUIRE(staged.value.Executable() != nullptr);
    return std::move(*staged.value.Executable());
}
struct TempGuard
{
    std::filesystem::path path;
    TempGuard() = default;
    TempGuard(const TempGuard&) = delete;
    TempGuard& operator=(const TempGuard&) = delete;
    TempGuard(TempGuard&& other) noexcept : path(std::move(other.path))
    { other.path.clear(); }
    TempGuard& operator=(TempGuard&& other) noexcept
    {
        if (this == &other) return *this;
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        path = std::move(other.path);
        other.path.clear();
        return *this;
    }
    ~TempGuard()
    {
        if (path.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        if (ec || std::filesystem::exists(path, ec)) std::abort();
    }
};

TempGuard Temp()
{
    static std::atomic<unsigned> sequence{0};
    TempGuard result;
    result.path = std::filesystem::temp_directory_path() /
        ("rt2_w4_s7_acceptance_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         "_" + std::to_string(sequence.fetch_add(1)));
    std::error_code ec;
    if (!std::filesystem::create_directories(result.path, ec) || ec)
        throw std::runtime_error("S7 temporary directory creation failed");
    return result;
}

bool HasKey(const DiscoveredPropagationPlan& plan, std::string_view wire)
{
    for (const auto& operation : plan.componentOperations)
        if (operation.Key().wire() == wire) return true;
    return false;
}

const UUID kA10Entity = UUID::Parse("11111111-1111-4111-8111-111111111111");
const UUID kA10Instance = UUID::Parse("22222222-2222-4222-8222-222222222222");
const UUID kA10Template = UUID::Parse("33333333-3333-4333-8333-333333333333");

MeshData A10Triangle(const char* name)
{
    MeshData mesh;
    mesh.name = name;
    mesh.vertices = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                     0.0f, 1.0f, 0.0f};
    mesh.indices = {0, 1, 2};
    return mesh;
}

std::string FileBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void PopulateA10(SceneManager& scene)
{
    auto& document = scene.AuthoringDoc();
    const auto entity = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(entity, kA10Entity));
    document.ecs.registry.emplace<NameComponent>(entity, NameComponent{"before"});
    document.ecs.registry.emplace<Transform>(entity, Transform{});
    document.ecs.registry.emplace<VisibleComponent>(entity, VisibleComponent{true});
    document.ecs.registry.emplace<PrefabMemberComponent>(
        entity, PrefabMemberComponent{kA10Instance, kA10Template, {}});
    document.ecs.registry.emplace<PrefabInstanceComponent>(entity,
        PrefabInstanceComponent{AssetReference{AssetKind::Prefab,
            "assets/source.rt2prefab", {}, {}, kA10Instance}, kA10Instance});
    document.ecs.registry.emplace<PrimitiveComponent>(
        entity, PrimitiveComponent{PrimitiveComponent::Cube, 1.0f, 24, 16});
    document.ecs.meshRegistry.AddMesh(A10Triangle("before"));
    document.ecs.registry.emplace<MeshRef>(entity, MeshRef{0, -1});
    scene.NotifyAuthoringChanged();
}

UUID A13Uuid(unsigned value)
{
    char text[48]{};
    std::snprintf(text, sizeof(text), "00000000-0000-4000-8000-%012u", value);
    return UUID::Parse(text);
}

PrefabEntityRecord A13Record(unsigned templateId, unsigned recordId,
                             const UUID& parent = UUID::Nil())
{
    PrefabEntityRecord result;
    result.templateId = A13Uuid(templateId);
    result.record.uuid = A13Uuid(recordId);
    result.record.parentUuid = parent;
    result.record.name = templateId == 1 ? "Root" : "Mesh";
    return result;
}

std::filesystem::path WriteA13Prefab(const TempGuard& temp,
                                     std::vector<PrefabEntityRecord> records)
{
    const auto path = temp.path / "a13.rt2prefab";
    PrefabDocument prefab;
    prefab.entities = std::move(records);
    Error error;
    REQUIRE(PrefabSerializer::Save(prefab, path, error));
    REQUIRE(WriteSidecarId(AssetSidecarPath(path), A13Uuid(1300), error));
    return path;
}

PrefabPropagationDiscoveryRequest A13Request(SceneDocument& document,
                                             const std::filesystem::path& source)
{
    PrefabPropagationDiscoveryRequest request;
    request.document = &document;
    request.assets.assetRoot = source.parent_path();
    request.changedSource = AssetReference{AssetKind::Prefab, source.string(),
                                           {}, {}, A13Uuid(1300)};
    request.documentGeneration = 11;
    request.resourceGeneration = 22;
    request.authoringRevision = 33;
    const auto captured = CapturePrefabSource(request.changedSource,
                                              request.assets);
    if (captured.IsOk()) request.capturedSource = captured.value;
    return request;
}

SceneDocument A13Document(const std::filesystem::path& source,
                          bool imported, unsigned instance)
{
    SceneDocument document;
    document.metadata.schemaVersion = SceneSerializer::SchemaVersion;
    const auto root = document.ecs.registry.create();
    const auto rootUuid = A13Uuid(1400 + instance * 10);
    const auto childUuid = A13Uuid(1401 + instance * 10);
    const auto instanceId = A13Uuid(1500 + instance);
    REQUIRE(document.AssignKnownUuid(root, rootUuid));
    document.ecs.registry.emplace<Hierarchy>(root);
    document.ecs.registry.emplace<PrefabInstanceComponent>(root,
        PrefabInstanceComponent{AssetReference{AssetKind::Prefab,
            source.filename().string(), {}, {}, A13Uuid(1300)}, instanceId});
    document.ecs.registry.emplace<PrefabMemberComponent>(root,
        PrefabMemberComponent{instanceId, A13Uuid(1), {}});
    const auto child = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(child, childUuid));
    document.ecs.registry.emplace<Hierarchy>(child, root,
                                              std::vector<entt::entity>{});
    document.ecs.registry.get<Hierarchy>(root).children.push_back(child);
    document.ecs.registry.emplace<PrefabMemberComponent>(child,
        PrefabMemberComponent{instanceId, A13Uuid(2), {}});
    if (imported)
    {
        document.ecs.registry.emplace<ImportedMeshSourceComponent>(child);
        document.ecs.registry.get<ImportedMeshSourceComponent>(child).model =
            AssetReference{AssetKind::Model, "old.glb", {}, "gltf:mesh=0", A13Uuid(1700)};
    }
    else
    {
        document.ecs.registry.emplace<PrimitiveComponent>(child,
            PrimitiveComponent{PrimitiveComponent::Cube, 2.0f, 4, 3});
    }
    return document;
}

const UUID kA11Entity = UUID::Parse("00000000-0000-4000-8000-000000000a11");
const UUID kA11Instance = UUID::Parse("00000000-0000-4000-8000-000000000a12");
const UUID kA11Template = UUID::Parse("00000000-0000-4000-8000-000000000a13");

AssetReference A11Source()
{
    return AssetReference{AssetKind::Prefab, "source.rt2prefab", {}, {}, kA11Instance};
}

void PopulateA11Document(SceneDocument& document)
{
    document.metadata.schemaVersion = SceneSerializer::SchemaVersion;
    const auto entity = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(entity, kA11Entity));
    document.ecs.registry.emplace<NameComponent>(entity, NameComponent{"a11"});
    document.ecs.registry.emplace<Transform>(entity, Transform{});
    document.ecs.registry.emplace<PrefabMemberComponent>(entity,
        PrefabMemberComponent{kA11Instance, kA11Template, {}});
    document.ecs.registry.emplace<PrefabInstanceComponent>(entity,
        PrefabInstanceComponent{A11Source(), kA11Instance});
}

DiscoveredPropagationPlan A11Plan(const SceneDocument& document,
                              const std::filesystem::path& path,
                              const std::string& digest)
{
    const auto entity = document.FindByUuid(kA11Entity);
    const auto before = document.ecs.registry.get<Transform>(entity);
    auto after = before;
    after.translation.x += 4.0f;
    DiscoveredPropagationPlan plan;
    plan.source = PrefabSourceFingerprint{path, kA11Instance, digest};
    plan.documentGeneration = 1;
    plan.resourceGeneration = 2;
    plan.authoringRevision = 3;
    plan.authoringRevisionCaptured = true;
    plan.sourceSchemaVersion = PrefabSerializer::FormatVersion;
    plan.resourceEvidenceCaptured = true;
    plan.componentOperations.push_back(PrefabPropagationComponentDelta::Make<Transform>(
        kA11Entity, kA11Template, before, after));
    plan.memberSnapshots.push_back({kA11Entity, kA11Instance, kA11Template, {}});
    plan.rootSnapshots.push_back({kA11Entity, kA11Instance, A11Source()});
    plan.instances.push_back({kA11Instance, kA11Entity,
        PrefabPropagationInstanceDisposition::Propagate, {kA11Entity}, {}});
    plan.affectedEntities = {kA11Entity};
    plan.syncImpact = SyncImpact::Transform;
    return plan;
}

bool SameTransform(const Transform& a, const Transform& b)
{
    return a.translation == b.translation && a.rotation == b.rotation &&
           a.scale == b.scale;
}

} // namespace

TEST_CASE("Phase 8 W4 S7 A1: source edit propagates through prepare stage command and reload")
{
    const auto temp = Temp();
    DeterministicUuidProvider ids;
    SceneManager manager;
    manager.SetUuidProvider(&ids);

    Error error;
    const auto model = temp.path / "source.glb";
    INFO(error.Format());
    REQUIRE(GenerateTinyTexturedGlb(model, error));
    REQUIRE(error.IsOk());
    REQUIRE(manager.LoadScene(model.string()));

    auto& registry = manager.GetECS().registry;
    UUID importedUuid;
    for (const auto entity : registry.view<ImportedMeshSourceComponent>())
    {
        importedUuid = registry.get<EntityIdComponent>(entity).id;
        break;
    }
    REQUIRE_FALSE(importedUuid.IsNull());
    const UUID fixtureRoot = manager.CreateEmpty("Fixture").affectedEntities.front();
    REQUIRE(manager.Reparent({importedUuid}, fixtureRoot).success);
    const auto prefabPath = temp.path / "source.rt2prefab";
    REQUIRE(manager.CreatePrefabFromSubtree({fixtureRoot}, prefabPath).ok);
    const auto canonical = manager.CountCanonicalPrefabEntities(prefabPath);
    REQUIRE(canonical.IsOk());
    REQUIRE(canonical.value == 2);

    SceneMaterial localTint;
    localTint.baseColor = {0.91f, 0.12f, 0.23f};
    localTint.roughness = 0.19f;
    const int localTintSlot = manager.AddMaterial(localTint);

    std::vector<UUID> meshMembers;
    std::vector<UUID> rootMembers;
    for (int i = 0; i < 3; ++i)
    {
        std::vector<AssetDiagnostic> diagnostics;
        const auto uuids = manager.ReserveKnownUuids(canonical.value);
        const auto instance = manager.InstantiatePrefabWithUuids(prefabPath, uuids,
                                                                  diagnostics);
        REQUIRE(instance.mutation.success);
        UUID meshMember;
        for (const auto& uuid : uuids)
        {
            const auto entity = manager.FindEntityByUuid(uuid);
            REQUIRE(static_cast<std::uint32_t>(entity) !=
                    static_cast<std::uint32_t>(entt::null));
            if (registry.all_of<MeshRef>(entity) &&
                registry.all_of<ImportedMeshSourceComponent>(entity))
                meshMember = uuid;
        }
        REQUIRE_FALSE(meshMember.IsNull());
        meshMembers.push_back(meshMember);
        rootMembers.push_back(uuids.front());
    }

    // Preserve one genuine local marker before the source edit.
    const UUID localUuid = meshMembers[1];
    const auto localEntity = manager.FindEntityByUuid(localUuid);
    const int localBefore = registry.get<MeshRef>(localEntity).materialIndex;
    const auto localBeforeOverride = manager.GetMaterialOverride(localUuid);
    const auto localStage = manager.StageMaterialIndex(localUuid, localTintSlot);
    REQUIRE(localStage.IsOk());
    REQUIRE(localStage.value.override.has_value());
    auto localCommand = MakeSetMaterialIndexCommandIfEffective(
        localUuid, localBefore, localTintSlot, localBeforeOverride,
        localStage.value.override);
    REQUIRE(localCommand);
    EditorCommandHistory history;
    REQUIRE(history.Execute(std::move(localCommand), manager).effective);

    // Change the durable prefab source after all three instances exist. This is
    // the W4 chain missing from the W3 A1 test: discovery must now plan the
    // source edit, staging must resolve resources, and the command must apply
    // it while retaining the local material marker above.
    PrefabDocument sourceDocument;
    REQUIRE(PrefabSerializer::Load(sourceDocument, prefabPath, error));
    auto imported = std::find_if(sourceDocument.entities.begin(),
        sourceDocument.entities.end(), [](const auto& entity) {
            return entity.record.hasImportedSource;
        });
    REQUIRE(imported != sourceDocument.entities.end());
    imported->record.translation.x += 2.5f;
    imported->record.visible = false;
    imported->record.hasMaterialOverride = true;
    imported->record.materialOverride.authored = true;
    imported->record.materialOverride.material.baseColor = {0.17f, 0.61f, 0.83f};
    imported->record.materialOverride.material.roughness = 0.41f;
    const auto expectedTranslation = imported->record.translation;
    const auto expectedSourceMaterial = imported->record.materialOverride.material;
    REQUIRE(PrefabSerializer::Save(sourceDocument, prefabPath, error));

    const auto rootEntity = manager.FindEntityByUuid(rootMembers.front());
    REQUIRE(registry.all_of<PrefabInstanceComponent>(rootEntity));
    const auto sourceReference = registry.get<PrefabInstanceComponent>(rootEntity).prefab;
    PrefabPropagationDiscoveryRequest request;
    request.document = &manager.AuthoringDoc();
    request.assets.assetRoot = temp.path;
    request.changedSource = AssetReference{AssetKind::Prefab, prefabPath.string(),
                                           {}, {}, sourceReference.assetId};
    request.documentGeneration = manager.DocumentGeneration();
    request.resourceGeneration = manager.ResourceGeneration();
    request.authoringRevision = manager.AuthoringRevision();
    const auto captured = CapturePrefabSource(request.changedSource, request.assets);
    REQUIRE(captured.IsOk());
    request.capturedSource = captured.value;
    const auto prepared = PreparePrefabPropagation(request);
    REQUIRE(prepared.IsOk());
    REQUIRE(prepared.value.instances.size() == 3);
    CHECK(prepared.value.componentOperations.size() >= 3);
    CHECK(HasKey(prepared.value, PrefabWireKeys::kTransform));
    CHECK(HasKey(prepared.value, PrefabWireKeys::kVisible));
    CHECK(HasKey(prepared.value, PrefabWireKeys::kMaterialOverride));
    CHECK(prepared.value.source.assetId == sourceReference.assetId);

    const auto beforeMeshExtent = manager.GetMeshRegistryCount();
    const auto beforeMaterialExtent = manager.GetMaterialCount();
    const auto beforeTextureExtent = manager.GetECS().textures.size();
    const auto staged = StagePrefabPropagationResources(
        prepared.value, manager.AuthoringDoc(), AssetResolutionContext{temp.path, nullptr});
    REQUIRE(staged.IsOk());
    REQUIRE(staged.value.Executable() != nullptr);
    REQUIRE_FALSE(staged.value.Executable()->resourceOwnership().empty());
    std::size_t ownedMeshes = 0;
    std::size_t ownedMaterials = 0;
    std::size_t ownedTextures = 0;
    for (const auto& ownership : staged.value.Executable()->resourceOwnership())
    {
        const auto count = ownership.rebase.owned.Entries().size();
        if (ownership.rebase.kind == PrefabPropagationResourceKind::Mesh)
            ownedMeshes += count;
        else if (ownership.rebase.kind == PrefabPropagationResourceKind::Material)
            ownedMaterials += count;
        else
            ownedTextures += count;
        CHECK(ownership.rebase.sceneBeforeExtent ==
              (ownership.rebase.kind == PrefabPropagationResourceKind::Mesh
                   ? beforeMeshExtent
               : ownership.rebase.kind == PrefabPropagationResourceKind::Material
                   ? beforeMaterialExtent : beforeTextureExtent));
        CHECK(ownership.rebase.sceneAfterExtent ==
              ownership.rebase.sceneBeforeExtent + count);
        REQUIRE(ownership.rebase.sceneSlots.size() == count);
        for (std::size_t i = 0; i < count; ++i)
            CHECK(ownership.rebase.sceneSlots[i].value ==
                  ownership.rebase.sceneBeforeExtent + i);
    }
    CHECK(staged.value.Summary().meshTableExtent == beforeMeshExtent);
    CHECK(staged.value.Summary().materialTableExtent == beforeMaterialExtent);
    CHECK(staged.value.Summary().textureTableExtent == beforeTextureExtent);
    const auto fingerprint = staged.value.Summary().source;
    auto propagation = std::make_unique<PrefabPropagationCommand>(
        *staged.value.Executable(), [fingerprint] {
            return Result<PrefabSourceFingerprint>::Ok(fingerprint);
        });
    const auto beforeRevision = manager.AuthoringRevision();
    const auto beforeResources = manager.ResourceGeneration();
    const auto applied = history.Execute(std::move(propagation), manager);
    REQUIRE(applied.success);
    REQUIRE(applied.effective);
    CHECK(manager.AuthoringRevision() > beforeRevision);
    CHECK(manager.ResourceGeneration() > beforeResources);
    CHECK(manager.GetMeshRegistryCount() == beforeMeshExtent + ownedMeshes);
    CHECK(manager.GetMaterialCount() == beforeMaterialExtent + ownedMaterials);
    CHECK(manager.GetECS().textures.size() == beforeTextureExtent + ownedTextures);

    const auto scenePath = temp.path / "after.rt2scene";
    std::vector<AssetDiagnostic> saveDiagnostics;
    REQUIRE(SceneSerializer::Save(manager.AuthoringDoc(), scenePath,
                                  saveDiagnostics, error));
    SceneDocument loaded;
    DeterministicUuidProvider loadIds;
    loaded.SetUuidProvider(&loadIds);
    REQUIRE(SceneSerializer::Load(loaded, scenePath, error));
    std::vector<AssetDiagnostic> resolveDiagnostics;
    REQUIRE(SceneAssetResolver::ResolveAll(
        loaded, AssetResolutionContext{temp.path, nullptr}, resolveDiagnostics, error));
    CHECK(loaded.ecs.meshRegistry.GetCount() == 1);
    CHECK(loaded.ecs.materials.size() == 13);
    CHECK(loaded.ecs.textures.size() == 1);

    for (std::size_t i = 0; i < meshMembers.size(); ++i)
    {
        const auto entity = loaded.FindByUuid(meshMembers[i]);
        REQUIRE(static_cast<std::uint32_t>(entity) !=
                static_cast<std::uint32_t>(entt::null));
        REQUIRE(loaded.ecs.registry.all_of<Transform>(entity));
        REQUIRE(loaded.ecs.registry.all_of<VisibleComponent>(entity));
        CHECK(loaded.ecs.registry.get<Transform>(entity).translation == expectedTranslation);
        CHECK_FALSE(loaded.ecs.registry.get<VisibleComponent>(entity).visible);
        REQUIRE(loaded.ecs.registry.all_of<MeshRef>(entity));
        const auto& ref = loaded.ecs.registry.get<MeshRef>(entity);
        CHECK(ref.meshIndex == 0);
        CHECK(ref.materialIndex == 12 - static_cast<int>(i));
        REQUIRE(ref.materialIndex >= 0);
        REQUIRE(static_cast<std::size_t>(ref.materialIndex) < loaded.ecs.materials.size());
        REQUIRE(loaded.ecs.registry.all_of<PrefabMemberComponent>(entity));
        const auto& member = loaded.ecs.registry.get<PrefabMemberComponent>(entity);
        if (i == 1)
        {
            CHECK(std::find_if(member.overrides.begin(), member.overrides.end(),
                [](const auto& key) { return key.wire() == "materialOverride"; }) !=
                member.overrides.end());
            REQUIRE(loaded.ecs.registry.all_of<MaterialOverrideComponent>(entity));
            const auto& material =
                loaded.ecs.registry.get<MaterialOverrideComponent>(entity).material;
            CHECK(material.baseColor == localTint.baseColor);
            CHECK(material.roughness == doctest::Approx(localTint.roughness));
        }
        else
        {
            CHECK(loaded.ecs.materials[ref.materialIndex].baseColor ==
                  expectedSourceMaterial.baseColor);
            CHECK(loaded.ecs.materials[ref.materialIndex].roughness ==
                  doctest::Approx(expectedSourceMaterial.roughness));
        }
    }

    // A5 continuation in the same literal chain: rebind the imported source,
    // retain the marked local material, and quarantine one malformed instance
    // without erasing its valid siblings.
    const auto replacementModel = temp.path / "replacement.glb";
    Error replacementError;
    REQUIRE(GenerateTinyTexturedGlb(replacementModel, replacementError));
    REQUIRE(replacementError.IsOk());
    PrefabDocument reboundSource;
    REQUIRE(PrefabSerializer::Load(reboundSource, prefabPath, replacementError));
    auto reboundImported = std::find_if(reboundSource.entities.begin(),
        reboundSource.entities.end(), [](const auto& entity) {
            return entity.record.hasImportedSource;
        });
    REQUIRE(reboundImported != reboundSource.entities.end());
    reboundImported->record.importedSource.model.path = replacementModel.string();
    reboundImported->record.materialOverride.authored = true;
    reboundImported->record.materialOverride.material.baseColor = {0.72f, 0.18f, 0.36f};
    REQUIRE(PrefabSerializer::Save(reboundSource, prefabPath, replacementError));

    auto& malformedMember = manager.AuthoringDoc().ecs.registry.get<PrefabMemberComponent>(
        manager.AuthoringDoc().FindByUuid(rootMembers[2]));
    const auto originalMalformedTemplate = malformedMember.templateId;
    malformedMember.templateId = UUID::Nil();
    PrefabPropagationDiscoveryRequest reboundRequest = request;
    reboundRequest.documentGeneration = manager.DocumentGeneration();
    reboundRequest.resourceGeneration = manager.ResourceGeneration();
    reboundRequest.authoringRevision = manager.AuthoringRevision();
    const auto reboundCaptured = CapturePrefabSource(
        reboundRequest.changedSource, reboundRequest.assets);
    REQUIRE(reboundCaptured.IsOk());
    reboundRequest.capturedSource = reboundCaptured.value;
    const auto reboundPlan = PreparePrefabPropagation(reboundRequest);
    REQUIRE(reboundPlan.IsOk());
    REQUIRE(reboundPlan.value.instances.size() == 3);
    std::size_t propagated = 0;
    std::size_t quarantined = 0;
    for (const auto& instance : reboundPlan.value.instances)
    {
        if (instance.disposition == PrefabPropagationInstanceDisposition::Propagate)
            ++propagated;
        if (instance.disposition == PrefabPropagationInstanceDisposition::Quarantined)
        {
            ++quarantined;
            REQUIRE_FALSE(instance.diagnostics.empty());
        }
    }
    CHECK(propagated == 2);
    CHECK(quarantined == 1);
    const auto reboundStaged = StagePrefabPropagationResources(
        reboundPlan.value, manager.AuthoringDoc(),
        AssetResolutionContext{temp.path, nullptr});
    REQUIRE(reboundStaged.IsOk());
    const auto reboundFingerprint = reboundStaged.value.Summary().source;
    auto reboundCommand = std::make_unique<PrefabPropagationCommand>(
        *reboundStaged.value.Executable(), [reboundFingerprint] {
            return Result<PrefabSourceFingerprint>::Ok(reboundFingerprint);
        });
    REQUIRE(history.Execute(std::move(reboundCommand), manager).success);

    // The quarantined sibling is intentionally not authored with malformed
    // durable data. Restore its pre-existing template identity before the
    // save/reload half so the test proves quarantine without manufacturing an
    // invalid scene file.
    malformedMember.templateId = originalMalformedTemplate;

    std::vector<AssetDiagnostic> reboundSaveDiagnostics;
    const auto reboundScenePath = temp.path / "rebound.rt2scene";
    REQUIRE(SceneSerializer::Save(manager.AuthoringDoc(), reboundScenePath,
                                  reboundSaveDiagnostics, replacementError));
    SceneDocument reboundLoaded;
    DeterministicUuidProvider reboundLoadIds;
    reboundLoaded.SetUuidProvider(&reboundLoadIds);
    INFO(replacementError.Format());
    REQUIRE(SceneSerializer::Load(reboundLoaded, reboundScenePath, replacementError));
    std::vector<AssetDiagnostic> reboundResolveDiagnostics;
    REQUIRE(SceneAssetResolver::ResolveAll(reboundLoaded,
        AssetResolutionContext{temp.path, nullptr}, reboundResolveDiagnostics,
        replacementError));
    for (std::size_t i = 0; i < meshMembers.size(); ++i)
    {
        const auto entity = reboundLoaded.FindByUuid(meshMembers[i]);
        REQUIRE(static_cast<std::uint32_t>(entity) !=
                static_cast<std::uint32_t>(entt::null));
        const auto* importedComponent =
            reboundLoaded.ecs.registry.try_get<ImportedMeshSourceComponent>(entity);
        REQUIRE(importedComponent);
        if (i < 2)
            CHECK(std::filesystem::path(importedComponent->model.path).filename() ==
                  replacementModel.filename());
        else
            CHECK(std::filesystem::path(importedComponent->model.path).filename() !=
                  replacementModel.filename());
        if (i == 1)
        {
            REQUIRE(reboundLoaded.ecs.registry.all_of<MaterialOverrideComponent>(entity));
            const auto& retained = reboundLoaded.ecs.registry.get<MaterialOverrideComponent>(entity);
            CHECK(retained.material.baseColor == localTint.baseColor);
        }
    }
}

TEST_CASE("Phase 8 W4 S7 A10: execute undo redo preserves serialized slots and history counts")
{
    const auto temp = Temp();
    SceneManager scene;
    PopulateA10(scene);
    auto& document = scene.AuthoringDoc();
    auto& registry = document.ecs.registry;
    const auto beforeRevision = scene.AuthoringRevision();
    const auto beforeResourceGeneration = scene.ResourceGeneration();
    const auto beforeMeshCount = scene.GetMeshRegistryCount();

    PrefabSourceFingerprint source{
        "assets/source.rt2prefab", kA10Instance, "a10-digest"};
    DiscoveredPropagationPlan plan;
    plan.source = source;
    plan.documentGeneration = scene.DocumentGeneration();
    plan.resourceGeneration = beforeResourceGeneration;
    plan.authoringRevision = beforeRevision;
    plan.authoringRevisionCaptured = true;
    plan.sourceSchemaVersion = PrefabSerializer::FormatVersion;
    plan.meshTableExtent = static_cast<std::uint32_t>(beforeMeshCount);
    plan.materialTableExtent = static_cast<std::uint32_t>(scene.GetMaterialCount());
    plan.textureTableExtent = static_cast<std::uint32_t>(document.ecs.textures.size());
    plan.resourceEvidenceCaptured = true;
    plan.componentOperations.push_back(PrefabPropagationComponentDelta::Make<NameComponent>(
        kA10Entity, kA10Template, NameComponent{"before"}, NameComponent{"after"}));
    plan.componentOperations.push_back(PrefabPropagationComponentDelta::Make<PrimitiveComponent>(
        kA10Entity, kA10Template,
        PrimitiveComponent{PrimitiveComponent::Cube, 1.0f, 24, 16},
        PrimitiveComponent{PrimitiveComponent::Sphere, 2.0f, 16, 8}));
    plan.memberSnapshots.push_back({kA10Entity, kA10Instance, kA10Template, {}});
    plan.rootSnapshots.push_back({kA10Entity, kA10Instance,
        AssetReference{AssetKind::Prefab, "assets/source.rt2prefab", {}, {},
                       kA10Instance}});
    plan.instances.push_back({kA10Instance, kA10Entity,
        PrefabPropagationInstanceDisposition::Propagate, {kA10Entity}, {}});
    plan.affectedEntities = {kA10Entity};
    plan.syncImpact = SyncImpact::Structural;
    REQUIRE(plan.componentOperations.size() == 2);
    REQUIRE(plan.IsEffective());

    std::vector<AssetDiagnostic> diagnostics;
    Error error;
    const auto beforePath = temp.path / "before.rt2scene";
    INFO(error.Format());
    REQUIRE(SceneSerializer::Save(document, beforePath, diagnostics, error));
    const auto beforeBytes = FileBytes(beforePath);
    EditorCommandHistory history;
    std::size_t fingerprintReads = 0;
    auto command = std::make_unique<PrefabPropagationCommand>(
        AsExecutable(document, &plan),
        [&] {
            ++fingerprintReads;
            return Result<PrefabSourceFingerprint>::Ok(source);
        });
    const auto executed = history.Execute(std::move(command), scene);
    INFO(executed.error.Format());
    REQUIRE(executed.success);
    REQUIRE(executed.effective);
    CHECK(executed.syncImpact == SyncImpact::Structural);
    CHECK(executed.affectedEntities == std::vector<UUID>{kA10Entity});
    CHECK(history.UndoDepthForTest() == 1);
    CHECK(scene.GetMeshRegistryCount() == beforeMeshCount + 1);
    CHECK(scene.ResourceGeneration() == beforeResourceGeneration + 1);
    const auto afterPath = temp.path / "after.rt2scene";
    diagnostics.clear();
    REQUIRE(SceneSerializer::Save(document, afterPath, diagnostics, error));
    const auto afterBytes = FileBytes(afterPath);
    CHECK(afterBytes != beforeBytes);

    REQUIRE(history.Undo(scene).success);
    CHECK(history.RedoDepthForTest() == 1);
    CHECK(registry.get<NameComponent>(document.FindByUuid(kA10Entity)).name == "before");
    CHECK(scene.GetMeshRegistryCount() == beforeMeshCount + 1);
    CHECK(scene.ResourceGeneration() == beforeResourceGeneration + 2);
    const auto undoPath = temp.path / "undo.rt2scene";
    diagnostics.clear();
    REQUIRE(SceneSerializer::Save(document, undoPath, diagnostics, error));
    CHECK(FileBytes(undoPath) == beforeBytes);

    REQUIRE(history.Redo(scene).success);
    CHECK(history.UndoDepthForTest() == 1);
    CHECK(registry.get<NameComponent>(document.FindByUuid(kA10Entity)).name == "after");
    CHECK(scene.GetMeshRegistryCount() == beforeMeshCount + 1);
    CHECK(scene.GetECS().meshRegistry.GetMesh(1).name != "before");
    CHECK(fingerprintReads == 1);
    const auto redoPath = temp.path / "redo.rt2scene";
    diagnostics.clear();
    REQUIRE(SceneSerializer::Save(document, redoPath, diagnostics, error));
    CHECK(FileBytes(redoPath) == afterBytes);

    // A plan with no effective operations is silent: no history entry,
    // revision, resource generation, or serialized change.
    DiscoveredPropagationPlan noop = plan;
    noop.componentOperations.clear();
    noop.affectedEntities.clear();
    noop.syncImpact = SyncImpact::None;
    noop.documentGeneration = scene.DocumentGeneration();
    noop.resourceGeneration = scene.ResourceGeneration();
    noop.authoringRevision = scene.AuthoringRevision();
    noop.instances.clear();
    const auto noopRevision = scene.AuthoringRevision();
    const auto noopResourceGeneration = scene.ResourceGeneration();
    const auto noopHistory = history.UndoDepthForTest();
    const auto noOpStage = StagePrefabPropagationResources(noop, document, {});
    REQUIRE(noOpStage.IsOk());
    CHECK(noOpStage.value.IsNoOp());
    CHECK(scene.AuthoringRevision() == noopRevision);
    CHECK(scene.ResourceGeneration() == noopResourceGeneration);
    CHECK(history.UndoDepthForTest() == noopHistory);
}

TEST_CASE("Phase 8 W4 S7 A13: provenance transitions and material conflict quarantine are literal")
{
    // Exercise both provenance directions through the complete production
    // chain: discovery Prepare, resource staging, history Execute, then
    // append-only Undo/Redo.  The seam-only checks below retain the focused
    // quarantine assertions, while this block proves durable component and
    // MeshRef state at the command boundary.
    const auto runTransition = [](bool sourceImported, bool sceneImported,
                                  unsigned instance) {
        const auto temp = Temp();
        std::filesystem::path modelPath;
        if (sourceImported)
        {
            modelPath = temp.path / "new.glb";
            Error modelError;
            REQUIRE(GenerateTinyTexturedGlb(modelPath, modelError));
            REQUIRE(modelError.IsOk());
            REQUIRE(WriteSidecarId(AssetSidecarPath(modelPath), A13Uuid(1702),
                                   modelError));
        }
        auto records = std::vector<PrefabEntityRecord>{
            A13Record(1, 101), A13Record(2, 102, A13Uuid(101))};
        if (sourceImported)
        {
            records[1].record.hasImportedSource = true;
            records[1].record.importedSource.model =
                AssetReference{AssetKind::Model, "new.glb", {},
                               "gltf:scene=0:node=0:mesh=0:primitive=0",
                               A13Uuid(1702)};
        }
        else
        {
            records[1].record.hasPrimitive = true;
            records[1].record.primitive =
                PrimitiveComponent{PrimitiveComponent::Sphere, 3.0f, 8, 6};
        }
        const auto source = WriteA13Prefab(temp, std::move(records));
        SceneManager scene;
        scene.ReplaceAuthoringDocument(A13Document(source, sceneImported, instance));
        auto& document = scene.AuthoringDoc();
        auto request = A13Request(document, source);
        request.documentGeneration = scene.DocumentGeneration();
        request.resourceGeneration = scene.ResourceGeneration();
        request.authoringRevision = scene.AuthoringRevision();
        const auto prepared = PreparePrefabPropagation(request);
        if (!prepared.IsOk()) INFO(prepared.error.Format());
        REQUIRE(prepared.IsOk());
        REQUIRE(prepared.value.instances.size() == 1);
        CHECK(prepared.value.instances.front().disposition ==
              PrefabPropagationInstanceDisposition::Propagate);
        CHECK(prepared.value.instances.front().affectedEntities.size() == 2);
        const auto staged = StagePrefabPropagationResources(
            prepared.value, document, AssetResolutionContext{temp.path, nullptr});
        if (!staged.IsOk()) INFO(staged.error.Format());
        REQUIRE(staged.IsOk());
        REQUIRE(staged.value.Summary().instances.size() == 1);
        CHECK(staged.value.Summary().instances.front().disposition ==
              PrefabPropagationInstanceDisposition::Propagate);
        REQUIRE(prepared.value.IsEffective());
        const auto childUuid = A13Uuid(1401 + instance * 10);
        const auto child = document.FindByUuid(childUuid);
        REQUIRE(static_cast<std::uint32_t>(child) !=
                static_cast<std::uint32_t>(entt::null));
        struct ResourceTables
        {
            std::vector<MeshData> meshes;
            std::vector<SceneMaterial> materials;
            std::vector<SceneTexture> textures;
        };
        const auto captureTables = [&]() {
            ResourceTables tables;
            for (std::uint32_t i = 0; i < scene.GetECS().meshRegistry.GetCount(); ++i)
                tables.meshes.push_back(scene.GetECS().meshRegistry.GetMesh(i));
            tables.materials = scene.GetECS().materials;
            tables.textures = scene.GetECS().textures;
            return tables;
        };
        const auto sameTables = [](const ResourceTables& a, const ResourceTables& b) {
            if (a.meshes.size() != b.meshes.size() ||
                a.materials.size() != b.materials.size() ||
                a.textures.size() != b.textures.size())
                return false;
            for (std::size_t i = 0; i < a.meshes.size(); ++i)
                if (!PrefabPropagationMeshEqual(a.meshes[i], b.meshes[i])) return false;
            for (std::size_t i = 0; i < a.materials.size(); ++i)
                if (!PrefabCanonicalMaterialEqual(a.materials[i], b.materials[i])) return false;
            for (std::size_t i = 0; i < a.textures.size(); ++i)
                if (!PrefabPropagationTextureEqual(a.textures[i], b.textures[i])) return false;
            return true;
        };
        const auto sameMeshRef = [](const std::optional<MeshRef>& a,
                                    const std::optional<MeshRef>& b) {
            return a.has_value() == b.has_value() &&
                (!a || (a->meshIndex == b->meshIndex &&
                        a->materialIndex == b->materialIndex));
        };
        const auto samePrimitive = [](const PrimitiveComponent& a,
                                      const PrimitiveComponent& b) {
            return a.kind == b.kind && a.size == b.size &&
                a.segments == b.segments && a.rings == b.rings;
        };
        const auto sameImported = [](const ImportedMeshSourceComponent& a,
                                     const ImportedMeshSourceComponent& b) {
            return a.model.kind == b.model.kind && a.model.path == b.model.path &&
                a.model.importSettings == b.model.importSettings &&
                a.model.sourceKey == b.model.sourceKey &&
                a.model.assetId == b.model.assetId;
        };
        const auto beforeTables = captureTables();
        const std::optional<PrimitiveComponent> beforePrimitive =
            document.ecs.registry.all_of<PrimitiveComponent>(child)
                ? std::optional<PrimitiveComponent>(document.ecs.registry.get<PrimitiveComponent>(child))
                : std::nullopt;
        const std::optional<ImportedMeshSourceComponent> beforeImported =
            document.ecs.registry.all_of<ImportedMeshSourceComponent>(child)
                ? std::optional<ImportedMeshSourceComponent>(
                    document.ecs.registry.get<ImportedMeshSourceComponent>(child))
                : std::nullopt;
        const std::optional<MeshRef> beforeMeshRef =
            document.ecs.registry.all_of<MeshRef>(child)
                ? std::optional<MeshRef>(document.ecs.registry.get<MeshRef>(child))
                : std::nullopt;
        const auto beforeMeshes = scene.GetMeshRegistryCount();
        const auto beforeMaterials = scene.GetMaterialCount();
        const auto beforeTextures = scene.GetECS().textures.size();
        std::size_t ownedMeshes = 0;
        std::size_t ownedMaterials = 0;
        std::size_t ownedTextures = 0;
        for (const auto& ownership : staged.value.Executable()->resourceOwnership())
        {
            const auto count = ownership.rebase.owned.Entries().size();
            if (ownership.rebase.kind == PrefabPropagationResourceKind::Mesh)
                ownedMeshes += count;
            else if (ownership.rebase.kind == PrefabPropagationResourceKind::Material)
                ownedMaterials += count;
            else
                ownedTextures += count;
            CHECK(ownership.rebase.sceneAfterExtent ==
                  ownership.rebase.sceneBeforeExtent + count);
            REQUIRE(ownership.rebase.sceneSlots.size() == count);
            for (std::size_t i = 0; i < count; ++i)
                CHECK(ownership.rebase.sceneSlots[i].value ==
                      ownership.rebase.sceneBeforeExtent + i);
        }
        const auto stagedRef = std::find_if(staged.value.Executable()->meshRefOperations().begin(),
            staged.value.Executable()->meshRefOperations().end(), [&](const auto& operation) {
            return operation.entityUuid == childUuid;
            });
        REQUIRE(stagedRef != staged.value.Executable()->meshRefOperations().end());
        REQUIRE(stagedRef->after.has_value());
        const auto importedOperation = std::find_if(
            staged.value.Summary().componentOperations.begin(), staged.value.Summary().componentOperations.end(),
            [&](const auto& operation) {
                return operation.EntityUuid() == childUuid &&
                    operation.Key().wire() == PrefabWireKeys::kImportedSource;
            });
        const auto primitiveOperation = std::find_if(
            staged.value.Summary().componentOperations.begin(), staged.value.Summary().componentOperations.end(),
            [&](const auto& operation) {
                return operation.EntityUuid() == childUuid &&
                    operation.Key().wire() == PrefabWireKeys::kPrimitive;
            });
        std::optional<ImportedMeshSourceComponent> expectedImported;
        std::optional<PrimitiveComponent> expectedPrimitive;
        if (sourceImported)
        {
            REQUIRE(importedOperation != staged.value.Summary().componentOperations.end());
            REQUIRE(importedOperation->AfterValue().has_value());
            expectedImported = std::get<ImportedMeshSourceComponent>(*importedOperation->AfterValue());
        }
        else
        {
            REQUIRE(primitiveOperation != staged.value.Summary().componentOperations.end());
            REQUIRE(primitiveOperation->AfterValue().has_value());
            expectedPrimitive = std::get<PrimitiveComponent>(*primitiveOperation->AfterValue());
        }
        const auto fingerprint = staged.value.Summary().source;
        EditorCommandHistory history;
        auto command = std::make_unique<PrefabPropagationCommand>(
            *staged.value.Executable(), [fingerprint] {
                return Result<PrefabSourceFingerprint>::Ok(fingerprint);
            });
        const auto applied = history.Execute(std::move(command), scene);
        REQUIRE(applied.success);
        REQUIRE(applied.effective);
        const auto afterMeshes = scene.GetMeshRegistryCount();
        const auto afterMaterials = scene.GetMaterialCount();
        const auto afterTextures = scene.GetECS().textures.size();
        CHECK(afterMeshes >= beforeMeshes);
        CHECK(afterMaterials >= beforeMaterials);
        CHECK(afterTextures >= beforeTextures);
        CHECK(afterMeshes == beforeMeshes + ownedMeshes);
        CHECK(afterMaterials == beforeMaterials + ownedMaterials);
        CHECK(afterTextures == beforeTextures + ownedTextures);
        const auto afterEntity = scene.FindEntityByUuid(childUuid);
        REQUIRE(static_cast<std::uint32_t>(afterEntity) !=
                static_cast<std::uint32_t>(entt::null));
        const bool hasPrimitive = document.ecs.registry.all_of<PrimitiveComponent>(afterEntity);
        const bool hasImported =
            document.ecs.registry.all_of<ImportedMeshSourceComponent>(afterEntity);
        CHECK(hasPrimitive != hasImported);
        if (sourceImported)
        {
            REQUIRE(hasImported);
            CHECK(document.ecs.registry.get<ImportedMeshSourceComponent>(afterEntity)
                      .model.path == "new.glb");
            REQUIRE(document.ecs.registry.all_of<MeshRef>(afterEntity));
            const auto& ref = document.ecs.registry.get<MeshRef>(afterEntity);
            REQUIRE(ref.meshIndex >= 0);
            REQUIRE(ref.materialIndex >= 0);
        CHECK(static_cast<std::size_t>(ref.meshIndex) < scene.GetMeshRegistryCount());
            CHECK(static_cast<std::size_t>(ref.materialIndex) < scene.GetMaterialCount());
            CHECK(ref.meshIndex == stagedRef->after->meshIndex);
            CHECK(ref.materialIndex == stagedRef->after->materialIndex);
            const auto& importedValue =
                document.ecs.registry.get<ImportedMeshSourceComponent>(afterEntity);
            REQUIRE(expectedImported.has_value());
            CHECK(sameImported(importedValue, *expectedImported));
        }
        else
        {
            REQUIRE(hasPrimitive);
            const auto& primitive =
                document.ecs.registry.get<PrimitiveComponent>(afterEntity);
            REQUIRE(expectedPrimitive.has_value());
            CHECK(samePrimitive(primitive, *expectedPrimitive));
            REQUIRE(document.ecs.registry.all_of<MeshRef>(afterEntity));
            const auto& ref = document.ecs.registry.get<MeshRef>(afterEntity);
            CHECK(ref.meshIndex == stagedRef->after->meshIndex);
            CHECK(ref.materialIndex == stagedRef->after->materialIndex);
            CHECK_FALSE(hasImported);
        }
        const auto afterTables = captureTables();
        CHECK(afterTables.meshes.size() == beforeTables.meshes.size() + ownedMeshes);
        CHECK(afterTables.materials.size() == beforeTables.materials.size() + ownedMaterials);
        CHECK(afterTables.textures.size() == beforeTables.textures.size() + ownedTextures);
        for (std::size_t i = 0; i < beforeTables.meshes.size(); ++i)
            CHECK(PrefabPropagationMeshEqual(afterTables.meshes[i], beforeTables.meshes[i]));
        for (std::size_t i = 0; i < beforeTables.materials.size(); ++i)
            CHECK(PrefabCanonicalMaterialEqual(afterTables.materials[i], beforeTables.materials[i]));
        for (std::size_t i = 0; i < beforeTables.textures.size(); ++i)
            CHECK(PrefabPropagationTextureEqual(afterTables.textures[i], beforeTables.textures[i]));
        for (const auto& ownership : staged.value.Executable()->resourceOwnership())
            for (std::size_t i = 0; i < ownership.rebase.sceneSlots.size(); ++i)
            {
                const auto slot = ownership.rebase.sceneSlots[i].value;
                const auto& payload = ownership.rebase.owned.Entries()[i].decoded;
                if (ownership.rebase.kind == PrefabPropagationResourceKind::Mesh)
                    CHECK(PrefabPropagationMeshEqual(afterTables.meshes[slot],
                        std::get<MeshData>(payload)));
                else if (ownership.rebase.kind == PrefabPropagationResourceKind::Material)
                    CHECK(PrefabCanonicalMaterialEqual(afterTables.materials[slot],
                        std::get<SceneMaterial>(payload)));
                else
                    CHECK(PrefabPropagationTextureEqual(afterTables.textures[slot],
                        std::get<SceneTexture>(payload)));
            }
        REQUIRE(history.Undo(scene).success);
        const auto undone = scene.FindEntityByUuid(childUuid);
        REQUIRE(static_cast<std::uint32_t>(undone) !=
                static_cast<std::uint32_t>(entt::null));
        CHECK(scene.GetMeshRegistryCount() == afterMeshes);
        CHECK(scene.GetMaterialCount() == afterMaterials);
        CHECK(scene.GetECS().textures.size() == afterTextures);
        const std::optional<PrimitiveComponent> undonePrimitive =
            document.ecs.registry.all_of<PrimitiveComponent>(undone)
                ? std::optional<PrimitiveComponent>(document.ecs.registry.get<PrimitiveComponent>(undone))
                : std::nullopt;
        const std::optional<ImportedMeshSourceComponent> undoneImported =
            document.ecs.registry.all_of<ImportedMeshSourceComponent>(undone)
                ? std::optional<ImportedMeshSourceComponent>(
                    document.ecs.registry.get<ImportedMeshSourceComponent>(undone))
                : std::nullopt;
        const std::optional<MeshRef> undoneMeshRef =
            document.ecs.registry.all_of<MeshRef>(undone)
                ? std::optional<MeshRef>(document.ecs.registry.get<MeshRef>(undone))
                : std::nullopt;
        CHECK(undonePrimitive.has_value() == beforePrimitive.has_value());
        CHECK(undoneImported.has_value() == beforeImported.has_value());
        if (undonePrimitive) { REQUIRE(beforePrimitive); CHECK(samePrimitive(*undonePrimitive, *beforePrimitive)); }
        if (undoneImported) { REQUIRE(beforeImported); CHECK(sameImported(*undoneImported, *beforeImported)); }
        CHECK(sameMeshRef(undoneMeshRef, beforeMeshRef));
        CHECK(sameTables(captureTables(), afterTables));
        REQUIRE(history.Redo(scene).success);
        const auto redone = scene.FindEntityByUuid(childUuid);
        REQUIRE(static_cast<std::uint32_t>(redone) !=
                static_cast<std::uint32_t>(entt::null));
        CHECK(scene.GetMeshRegistryCount() == afterMeshes);
        CHECK(scene.GetMaterialCount() == afterMaterials);
        CHECK(scene.GetECS().textures.size() == afterTextures);
        const std::optional<PrimitiveComponent> redonePrimitive =
            document.ecs.registry.all_of<PrimitiveComponent>(redone)
                ? std::optional<PrimitiveComponent>(document.ecs.registry.get<PrimitiveComponent>(redone))
                : std::nullopt;
        const std::optional<ImportedMeshSourceComponent> redoneImported =
            document.ecs.registry.all_of<ImportedMeshSourceComponent>(redone)
                ? std::optional<ImportedMeshSourceComponent>(
                    document.ecs.registry.get<ImportedMeshSourceComponent>(redone))
                : std::nullopt;
        const std::optional<MeshRef> redoneMeshRef =
            document.ecs.registry.all_of<MeshRef>(redone)
                ? std::optional<MeshRef>(document.ecs.registry.get<MeshRef>(redone))
                : std::nullopt;
        CHECK(redonePrimitive.has_value() == expectedPrimitive.has_value());
        CHECK(redoneImported.has_value() == expectedImported.has_value());
        if (redonePrimitive) { REQUIRE(expectedPrimitive); CHECK(samePrimitive(*redonePrimitive, *expectedPrimitive)); }
        if (redoneImported) { REQUIRE(expectedImported); CHECK(sameImported(*redoneImported, *expectedImported)); }
        CHECK(sameMeshRef(redoneMeshRef, stagedRef->after));
        CHECK(sameTables(captureTables(), afterTables));
    };
    runTransition(true, false, 0);
    runTransition(false, true, 1);

    // Primitive -> imported and imported -> primitive are both planned from
    // the same discovery seam, not inferred from separate classification tests.
    {
        const auto temp = Temp();
        auto records = std::vector<PrefabEntityRecord>{
            A13Record(1, 101), A13Record(2, 102, A13Uuid(101))};
        records[1].record.hasImportedSource = true;
        records[1].record.importedSource.model =
            AssetReference{AssetKind::Model, "new.glb", {}, "gltf:mesh=2", A13Uuid(1702)};
        const auto source = WriteA13Prefab(temp, std::move(records));
        auto document = A13Document(source, false, 0);
        const auto result = PreparePrefabPropagation(A13Request(document, source));
        REQUIRE(result.IsOk());
        bool addedImported = false;
        bool removedPrimitive = false;
        for (const auto& operation : result.value.componentOperations)
        {
            if (operation.Key().wire() == PrefabWireKeys::kImportedSource)
                addedImported = !operation.BeforeValue().has_value() && operation.AfterValue().has_value();
            if (operation.Key().wire() == PrefabWireKeys::kPrimitive)
                removedPrimitive = operation.AfterValue().has_value() == false;
        }
        CHECK(addedImported);
        CHECK(removedPrimitive);
    }
    {
        const auto temp = Temp();
        auto records = std::vector<PrefabEntityRecord>{
            A13Record(1, 101), A13Record(2, 102, A13Uuid(101))};
        records[1].record.hasPrimitive = true;
        records[1].record.primitive = PrimitiveComponent{PrimitiveComponent::Sphere, 3.0f, 8, 6};
        const auto source = WriteA13Prefab(temp, std::move(records));
        auto document = A13Document(source, true, 1);
        const auto result = PreparePrefabPropagation(A13Request(document, source));
        REQUIRE(result.IsOk());
        bool addedPrimitive = false;
        bool removedImported = false;
        for (const auto& operation : result.value.componentOperations)
        {
            if (operation.Key().wire() == PrefabWireKeys::kPrimitive)
                addedPrimitive = !operation.BeforeValue().has_value() && operation.AfterValue().has_value();
            if (operation.Key().wire() == PrefabWireKeys::kImportedSource)
                removedImported = operation.AfterValue().has_value() == false;
        }
        CHECK(addedPrimitive);
        CHECK(removedImported);
    }
    {
        const auto temp = Temp();
        auto records = std::vector<PrefabEntityRecord>{
            A13Record(1, 101), A13Record(2, 102, A13Uuid(101))};
        records[1].record.hasPrimitive = true;
        records[1].record.primitive = PrimitiveComponent{PrimitiveComponent::Cube, 1.0f, 3, 3};
        records[1].record.hasMaterialOverride = true;
        records[1].record.materialOverride.authored = true;
        records[1].record.materialOverride.material.baseColor = {0.2f, 0.4f, 0.6f};
        const auto source = WriteA13Prefab(temp, std::move(records));
        auto document = A13Document(source, true, 2);
        const auto conflictChild = document.FindByUuid(A13Uuid(1421));
        REQUIRE(static_cast<std::uint32_t>(conflictChild) !=
                static_cast<std::uint32_t>(entt::null));
        document.ecs.registry.get<PrefabMemberComponent>(conflictChild).overrides.push_back(
            PrefabComponentKeyFor<MaterialOverrideComponent>::value);
        document.ecs.registry.emplace<MaterialOverrideComponent>(conflictChild);
        // Add a valid sibling with the same source and a separate instance ID.
        // Merge the sibling entities into one document by repeating the exact
        // durable shape; the first instance remains the material-conflict case.
        const auto root = document.ecs.registry.create();
        REQUIRE(document.AssignKnownUuid(root, A13Uuid(1430)));
        document.ecs.registry.emplace<Hierarchy>(root);
        document.ecs.registry.emplace<PrefabInstanceComponent>(root,
            PrefabInstanceComponent{AssetReference{AssetKind::Prefab,
                source.filename().string(), {}, {}, A13Uuid(1300)}, A13Uuid(1503)});
        document.ecs.registry.emplace<PrefabMemberComponent>(root,
            PrefabMemberComponent{A13Uuid(1503), A13Uuid(1), {}});
        const auto child = document.ecs.registry.create();
        REQUIRE(document.AssignKnownUuid(child, A13Uuid(1431)));
        document.ecs.registry.emplace<Hierarchy>(child, root,
                                                  std::vector<entt::entity>{});
        document.ecs.registry.get<Hierarchy>(root).children.push_back(child);
        document.ecs.registry.emplace<PrefabMemberComponent>(child,
            PrefabMemberComponent{A13Uuid(1503), A13Uuid(2), {}});
        document.ecs.registry.get<PrefabMemberComponent>(child).overrides.push_back(
            PrefabComponentKeyFor<MaterialOverrideComponent>::value);
        const auto result = PreparePrefabPropagation(A13Request(document, source));
        REQUIRE(result.IsOk());
        REQUIRE(result.value.instances.size() == 2);
        for (const auto& instance : result.value.instances)
            for (const auto& diagnostic : instance.diagnostics)
                MESSAGE(diagnostic.reason);
        CHECK(result.value.instances[0].disposition ==
              PrefabPropagationInstanceDisposition::Quarantined);
        CHECK(result.value.instances[1].disposition ==
              PrefabPropagationInstanceDisposition::Propagate);
        CHECK(result.value.instances[0].affectedEntities.empty());
        CHECK_FALSE(result.value.instances[1].affectedEntities.empty());
        REQUIRE_FALSE(result.value.instances[0].diagnostics.empty());
    }
}

TEST_CASE("Phase 8 W4 S7 A11: host orchestration routes converge")
{
    // This test proves production host sequencing and context boundaries.
    // Literal default source-byte -> Prepare -> Stage coverage is provided by
    // S5 load integration and S6 live reimport tests; this control-flow case
    // intentionally does not claim handcrafted cross-route equivalence.
    const auto temp = Temp();
    const auto sourcePath = temp.path / "source.rt2prefab";
    PrefabDocument sourceDocument;
    sourceDocument.entities = {A13Record(1, 101)};
    Error sourceError;
    REQUIRE(PrefabSerializer::Save(sourceDocument, sourcePath, sourceError));
    REQUIRE(WriteSidecarId(AssetSidecarPath(sourcePath), kA11Instance, sourceError));
    int resolveCalls = 0;
    PrefabPropagationLoadHooks loadHooks;
    loadHooks.prepare = [&](const PrefabPropagationDiscoveryRequest& request) {
        auto plan = A11Plan(*request.document, sourcePath, "a11-source-change");
        plan.documentGeneration = request.documentGeneration;
        plan.resourceGeneration = request.resourceGeneration;
        plan.authoringRevision = request.authoringRevision;
        return Result<DiscoveredPropagationPlan>::Ok(std::move(plan));
    };
    loadHooks.resolveAll = [&](SceneDocument&, const AssetResolutionContext&,
                               std::vector<AssetDiagnostic>&, Error&) {
        ++resolveCalls;
        return true;
    };

    SceneDocument opened;
    PopulateA11Document(opened);
    std::vector<AssetDiagnostic> openDiagnostics;
    Error openError;
    const auto openReport = RunPrefabPropagationSceneOpen(
        opened, PrefabPropagationSceneOpenContext{temp.path / "opened.rt2scene",
                                                  temp.path, nullptr},
        openDiagnostics, openError, loadHooks);
    INFO(openReport.error.Format());
    REQUIRE(openReport.IsOk());
    CHECK(openReport.value.changed);
    CHECK(openReport.value.propagatedInstances == 1);
    CHECK(openReport.value.quarantinedInstances == 0);
    CHECK(resolveCalls == 1);
    CHECK(opened.ecs.registry.get<Transform>(opened.FindByUuid(kA11Entity))
              .translation.x == doctest::Approx(4.0f));

    SceneDocument recovered;
    PopulateA11Document(recovered);
    std::vector<AssetDiagnostic> recoveryDiagnostics;
    Error recoveryError;
    const auto recoveryReport = RunPrefabPropagationLoadIntegration(
        recovered, AssetResolutionContext{temp.path, nullptr},
        recoveryDiagnostics, recoveryError, loadHooks);
    REQUIRE(recoveryReport.IsOk());
    CHECK(recoveryReport.value.changed);
    CHECK(recoveryReport.value.propagatedInstances == 1);
    CHECK(resolveCalls == 2);
    CHECK(recovered.ecs.registry.get<Transform>(recovered.FindByUuid(kA11Entity))
              .translation.x == doctest::Approx(4.0f));
    CHECK(SameTransform(
        opened.ecs.registry.get<Transform>(opened.FindByUuid(kA11Entity)),
        recovered.ecs.registry.get<Transform>(recovered.FindByUuid(kA11Entity))));

    auto runLive = [&](SceneRunState state, PrefabPropagationLiveTrigger trigger,
                       bool refreshBeforeSubmit, bool backgroundBusy) {
        SceneManager scene;
        PopulateA11Document(scene.AuthoringDoc());
        EditorCommandHistory history;
        PrefabPropagationLiveQueue queue;
        PrefabPropagationLiveHost host(queue);
        int routed = 0;
        int published = 0;
        PrefabPropagationLiveHostCallbacks callbacks;
        callbacks.acquireContext = [&] {
            return Result<PrefabPropagationLiveContext>::Ok(
                PrefabPropagationLiveContext{temp.path,
                    std::make_shared<AssetDatabase>()});
        };
        callbacks.refreshContext = callbacks.acquireContext;
        callbacks.publish = [&](const auto&, const char*) { ++published; };
        callbacks.route = [&](const EditorMutationResult&) { ++routed; };
        PrefabPropagationLiveHooks hooks;
        hooks.fingerprint = [&](const AssetReference&, const AssetResolutionContext&) {
            return Result<PrefabSourceFingerprint>::Ok(
                PrefabSourceFingerprint{sourcePath, kA11Instance, "a11-source-change"});
        };
        hooks.prepare = [&](const PrefabPropagationDiscoveryRequest& request) {
            auto plan = A11Plan(*request.document, sourcePath, "a11-source-change");
            plan.documentGeneration = request.documentGeneration;
            plan.resourceGeneration = request.resourceGeneration;
            plan.authoringRevision = request.authoringRevision;
            return Result<DiscoveredPropagationPlan>::Ok(std::move(plan));
        };
        hooks.stage = [](const DiscoveredPropagationPlan& plan,
                         const SceneDocument& document,
                         const AssetResolutionContext& assets) {
            return StagePrefabPropagationResources(plan, document, assets);
        };
        const auto before = scene.AuthoringDoc().ecs.registry.get<Transform>(
            scene.AuthoringDoc().FindByUuid(kA11Entity));
        const auto submitted = host.Submit(scene, history, A11Source(), state,
            backgroundBusy, false, trigger, refreshBeforeSubmit, callbacks, hooks);
        if (state == SceneRunState::Playing)
        {
            CHECK(submitted.queued);
            CHECK(SameTransform(scene.AuthoringDoc().ecs.registry.get<Transform>(
                scene.AuthoringDoc().FindByUuid(kA11Entity)), before));
            const auto drained = host.Drain(scene, history, SceneRunState::Edit,
                                            false, callbacks, hooks);
            CHECK(drained.applied);
        }
        else
        {
            CHECK(submitted.applied);
        }
        CHECK(scene.AuthoringDoc().ecs.registry.get<Transform>(
            scene.AuthoringDoc().FindByUuid(kA11Entity)).translation.x ==
              doctest::Approx(4.0f));
        CHECK(history.UndoDepthForTest() == 1);
        CHECK(routed == 1);
        CHECK(published >= 1);
        return scene.AuthoringDoc().ecs.registry.get<Transform>(
            scene.AuthoringDoc().FindByUuid(kA11Entity));
    };

    const auto explicitResult = runLive(SceneRunState::Edit,
        PrefabPropagationLiveTrigger::Explicit, true, false);
    const auto watcherResult = runLive(SceneRunState::Edit,
        PrefabPropagationLiveTrigger::Watcher, false, false);
    const auto playResult = runLive(SceneRunState::Playing,
        PrefabPropagationLiveTrigger::Watcher, false, false);
    CHECK(SameTransform(explicitResult, watcherResult));
    CHECK(SameTransform(explicitResult, playResult));
}
