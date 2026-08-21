#include <doctest/doctest.h>

#include "AssetIdentity.h"
#include "PrefabPropagationDiscovery.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace rt2::core;

namespace
{
UUID U(unsigned value)
{
    char text[48]{};
    std::snprintf(text, sizeof(text), "00000000-0000-4000-8000-%012u", value);
    return UUID::Parse(text);
}

const UUID kAsset = U(900);

struct TempGuard
{
    std::filesystem::path directory;
    TempGuard() = default;
    explicit TempGuard(std::filesystem::path value) : directory(std::move(value)) {}
    TempGuard(const TempGuard&) = delete;
    TempGuard& operator=(const TempGuard&) = delete;
    TempGuard(TempGuard&& other) noexcept : directory(std::move(other.directory))
    { other.directory.clear(); }
    ~TempGuard()
    {
        if (directory.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
        if (ec || std::filesystem::exists(directory, ec)) std::abort();
    }
};

TempGuard Temp()
{
    static std::atomic<unsigned> sequence{0};
    TempGuard guard{std::filesystem::temp_directory_path() /
        ("rt2_w4_s3_reconcile_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         "_" + std::to_string(sequence.fetch_add(1)))};
    std::error_code ec;
    if (!std::filesystem::create_directories(guard.directory, ec) || ec)
        throw std::runtime_error("S3 temporary directory creation failed");
    return guard;
}

std::string Bytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("S3 snapshot open failed");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

PrefabPropagationDiscoveryRequest Request(SceneDocument& document,
                                           const std::filesystem::path& source)
{
    PrefabPropagationDiscoveryRequest request;
    request.document = &document;
    request.assets.assetRoot = source.parent_path();
    request.changedSource = AssetReference{AssetKind::Prefab, source.string(), {}, {}, kAsset};
    request.documentGeneration = 101;
    request.resourceGeneration = 202;
    return request;
}

PrefabEntityRecord Record(unsigned templateId, unsigned recordId,
                          const UUID& parent = UUID::Nil())
{
    PrefabEntityRecord result;
    result.templateId = U(templateId);
    result.record.uuid = U(recordId);
    result.record.parentUuid = parent;
    result.record.name = templateId == 1 ? "Template" : "Child";
    return result;
}

std::filesystem::path WritePrefab(const TempGuard& temp,
                                  std::vector<PrefabEntityRecord> records)
{
    const auto source = temp.directory / "source.rt2prefab";
    PrefabDocument prefab;
    prefab.entities = std::move(records);
    Error error;
    REQUIRE(PrefabSerializer::Save(prefab, source, error));
    REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));
    return source;
}

entt::entity AddMember(SceneDocument& document, const UUID& entityUuid,
                       const UUID& instance, const UUID& templateId,
                       entt::entity parent, const PrefabComponentKey* marker = nullptr)
{
    const auto entity = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(entity, entityUuid));
    document.ecs.registry.emplace<Hierarchy>(entity, parent,
                                               std::vector<entt::entity>{});
    PrefabMemberComponent member{instance, templateId, {}};
    if (marker) member.overrides.push_back(*marker);
    document.ecs.registry.emplace<PrefabMemberComponent>(entity, member);
    if (parent != entt::null)
        document.ecs.registry.get<Hierarchy>(parent).children.push_back(entity);
    return entity;
}

SceneDocument MakeDocument(const std::filesystem::path& source,
                           entt::entity& root, entt::entity& child,
                           const UUID& instance,
                           const PrefabComponentKey* marker = nullptr)
{
    SceneDocument document;
    document.metadata.schemaVersion = SceneSerializer::SchemaVersion;
    root = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(root, U(200)));
    document.ecs.registry.emplace<Hierarchy>(root);
    document.ecs.registry.emplace<PrefabInstanceComponent>(
        root, PrefabInstanceComponent{
            AssetReference{AssetKind::Prefab, source.filename().string(), {}, {}, kAsset},
            instance});
    document.ecs.registry.emplace<PrefabMemberComponent>(
        root, PrefabMemberComponent{instance, U(1), {}});
    child = AddMember(document, U(201), instance, U(2), root, marker);
    return document;
}

enum class OptionalKind { Primitive, Material, Light, Camera, Motion, Script };

PrefabComponentKey Key(OptionalKind kind)
{
    switch (kind)
    {
    case OptionalKind::Primitive: return PrefabComponentKeyFor<PrimitiveComponent>::value;
    case OptionalKind::Material:  return PrefabComponentKeyFor<MaterialOverrideComponent>::value;
    case OptionalKind::Light:     return PrefabComponentKeyFor<LightComponent>::value;
    case OptionalKind::Camera:    return PrefabComponentKeyFor<CameraComponent>::value;
    case OptionalKind::Motion:    return PrefabComponentKeyFor<MotionComponent>::value;
    case OptionalKind::Script:    return PrefabComponentKeyFor<ScriptComponent>::value;
    }
    return {};
}

void SetSourceOptional(PrefabEntityRecord& record, OptionalKind kind, bool present)
{
    if (!present) return;
    switch (kind)
    {
    case OptionalKind::Primitive:
        record.record.hasPrimitive = true;
        record.record.primitive = PrimitiveComponent{PrimitiveComponent::Cube, 3.0f, 8, 6};
        break;
    case OptionalKind::Material:
        record.record.hasMaterialOverride = true;
        record.record.materialOverride.authored = true;
        record.record.materialOverride.material.baseColor = {0.2f, 0.3f, 0.4f};
        break;
    case OptionalKind::Light:
        record.record.hasLight = true;
        record.record.light.intensity = 4.0f;
        break;
    case OptionalKind::Camera:
        record.record.hasCamera = true;
        record.record.camera.verticalFOV = 33.0f;
        break;
    case OptionalKind::Motion:
        record.record.hasMotion = true;
        record.record.motion.linearVelocity = {1.0f, 2.0f, 3.0f};
        break;
    case OptionalKind::Script:
        record.record.hasScript = true;
        record.record.script.asset = AssetReference{AssetKind::Script, "logic.lua", {}, {}, U(700)};
        record.record.script.fieldValues["target"] =
            ScriptFieldEntry{ScriptFieldType::Uuid, U(101)};
        break;
    }
}

void SetLiveOptional(SceneDocument& document, entt::entity entity,
                     OptionalKind kind, bool present)
{
    if (!present) return;
    switch (kind)
    {
    case OptionalKind::Primitive:
        document.ecs.registry.emplace<PrimitiveComponent>(
            entity, PrimitiveComponent{PrimitiveComponent::Sphere, 9.0f, 4, 3});
        break;
    case OptionalKind::Material:
        document.ecs.registry.emplace<MaterialOverrideComponent>(entity);
        document.ecs.registry.get<MaterialOverrideComponent>(entity).authored = false;
        break;
    case OptionalKind::Light:
        document.ecs.registry.emplace<LightComponent>(entity);
        document.ecs.registry.get<LightComponent>(entity).intensity = 9.0f;
        break;
    case OptionalKind::Camera:
        document.ecs.registry.emplace<CameraComponent>(entity);
        document.ecs.registry.get<CameraComponent>(entity).verticalFOV = 77.0f;
        break;
    case OptionalKind::Motion:
        document.ecs.registry.emplace<MotionComponent>(entity);
        document.ecs.registry.get<MotionComponent>(entity).linearVelocity = {9.0f, 8.0f, 7.0f};
        break;
    case OptionalKind::Script:
        document.ecs.registry.emplace<ScriptComponent>(entity);
        document.ecs.registry.get<ScriptComponent>(entity).asset =
            AssetReference{AssetKind::Script, "local.lua", {}, {}, U(701)};
        document.ecs.registry.get<ScriptComponent>(entity).fieldValues["target"] =
            ScriptFieldEntry{ScriptFieldType::Uuid, U(999)};
        break;
    }
}

const PrefabPropagationComponentOperation* FindOperation(
    const PrefabPropagationPlan& plan, const UUID& entity,
    const PrefabComponentKey& key)
{
    for (const auto& operation : plan.componentOperations)
        if (operation.entityUuid == entity && operation.key == key)
            return &operation;
    return nullptr;
}
}

TEST_CASE("Phase 8 W4 S3: all optional marker presence states reconcile")
{
    const OptionalKind kinds[] = {OptionalKind::Primitive, OptionalKind::Material,
                                  OptionalKind::Light, OptionalKind::Camera,
                                  OptionalKind::Motion, OptionalKind::Script};
    for (const auto kind : kinds)
    for (const bool sourcePresent : {false, true})
    for (const bool livePresent : {false, true})
    for (const bool marked : {false, true})
    {
        const auto temp = Temp();
        auto records = std::vector<PrefabEntityRecord>{Record(1, 101), Record(2, 102, U(101))};
        SetSourceOptional(records[1], kind, sourcePresent);
        const auto source = WritePrefab(temp, records);
        entt::entity root, child;
        const auto marker = Key(kind);
        auto document = MakeDocument(source, root, child, U(300), marked ? &marker : nullptr);
        SetLiveOptional(document, child, kind, livePresent);
        const auto beforeMarkers = document.ecs.registry.get<PrefabMemberComponent>(child).overrides;
        const auto result = PreparePrefabPropagation(Request(document, source));
        REQUIRE(result.IsOk());
        REQUIRE(result.value.IsValid());
        const auto* operation = FindOperation(result.value, U(201), Key(kind));
        const bool shouldChange = !marked && (sourcePresent || livePresent);
        CHECK((operation != nullptr) == shouldChange);
        if (operation)
        {
            CHECK(operation->before.has_value() == livePresent);
            CHECK(operation->after.has_value() == sourcePresent);
        }
        CHECK(document.ecs.registry.get<PrefabMemberComponent>(child).overrides == beforeMarkers);
    }
}

TEST_CASE("Phase 8 W4 S3: naming, markers, and script UUID remap are deterministic")
{
    const auto temp = Temp();
    auto records = std::vector<PrefabEntityRecord>{Record(1, 101), Record(2, 102, U(101))};
    records[0].record.name = "New Copy";
    records[1].record.name = "Child New";
    records[0].record.hasScript = true;
    records[0].record.script.asset =
        AssetReference{AssetKind::Script, "logic.lua", {}, {}, U(700)};
    records[0].record.script.fieldValues["child"] =
        ScriptFieldEntry{ScriptFieldType::Uuid, U(102)};
    records[0].record.script.fieldValues["external"] =
        ScriptFieldEntry{ScriptFieldType::Uuid, U(999)};
    records[1].record.hasScript = true;
    records[1].record.script.asset =
        AssetReference{AssetKind::Script, "logic.lua", {}, {}, U(700)};
    records[1].record.script.fieldValues["root"] =
        ScriptFieldEntry{ScriptFieldType::Uuid, U(101)};
    const auto source = WritePrefab(temp, records);
    entt::entity root, child;
    auto document = MakeDocument(source, root, child, U(300));
    document.ecs.registry.emplace<NameComponent>(root, NameComponent{"Old Root"});
    document.ecs.registry.emplace<NameComponent>(child, NameComponent{"Old Child"});
    document.ecs.registry.emplace<ScriptComponent>(root);
    document.ecs.registry.emplace<ScriptComponent>(child);
    document.ecs.registry.get<ScriptComponent>(root).fieldValues["child"] =
        ScriptFieldEntry{ScriptFieldType::Uuid, U(999)};
    document.ecs.registry.get<ScriptComponent>(child).fieldValues["root"] =
        ScriptFieldEntry{ScriptFieldType::Uuid, U(998)};
    const auto result = PreparePrefabPropagation(Request(document, source));
    REQUIRE(result.IsOk());
    REQUIRE(result.value.IsValid());
    const auto* rootName = FindOperation(result.value, U(200),
                                         PrefabComponentKeyFor<NameComponent>::value);
    REQUIRE(rootName != nullptr);
    CHECK(std::get<NameComponent>(*rootName->after).name == "New Copy");
    const auto* childName = FindOperation(result.value, U(201),
                                          PrefabComponentKeyFor<NameComponent>::value);
    REQUIRE(childName != nullptr);
    CHECK(std::get<NameComponent>(*childName->after).name == "Child New");
    const auto* childScript = FindOperation(result.value, U(201),
                                            PrefabComponentKeyFor<ScriptComponent>::value);
    REQUIRE(childScript != nullptr);
    const auto& remapped = std::get<ScriptComponent>(*childScript->after);
    CHECK(std::get<UUID>(remapped.fieldValues.at("root").value) == U(200));
}

TEST_CASE("Phase 8 W4 S3: script remap is instance-local and preserves non-local fields")
{
    const auto temp = Temp();
    auto records = std::vector<PrefabEntityRecord>{Record(1, 101), Record(2, 102, U(101))};
    for (auto& record : records)
    {
        record.record.hasScript = true;
        record.record.script.asset =
            AssetReference{AssetKind::Script, "logic.lua", {}, {}, U(700)};
    }
    records[0].record.script.fieldValues["child"] =
        ScriptFieldEntry{ScriptFieldType::Uuid, U(102)};
    records[0].record.script.fieldValues["root"] =
        ScriptFieldEntry{ScriptFieldType::Uuid, U(101)};
    records[0].record.script.fieldValues["external"] =
        ScriptFieldEntry{ScriptFieldType::Uuid, U(999)};
    records[0].record.script.fieldValues["nil"] =
        ScriptFieldEntry{ScriptFieldType::Uuid, UUID::Nil()};
    records[0].record.script.fieldValues["stale"] =
        ScriptFieldEntry{ScriptFieldType::Uuid, U(998)};
    records[0].record.script.fieldValues["text"] =
        ScriptFieldEntry{ScriptFieldType::String, std::string("102")};
    const auto source = WritePrefab(temp, records);

    SceneDocument document;
    document.metadata.schemaVersion = SceneSerializer::SchemaVersion;
    for (unsigned instance = 0; instance != 2; ++instance)
    {
        const auto root = document.ecs.registry.create();
        const auto child = document.ecs.registry.create();
        const auto rootUuid = U(200 + instance * 10);
        const auto childUuid = U(201 + instance * 10);
        REQUIRE(document.AssignKnownUuid(root, rootUuid));
        REQUIRE(document.AssignKnownUuid(child, childUuid));
        document.ecs.registry.emplace<Hierarchy>(root);
        document.ecs.registry.emplace<Hierarchy>(child, root);
        document.ecs.registry.get<Hierarchy>(root).children.push_back(child);
        document.ecs.registry.emplace<PrefabInstanceComponent>(root,
            PrefabInstanceComponent{AssetReference{AssetKind::Prefab,
                source.filename().string(), {}, {}, kAsset}, U(300 + instance)});
        const auto marker = instance == 1
            ? std::optional<PrefabComponentKey>(PrefabComponentKeyFor<ScriptComponent>::value)
            : std::nullopt;
        document.ecs.registry.emplace<PrefabMemberComponent>(root,
            PrefabMemberComponent{U(300 + instance), U(1),
                marker ? std::vector<PrefabComponentKey>{*marker} : std::vector<PrefabComponentKey>{}});
        document.ecs.registry.emplace<PrefabMemberComponent>(child,
            PrefabMemberComponent{U(300 + instance), U(2), {}});
        document.ecs.registry.emplace<ScriptComponent>(root);
        document.ecs.registry.emplace<ScriptComponent>(child);
        if (instance == 1)
        {
            auto& local = document.ecs.registry.get<ScriptComponent>(root);
            local.asset = AssetReference{AssetKind::Script, "local.lua", {}, {}, U(701)};
            local.fieldValues["child"] = ScriptFieldEntry{ScriptFieldType::Uuid, U(777)};
        }
    }

    const auto result = PreparePrefabPropagation(Request(document, source));
    REQUIRE(result.IsOk());
    REQUIRE(result.value.IsValid());
    const auto* firstScript = FindOperation(result.value, U(200),
                                            PrefabComponentKeyFor<ScriptComponent>::value);
    REQUIRE(firstScript != nullptr);
    const auto& first = std::get<ScriptComponent>(*firstScript->after);
    CHECK(std::get<UUID>(first.fieldValues.at("child").value) == U(201));
    CHECK(std::get<UUID>(first.fieldValues.at("root").value) == U(200));
    CHECK(std::get<UUID>(first.fieldValues.at("external").value) == U(999));
    CHECK(std::get<UUID>(first.fieldValues.at("nil").value).IsNull());
    CHECK(std::get<UUID>(first.fieldValues.at("stale").value) == U(998));
    CHECK(std::get<std::string>(first.fieldValues.at("text").value) == "102");
    CHECK(FindOperation(result.value, U(210),
                        PrefabComponentKeyFor<ScriptComponent>::value) == nullptr);
    const auto secondRoot = document.FindByUuid(U(210));
    const bool hasSecondRoot = secondRoot != entt::null;
    REQUIRE(hasSecondRoot);
    CHECK(std::get<UUID>(document.ecs.registry.get<ScriptComponent>(secondRoot)
                              .fieldValues.at("child").value) == U(777));
}

TEST_CASE("Phase 8 W4 S3: marked values and explicit absence remain exact")
{
    const auto temp = Temp();
    auto records = std::vector<PrefabEntityRecord>{Record(1, 101), Record(2, 102, U(101))};
    records[1].record.hasLight = true;
    records[1].record.light.intensity = 3.0f;
    const auto source = WritePrefab(temp, records);
    entt::entity root, child;
    auto document = MakeDocument(source, root, child, U(300),
                                 &PrefabComponentKeyFor<LightComponent>::value);
    document.ecs.registry.emplace<LightComponent>(child);
    document.ecs.registry.get<LightComponent>(child).intensity = 17.0f;
    const auto beforeMarkers = document.ecs.registry.get<PrefabMemberComponent>(child).overrides;
    const auto result = PreparePrefabPropagation(Request(document, source));
    REQUIRE(result.IsOk());
    REQUIRE(result.value.IsValid());
    CHECK(FindOperation(result.value, U(201), PrefabComponentKeyFor<LightComponent>::value) == nullptr);
    CHECK(document.ecs.registry.get<PrefabMemberComponent>(child).overrides == beforeMarkers);

    const auto tempAbsent = Temp();
    auto absentRecords = std::vector<PrefabEntityRecord>{Record(1, 101), Record(2, 102, U(101))};
    absentRecords[1].record.hasLight = true;
    const auto absentSource = WritePrefab(tempAbsent, absentRecords);
    entt::entity absentRoot, absentChild;
    auto absentDocument = MakeDocument(absentSource, absentRoot, absentChild, U(301),
                                       &PrefabComponentKeyFor<LightComponent>::value);
    const auto absentResult = PreparePrefabPropagation(Request(absentDocument, absentSource));
    REQUIRE(absentResult.IsOk());
    REQUIRE(absentResult.value.IsValid());
    CHECK(FindOperation(absentResult.value, U(201),
                        PrefabComponentKeyFor<LightComponent>::value) == nullptr);
}

TEST_CASE("Phase 8 W4 S3: mandatory values obey marker presence and canonical replacement")
{
    const auto temp = Temp();
    auto records = std::vector<PrefabEntityRecord>{Record(1, 101), Record(2, 102, U(101))};
    records[1].record.translation = {1.0f, 2.0f, 3.0f};
    records[1].record.visible = true;
    const auto source = WritePrefab(temp, records);

    for (const auto& key : {PrefabComponentKeyFor<NameComponent>::value,
                            PrefabComponentKeyFor<Transform>::value,
                            PrefabComponentKeyFor<VisibleComponent>::value})
    {
        entt::entity root, child;
        auto document = MakeDocument(source, root, child, U(300), &key);
        document.ecs.registry.emplace<NameComponent>(child, NameComponent{"Local"});
        document.ecs.registry.emplace<Transform>(child);
        document.ecs.registry.get<Transform>(child).translation = {9.0f, 8.0f, 7.0f};
        document.ecs.registry.emplace<VisibleComponent>(child);
        document.ecs.registry.get<VisibleComponent>(child).visible = false;
        const auto beforeMarkers = document.ecs.registry.get<PrefabMemberComponent>(child).overrides;
        const auto result = PreparePrefabPropagation(Request(document, source));
        REQUIRE(result.IsOk());
        REQUIRE(result.value.IsValid());
        CHECK(FindOperation(result.value, U(201), key) == nullptr);
        CHECK(document.ecs.registry.get<PrefabMemberComponent>(child).overrides == beforeMarkers);
    }

    entt::entity root, child;
    auto document = MakeDocument(source, root, child, U(301));
    const auto result = PreparePrefabPropagation(Request(document, source));
    REQUIRE(result.IsOk());
    REQUIRE(result.value.IsValid());
    const auto* name = FindOperation(result.value, U(201),
                                     PrefabComponentKeyFor<NameComponent>::value);
    const auto* transform = FindOperation(result.value, U(201),
                                          PrefabComponentKeyFor<Transform>::value);
    const auto* visible = FindOperation(result.value, U(201),
                                        PrefabComponentKeyFor<VisibleComponent>::value);
    REQUIRE(name != nullptr);
    REQUIRE(transform != nullptr);
    REQUIRE(visible != nullptr);
    CHECK(std::get<NameComponent>(*name->after).name == "Child");
    CHECK(std::get<Transform>(*transform->after).translation == glm::vec3{1.0f, 2.0f, 3.0f});
    CHECK(std::get<VisibleComponent>(*visible->after).visible);
}

TEST_CASE("Phase 8 W4 S3: imported source is authoritative and MeshRef is derived")
{
    const auto temp = Temp();
    auto records = std::vector<PrefabEntityRecord>{Record(1, 101), Record(2, 102, U(101))};
    records[1].record.hasImportedSource = true;
    records[1].record.importedSource.model =
        AssetReference{AssetKind::Model, "new.glb", {}, "gltf:mesh=2", U(700)};
    const auto source = WritePrefab(temp, records);
    entt::entity root, child;
    auto document = MakeDocument(source, root, child, U(300));
    document.ecs.registry.emplace<ImportedMeshSourceComponent>(child);
    document.ecs.registry.emplace<MeshRef>(child, MeshRef{77, 5});
    const auto result = PreparePrefabPropagation(Request(document, source));
    REQUIRE(result.IsOk());
    REQUIRE(result.value.IsValid());
    const auto* operation = FindOperation(
        result.value, U(201), PrefabComponentKeyFor<ImportedMeshSourceComponent>::value);
    REQUIRE(operation != nullptr);
    CHECK(std::get<ImportedMeshSourceComponent>(*operation->after).model.sourceKey ==
          "gltf:mesh=2");
    CHECK(FindOperation(result.value, U(201), PrefabComponentKeyFor<MeshRef>::value) == nullptr);
    CHECK(document.ecs.registry.get<MeshRef>(child).meshIndex == 77);
    CHECK(document.ecs.registry.get<PrefabInstanceComponent>(root).instanceId == U(300));
}

TEST_CASE("Phase 8 W4 S3: clean geometry provenance transitions are planned without resources")
{
    const auto temp = Temp();
    auto primitiveRecords = std::vector<PrefabEntityRecord>{Record(1, 101), Record(2, 102, U(101))};
    primitiveRecords[1].record.hasPrimitive = true;
    primitiveRecords[1].record.primitive = PrimitiveComponent{PrimitiveComponent::Cube, 2.0f, 4, 3};
    const auto primitiveSource = WritePrefab(temp, primitiveRecords);
    entt::entity primitiveRoot, primitiveChild;
    auto primitiveDocument = MakeDocument(primitiveSource, primitiveRoot, primitiveChild, U(300));
    primitiveDocument.ecs.registry.emplace<ImportedMeshSourceComponent>(primitiveChild);
    primitiveDocument.ecs.registry.get<ImportedMeshSourceComponent>(primitiveChild).model =
        AssetReference{AssetKind::Model, "old.glb", {}, "gltf:mesh=1", U(701)};
    const auto primitiveResult = PreparePrefabPropagation(Request(primitiveDocument, primitiveSource));
    REQUIRE(primitiveResult.IsOk());
    REQUIRE(primitiveResult.value.IsValid());
    const auto* primitive = FindOperation(primitiveResult.value, U(201),
                                          PrefabComponentKeyFor<PrimitiveComponent>::value);
    const auto* removedImported = FindOperation(primitiveResult.value, U(201),
                                                PrefabComponentKeyFor<ImportedMeshSourceComponent>::value);
    REQUIRE(primitive != nullptr);
    REQUIRE(removedImported != nullptr);
    CHECK(primitive->before.has_value() == false);
    CHECK(primitive->after.has_value());
    CHECK(removedImported->before.has_value());
    CHECK(removedImported->after.has_value() == false);

    const auto importedTemp = Temp();
    auto importedRecords = std::vector<PrefabEntityRecord>{Record(1, 101), Record(2, 102, U(101))};
    importedRecords[1].record.hasImportedSource = true;
    importedRecords[1].record.importedSource.model =
        AssetReference{AssetKind::Model, "new.glb", {}, "gltf:mesh=2", U(702)};
    const auto importedSource = WritePrefab(importedTemp, importedRecords);
    entt::entity importedRoot, importedChild;
    auto importedDocument = MakeDocument(importedSource, importedRoot, importedChild, U(301));
    importedDocument.ecs.registry.emplace<PrimitiveComponent>(importedChild);
    importedDocument.ecs.registry.get<PrimitiveComponent>(importedChild).size = 8.0f;
    const auto importedResult = PreparePrefabPropagation(Request(importedDocument, importedSource));
    REQUIRE(importedResult.IsOk());
    REQUIRE(importedResult.value.IsValid());
    const auto* removedPrimitive = FindOperation(importedResult.value, U(201),
                                                 PrefabComponentKeyFor<PrimitiveComponent>::value);
    const auto* imported = FindOperation(importedResult.value, U(201),
                                         PrefabComponentKeyFor<ImportedMeshSourceComponent>::value);
    REQUIRE(removedPrimitive != nullptr);
    REQUIRE(imported != nullptr);
    CHECK(removedPrimitive->before.has_value());
    CHECK(removedPrimitive->after.has_value() == false);
    CHECK(imported->before.has_value() == false);
    CHECK(imported->after.has_value());
}

TEST_CASE("Phase 8 W4 S3: provenance conflicts quarantine only the bad sibling")
{
    const auto temp = Temp();
    auto records = std::vector<PrefabEntityRecord>{Record(1, 101), Record(2, 102, U(101))};
    records[1].record.hasImportedSource = true;
    records[1].record.importedSource.model =
        AssetReference{AssetKind::Model, "new.glb", {}, "gltf:mesh=2", U(700)};
    const auto source = WritePrefab(temp, records);
    SceneDocument document;
    document.metadata.schemaVersion = SceneSerializer::SchemaVersion;
    for (unsigned instance = 0; instance != 2; ++instance)
    {
        const auto root = document.ecs.registry.create();
        const auto rootUuid = U(200 + instance * 10);
        const auto childUuid = U(201 + instance * 10);
        REQUIRE(document.AssignKnownUuid(root, rootUuid));
        document.ecs.registry.emplace<Hierarchy>(root);
        document.ecs.registry.emplace<PrefabInstanceComponent>(
            root, PrefabInstanceComponent{
                AssetReference{AssetKind::Prefab, source.filename().string(), {}, {}, kAsset},
                U(300 + instance)});
        document.ecs.registry.emplace<PrefabMemberComponent>(
            root, PrefabMemberComponent{U(300 + instance), U(1), {}});
        const auto child = AddMember(document, childUuid, U(300 + instance), U(2), root,
            instance == 0 ? &PrefabComponentKeyFor<PrimitiveComponent>::value : nullptr);
        if (instance == 0)
            document.ecs.registry.emplace<PrimitiveComponent>(child);
    }
    const auto result = PreparePrefabPropagation(Request(document, source));
    REQUIRE(result.IsOk());
    REQUIRE(result.value.instances.size() == 2);
    CHECK(result.value.instances[0].disposition == PrefabPropagationInstanceDisposition::Quarantined);
    CHECK(result.value.instances[1].disposition == PrefabPropagationInstanceDisposition::Propagate);
    CHECK(result.value.instances[0].affectedEntities.empty());
    CHECK(!result.value.componentOperations.empty());
}

TEST_CASE("Phase 8 W4 S3: imported-only material conflict quarantines only its instance")
{
    const auto temp = Temp();
    auto records = std::vector<PrefabEntityRecord>{Record(1, 101), Record(2, 102, U(101))};
    records[1].record.hasPrimitive = true;
    records[1].record.primitive = PrimitiveComponent{PrimitiveComponent::Cube, 1.0f, 3, 3};
    const auto source = WritePrefab(temp, records);
    SceneDocument document;
    document.metadata.schemaVersion = SceneSerializer::SchemaVersion;
    for (unsigned instance = 0; instance != 2; ++instance)
    {
        const auto root = document.ecs.registry.create();
        const auto rootUuid = U(200 + instance * 10);
        const auto childUuid = U(201 + instance * 10);
        REQUIRE(document.AssignKnownUuid(root, rootUuid));
        document.ecs.registry.emplace<Hierarchy>(root);
        document.ecs.registry.emplace<PrefabInstanceComponent>(root,
            PrefabInstanceComponent{AssetReference{AssetKind::Prefab,
                source.filename().string(), {}, {}, kAsset}, U(300 + instance)});
        document.ecs.registry.emplace<PrefabMemberComponent>(root,
            PrefabMemberComponent{U(300 + instance), U(1), {}});
        const auto marker = instance == 0
            ? &PrefabComponentKeyFor<MaterialOverrideComponent>::value : nullptr;
        const auto child = AddMember(document, childUuid, U(300 + instance), U(2), root, marker);
        if (instance == 0)
        {
            document.ecs.registry.emplace<ImportedMeshSourceComponent>(child);
            document.ecs.registry.emplace<MaterialOverrideComponent>(child);
        }
    }
    const auto result = PreparePrefabPropagation(Request(document, source));
    REQUIRE(result.IsOk());
    REQUIRE(result.value.instances.size() == 2);
    CHECK(result.value.instances[0].disposition == PrefabPropagationInstanceDisposition::Quarantined);
    CHECK(result.value.instances[1].disposition == PrefabPropagationInstanceDisposition::Propagate);
    CHECK(result.value.instances[0].affectedEntities.empty());
    CHECK(!result.value.componentOperations.empty());
}

TEST_CASE("Phase 8 W4 S3: preparation is a pure no-op and omits exact operations")
{
    const auto temp = Temp();
    const auto source = WritePrefab(temp, {Record(1, 101), Record(2, 102, U(101))});
    entt::entity root, child;
    auto document = MakeDocument(source, root, child, U(300));
    document.ecs.registry.emplace<NameComponent>(root, NameComponent{"Template Copy"});
    document.ecs.registry.emplace<NameComponent>(child, NameComponent{"Child"});
    document.ecs.registry.emplace<Transform>(root);
    document.ecs.registry.emplace<Transform>(child);
    document.ecs.registry.emplace<VisibleComponent>(root);
    document.ecs.registry.emplace<VisibleComponent>(child);
    std::vector<AssetDiagnostic> diagnostics;
    Error error;
    const auto beforePath = temp.directory / "before.rt2scene";
    const auto afterPath = temp.directory / "after.rt2scene";
    REQUIRE(SceneSerializer::Save(document, beforePath, diagnostics, error));
    const auto before = Bytes(beforePath);
    const auto beforeSource = Bytes(source);
    const auto beforeSidecar = Bytes(AssetSidecarPath(source));
    const auto beforeSchema = document.metadata.schemaVersion;
    const auto beforeDirty = document.metadata.dirty;
    const auto beforeEntityCount = document.ecs.registry.storage<EntityIdComponent>().size();
    const auto beforeUuidIndex = document.uuidIndex.All();
    const auto markers = document.ecs.registry.get<PrefabMemberComponent>(child).overrides;
    const auto result = PreparePrefabPropagation(Request(document, source));
    REQUIRE(result.IsOk());
    REQUIRE(result.value.IsValid());
    CHECK(result.value.componentOperations.empty());
    CHECK(result.value.instances.front().disposition == PrefabPropagationInstanceDisposition::NoOp);
    CHECK(result.value.affectedEntities.empty());
    CHECK(result.value.syncImpact == SyncImpact::None);
    CHECK(SceneSerializer::Save(document, afterPath, diagnostics, error));
    CHECK(Bytes(afterPath) == before);
    CHECK(Bytes(source) == beforeSource);
    CHECK(Bytes(AssetSidecarPath(source)) == beforeSidecar);
    CHECK(document.metadata.schemaVersion == beforeSchema);
    CHECK(document.metadata.dirty == beforeDirty);
    CHECK(document.ecs.registry.storage<EntityIdComponent>().size() == beforeEntityCount);
    CHECK(document.uuidIndex.All() == beforeUuidIndex);
    CHECK(document.ecs.registry.get<PrefabMemberComponent>(child).overrides == markers);
}

TEST_CASE("Phase 8 W4 S3: shuffled source and registry order produce identical operations")
{
    const auto firstTemp = Temp();
    const auto secondTemp = Temp();
    const auto records = std::vector<PrefabEntityRecord>{Record(1, 101), Record(2, 102, U(101))};
    const auto firstSource = WritePrefab(firstTemp, records);
    auto reversed = records;
    std::reverse(reversed.begin(), reversed.end());
    const auto secondSource = WritePrefab(secondTemp, reversed);

    entt::entity firstRoot, firstChild;
    auto firstDocument = MakeDocument(firstSource, firstRoot, firstChild, U(300));
    SceneDocument secondDocument;
    secondDocument.metadata.schemaVersion = SceneSerializer::SchemaVersion;
    const auto secondChild = secondDocument.ecs.registry.create();
    REQUIRE(secondDocument.AssignKnownUuid(secondChild, U(201)));
    secondDocument.ecs.registry.emplace<Hierarchy>(secondChild);
    secondDocument.ecs.registry.emplace<PrefabMemberComponent>(secondChild,
        PrefabMemberComponent{U(300), U(2), {}});
    const auto secondRoot = secondDocument.ecs.registry.create();
    REQUIRE(secondDocument.AssignKnownUuid(secondRoot, U(200)));
    secondDocument.ecs.registry.emplace<Hierarchy>(secondRoot);
    secondDocument.ecs.registry.get<Hierarchy>(secondRoot).children.push_back(secondChild);
    secondDocument.ecs.registry.get<Hierarchy>(secondChild).parent = secondRoot;
    secondDocument.ecs.registry.emplace<PrefabInstanceComponent>(secondRoot,
        PrefabInstanceComponent{AssetReference{AssetKind::Prefab,
            secondSource.filename().string(), {}, {}, kAsset}, U(300)});
    secondDocument.ecs.registry.emplace<PrefabMemberComponent>(secondRoot,
        PrefabMemberComponent{U(300), U(1), {}});

    const auto first = PreparePrefabPropagation(Request(firstDocument, firstSource));
    const auto second = PreparePrefabPropagation(Request(secondDocument, secondSource));
    REQUIRE(first.IsOk());
    REQUIRE(second.IsOk());
    REQUIRE(first.value.componentOperations.size() == second.value.componentOperations.size());
    for (std::size_t i = 0; i < first.value.componentOperations.size(); ++i)
        CHECK(first.value.componentOperations[i] == second.value.componentOperations[i]);
}
