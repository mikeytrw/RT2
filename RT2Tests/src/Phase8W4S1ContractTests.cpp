#include <doctest/doctest.h>

#include "PrefabComponentKey.h"
#include "PrefabComponentValueEquality.h"
#include "PrefabPropagationContracts.h"
#include "SceneSerializer.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <variant>

using namespace rt2::core;

namespace
{
const UUID kEntity = UUID::Parse("11111111-1111-4111-8111-111111111111");
const UUID kInstance = UUID::Parse("22222222-2222-4222-8222-222222222222");
const UUID kTemplate = UUID::Parse("33333333-3333-4333-8333-333333333333");

std::filesystem::path S1Temp(const char* name)
{
    auto path = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path;
}

void WriteMarkerScene(const std::filesystem::path& path, std::uint32_t version,
                      const char* marker)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << "{\n"
           " \"version\":" << version << ",\n"
           " \"metadata\":{\"name\":\"s1\"},\n"
           " \"entities\":[{\"uuid\":\"" << kEntity.ToString() <<
           "\",\"name\":\"Cube\",\"parent\":\"\",\"visible\":true,"
           "\"primitive\":{\"kind\":\"cube\",\"size\":1.0,"
           "\"segments\":24,\"rings\":16},"
           "\"prefabMember\":{\"instanceId\":\"" <<
           kInstance.ToString() << "\",\"templateId\":\"" <<
           kTemplate.ToString() << "\",\"overrides\":[\"" << marker <<
           "\"]}}],\n"
           " \"materials\":[],\n"
           " \"textures\":[]\n"
           "}\n";
}
} // namespace

static_assert(std::variant_size_v<PropagationComponentSet> == 10);
static_assert(std::is_same_v<std::variant_alternative_t<0, PropagationComponentSet>, NameComponent>);
static_assert(std::is_same_v<std::variant_alternative_t<1, PropagationComponentSet>, Transform>);
static_assert(std::is_same_v<std::variant_alternative_t<2, PropagationComponentSet>, VisibleComponent>);
static_assert(std::is_same_v<std::variant_alternative_t<3, PropagationComponentSet>, PrimitiveComponent>);
static_assert(std::is_same_v<std::variant_alternative_t<4, PropagationComponentSet>, ImportedMeshSourceComponent>);
static_assert(std::is_same_v<std::variant_alternative_t<5, PropagationComponentSet>, MaterialOverrideComponent>);
static_assert(std::is_same_v<std::variant_alternative_t<6, PropagationComponentSet>, LightComponent>);
static_assert(std::is_same_v<std::variant_alternative_t<7, PropagationComponentSet>, CameraComponent>);
static_assert(std::is_same_v<std::variant_alternative_t<8, PropagationComponentSet>, MotionComponent>);
static_assert(std::is_same_v<std::variant_alternative_t<9, PropagationComponentSet>, ScriptComponent>);

template<typename T>
void ExerciseTypedPropagationAdapter(entt::registry& registry, entt::entity entity,
                                     const UUID& templateId)
{
    const T value{};
    const auto delta = PrefabPropagationComponentDelta::Make<T>(
        kEntity, templateId, std::nullopt, value);
    CHECK(delta.IsValid());
    CHECK(delta.Key() == PrefabComponentKeyFor<T>::value);
    CHECK(delta.Impact() == PropagationComponentImpact<T>);
    CHECK(delta.IsOverrideable() == PrefabComponentKeyFor<T>::value.overridable());
    CHECK(PreflightPropagationComponent(delta, registry, entity));

    WritePropagationComponent(delta, registry, entity, true);
    const auto current = ReadPropagationComponent(delta, registry, entity);
    REQUIRE(current.has_value());
    CHECK(PropagationComponentEqual(*current, *delta.AfterValue()));

    WritePropagationComponent(delta, registry, entity, false);
    CHECK_FALSE(registry.all_of<T>(entity));
}

TEST_CASE("Phase 8 typed foundation: ten typed adapters are exhaustive and total")
{
    entt::registry registry;
    const auto entity = registry.create();
    ExerciseTypedPropagationAdapter<NameComponent>(registry, entity, kTemplate);
    ExerciseTypedPropagationAdapter<Transform>(registry, entity, kTemplate);
    ExerciseTypedPropagationAdapter<VisibleComponent>(registry, entity, kTemplate);
    ExerciseTypedPropagationAdapter<PrimitiveComponent>(registry, entity, kTemplate);
    ExerciseTypedPropagationAdapter<ImportedMeshSourceComponent>(registry, entity, kTemplate);
    ExerciseTypedPropagationAdapter<MaterialOverrideComponent>(registry, entity, kTemplate);
    ExerciseTypedPropagationAdapter<LightComponent>(registry, entity, kTemplate);
    ExerciseTypedPropagationAdapter<CameraComponent>(registry, entity, kTemplate);
    ExerciseTypedPropagationAdapter<MotionComponent>(registry, entity, kTemplate);
    ExerciseTypedPropagationAdapter<ScriptComponent>(registry, entity, kTemplate);
}

TEST_CASE("Phase 8 typed foundation: later adapter preflight is zero mutation")
{
    entt::registry registry;
    const auto entity = registry.create();
    const auto first = PrefabPropagationComponentDelta::Make<NameComponent>(
        kEntity, kTemplate, std::nullopt, NameComponent{"first"});
    WritePropagationComponent(first, registry, entity, true);
    const auto stale = PrefabPropagationComponentDelta::Make<NameComponent>(
        kEntity, kTemplate, NameComponent{"not-current"}, NameComponent{"later"});
    CHECK_FALSE(PreflightPropagationComponent(stale, registry, entity));
    const auto current = ReadPropagationComponent<NameComponent>(registry, entity);
    REQUIRE(current.has_value());
    CHECK(current->name == "first");
    // Named RED/GREEN fault: replacing preflight with unconditional success
    // makes this test fail before any later operation can mutate the entity.
}

TEST_CASE("Phase 8 W4 S1: primitive is the ninth overridable key")
{
    CHECK(PersistedComponents::Count == 13);
    CHECK(CountOverridableEntries() == 9);
    CHECK(PrefabComponentKeyFor<PrimitiveComponent>::value.overridable());
    CHECK(FindComponentByWire("primitive").has_value());
}

TEST_CASE("Phase 8 W4 S1: canonical primitive and optional equality")
{
    PrimitiveComponent a{PrimitiveComponent::Sphere, 2.0f, 32, 12};
    PrimitiveComponent b = a;
    CHECK(PrimitiveComponentCanonicalEqual(a, b));
    b.segments++;
    CHECK_FALSE(PrimitiveComponentCanonicalEqual(a, b));
    CHECK(OptionalPrimitiveComponentCanonicalEqual(a, a));
    CHECK(OptionalPrimitiveComponentCanonicalEqual(
        std::optional<PrimitiveComponent>{}, std::optional<PrimitiveComponent>{}));
    CHECK_FALSE(OptionalPrimitiveComponentCanonicalEqual(a, std::nullopt));

    const auto equal = [](const int& x, const int& y) { return x == y; };
    CHECK(OptionalComponentCanonicalEqual(std::optional<int>{3},
                                          std::optional<int>{3}, equal));
    CHECK_FALSE(OptionalComponentCanonicalEqual(std::optional<int>{3},
                                                std::optional<int>{4}, equal));
}

TEST_CASE("Phase 8 W4 S1: v6 original override keys remain readable")
{
    const auto path = S1Temp("rt2_p8w4_s1_v6_name.rt2scene");
    WriteMarkerScene(path, 6, "name");
    SceneDocument loaded;
    Error err;
    REQUIRE(SceneSerializer::Load(loaded, path, err));
    const auto entity = loaded.FindByUuid(kEntity);
    REQUIRE(static_cast<std::uint32_t>(entity) !=
            static_cast<std::uint32_t>(entt::null));
    const auto* member = loaded.ecs.registry.try_get<PrefabMemberComponent>(entity);
    REQUIRE(member);
    REQUIRE(member->overrides.size() == 1);
    CHECK(member->overrides.front() == PrefabComponentKeyFor<NameComponent>::value);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("Phase 8 W4 S1: v6 primitive marker fails transactionally")
{
    const auto path = S1Temp("rt2_p8w4_s1_v6_primitive.rt2scene");
    WriteMarkerScene(path, 6, "primitive");
    SceneDocument loaded;
    loaded.metadata.name = "sentinel";
    Error err;
    CHECK_FALSE(SceneSerializer::Load(loaded, path, err));
    CHECK(err.code == Error::SchemaVersion);
    CHECK(loaded.metadata.name == "sentinel");
    CHECK(loaded.uuidIndex.Size() == 0);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("Phase 8 W4 S1: primitive marker round-trips only at v7")
{
    const auto path = S1Temp("rt2_p8w4_s1_v7_primitive.rt2scene");
    SceneDocument source;
    source.metadata.schemaVersion = SceneSerializer::SchemaVersion;
    source.metadata.name = "primitive-source";
    const auto entity = source.ecs.registry.create();
    REQUIRE(source.AssignKnownUuid(entity, kEntity));
    source.ecs.registry.emplace<NameComponent>(entity, NameComponent{"Cube"});
    source.ecs.registry.emplace<VisibleComponent>(entity, VisibleComponent{true});
    source.ecs.registry.emplace<Transform>(entity, Transform{});
    source.ecs.registry.emplace<PrimitiveComponent>(
        entity, PrimitiveComponent{PrimitiveComponent::Cube, 1.0f, 24, 16});
    source.ecs.registry.emplace<PrefabMemberComponent>(
        entity, PrefabMemberComponent{kInstance, kTemplate,
                                      {PrefabComponentKeyFor<PrimitiveComponent>::value}});
    std::vector<AssetDiagnostic> diagnostics;
    Error err;
    REQUIRE(SceneSerializer::Save(source, path, diagnostics, err));

    SceneDocument loaded;
    REQUIRE(SceneSerializer::Load(loaded, path, err));
    const auto loadedEntity = loaded.FindByUuid(kEntity);
    REQUIRE(static_cast<std::uint32_t>(loadedEntity) !=
            static_cast<std::uint32_t>(entt::null));
    const auto* member = loaded.ecs.registry.try_get<PrefabMemberComponent>(loadedEntity);
    REQUIRE(member);
    REQUIRE(member->overrides.size() == 1);
    CHECK(member->overrides.front() == PrefabComponentKeyFor<PrimitiveComponent>::value);
    CHECK(loaded.metadata.schemaVersion == SceneSerializer::SchemaVersion);

    source.metadata.schemaVersion = SceneSerializer::PrefabOverrideSchemaVersion;
    const auto oldPath = S1Temp("rt2_p8w4_s1_v6_write_primitive.rt2scene");
    CHECK_FALSE(SceneSerializer::SaveTo(source, oldPath, oldPath, diagnostics, err));
    CHECK(err.code == Error::SchemaVersion);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(oldPath, ec);
}

TEST_CASE("Phase 8 W4 S1: promotion reaches v7 and can be restored")
{
    SceneDocument doc;
    doc.metadata.schemaVersion = SceneSerializer::PrefabOverrideSchemaVersion;
    CHECK(SceneSerializer::PromoteSchemaVersion(doc));
    CHECK(doc.metadata.schemaVersion == SceneSerializer::SchemaVersion);
    CHECK_FALSE(SceneSerializer::PromoteSchemaVersion(doc));
    // Command layers capture this prior value for Undo; the contract itself
    // does not mutate history or invent a command.
    doc.metadata.schemaVersion = SceneSerializer::PrefabOverrideSchemaVersion;
    CHECK(doc.metadata.schemaVersion == 6);
}

TEST_CASE("Phase 8 W4 S1: contracts carry deterministic identity and dispositions")
{
    PrefabSourceFingerprint a;
    a.normalizedPath = "assets/fixture.rt2prefab";
    a.assetId = kInstance;
    a.contentDigest = "sha256:abc";
    auto b = a;
    CHECK(a.IsValid());
    CHECK(a == b);
    b.contentDigest = "sha256:def";
    CHECK(a != b);

    PrefabPropagationDiagnostic diagnostic;
    diagnostic.prefabPath = a.normalizedPath;
    diagnostic.prefabAssetId = a.assetId;
    diagnostic.instanceId = kInstance;
    diagnostic.rootUuid = kEntity;
    diagnostic.templateId = kTemplate;
    diagnostic.reason = "test";
    auto colliding = diagnostic;
    colliding.reason = "test|other";
    auto reordered = diagnostic;
    reordered.prefabPath = "assets/fixture|rt2prefab";
    CHECK(diagnostic == diagnostic);
    CHECK((colliding < reordered) != (reordered < colliding));

    PrefabPropagationPlan plan;
    plan.source = a;
    plan.instances.push_back({kInstance, kEntity,
                              PrefabPropagationInstanceDisposition::Quarantined,
                              {}, {diagnostic}});
    plan.diagnostics.push_back(diagnostic);
    CHECK(plan.IsNoOp());
    CHECK(plan.instances.front().disposition ==
          PrefabPropagationInstanceDisposition::Quarantined);
}

TEST_CASE("Phase 8 W4 S1: project binding boundary is v6, not current v7")
{
    CHECK_FALSE(SceneSerializer::UsesProjectBinding(5));
    CHECK(SceneSerializer::UsesProjectBinding(6));
    CHECK(SceneSerializer::UsesProjectBinding(SceneSerializer::SchemaVersion));
}

TEST_CASE("Phase 8 W4 S1: canonical payload equality is durable and NaN-explicit")
{
    PrimitiveComponent primitive;
    auto same = primitive;
    CHECK(PrefabCanonicalComponentEqual(primitive, same));
    primitive.size = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(PrefabCanonicalComponentEqual(primitive, primitive));

    Transform transform;
    auto runtimeDifferent = transform;
    runtimeDifferent.worldMatrix[0][0] = 9.0f;
    runtimeDifferent.dirty = false;
    CHECK(PrefabCanonicalComponentEqual(transform, runtimeDifferent));

    std::optional<PrimitiveComponent> absent;
    CHECK(OptionalPrimitiveComponentCanonicalEqual(absent, absent));
    CHECK_FALSE(OptionalPrimitiveComponentCanonicalEqual(absent, same));

    PropagationComponentSet variantA = PrimitiveComponent{};
    PropagationComponentSet variantB = PrimitiveComponent{};
    std::get<PrimitiveComponent>(variantA).size =
        std::numeric_limits<float>::quiet_NaN();
    std::get<PrimitiveComponent>(variantB).size =
        std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(PropagationComponentEqual(variantA, variantB));
    std::optional<PropagationComponentSet> optionalA = variantA;
    std::optional<PropagationComponentSet> optionalB = variantB;
    CHECK_FALSE(OptionalComponentCanonicalEqual(
        optionalA, optionalB,
        [](const auto& x, const auto& y) {
            return PropagationComponentEqual(x, y);
        }));
    optionalB.reset();
    CHECK_FALSE(OptionalComponentCanonicalEqual(
        optionalA, optionalB,
        [](const auto& x, const auto& y) {
            return PropagationComponentEqual(x, y);
        }));
}

TEST_CASE("Phase 8 W4 S1: component operations enforce key and payload correspondence")
{
    const auto entity = kEntity;
    const auto templ = kTemplate;
    auto name = PrefabPropagationComponentDelta::Make<NameComponent>(
        entity, templ, NameComponent{"before"}, NameComponent{"after"});
    CHECK(name.IsValid());

    ImportedMeshSourceComponent source;
    source.model.kind = AssetKind::Model;
    source.model.path = "mesh.glb";
    source.model.sourceKey = "gltf:scene=0";
    auto imported = PrefabPropagationComponentDelta::Make<ImportedMeshSourceComponent>(
        entity, templ, source, std::nullopt);
    CHECK(imported.IsValid());
    CHECK(name.Key() == PrefabComponentKeyFor<NameComponent>::value);
    CHECK(imported.Key() == PrefabComponentKeyFor<ImportedMeshSourceComponent>::value);
    CHECK_FALSE(std::is_constructible_v<PrefabPropagationComponentDelta,
                                        PrefabComponentKey>);
    CHECK_FALSE(std::is_constructible_v<PrefabPropagationComponentDelta,
                                        PropagationComponentSet>);
}

TEST_CASE("Phase 8 W4 S1: typed resource rebases validate immutable append-only blocks")
{
    MeshData mesh;
    mesh.vertices = {0.0f, 0.0f, 0.0f,
                     1.0f, 0.0f, 0.0f,
                     0.0f, 1.0f, 0.0f};
    mesh.indices = {0, 1, 2};
    mesh.name = "owned-mesh";
    const auto originalMesh = mesh;
    auto mutablePayloads = std::make_shared<std::vector<PrefabPropagationResourcePayload>>(
        std::vector<PrefabPropagationResourcePayload>{
            {"mesh:0", "digest-a", PrefabPropagationResourceValue{mesh}},
            {"mesh:1", "digest-b", PrefabPropagationResourceValue{mesh}}});
    PrefabPropagationResourceOwnership ownership;
    ownership.rebase.kind = PrefabPropagationResourceKind::Mesh;
    ownership.rebase.sourceBeforeExtent = 4;
    ownership.rebase.sceneBeforeExtent = 5;
    ownership.rebase.sceneAppendBase = 5;
    ownership.rebase.sceneAfterExtent = 7;
    ownership.rebase.sourceSlots = {{1}, {3}};
    ownership.rebase.sceneSlots = {{5}, {6}};
    ownership.rebase.owned = PrefabPropagationResourceBlock::FromDecoded(
        PrefabPropagationResourceKind::Mesh, *mutablePayloads);
    CHECK(ownership.IsValid());

    mesh.vertices[0] = 99.0f;
    mesh.vertices.clear();
    std::optional<MeshData> externalSource = mesh;
    externalSource.reset();
    mutablePayloads->at(0).decoded = MeshData{};
    CHECK(PrefabPropagationMeshEqual(
        std::get<MeshData>(ownership.rebase.owned.Entries()[0].decoded), originalMesh));
    auto changedDecoded = ownership.rebase.owned.Entries()[0];
    std::get<MeshData>(changedDecoded.decoded).vertices[0] = 42.0f;
    CHECK_FALSE(ownership.rebase.owned.Entries()[0] == changedDecoded);

    auto duplicate = ownership;
    duplicate.rebase.sourceSlots[1].value = 1;
    CHECK_FALSE(duplicate.IsValid());
    auto nonContiguous = ownership;
    nonContiguous.rebase.sceneSlots[1].value = 7;
    CHECK_FALSE(nonContiguous.IsValid());
    auto wrongAfterExtent = ownership;
    wrongAfterExtent.rebase.sceneAfterExtent = 8;
    CHECK_FALSE(wrongAfterExtent.IsValid());
    auto overflow = ownership;
    overflow.rebase.sceneBeforeExtent =
        std::numeric_limits<std::uint32_t>::max() - 1;
    overflow.rebase.sceneAppendBase = overflow.rebase.sceneBeforeExtent;
    overflow.rebase.sceneAfterExtent = std::numeric_limits<std::uint32_t>::max();
    CHECK_FALSE(overflow.IsValid());
    auto mismatched = ownership;
    mismatched.rebase.owned.kind = PrefabPropagationResourceKind::Material;
    CHECK_FALSE(mismatched.IsValid());

    SceneMaterial material;
    const auto materialValues = std::vector<PrefabPropagationResourcePayload>(
        std::vector<PrefabPropagationResourcePayload>{
            {"material:0", "digest-m", PrefabPropagationResourceValue{material}}});
    PrefabPropagationResourceOwnership materialOwnership;
    materialOwnership.rebase.kind = PrefabPropagationResourceKind::Material;
    materialOwnership.rebase.sourceBeforeExtent = 1;
    materialOwnership.rebase.sceneBeforeExtent = 2;
    materialOwnership.rebase.sceneAppendBase = 2;
    materialOwnership.rebase.sceneAfterExtent = 3;
    materialOwnership.rebase.sourceSlots = {{0}};
    materialOwnership.rebase.sceneSlots = {{2}};
    materialOwnership.rebase.owned = PrefabPropagationResourceBlock::FromDecoded(
        PrefabPropagationResourceKind::Material, materialValues);
    CHECK(materialOwnership.IsValid());
    const auto originalMaterial = material;
    material.baseColor = {0.0f, 0.0f, 0.0f};
    material.alphaMode.clear();
    CHECK(PrefabCanonicalMaterialEqual(
        std::get<SceneMaterial>(materialOwnership.rebase.owned.Entries()[0].decoded),
        originalMaterial));

    SceneTexture texture;
    texture.width = 1;
    texture.height = 1;
    texture.channels = 4;
    texture.pixels = {1, 2, 3, 4};
    const auto textureValues = std::vector<PrefabPropagationResourcePayload>(
        std::vector<PrefabPropagationResourcePayload>{
            {"texture:0", "digest-t", PrefabPropagationResourceValue{texture}}});
    PrefabPropagationResourceOwnership textureOwnership = materialOwnership;
    textureOwnership.rebase.kind = PrefabPropagationResourceKind::Texture;
    textureOwnership.rebase.owned = PrefabPropagationResourceBlock::FromDecoded(
        PrefabPropagationResourceKind::Texture, textureValues);
    CHECK(textureOwnership.IsValid());
    const auto originalTexture = texture;
    texture.width = 0;
    texture.pixels.clear();
    CHECK(PrefabPropagationTextureEqual(
        std::get<SceneTexture>(textureOwnership.rebase.owned.Entries()[0].decoded),
        originalTexture));

    PrefabPropagationPlan resourceOnly;
    resourceOnly.resourceOwnership.push_back(ownership);
    resourceOnly.syncImpact = SyncImpact::Structural;
    CHECK(resourceOnly.IsValid());
    CHECK_FALSE(resourceOnly.IsNoOp());
    CHECK(resourceOnly.IsEffective());

    auto duplicatePlan = resourceOnly;
    duplicatePlan.resourceOwnership.push_back(ownership);
    CHECK(duplicatePlan.resourceOwnership[0].IsValid());
    CHECK(duplicatePlan.resourceOwnership[1].IsValid());
    CHECK_FALSE(duplicatePlan.IsValid());

    CHECK(PropagationComponentImpact<Transform> == SyncImpact::Transform);
    CHECK(PropagationComponentImpact<CameraComponent> == SyncImpact::None);
    CHECK(PropagationComponentImpact<VisibleComponent> == SyncImpact::Structural);
    CHECK(PropagationComponentImpact<MotionComponent> == SyncImpact::None);
    CHECK(PrefabPropagationImpactForResource(PrefabPropagationResourceKind::Mesh) ==
          SyncImpact::Structural);
    CHECK(PrefabPropagationImpactForResource(PrefabPropagationResourceKind::Texture) ==
          SyncImpact::Structural);
    CHECK(PrefabPropagationImpactForResource(PrefabPropagationResourceKind::Material) ==
          SyncImpact::Material);

    auto invalidPlan = resourceOnly;
    invalidPlan.resourceOwnership[0].rebase.sceneAfterExtent = 8;
    CHECK_FALSE(invalidPlan.IsValid());
    CHECK_FALSE(invalidPlan.IsEffective());

    PrefabPropagationPlan summaryPlan;
    summaryPlan.componentOperations.push_back(
        PrefabPropagationComponentDelta::Make<NameComponent>(
            kEntity, kTemplate, NameComponent{"before"}, NameComponent{"after"}));
    CHECK_FALSE(summaryPlan.IsValid());
    CHECK_FALSE(summaryPlan.IsEffective());
    summaryPlan.affectedEntities = {kEntity};
    summaryPlan.syncImpact = SyncImpact::Material;
    CHECK_FALSE(summaryPlan.IsValid());
    CHECK_FALSE(summaryPlan.IsEffective());
    summaryPlan.syncImpact = SyncImpact::None;
    CHECK(summaryPlan.IsValid());
    CHECK(summaryPlan.IsEffective());

    PrefabPropagationPlan noOpOnly;
    noOpOnly.componentOperations.push_back(
        PrefabPropagationComponentDelta::Make<Transform>(
            kEntity, kTemplate, Transform{}, Transform{}));
    CHECK(noOpOnly.affectedEntities.empty());
    CHECK(noOpOnly.syncImpact == SyncImpact::None);
    CHECK(noOpOnly.IsValid());
    CHECK(noOpOnly.IsNoOp());
    CHECK_FALSE(noOpOnly.IsEffective());

    PrefabPropagationPlan mixed;
    mixed.componentOperations = noOpOnly.componentOperations;
    mixed.componentOperations.push_back(
        PrefabPropagationComponentDelta::Make<NameComponent>(
            kEntity, kTemplate, NameComponent{"before"}, NameComponent{"after"}));
    mixed.affectedEntities = {kEntity};
    mixed.syncImpact = SyncImpact::None;
    CHECK(mixed.IsValid());
    CHECK_FALSE(mixed.IsNoOp());
    CHECK(mixed.IsEffective());
}

TEST_CASE("Phase 8 W4 S1: plan and result equality cover complete durable state")
{
    PrefabPropagationPlan a;
    a.source.normalizedPath = "assets/a.rt2prefab";
    a.source.assetId = kInstance;
    a.source.contentDigest = "digest";
    a.syncImpact = SyncImpact::Structural;
    a.affectedEntities.push_back(kEntity);
    a.instances.push_back({kInstance, kEntity,
        PrefabPropagationInstanceDisposition::Propagate, {kEntity}, {}});
    auto b = a;
    CHECK(a == b);
    b.syncImpact = SyncImpact::Material;
    CHECK_FALSE(a == b);

}
