#include <doctest/doctest.h>

#include "PrefabComponentKey.h"
#include "PrefabComponentValueEquality.h"
#include "PrefabPropagationContracts.h"
#include "SceneSerializer.h"

#include <filesystem>
#include <fstream>
#include <string>

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
    CHECK(diagnostic.SortKey() < (diagnostic.SortKey() + "x"));

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
