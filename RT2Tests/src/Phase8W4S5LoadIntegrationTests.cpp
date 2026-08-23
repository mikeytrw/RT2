#include <doctest/doctest.h>

#include "AssetIdentity.h"
#include "PrefabPropagationService.h"
#include "SceneRecoveryService.h"
#include "SceneManager.h"
#include "SceneSerializer.h"

#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

using namespace rt2::core;

namespace
{
UUID U(unsigned value)
{
    char text[48]{};
    std::snprintf(text, sizeof(text), "00000000-0000-4000-8000-%012u", value);
    return UUID::Parse(text);
}

const UUID kAsset = U(9000);
const UUID kInstanceA = U(9100);
const UUID kInstanceB = U(9200);

struct TempGuard
{
    std::filesystem::path directory;
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

struct DirectoryAliasGuard
{
    std::filesystem::path alias;
    bool armed = false;
    DirectoryAliasGuard() = default;
    DirectoryAliasGuard(const DirectoryAliasGuard&) = delete;
    DirectoryAliasGuard& operator=(const DirectoryAliasGuard&) = delete;
    DirectoryAliasGuard(DirectoryAliasGuard&& other) noexcept
        : alias(std::move(other.alias)), armed(other.armed)
    { other.armed = false; }
    ~DirectoryAliasGuard()
    {
        if (!armed) return;
        std::error_code ec;
        std::filesystem::remove(alias, ec);
        if (ec || std::filesystem::exists(alias, ec)) std::abort();
    }
};

DirectoryAliasGuard MakeDirectoryAlias(const std::filesystem::path& target,
                                       const std::filesystem::path& alias)
{
    DirectoryAliasGuard result;
    result.alias = alias;
    std::error_code ec;
#ifdef _WIN32
    const std::string command = "cmd.exe /d /c mklink /J \"" +
        alias.string() + "\" \"" + target.string() + "\" >nul 2>&1";
    if (std::system(command.c_str()) != 0)
        throw std::runtime_error("S5 checked directory junction creation failed");
#else
    std::filesystem::create_directory_symlink(target, alias, ec);
    if (ec)
        throw std::runtime_error("S5 checked directory symlink creation failed");
#endif
    if (!std::filesystem::is_directory(alias, ec) || ec)
        throw std::runtime_error("S5 directory alias is not a directory");
    result.armed = true;
    return result;
}

std::filesystem::path ForcedCanonicalFailure(
    const std::filesystem::path&, std::error_code& ec)
{
    ec = std::make_error_code(std::errc::permission_denied);
    return {};
}

TempGuard Temp()
{
    static std::atomic<unsigned> sequence{0};
    TempGuard result{std::filesystem::temp_directory_path() /
        ("rt2_w4_s5_load_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         "_" + std::to_string(sequence.fetch_add(1)))};
    std::error_code ec;
    if (!std::filesystem::create_directories(result.directory, ec) || ec)
        throw std::runtime_error("S5 temporary directory creation failed");
    return result;
}

std::filesystem::path WritePrefab(const TempGuard& temp, bool malformed = false)
{
    const auto source = temp.directory / "source.rt2prefab";
    Error error;
    if (malformed)
    {
        std::ofstream output(source, std::ios::binary);
        output << "not a prefab";
        output.close();
    }
    else
    {
        PrefabDocument prefab;
        PrefabEntityRecord root;
        root.templateId = U(1);
        root.record.uuid = U(100);
        root.record.name = "SourceRoot";
        root.record.translation = {1.0f, 2.0f, 3.0f};
        PrefabEntityRecord child;
        child.templateId = U(2);
        child.record.uuid = U(101);
        child.record.parentUuid = U(100);
        child.record.name = "SourceChild";
        child.record.translation = {4.0f, 5.0f, 6.0f};
        prefab.entities = {root, child};
        REQUIRE(PrefabSerializer::Save(prefab, source, error));
    }
    REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));
    return source;
}

entt::entity AddMember(SceneDocument& document, const UUID& entityUuid,
                       const UUID& instance, const UUID& templateId,
                       entt::entity parent)
{
    const auto entity = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(entity, entityUuid));
    document.ecs.registry.emplace<Hierarchy>(entity, parent,
                                               std::vector<entt::entity>{});
    document.ecs.registry.emplace<PrefabMemberComponent>(
        entity, PrefabMemberComponent{instance, templateId, {}});
    document.ecs.registry.emplace<NameComponent>(entity, NameComponent{"Old"});
    document.ecs.registry.emplace<Transform>(entity);
    document.ecs.registry.emplace<VisibleComponent>(entity);
    if (parent != entt::null)
        document.ecs.registry.get<Hierarchy>(parent).children.push_back(entity);
    return entity;
}

SceneDocument MakeDocument(const std::filesystem::path& source,
                           const UUID& instance = kInstanceA,
                           bool addMalformedSibling = false,
                           const std::string& pathOverride = {})
{
    SceneDocument document;
    document.metadata.schemaVersion = SceneSerializer::SchemaVersion;
    document.metadata.assetRoot = source.parent_path();
    const auto root = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(root, U(200)));
    document.ecs.registry.emplace<Hierarchy>(root);
    document.ecs.registry.emplace<PrefabInstanceComponent>(root,
        PrefabInstanceComponent{AssetReference{AssetKind::Prefab,
            pathOverride.empty() ? source.filename().string() : pathOverride,
            {}, {}, kAsset}, instance});
    document.ecs.registry.emplace<PrefabMemberComponent>(root,
        PrefabMemberComponent{instance, U(1), {}});
    document.ecs.registry.emplace<NameComponent>(root, NameComponent{"OldRoot"});
    document.ecs.registry.emplace<Transform>(root);
    document.ecs.registry.emplace<VisibleComponent>(root);
    AddMember(document, U(201), instance, U(2), root);

    if (addMalformedSibling)
    {
        const auto badRoot = document.ecs.registry.create();
        REQUIRE(document.AssignKnownUuid(badRoot, U(300)));
        document.ecs.registry.emplace<Hierarchy>(badRoot);
        document.ecs.registry.emplace<PrefabInstanceComponent>(badRoot,
            PrefabInstanceComponent{AssetReference{AssetKind::Prefab,
                source.filename().string(), {}, {}, kAsset}, kInstanceB});
        document.ecs.registry.emplace<PrefabMemberComponent>(badRoot,
            PrefabMemberComponent{kInstanceB, U(1), {}});
        document.ecs.registry.emplace<NameComponent>(badRoot, NameComponent{"BadOld"});
        document.ecs.registry.emplace<Transform>(badRoot);
        document.ecs.registry.emplace<VisibleComponent>(badRoot);
        // The extra template ID deliberately quarantines only this instance.
        AddMember(document, U(301), kInstanceB, U(999), badRoot);
    }
    return document;
}

std::string Bytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void AddInstance(SceneDocument& document, const std::filesystem::path& source,
                 const UUID& instance, const UUID& rootUuid,
                 const UUID& childUuid, const std::string& pathOverride = {})
{
    const auto root = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(root, rootUuid));
    document.ecs.registry.emplace<Hierarchy>(root);
    document.ecs.registry.emplace<PrefabInstanceComponent>(root,
        PrefabInstanceComponent{AssetReference{AssetKind::Prefab,
            pathOverride.empty() ? source.filename().string() : pathOverride,
            {}, {}, kAsset}, instance});
    document.ecs.registry.emplace<PrefabMemberComponent>(root,
        PrefabMemberComponent{instance, U(1), {}});
    document.ecs.registry.emplace<NameComponent>(root, NameComponent{"OldRoot"});
    document.ecs.registry.emplace<Transform>(root);
    document.ecs.registry.emplace<VisibleComponent>(root);
    AddMember(document, childUuid, instance, U(2), root);
}

std::string SerializedSnapshot(const SceneDocument& document,
                               const std::filesystem::path& path)
{
    std::vector<AssetDiagnostic> diagnostics;
    Error error;
    REQUIRE(SceneSerializer::Save(document, path, diagnostics, error));
    return Bytes(path);
}

template <typename T>
void AppendRaw(std::ostringstream& out, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
void AppendRawVector(std::ostringstream& out, const std::vector<T>& values)
{
    AppendRaw(out, values.size());
    if (!values.empty())
    {
        static_assert(std::is_trivially_copyable_v<T>);
        out.write(reinterpret_cast<const char*>(values.data()),
                  static_cast<std::streamsize>(values.size() * sizeof(T)));
    }
}

void AppendString(std::ostringstream& out, const std::string& value)
{
    AppendRaw(out, value.size());
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
}

void AppendPath(std::ostringstream& out, const std::filesystem::path& value)
{
    AppendString(out, value.generic_string());
}

void AppendUuid(std::ostringstream& out, const UUID& value)
{
    AppendString(out, value.ToString());
}

void AppendAsset(std::ostringstream& out, const AssetReference& value)
{
    AppendRaw(out, value.kind);
    AppendString(out, value.path);
    AppendRaw(out, value.importSettings.triangulate);
    AppendRaw(out, value.importSettings.generateNormals);
    AppendRaw(out, value.importSettings.mergeMegaMesh);
    AppendRaw(out, value.importSettings.assumeDielectricWithoutMetalRough);
    AppendString(out, value.sourceKey);
    AppendUuid(out, value.assetId);
}

void AppendMaterial(std::ostringstream& out, const SceneMaterial& value)
{
    AppendRaw(out, value.type);
    AppendRaw(out, value.baseColor); AppendRaw(out, value.baseAlpha);
    AppendRaw(out, value.metallic); AppendRaw(out, value.roughness);
    AppendRaw(out, value.ior); AppendRaw(out, value.transmissionFactor);
    AppendRaw(out, value.emissiveColor); AppendRaw(out, value.emissiveIntensity);
    AppendRaw(out, value.baseColorTextureIndex);
    AppendRaw(out, value.normalTextureIndex);
    AppendRaw(out, value.emissiveTextureIndex);
    AppendRaw(out, value.metallicRoughnessTextureIndex);
    AppendString(out, value.alphaMode); AppendRaw(out, value.alphaCutoff);
    AppendString(out, value.sourceKey);
}

void AppendTexture(std::ostringstream& out, const SceneTexture& value)
{
    AppendAsset(out, value.ref);
    AppendRaw(out, value.width); AppendRaw(out, value.height);
    AppendRaw(out, value.channels); AppendRawVector(out, value.pixels);
    AppendRaw(out, value.isHDR); AppendRawVector(out, value.floatPixels);
    AppendRaw(out, value.isSRGB);
}

void AppendMesh(std::ostringstream& out, const MeshData& value)
{
    AppendRawVector(out, value.vertices); AppendRawVector(out, value.indices);
    AppendRawVector(out, value.normals); AppendRawVector(out, value.uvs);
    AppendRawVector(out, value.tangents);
    AppendRawVector(out, value.materialIndices); AppendString(out, value.name);
    AppendRaw(out, value.boundsMin); AppendRaw(out, value.boundsMax);
    AppendRaw(out, value.boundsValid);
}

void AppendFields(std::ostringstream& out, const rt2::core::ScriptFieldMap& fields)
{
    std::vector<std::string> names;
    for (const auto& [name, entry] : fields) names.push_back(name);
    std::sort(names.begin(), names.end());
    AppendRaw(out, names.size());
    for (const auto& name : names)
    {
        AppendString(out, name);
        const auto& entry = fields.at(name);
        AppendRaw(out, entry.type);
        AppendRaw(out, entry.value.index());
        std::visit([&](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, std::string>)
                AppendString(out, value);
            else if constexpr (std::is_same_v<Value, UUID>)
                AppendUuid(out, value);
            else if constexpr (std::is_same_v<Value, glm::vec3>)
                AppendRaw(out, value);
            else
                AppendRaw(out, value);
        }, entry.value);
    }
}

void AppendComponentState(std::ostringstream& out, const entt::registry& registry,
                          entt::entity entity)
{
    const auto appendPresence = [&](bool present) { AppendRaw(out, present); };
    if (const auto* value = registry.try_get<Transform>(entity))
    { appendPresence(true); AppendRaw(out, *value); } else appendPresence(false);
    if (const auto* value = registry.try_get<Hierarchy>(entity))
    {
        appendPresence(true); AppendRaw(out, value->parent);
        AppendRawVector(out, value->children);
    } else appendPresence(false);
    if (const auto* value = registry.try_get<MeshRef>(entity))
    { appendPresence(true); AppendRaw(out, *value); } else appendPresence(false);
    if (const auto* value = registry.try_get<LightComponent>(entity))
    { appendPresence(true); AppendRaw(out, *value); } else appendPresence(false);
    if (const auto* value = registry.try_get<CameraComponent>(entity))
    { appendPresence(true); AppendRaw(out, *value); } else appendPresence(false);
    if (const auto* value = registry.try_get<NameComponent>(entity))
    { appendPresence(true); AppendString(out, value->name); } else appendPresence(false);
    if (const auto* value = registry.try_get<VisibleComponent>(entity))
    { appendPresence(true); AppendRaw(out, *value); } else appendPresence(false);
    if (const auto* value = registry.try_get<EntityIdComponent>(entity))
    { appendPresence(true); AppendUuid(out, value->id); } else appendPresence(false);
    if (const auto* value = registry.try_get<PrimitiveComponent>(entity))
    {
        appendPresence(true); AppendRaw(out, value->kind); AppendRaw(out, value->size);
        AppendRaw(out, value->segments); AppendRaw(out, value->rings);
    } else appendPresence(false);
    if (const auto* value = registry.try_get<MotionComponent>(entity))
    { appendPresence(true); AppendRaw(out, *value); } else appendPresence(false);
    if (const auto* value = registry.try_get<ImportedMeshSourceComponent>(entity))
    { appendPresence(true); AppendAsset(out, value->model); } else appendPresence(false);
    if (const auto* value = registry.try_get<MaterialOverrideComponent>(entity))
    {
        appendPresence(true); AppendMaterial(out, value->material);
        AppendRaw(out, value->authored); AppendString(out, value->sourceMaterialKey);
        AppendRaw(out, value->materialIndex);
    } else appendPresence(false);
    if (const auto* value = registry.try_get<ScriptComponent>(entity))
    {
        appendPresence(true); AppendAsset(out, value->asset);
        AppendFields(out, value->fieldValues);
    } else appendPresence(false);
    if (const auto* value = registry.try_get<PrefabInstanceComponent>(entity))
    { appendPresence(true); AppendAsset(out, value->prefab); AppendUuid(out, value->instanceId); }
    else appendPresence(false);
    if (const auto* value = registry.try_get<PrefabMemberComponent>(entity))
    {
        appendPresence(true); AppendUuid(out, value->instanceId);
        AppendUuid(out, value->templateId); AppendRaw(out, value->overrides.size());
        for (const auto& key : value->overrides)
        { AppendString(out, std::string(key.wire())); AppendRaw(out, key.overridable()); }
    } else appendPresence(false);
}

void AppendGpu(std::ostringstream& out, const GPUSceneData& value)
{
    AppendRaw(out, value.meshes.size());
    for (const auto& mesh : value.meshes)
    {
        AppendRaw(out, mesh.vertices != nullptr);
        if (mesh.vertices) AppendRawVector(out, *mesh.vertices);
        AppendRaw(out, mesh.indices != nullptr);
        if (mesh.indices) AppendRawVector(out, *mesh.indices);
        AppendRaw(out, mesh.normals != nullptr);
        if (mesh.normals) AppendRawVector(out, *mesh.normals);
        AppendRaw(out, mesh.uvs != nullptr);
        if (mesh.uvs) AppendRawVector(out, *mesh.uvs);
        AppendRaw(out, mesh.tangents != nullptr);
        if (mesh.tangents) AppendRawVector(out, *mesh.tangents);
        AppendRaw(out, mesh.materialIndices != nullptr);
        if (mesh.materialIndices) AppendRawVector(out, *mesh.materialIndices);
        AppendRaw(out, mesh.materialIndex);
    }
    AppendRawVector(out, value.instances); AppendRawVector(out, value.materials);
    AppendRaw(out, value.textures.size());
    for (const auto& texture : value.textures) AppendTexture(out, texture);
    AppendRawVector(out, value.lights); AppendRawVector(out, value.punctualLights);
    AppendRaw(out, value.emissiveTextureOccupancy.size());
    for (const auto& occupancy : value.emissiveTextureOccupancy)
    { AppendRaw(out, occupancy.blockWidth); AppendRaw(out, occupancy.blockHeight);
      AppendRawVector(out, occupancy.summedArea); }
    AppendRaw(out, value.totalLightArea);
    AppendRaw(out, value.sourceEmissiveTriangleCount);
    AppendRaw(out, value.filteredBlackEmissiveTriangleCount);
    AppendRaw(out, value.envMapIndex); AppendRaw(out, value.envIntensity);
    AppendRawVector(out, value.marginalCDF); AppendRawVector(out, value.conditionalCDF);
    AppendRaw(out, value.cdfWidth); AppendRaw(out, value.cdfHeight);
}

std::string DeepDocumentSnapshot(const SceneDocument& document)
{
    std::ostringstream out(std::ios::binary);
    AppendRaw(out, document.GetUuidProvider());
    AppendRaw(out, document.metadata.schemaVersion);
    AppendPath(out, document.metadata.sourcePath);
    AppendString(out, document.metadata.name);
    AppendUuid(out, document.metadata.projectId);
    AppendPath(out, document.metadata.assetRoot);
    AppendRaw(out, document.metadata.dirty);
    AppendAsset(out, document.environment.ref);
    AppendRaw(out, document.environment.width); AppendRaw(out, document.environment.height);
    AppendRawVector(out, document.environment.floatPixels);
    AppendRaw(out, document.ecs.camera);
    AppendRaw(out, document.ecs.materials.size());
    for (const auto& value : document.ecs.materials) AppendMaterial(out, value);
    AppendRaw(out, document.ecs.textures.size());
    for (const auto& value : document.ecs.textures) AppendTexture(out, value);
    AppendRaw(out, document.ecs.meshRegistry.GetCount());
    for (const auto& value : document.ecs.meshRegistry.GetMeshes()) AppendMesh(out, value);
    std::vector<entt::entity> entities;
    for (const auto entity : document.ecs.registry.view<EntityIdComponent>())
        entities.push_back(entity);
    std::sort(entities.begin(), entities.end(), [](auto a, auto b) {
        return entt::to_entity(a) < entt::to_entity(b);
    });
    AppendRaw(out, entities.size());
    for (const auto entity : entities)
    { AppendRaw(out, entity); AppendComponentState(out, document.ecs.registry, entity); }
    std::vector<std::pair<std::string, entt::entity>> index;
    for (const auto& [uuid, entity] : document.uuidIndex.All())
        index.emplace_back(uuid.ToString(), entity);
    std::sort(index.begin(), index.end());
    AppendRaw(out, index.size());
    for (const auto& [uuid, entity] : index) { AppendString(out, uuid); AppendRaw(out, entity); }
    AppendGpu(out, document.gpuCache);
    return out.str();
}

std::string SourceFilesSnapshot(const std::vector<std::filesystem::path>& paths)
{
    std::ostringstream out(std::ios::binary);
    for (const auto& path : paths)
    {
        AppendPath(out, path);
        std::error_code ec;
        AppendRaw(out, std::filesystem::exists(path, ec));
        AppendString(out, Bytes(path));
    }
    return out.str();
}
}

TEST_CASE("Phase 8 W4 S5: load reconciliation is deterministic, dirty, and sibling-safe")
{
    const auto temp = Temp();
    const auto source = WritePrefab(temp);
    auto document = MakeDocument(source, kInstanceA, true);
    const auto badBefore = document.ecs.registry.get<NameComponent>(
        document.FindByUuid(U(301))).name;

    const auto report = ReconcilePrefabPropagationForLoad(
        document, AssetResolutionContext{temp.directory, nullptr});
    REQUIRE(report.IsOk());
    CHECK(report.value.changed);
    CHECK(report.value.propagatedInstances == 1);
    CHECK(report.value.quarantinedInstances == 1);
    CHECK(document.metadata.dirty);
    CHECK(document.ecs.registry.get<NameComponent>(document.FindByUuid(U(201))).name ==
          "SourceChild");
    CHECK(document.ecs.registry.get<Transform>(document.FindByUuid(U(201))).translation.x ==
          doctest::Approx(4.0f));
    CHECK(document.ecs.registry.get<NameComponent>(document.FindByUuid(U(301))).name ==
          badBefore);
    CHECK(report.value.diagnostics.size() == 1);

    document.metadata.dirty = false;
    const auto noOp = ReconcilePrefabPropagationForLoad(
        document, AssetResolutionContext{temp.directory, nullptr});
    REQUIRE(noOp.IsOk());
    CHECK_FALSE(noOp.value.changed);
    CHECK_FALSE(document.metadata.dirty);
}

TEST_CASE("Phase 8 W4 S5: global source failure is transactional")
{
    const auto temp = Temp();
    const auto source = WritePrefab(temp, true);
    auto document = MakeDocument(source);
    document.metadata.dirty = false;
    const auto before = document.ecs.registry.get<NameComponent>(
        document.FindByUuid(U(201))).name;
    const auto result = ReconcilePrefabPropagationForLoad(
        document, AssetResolutionContext{temp.directory, nullptr});
    CHECK_FALSE(result.IsOk());
    CHECK_FALSE(document.metadata.dirty);
    CHECK(document.ecs.registry.get<NameComponent>(document.FindByUuid(U(201))).name ==
          before);
}

TEST_CASE("Phase 8 typed foundation: load batch preflights every operation before writing")
{
    const auto temp = Temp();
    const auto source = WritePrefab(temp);
    auto document = MakeDocument(source);
    document.metadata.dirty = false;
    const auto entity = document.FindByUuid(U(201));
    REQUIRE(static_cast<std::uint32_t>(entity) !=
            static_cast<std::uint32_t>(entt::null));
    const auto beforeTransform = document.ecs.registry.get<Transform>(entity);
    auto staleBefore = beforeTransform;
    staleBefore.translation.x += 100.0f;

    PrefabPropagationPlan plan;
    plan.source = PrefabSourceFingerprint{source, kAsset, "digest-a"};
    plan.documentGeneration = 1;
    plan.resourceGeneration = 1;
    plan.authoringRevision = 0;
    plan.componentOperations.push_back(
        PrefabPropagationComponentDelta::Make<NameComponent>(
            U(201), U(2), NameComponent{"Old"}, NameComponent{"written-first"}));
    plan.componentOperations.push_back(
        PrefabPropagationComponentDelta::Make<Transform>(
            U(201), U(2), staleBefore, Transform{}));
    plan.memberSnapshots.push_back({U(201), kInstanceA, U(2), {}});
    plan.affectedEntities = {U(201)};
    plan.syncImpact = SyncImpact::Transform;
    REQUIRE(plan.IsEffective());

    const auto beforeDeep = DeepDocumentSnapshot(document);
    const auto beforePath = temp.directory / "typed-load-before.rt2scene";
    const auto afterPath = temp.directory / "typed-load-after.rt2scene";
    const auto beforeBytes = SerializedSnapshot(document, beforePath);

    PrefabPropagationLoadHooks hooks;
    hooks.prepare = [plan](const PrefabPropagationDiscoveryRequest&) {
        return Result<PrefabPropagationPlan>::Ok(plan);
    };
    const auto result = ReconcilePrefabPropagationForLoad(
        document, AssetResolutionContext{temp.directory, nullptr}, hooks);
    CHECK_FALSE(result.IsOk());
    CHECK(DeepDocumentSnapshot(document) == beforeDeep);
    CHECK(SerializedSnapshot(document, afterPath) == beforeBytes);
    CHECK(document.ecs.registry.get<NameComponent>(entity).name == "Old");
    CHECK(PrefabCanonicalComponentEqual(
        document.ecs.registry.get<Transform>(entity), beforeTransform));
    CHECK_FALSE(document.metadata.dirty);
    // Named RED/GREEN fault: applying the first load operation before the
    // later stale preflight changes both snapshots and the durable name.
}

TEST_CASE("Phase 8 W4 S5: recovery restores and reconciles before asset resolution")
{
    const auto temp = Temp();
    const auto source = WritePrefab(temp);
    auto sourceDocument = MakeDocument(source);
    const auto scenePath = temp.directory / "scene.rt2scene";
    std::vector<AssetDiagnostic> saveDiagnostics;
    Error error;
    REQUIRE(SceneSerializer::Save(sourceDocument, scenePath, saveDiagnostics, error));

    const auto recoveryRoot = temp.directory / "Recovery";
    std::filesystem::create_directories(recoveryRoot);
    SceneRecoveryService service(recoveryRoot);
    SceneRecoveryService::RecoveryRecord record;
    record.recordPath = recoveryRoot / "scene.rt2recovery";
    record.originalSourcePath = scenePath;
    record.assetRoot = temp.directory;
    record.snapshotJson = Bytes(scenePath);
    record.valid = true;

    SceneDocument restored;
    std::vector<AssetDiagnostic> diagnostics;
    SceneLoadReport loadReport;
    const bool restoredOk = service.Restore(record,
        AssetResolutionContext{temp.directory, nullptr}, restored, diagnostics,
        loadReport, error);
    INFO(error.Format());
    REQUIRE(restoredOk);
    CHECK(restored.metadata.dirty);
    CHECK(restored.ecs.registry.get<NameComponent>(restored.FindByUuid(U(201))).name ==
          "SourceChild");
}

TEST_CASE("Phase 8 W4 S5: filesystem aliases prepare once in either order")
{
    const auto temp = Temp();
    const auto source = WritePrefab(temp);
    const auto aliasDirectory = temp.directory / "source-alias";
    auto alias = MakeDirectoryAlias(temp.directory, aliasDirectory);
    const std::string aliasPath = "source-alias/source.rt2prefab";

    auto run = [&](bool aliasFirst) {
        SceneDocument document = aliasFirst
            ? MakeDocument(source, kInstanceA, false, aliasPath)
            : MakeDocument(source);
        if (aliasFirst)
            AddInstance(document, source, kInstanceB, U(300), U(301));
        else
            AddInstance(document, source, kInstanceB, U(300), U(301), aliasPath);
        std::size_t prepareCalls = 0;
        PrefabPropagationLoadHooks hooks;
        hooks.prepare = [&](const PrefabPropagationDiscoveryRequest& request) {
            ++prepareCalls;
            return PreparePrefabPropagation(request);
        };
        const auto report = ReconcilePrefabPropagationForLoad(
            document, AssetResolutionContext{temp.directory, nullptr}, hooks);
        REQUIRE(report.IsOk());
        CHECK(prepareCalls == 1);
        CHECK(report.value.propagatedInstances == 2);
        CHECK(report.value.quarantinedInstances == 0);
        CHECK(report.value.diagnostics.empty());
        return std::pair<std::string, PrefabPropagationLoadReport>{
            SerializedSnapshot(document, temp.directory /
                (aliasFirst ? "alias-first.rt2scene" : "real-first.rt2scene")),
            report.value};
    };
    const auto aliasFirst = run(true);
    const auto realFirst = run(false);
    auto normalizedAlias = aliasFirst.first;
    const std::string aliasToken = "source-alias/source.rt2prefab";
    for (std::size_t offset = normalizedAlias.find(aliasToken);
         offset != std::string::npos;
         offset = normalizedAlias.find(aliasToken, offset))
    {
        normalizedAlias.replace(offset, aliasToken.size(), "source.rt2prefab");
        offset += std::string("source.rt2prefab").size();
    }
    auto normalizedReal = realFirst.first;
    for (std::size_t offset = normalizedReal.find(aliasToken);
         offset != std::string::npos;
         offset = normalizedReal.find(aliasToken, offset))
    {
        normalizedReal.replace(offset, aliasToken.size(), "source.rt2prefab");
        offset += std::string("source.rt2prefab").size();
    }
    CHECK(normalizedAlias == normalizedReal);
    CHECK(aliasFirst.second.changed == realFirst.second.changed);
    CHECK(aliasFirst.second.propagatedInstances ==
          realFirst.second.propagatedInstances);
    CHECK(aliasFirst.second.quarantinedInstances ==
          realFirst.second.quarantinedInstances);
}

TEST_CASE("Phase 8 W4 S5: missing canonical fallback is lexical and checked")
{
    const auto temp = Temp();
    const auto missing = temp.directory / "missing" / ".." / "not-present.rt2prefab";
    auto expected = missing.lexically_normal();
#ifdef _WIN32
    auto folded = expected.generic_string();
    std::transform(folded.begin(), folded.end(), folded.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    expected = std::filesystem::u8path(folded);
#endif
    CHECK(CanonicalAssetPath(missing) == expected);
}

TEST_CASE("Phase 8 W4 S5: injected canonical failure preserves stable fallback identity")
{
    const auto temp = Temp();
    const auto spellingA = temp.directory / "Alias" / ".." /
        "missing" / "Prefab.rt2prefab";
    const auto spellingB = temp.directory / "missing" / "Prefab.rt2prefab";
    auto expected = spellingB.lexically_normal();
#ifdef _WIN32
    auto folded = expected.generic_string();
    std::transform(folded.begin(), folded.end(), folded.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    expected = std::filesystem::u8path(folded);
#endif
    const auto normalizedA = CanonicalAssetPathWithProbe(
        spellingA, &ForcedCanonicalFailure);
    const auto normalizedB = CanonicalAssetPathWithProbe(
        spellingB, &ForcedCanonicalFailure);
    CHECK(normalizedA == expected);
    CHECK(normalizedB == expected);
    CHECK(normalizedA.generic_string() + "|" + kAsset.ToString() ==
          normalizedB.generic_string() + "|" + kAsset.ToString());
}

TEST_CASE("Phase 8 W4 S5: valid-first malformed-later failure is deeply atomic")
{
    const auto temp = Temp();
    const auto valid = WritePrefab(temp);
    const auto malformed = temp.directory / "z-malformed.rt2prefab";
    Error error;
    {
        std::ofstream output(malformed, std::ios::binary);
        output << "not a prefab";
    }
    REQUIRE(WriteSidecarId(AssetSidecarPath(malformed), kAsset, error));
    auto document = MakeDocument(valid);
    AddInstance(document, malformed, kInstanceB, U(300), U(301));
    document.metadata.name = "atomic-seed";
    document.metadata.sourcePath = temp.directory / "atomic.rt2scene";
    document.metadata.projectId = U(9900);
    document.metadata.dirty = true;
    document.environment.ref = AssetReference{AssetKind::Environment,
        "environment.exr", {}, "env:seed", U(9901)};
    document.environment.width = 2;
    document.environment.height = 1;
    document.environment.floatPixels = {0.1f, 0.2f, 0.3f, 1.0f,
                                         0.4f, 0.5f, 0.6f, 1.0f};
    MeshData seedMesh;
    seedMesh.name = "atomic-mesh";
    seedMesh.vertices = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                         0.0f, 1.0f, 0.0f};
    seedMesh.indices = {0, 1, 2};
    seedMesh.normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 1.0f};
    seedMesh.uvs = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    seedMesh.materialIndices = {0};
    const auto seedMeshIndex = document.ecs.meshRegistry.AddMesh(seedMesh);
    SceneMaterial seedMaterial;
    seedMaterial.baseColor = {0.2f, 0.4f, 0.8f};
    seedMaterial.baseColorTextureIndex = 0;
    seedMaterial.sourceKey = "material:seed";
    document.ecs.materials.push_back(seedMaterial);
    SceneTexture seedTexture;
    seedTexture.ref = AssetReference{AssetKind::Texture, "seed.png", {},
                                     "texture:seed", U(9902)};
    seedTexture.width = 1; seedTexture.height = 1; seedTexture.pixels = {7, 8, 9, 255};
    document.ecs.textures.push_back(seedTexture);
    auto seededEntity = document.FindByUuid(U(201));
    document.ecs.registry.emplace<PrimitiveComponent>(seededEntity,
        PrimitiveComponent{PrimitiveComponent::Cube, 2.0f, 3, 2});
    document.ecs.registry.emplace<MeshRef>(seededEntity,
        MeshRef{seedMeshIndex, 0});
    document.gpuCache.meshes.push_back(GPUMeshGeometry{
        &document.ecs.meshRegistry.GetMesh(seedMeshIndex).vertices,
        &document.ecs.meshRegistry.GetMesh(seedMeshIndex).indices,
        &document.ecs.meshRegistry.GetMesh(seedMeshIndex).normals,
        &document.ecs.meshRegistry.GetMesh(seedMeshIndex).uvs,
        nullptr,
        &document.ecs.meshRegistry.GetMesh(seedMeshIndex).materialIndices,
        0});
    document.gpuCache.instances.push_back(GPUInstance{});
    document.gpuCache.instances.back().meshIndex = seedMeshIndex;
    document.gpuCache.materials.push_back(GPUMaterial::fromSceneMaterial(seedMaterial));
    document.gpuCache.textures.push_back(seedTexture);
    document.gpuCache.sourceEmissiveTriangleCount = 3;
    document.gpuCache.filteredBlackEmissiveTriangleCount = 1;
    document.gpuCache.emissiveTextureOccupancy.push_back(
        EmissiveTextureOccupancy{1, 1, {0, 1, 1, 1}});
    document.gpuCache.envMapIndex = 0;
    document.gpuCache.envIntensity = 1.25f;
    document.gpuCache.marginalCDF = {0.0f, 1.0f};
    document.gpuCache.conditionalCDF = {0.0f, 1.0f};
    document.gpuCache.cdfWidth = 1;
    document.gpuCache.cdfHeight = 2;
    const auto beforePath = temp.directory / "before.rt2scene";
    const auto afterPath = temp.directory / "after.rt2scene";
    const std::string before = SerializedSnapshot(document, beforePath);
    const std::string deepBefore = DeepDocumentSnapshot(document);
    const std::vector<std::filesystem::path> sourceFiles{
        valid, AssetSidecarPath(valid), malformed, AssetSidecarPath(malformed)};
    const std::string filesBefore = SourceFilesSnapshot(sourceFiles);

    const auto result = ReconcilePrefabPropagationForLoad(
        document, AssetResolutionContext{temp.directory, nullptr});
    CHECK_FALSE(result.IsOk());
    const std::string after = SerializedSnapshot(document, afterPath);
    CHECK(after == before);
    CHECK(DeepDocumentSnapshot(document) == deepBefore);
    CHECK(SourceFilesSnapshot(sourceFiles) == filesBefore);
}

TEST_CASE("Phase 8 W4 S5: host orchestration proves context, one resolve, and adoption semantics")
{
    const auto temp = Temp();
    const auto source = WritePrefab(temp);
    const auto projectRoot = temp.directory / "project-assets";
    const auto projectSceneParent = temp.directory / "project-scenes";
    const auto standaloneRoot = temp.directory / "standalone-scenes";
    std::error_code copyError;
    const auto projectCreated = std::filesystem::create_directories(projectRoot, copyError);
    const bool projectOk = projectCreated || !static_cast<bool>(copyError);
    REQUIRE(projectOk);
    const auto sceneCreated = std::filesystem::create_directories(projectSceneParent, copyError);
    const bool sceneOk = sceneCreated || !static_cast<bool>(copyError);
    REQUIRE(sceneOk);
    const auto standaloneCreated = std::filesystem::create_directories(standaloneRoot, copyError);
    const bool standaloneOk = standaloneCreated || !static_cast<bool>(copyError);
    REQUIRE(standaloneOk);
    const auto projectSource = projectRoot / source.filename();
    const auto standaloneSource = standaloneRoot / source.filename();
    std::filesystem::copy_file(source, projectSource,
        std::filesystem::copy_options::overwrite_existing, copyError);
    REQUIRE(!copyError);
    std::filesystem::copy_file(AssetSidecarPath(source), AssetSidecarPath(projectSource),
        std::filesystem::copy_options::overwrite_existing, copyError);
    REQUIRE(!copyError);
    std::filesystem::copy_file(source, standaloneSource,
        std::filesystem::copy_options::overwrite_existing, copyError);
    REQUIRE(!copyError);
    std::filesystem::copy_file(AssetSidecarPath(source), AssetSidecarPath(standaloneSource),
        std::filesystem::copy_options::overwrite_existing, copyError);
    REQUIRE(!copyError);
    for (const bool projectBound : {false, true})
    {
        const auto durableSource = projectBound ? projectSource : standaloneSource;
        auto document = MakeDocument(durableSource);
        const auto scenePath = projectBound
            ? projectSceneParent / "closed-project-scene.rt2scene"
            : standaloneRoot / "closed-standalone-scene.rt2scene";
        const auto projectId = U(projectBound ? 9900 : 9901);
        if (projectBound) document.metadata.projectId = projectId;
        std::size_t resolveCalls = 0;
        std::filesystem::path seenRoot;
        PrefabPropagationLoadHooks hooks;
        hooks.resolveAll = [&](SceneDocument&, const AssetResolutionContext& seen,
                               std::vector<AssetDiagnostic>&, Error&) {
            ++resolveCalls;
            seenRoot = seen.assetRoot;
            return true;
        };
        std::vector<AssetDiagnostic> diagnostics;
        Error error;
        const PrefabPropagationSceneOpenContext hostContext{
            scenePath, projectBound ? projectRoot : std::filesystem::path{}, nullptr};
        const auto report = RunPrefabPropagationSceneOpen(
            document, hostContext, diagnostics, error, hooks);
        REQUIRE(report.IsOk());
        CHECK(report.value.changed);
        CHECK(resolveCalls == 1);
        CHECK(seenRoot == (projectBound ? projectRoot : standaloneRoot));
        CHECK(document.metadata.dirty);

        SceneManager manager;
        const auto revisionBefore = manager.AuthoringRevision();
        const auto documentGenerationBefore = manager.DocumentGeneration();
        const auto resourceGenerationBefore = manager.ResourceGeneration();
        manager.AdoptLoadedDocument(std::move(document), report.value.changed,
                                    revisionBefore);
        CHECK(manager.AuthoringRevision() == revisionBefore + 1);
        CHECK(manager.DocumentGeneration() == documentGenerationBefore + 1);
        CHECK(manager.ResourceGeneration() == resourceGenerationBefore + 1);
        CHECK(manager.AuthoringDoc().metadata.dirty);
        CHECK(manager.AuthoringRevision() == revisionBefore + 1);
    }
}
