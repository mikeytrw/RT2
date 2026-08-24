#include "PrefabPropagationDiscovery.h"

#include "AssetIdentity.h"
#include "EntityReferenceRemapper.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace rt2::core {
namespace {

struct SourceModel
{
    PrefabDocument document;
    std::unordered_map<UUID, const PrefabEntityRecord*> byTemplate;
    std::unordered_map<UUID, const PrefabEntityRecord*> byRecord;
    std::unordered_map<UUID, UUID> parentTemplate;
    UUID rootTemplate;
};

struct LoadedSource
{
    PrefabSourceFingerprint fingerprint;
    SourceModel model;
};

std::string DigestBytes(const std::string& bytes, const std::string& sidecar)
{
    // A deterministic content digest is sufficient at this contract boundary;
    // the digest covers bytes and the durable sidecar identity, never mtime.
    std::uint64_t hash = 1469598103934665603ull;
    auto add = [&](unsigned char c) {
        hash ^= c;
        hash *= 1099511628211ull;
    };
    for (const unsigned char c : bytes) add(c);
    for (const unsigned char c : sidecar) add(c);
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

bool ReadBytes(const std::filesystem::path& path, std::string& bytes,
               Error& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        error = { Error::Io, path.string(), "prefab source could not be opened" };
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(input),
                 std::istreambuf_iterator<char>());
    if (input.bad())
    {
        error = { Error::Io, path.string(), "prefab source could not be read" };
        return false;
    }
    return true;
}

bool BuildSourceModel(PrefabDocument document, SourceModel& out, Error& error)
{
    if (document.version != PrefabSerializer::FormatVersion ||
        document.entities.empty())
    {
        error = { Error::Parse, {}, "prefab source has an invalid version or no entities" };
        return false;
    }
    out = {};
    out.document = std::move(document);
    for (const auto& entity : out.document.entities)
    {
        if (entity.templateId.IsNull() || entity.record.uuid.IsNull())
        {
            error = { Error::Parse, {}, "prefab source contains a nil template or record UUID" };
            return false;
        }
        if (!out.byTemplate.emplace(entity.templateId, &entity).second)
        {
            error = { Error::DuplicateUuid, {}, "prefab source contains duplicate template IDs" };
            return false;
        }
        if (!out.byRecord.emplace(entity.record.uuid, &entity).second)
        {
            error = { Error::DuplicateUuid, {}, "prefab source contains duplicate record UUIDs" };
            return false;
        }
    }
    for (const auto& entity : out.document.entities)
    {
        const UUID parent = entity.record.parentUuid;
        if (parent.IsNull())
        {
            if (!out.rootTemplate.IsNull())
            {
                error = { Error::InvalidHierarchy, {}, "prefab source has multiple roots" };
                return false;
            }
            out.rootTemplate = entity.templateId;
            out.parentTemplate.emplace(entity.templateId, UUID::Nil());
            continue;
        }
        const auto parentIt = out.byRecord.find(parent);
        if (parentIt == out.byRecord.end())
        {
            error = { Error::MissingParent, {}, "prefab source parent UUID is missing" };
            return false;
        }
        out.parentTemplate.emplace(entity.templateId, parentIt->second->templateId);
    }
    if (out.rootTemplate.IsNull())
    {
        error = { Error::InvalidHierarchy, {}, "prefab source has no root" };
        return false;
    }
    for (const auto& entity : out.document.entities)
    {
        UUID cursor = entity.templateId;
        std::unordered_set<UUID> seen;
        while (!cursor.IsNull())
        {
            if (!seen.insert(cursor).second)
            {
                error = { Error::HierarchyCycle, {}, "prefab source hierarchy contains a cycle" };
                return false;
            }
            const auto it = out.parentTemplate.find(cursor);
            if (it == out.parentTemplate.end())
            {
                error = { Error::InvalidHierarchy, {}, "prefab source hierarchy is incomplete" };
                return false;
            }
            cursor = it->second;
        }
    }
    return true;
}

std::string StructuralReason(const std::string& reason)
{
    return "structural validation: " + reason;
}

bool HasEntity(const SceneDocument& document, entt::entity entity,
               const std::unordered_map<UUID, std::size_t>& idCounts,
               std::string& reason)
{
    if (entity == entt::null || !document.ecs.registry.valid(entity) ||
        !document.ecs.registry.all_of<EntityIdComponent>(entity))
    {
        reason = "instance member has no durable entity UUID";
        return false;
    }
    const UUID id = document.ecs.registry.get<EntityIdComponent>(entity).id;
    if (id.IsNull())
    {
        reason = "instance member has a nil durable entity UUID";
        return false;
    }
    const auto count = idCounts.find(id);
    if (count == idCounts.end() || count->second != 1 ||
        document.FindByUuid(id) != entity)
    {
        reason = "instance member has a duplicate or misindexed durable entity UUID";
        return false;
    }
    return true;
}

bool ValidateOverrides(const PrefabMemberComponent& member,
                       std::uint32_t sceneSchemaVersion,
                       std::string& reason)
{
    if (!member.overrides.empty() &&
        sceneSchemaVersion < SceneSerializer::PrefabOverrideSchemaVersion)
    {
        reason = "override vector is not representable by the scene schema";
        return false;
    }
    std::vector<PrefabComponentKey> canonical;
    canonical.reserve(member.overrides.size());
    for (const auto& key : member.overrides)
    {
        const auto resolved = FindComponentByWire(key.wire());
        if (!resolved || *resolved != key ||
            !IsPropagationComponentOverrideableKey(*resolved))
        {
            reason = "override vector contains an unknown, excluded, or forged key";
            return false;
        }
        if (key.wire() == PrefabWireKeys::kPrimitive &&
            sceneSchemaVersion < SceneSerializer::PrimitiveOverrideSchemaVersion)
        {
            reason = "override vector contains a key not representable by the scene schema";
            return false;
        }
        canonical.push_back(*resolved);
    }
    std::sort(canonical.begin(), canonical.end(),
              [](const auto& a, const auto& b) { return a.wire() < b.wire(); });
    if (std::adjacent_find(canonical.begin(), canonical.end()) != canonical.end())
    {
        reason = "override vector contains duplicate keys";
        return false;
    }
    if (canonical != member.overrides)
    {
        reason = "override vector is not canonically sorted";
        return false;
    }
    return true;
}

struct InstanceValidation
{
    bool valid = false;
    std::string reason;
    UUID templateId;
    std::vector<UUID> entities;
    std::unordered_map<UUID, entt::entity> byTemplate;
};

InstanceValidation ValidateInstance(const SceneDocument& document,
                                    entt::entity root,
                                    const PrefabInstanceComponent& link,
                                    const SourceModel& source,
                                    const std::unordered_map<UUID, std::size_t>& idCounts)
{
    InstanceValidation result;
    const auto& registry = document.ecs.registry;
    if (link.instanceId.IsNull())
    {
        result.reason = "root instance ID is nil";
        return result;
    }
    if (!registry.all_of<PrefabMemberComponent>(root))
    {
        result.reason = "root has no PrefabMemberComponent";
        return result;
    }

    std::vector<entt::entity> stack{ root };
    std::unordered_set<entt::entity> visited;
    std::unordered_map<UUID, entt::entity> byTemplate;
    std::set<UUID> liveEntityIds;
    while (!stack.empty())
    {
        const entt::entity entity = stack.back();
        stack.pop_back();
        if (!visited.insert(entity).second)
        {
            result.reason = "instance hierarchy contains a cycle or duplicate child";
            return result;
        }
        if (!HasEntity(document, entity, idCounts, result.reason))
        {
            return result;
        }
        const UUID entityUuid = registry.get<EntityIdComponent>(entity).id;
        if (!liveEntityIds.insert(entityUuid).second)
        {
            result.reason = "instance contains duplicate durable entity UUIDs";
            return result;
        }
        if (!registry.all_of<PrefabMemberComponent>(entity))
        {
            result.reason = "instance subtree contains a member without PrefabMemberComponent";
            return result;
        }
        const auto& member = registry.get<PrefabMemberComponent>(entity);
        if (member.instanceId != link.instanceId)
        {
            result.reason = "instance subtree contains a cross-instance member";
            return result;
        }
        if (member.templateId.IsNull())
        {
            result.reason = "instance member template ID is nil";
            return result;
        }
        if (!byTemplate.emplace(member.templateId, entity).second)
        {
            result.reason = "instance contains duplicate template IDs";
            result.templateId = member.templateId;
            return result;
        }
        std::string overrideReason;
        if (!ValidateOverrides(member, document.metadata.schemaVersion, overrideReason))
        {
            result.reason = overrideReason;
            result.templateId = member.templateId;
            return result;
        }
        if (entity != root && registry.all_of<PrefabInstanceComponent>(entity))
        {
            result.reason = "nested prefab root is not valid in S2";
            result.templateId = member.templateId;
            return result;
        }
        const auto* hierarchy = registry.try_get<Hierarchy>(entity);
        if (!hierarchy) continue;
        for (const entt::entity child : hierarchy->children)
        {
            if (child == entt::null || !registry.valid(child))
            {
                result.reason = "instance hierarchy contains an invalid child";
                result.templateId = member.templateId;
                return result;
            }
            if (!registry.all_of<Hierarchy>(child) ||
                registry.get<Hierarchy>(child).parent != entity)
            {
                result.reason = "instance hierarchy has an orphan or parent mismatch";
                result.templateId = member.templateId;
                return result;
            }
            stack.push_back(child);
        }
    }

    std::unordered_map<UUID, std::vector<entt::entity>> allMembers;
    // Count every member component, including malformed members that lack a
    // durable EntityIdComponent.  Omitting those from this closure check
    // would let a detached/corrupt member disappear from validation.
    auto memberView = registry.view<PrefabMemberComponent>();
    for (const entt::entity entity : memberView)
    {
        const auto& member = memberView.get<PrefabMemberComponent>(entity);
        if (member.instanceId == link.instanceId)
        {
            std::string identityReason;
            if (!HasEntity(document, entity, idCounts, identityReason))
            {
                result.reason = identityReason;
                return result;
            }
            allMembers[member.instanceId].push_back(entity);
        }
    }
    if (allMembers[link.instanceId].size() != visited.size())
    {
        result.reason = "instance has an extra or detached member";
        return result;
    }
    if (byTemplate.size() != source.byTemplate.size())
    {
        result.reason = "instance template membership is not an exact source closure";
        return result;
    }
    for (const auto& [templateId, sourceRecord] : source.byTemplate)
    {
        (void)sourceRecord;
        if (!byTemplate.count(templateId))
        {
            result.reason = "instance is missing a source template member";
            result.templateId = templateId;
            return result;
        }
        const entt::entity entity = byTemplate.at(templateId);
        const auto parentTemplate = source.parentTemplate.at(templateId);
        if (parentTemplate.IsNull())
        {
            if (entity != root)
            {
                result.reason = "source root is not the live instance root";
                result.templateId = templateId;
                return result;
            }
        }
        else
        {
            const auto parentIt = byTemplate.find(parentTemplate);
            if (parentIt == byTemplate.end() || !registry.all_of<Hierarchy>(entity) ||
                registry.get<Hierarchy>(entity).parent != parentIt->second)
            {
                result.reason = "instance hierarchy does not match source topology";
                result.templateId = templateId;
                return result;
            }
        }
    }
    for (const auto& [templateId, entity] : byTemplate)
    {
        std::set<UUID> liveChildren;
        if (registry.all_of<Hierarchy>(entity))
            for (const entt::entity child : registry.get<Hierarchy>(entity).children)
                if (byTemplate.count(registry.try_get<PrefabMemberComponent>(child)
                                         ? registry.get<PrefabMemberComponent>(child).templateId
                                         : UUID::Nil()))
                    liveChildren.insert(registry.get<PrefabMemberComponent>(child).templateId);
        std::set<UUID> sourceChildren;
        for (const auto& [childTemplate, parent] : source.parentTemplate)
            if (parent == templateId) sourceChildren.insert(childTemplate);
        if (liveChildren != sourceChildren)
        {
            result.reason = "instance child membership does not match source topology";
            result.templateId = templateId;
            return result;
        }
    }
    result.valid = true;
    result.entities.assign(liveEntityIds.begin(), liveEntityIds.end());
    result.byTemplate = std::move(byTemplate);
    return result;
}

bool HasOverride(const PrefabMemberComponent& member, const PrefabComponentKey& key)
{
    return std::find(member.overrides.begin(), member.overrides.end(), key) !=
           member.overrides.end();
}

template<typename T>
void AddOperation(std::vector<PrefabPropagationComponentDelta>& operations,
                  const UUID& entityUuid, const UUID& templateId,
                  const std::optional<T>& before,
                  const std::optional<T>& after)
{
    auto operation = PrefabPropagationComponentDelta::Make<T>(
        entityUuid, templateId, before, after);
    if (!operation.IsNoOp())
        operations.push_back(std::move(operation));
}

UUID ParseCapturedSidecar(const std::string& bytes, Error& error)
{
    error = Error{};
    std::string line;
    const auto end = bytes.find_first_of("\r\n");
    line = bytes.substr(0, end == std::string::npos ? bytes.size() : end);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
    const UUID id = UUID::Parse(line);
    if (id.IsNull())
    {
        error = {Error::Parse, {},
            "captured sidecar does not contain a valid UUID"};
        return UUID::Nil();
    }
    return id;
}

std::string InheritedRootName(const std::string& name)
{
    constexpr std::string_view suffix = " Copy";
    if (name.size() >= suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
        return name;
    return name + std::string(suffix);
}

bool BuildReconciliation(const SceneDocument& document,
                         entt::entity root,
                         const SourceModel& source,
                         const InstanceValidation& validation,
                         std::vector<PrefabPropagationComponentDelta>& output,
                         std::string& reason,
                         UUID& reasonTemplate)
{
    (void)root;
    const auto& registry = document.ecs.registry;
    std::vector<UUID> templates;
    templates.reserve(source.byTemplate.size());
    for (const auto& [templateId, record] : source.byTemplate)
    {
        (void)record;
        templates.push_back(templateId);
    }
    std::sort(templates.begin(), templates.end(),
              [](const UUID& a, const UUID& b) { return a < b; });

    EntityUuidRemap remap;
    for (const UUID& templateId : templates)
    {
        const auto sourceIt = source.byTemplate.find(templateId);
        const auto liveIt = validation.byTemplate.find(templateId);
        if (sourceIt == source.byTemplate.end() || liveIt == validation.byTemplate.end())
        {
            reason = "reconciliation template mapping is incomplete";
            reasonTemplate = templateId;
            return false;
        }
        const auto* id = registry.try_get<EntityIdComponent>(liveIt->second);
        if (!id || id->id.IsNull())
        {
            reason = "reconciliation member has no durable UUID";
            reasonTemplate = templateId;
            return false;
        }
        remap[sourceIt->second->record.uuid] = id->id;
    }

    std::vector<PrefabPropagationComponentDelta> staged;
    for (const UUID& templateId : templates)
    {
        const entt::entity entity = validation.byTemplate.at(templateId);
        const auto* member = registry.try_get<PrefabMemberComponent>(entity);
        const auto* id = registry.try_get<EntityIdComponent>(entity);
        if (!member || !id)
        {
            reason = "reconciliation member metadata is missing";
            reasonTemplate = templateId;
            return false;
        }

        const bool markerPrimitive = HasOverride(
            *member, PropagationComponentKey<PrimitiveComponent>());
        const bool markerMaterial = HasOverride(
            *member, PropagationComponentKey<MaterialOverrideComponent>());
        const auto beforeName = ReadPropagationComponent<NameComponent>(registry, entity);
        const auto beforeTransform = ReadPropagationComponent<Transform>(registry, entity);
        const auto beforeVisible = ReadPropagationComponent<VisibleComponent>(registry, entity);
        const auto beforePrimitive = ReadPropagationComponent<PrimitiveComponent>(registry, entity);
        const auto beforeImported = ReadPropagationComponent<ImportedMeshSourceComponent>(registry, entity);
        const auto beforeMaterial = ReadPropagationComponent<MaterialOverrideComponent>(registry, entity);
        const auto beforeLight = ReadPropagationComponent<LightComponent>(registry, entity);
        const auto beforeCamera = ReadPropagationComponent<CameraComponent>(registry, entity);
        const auto beforeMotion = ReadPropagationComponent<MotionComponent>(registry, entity);
        const auto beforeScript = ReadPropagationComponent<ScriptComponent>(registry, entity);

        const auto sourcePrimitiveValue = ReadPropagationSource<PrimitiveComponent>(
            *source.byTemplate.at(templateId));
        const auto sourceImportedValue = ReadPropagationSource<ImportedMeshSourceComponent>(
            *source.byTemplate.at(templateId));
        const auto sourceMaterialValue = ReadPropagationSource<MaterialOverrideComponent>(
            *source.byTemplate.at(templateId));
        const auto sourceNameValue = ReadPropagationSource<NameComponent>(
            *source.byTemplate.at(templateId));
        const auto sourceTransformValue = ReadPropagationSource<Transform>(
            *source.byTemplate.at(templateId));
        const auto sourceVisibleValue = ReadPropagationSource<VisibleComponent>(
            *source.byTemplate.at(templateId));
        const auto sourceLightValue = ReadPropagationSource<LightComponent>(
            *source.byTemplate.at(templateId));
        const auto sourceCameraValue = ReadPropagationSource<CameraComponent>(
            *source.byTemplate.at(templateId));
        const auto sourceMotionValue = ReadPropagationSource<MotionComponent>(
            *source.byTemplate.at(templateId));
        auto sourceScriptValue = ReadPropagationSource<ScriptComponent>(
            *source.byTemplate.at(templateId));
        const auto afterPrimitive = markerPrimitive ? beforePrimitive : sourcePrimitiveValue;
        const auto afterImported = sourceImportedValue;
        const auto afterMaterial = markerMaterial ? beforeMaterial : sourceMaterialValue;

        std::optional<NameComponent> afterName;
        if (HasOverride(*member, PropagationComponentKey<NameComponent>()))
            afterName = beforeName;
        else
            afterName = sourceNameValue;
            if (afterName && templateId == source.rootTemplate)
                afterName->name = InheritedRootName(afterName->name);
        if (!HasOverride(*member, PropagationComponentKey<NameComponent>()))
            AddOperation(staged, id->id, templateId, beforeName, afterName);

        std::optional<Transform> afterTransform;
        if (HasOverride(*member, PropagationComponentKey<Transform>()))
            afterTransform = beforeTransform;
        else
            afterTransform = sourceTransformValue;
        if (!HasOverride(*member, PropagationComponentKey<Transform>()))
            AddOperation(staged, id->id, templateId, beforeTransform, afterTransform);

        std::optional<VisibleComponent> afterVisible;
        if (HasOverride(*member, PropagationComponentKey<VisibleComponent>()))
            afterVisible = beforeVisible;
        else
            afterVisible = sourceVisibleValue;
        if (!HasOverride(*member, PropagationComponentKey<VisibleComponent>()))
            AddOperation(staged, id->id, templateId, beforeVisible, afterVisible);

        if (!markerPrimitive)
            AddOperation(staged, id->id, templateId, beforePrimitive, afterPrimitive);
        AddOperation(staged, id->id, templateId, beforeImported, afterImported);
        if (!markerMaterial)
            AddOperation(staged, id->id, templateId, beforeMaterial, afterMaterial);

        if (afterPrimitive && afterImported)
        {
            reason = "planned member carries both Primitive and ImportedMeshSource provenance";
            reasonTemplate = templateId;
            return false;
        }
        if (afterMaterial && !afterImported)
        {
            reason = "planned material override has no imported source provenance";
            reasonTemplate = templateId;
            return false;
        }

        std::optional<LightComponent> afterLight =
            HasOverride(*member, PropagationComponentKey<LightComponent>())
                ? beforeLight : sourceLightValue;
        if (!HasOverride(*member, PropagationComponentKey<LightComponent>()))
            AddOperation(staged, id->id, templateId, beforeLight, afterLight);

        std::optional<CameraComponent> afterCamera =
            HasOverride(*member, PropagationComponentKey<CameraComponent>())
                ? beforeCamera : sourceCameraValue;
        if (!HasOverride(*member, PropagationComponentKey<CameraComponent>()))
            AddOperation(staged, id->id, templateId, beforeCamera, afterCamera);

        std::optional<MotionComponent> afterMotion =
            HasOverride(*member, PropagationComponentKey<MotionComponent>())
                ? beforeMotion : sourceMotionValue;
        if (!HasOverride(*member, PropagationComponentKey<MotionComponent>()))
            AddOperation(staged, id->id, templateId, beforeMotion, afterMotion);

        if (sourceScriptValue)
        {
            std::vector<ScriptComponent*> remapped{&*sourceScriptValue};
            RemapEntityReferences(remap, remapped);
        }
        std::optional<ScriptComponent> afterScript =
            HasOverride(*member, PropagationComponentKey<ScriptComponent>())
                ? beforeScript : sourceScriptValue;
        if (!HasOverride(*member, PropagationComponentKey<ScriptComponent>()))
            AddOperation(staged, id->id, templateId, beforeScript, afterScript);
    }
    output.insert(output.end(), std::make_move_iterator(staged.begin()),
                  std::make_move_iterator(staged.end()));
    return true;
}

PrefabPropagationDiagnostic MakeDiagnostic(const PrefabSourceFingerprint& source,
                                           const PrefabInstanceComponent& link,
                                           const UUID& rootUuid,
                                           const InstanceValidation& validation)
{
    PrefabPropagationDiagnostic diagnostic;
    diagnostic.severity = AssetDiagnostic::Malformed;
    diagnostic.prefabPath = source.normalizedPath;
    diagnostic.prefabAssetId = source.assetId;
    diagnostic.instanceId = link.instanceId;
    diagnostic.rootUuid = rootUuid;
    diagnostic.templateId = validation.templateId;
    diagnostic.reason = StructuralReason(validation.reason);
    return diagnostic;
}

bool MatchesCapturedIdentity(const AssetReference& reference,
                             const PrefabSourceFingerprint& captured,
                             const AssetResolutionContext& assets)
{
    const auto resolved = ResolveCapturedAssetIdentity(
        reference, assets, captured.normalizedPath, captured.assetId);
    return resolved.IsOk() && resolved.value.normalizedPath == captured.normalizedPath &&
           resolved.value.effectiveId == captured.assetId;
}

} // namespace

Result<CapturedPrefabSource> CapturePrefabSource(
    const AssetReference& source, const AssetResolutionContext& assets,
    const std::function<bool(const std::filesystem::path&, std::string&, Error&)>&
        injectedRead)
{
    if (source.kind != AssetKind::Prefab ||
        (source.path.empty() && source.assetId.IsNull()))
        return Result<CapturedPrefabSource>::Fail(
            Error::InvalidArgument, {}, "prefab fingerprint requires a prefab identity");
    const auto preliminary = ResolveCapturedAssetIdentity(source, assets);
    if (!preliminary.IsOk())
        return Result<CapturedPrefabSource>::Fail(
            preliminary.error.code, preliminary.error.path,
            preliminary.error.detail);
    auto path = preliminary.value.normalizedPath;
    // AssetResolver's ID-first rule still permits an authored-path fallback
    // when the unique database record is stale.  Existence probes choose that
    // fallback without opening either source or sidecar; the injected reader
    // below still performs exactly one source read and one sidecar read.
    if (!source.path.empty() && assets.database != nullptr &&
        !std::filesystem::is_regular_file(path))
    {
        const std::filesystem::path authored(source.path);
        // A stale unique DB claimant may fall back to the authored path, but
        // only after that path has been resolved against an explicit absolute
        // asset root.  Never turn a rootless relative watcher reference into
        // a process-CWD probe.
        if (authored.is_absolute() ||
            (!assets.assetRoot.empty() && assets.assetRoot.is_absolute()))
        {
            const auto authoredPath = CanonicalAssetPath(authored.is_relative()
                ? assets.assetRoot / authored : authored);
            if (std::filesystem::is_regular_file(authoredPath))
                path = authoredPath;
        }
    }
    std::string bytes;
    Error error;
    const auto reader = injectedRead ? injectedRead :
        std::function<bool(const std::filesystem::path&, std::string&, Error&)>(
            [](const std::filesystem::path& p, std::string& b, Error& e) {
                return ReadBytes(p, b, e);
            });
    if (!reader(path, bytes, error))
        return Result<CapturedPrefabSource>::Fail(error.code, error.path, error.detail);
    std::string sidecarBytes;
    if (!reader(AssetSidecarPath(path), sidecarBytes, error))
        return Result<CapturedPrefabSource>::Fail(error.code, error.path, error.detail);
    Error sidecarError;
    const UUID sidecar = ParseCapturedSidecar(sidecarBytes, sidecarError);
    const auto identity = ResolveCapturedAssetIdentity(
        source, assets, path, sidecar);
    if (!sidecarError.IsOk() || !identity.IsOk())
        return Result<CapturedPrefabSource>::Fail(
            sidecarError.IsOk() ? identity.error.code : sidecarError.code,
            path.string(), sidecarError.IsOk() ? identity.error.detail
                                                : "captured prefab sidecar identity is invalid");
    const auto digest = DigestBytes(bytes, sidecarBytes);
    if (digest.empty())
        return Result<CapturedPrefabSource>::Fail(
            Error::Parse, path.string(), "prefab source fingerprint is empty");
    const PrefabSourceFingerprint fingerprint{identity.value.normalizedPath,
                                              identity.value.effectiveId, digest};
    return Result<CapturedPrefabSource>::Ok(
        CapturedPrefabSource{fingerprint, std::move(bytes), std::move(sidecarBytes)});
}

Result<PrefabSourceFingerprint> ReadPrefabSourceFingerprint(
    const AssetReference& source, const AssetResolutionContext& assets,
    const std::function<bool(const std::filesystem::path&, std::string&, Error&)>&
        injectedRead)
{
    const auto captured = CapturePrefabSource(source, assets, injectedRead);
    if (!captured.IsOk())
        return Result<PrefabSourceFingerprint>::Fail(
            captured.error.code, captured.error.path, captured.error.detail);
    return Result<PrefabSourceFingerprint>::Ok(captured.value.fingerprint);
}

Result<DiscoveredPropagationPlan> PreparePrefabPropagation(
    const PrefabPropagationDiscoveryRequest& request)
{
    if (!request.document)
        return Result<DiscoveredPropagationPlan>::Fail(
            Error::InvalidArgument, {}, "discovery requires a scene document");
    if (request.changedSource.kind != AssetKind::Prefab ||
        (request.changedSource.path.empty() && request.changedSource.assetId.IsNull()))
        return Result<DiscoveredPropagationPlan>::Fail(
            Error::InvalidArgument, {}, "changed source is not a prefab identity");

    const auto& captured = request.capturedSource;
    if (!captured.IsValid() ||
        !MatchesCapturedIdentity(request.changedSource, captured.fingerprint,
                                 request.assets))
        return Result<DiscoveredPropagationPlan>::Fail(
            Error::InvalidArgument, captured.fingerprint.normalizedPath.string(),
            "Prepare requires one coherent captured prefab source and sidecar");
    Error sidecarError;
    const UUID sidecarId = ParseCapturedSidecar(captured.sidecarBytes, sidecarError);
    const auto fingerprintDigest = DigestBytes(captured.prefabBytes,
                                               captured.sidecarBytes);
    if (!sidecarError.IsOk() || sidecarId.IsNull() ||
        sidecarId != captured.fingerprint.assetId ||
        fingerprintDigest.empty() || fingerprintDigest != captured.fingerprint.contentDigest)
        return Result<DiscoveredPropagationPlan>::Fail(
            Error::Parse, captured.fingerprint.normalizedPath.string(),
            "prefab source fingerprint or sidecar identity is invalid");
    PrefabSourceFingerprint fingerprint{ captured.fingerprint.normalizedPath, sidecarId,
                                         fingerprintDigest };
    const std::string& sourceBytes = captured.prefabBytes;
    const std::string& sidecarBytes = captured.sidecarBytes;
    Error sourceError;

    std::unordered_map<UUID, std::size_t> idCounts;
    const auto idView = request.document->ecs.registry.view<EntityIdComponent>();
    for (const entt::entity entity : idView)
    {
        const UUID id = idView.get<EntityIdComponent>(entity).id;
        if (!id.IsNull()) ++idCounts[id];
    }

    std::vector<entt::entity> roots;
    auto rootView = request.document->ecs.registry.view<PrefabInstanceComponent>();
    for (const entt::entity root : rootView)
    {
        const auto& link = rootView.get<PrefabInstanceComponent>(root);
        const auto* idComponent = request.document->ecs.registry.try_get<EntityIdComponent>(root);
        const UUID rootUuid = idComponent ? idComponent->id : UUID::Nil();
        std::vector<AssetDiagnostic> rootDiagnostics;
        if (!MatchesCapturedIdentity(link.prefab, fingerprint, request.assets))
            continue;
        roots.push_back(root);
    }

    DiscoveredPropagationPlan plan;
    plan.source = fingerprint;
    plan.capturedSource = CapturedPrefabSource{fingerprint, sourceBytes, sidecarBytes};
    plan.documentGeneration = request.documentGeneration;
    plan.resourceGeneration = request.resourceGeneration;
    plan.documentGenerationCaptured = request.documentGenerationCaptured;
    plan.resourceGenerationCaptured = request.resourceGenerationCaptured;
    plan.authoringRevision = request.authoringRevision;
    plan.authoringRevisionCaptured = true;
    plan.sourceSchemaVersion = PrefabSerializer::FormatVersion;
    plan.meshTableExtent = request.document->ecs.meshRegistry.GetCount();
    plan.materialTableExtent = static_cast<std::uint32_t>(request.document->ecs.materials.size());
    plan.textureTableExtent = static_cast<std::uint32_t>(request.document->ecs.textures.size());
    plan.resourceEvidenceCaptured = true;

    std::sort(roots.begin(), roots.end(), [&](entt::entity a, entt::entity b) {
        const auto& am = rootView.get<PrefabInstanceComponent>(a);
        const auto& bm = rootView.get<PrefabInstanceComponent>(b);
        if (am.instanceId != bm.instanceId) return am.instanceId < bm.instanceId;
        const auto* aid = request.document->ecs.registry.try_get<EntityIdComponent>(a);
        const auto* bid = request.document->ecs.registry.try_get<EntityIdComponent>(b);
        const UUID auid = aid ? aid->id : UUID::Nil();
        const UUID buid = bid ? bid->id : UUID::Nil();
        if (auid != buid) return auid < buid;
        return a < b;
    });

    std::unordered_set<UUID> duplicateInstanceIds;
    for (std::size_t i = 1; i < roots.size(); ++i)
    {
        const auto& previous = rootView.get<PrefabInstanceComponent>(roots[i - 1]);
        const auto& current = rootView.get<PrefabInstanceComponent>(roots[i]);
        if (!previous.instanceId.IsNull() && previous.instanceId == current.instanceId)
            duplicateInstanceIds.insert(current.instanceId);
    }

    // Parse the exact immutable bytes already fingerprinted. The optional
    // parser is a CPU test seam; neither path performs source I/O here.
    PrefabDocument document;
    bool parsed = false;
    if (request.parseBytes)
        parsed = request.parseBytes(document, sourceBytes,
                                    captured.fingerprint.normalizedPath, sourceError);
    else
        parsed = PrefabSerializer::LoadBytes(document, sourceBytes,
                                             captured.fingerprint.normalizedPath,
                                             sourceError);
    if (!parsed)
        return Result<DiscoveredPropagationPlan>::Fail(
            sourceError.code, sourceError.path, sourceError.detail);
    SourceModel model;
    if (!BuildSourceModel(std::move(document), model, sourceError))
        return Result<DiscoveredPropagationPlan>::Fail(
            sourceError.code, captured.fingerprint.normalizedPath.string(), sourceError.detail);
    const LoadedSource loaded{ fingerprint, std::move(model) };

    for (const entt::entity root : roots)
    {
        const auto& link = rootView.get<PrefabInstanceComponent>(root);
        const auto* idComponent = request.document->ecs.registry.try_get<EntityIdComponent>(root);
        const UUID rootUuid = idComponent ? idComponent->id : UUID::Nil();
        InstanceValidation validation;
        std::string rootIdentityReason;
        if (!HasEntity(*request.document, root, idCounts, rootIdentityReason))
            validation.reason = "root " + rootIdentityReason;
        else if (duplicateInstanceIds.count(link.instanceId) != 0)
            validation.reason = "instance has multiple PrefabInstanceComponent roots";
        else
            validation = ValidateInstance(*request.document, root, link,
                                          loaded.model, idCounts);
        PrefabPropagationInstancePlan instance;
        instance.instanceId = link.instanceId;
        instance.rootUuid = rootUuid;
        if (!rootUuid.IsNull() && !link.instanceId.IsNull() && link.prefab.IsValid())
            plan.rootSnapshots.push_back({rootUuid, link.instanceId, link.prefab});
        if (validation.valid)
        {
            std::vector<PrefabPropagationComponentDelta> reconciled;
            std::string reconciliationReason;
            UUID reconciliationTemplate;
            if (!BuildReconciliation(*request.document, root, loaded.model,
                                     validation, reconciled,
                                     reconciliationReason,
                                     reconciliationTemplate))
            {
                validation.valid = false;
                validation.reason = reconciliationReason;
                validation.templateId = reconciliationTemplate;
            }
            else
            {
                for (const UUID& entityUuid : validation.entities)
                {
                    const auto entity = request.document->FindByUuid(entityUuid);
                    const auto* member = entity == entt::null ? nullptr
                        : request.document->ecs.registry.try_get<PrefabMemberComponent>(entity);
                    if (!member)
                    {
                        validation.valid = false;
                        validation.reason = "member snapshot target disappeared";
                        break;
                    }
                    plan.memberSnapshots.push_back({entityUuid, member->instanceId,
                                                    member->templateId, member->overrides});
                }
            }
            if (validation.valid)
            {
                for (const auto& operation : reconciled)
                    if (std::find(instance.affectedEntities.begin(),
                                  instance.affectedEntities.end(),
                                  operation.EntityUuid()) == instance.affectedEntities.end())
                        instance.affectedEntities.push_back(operation.EntityUuid());
                instance.disposition = reconciled.empty()
                    ? PrefabPropagationInstanceDisposition::NoOp
                    : PrefabPropagationInstanceDisposition::Propagate;
                plan.componentOperations.insert(
                    plan.componentOperations.end(),
                    std::make_move_iterator(reconciled.begin()),
                    std::make_move_iterator(reconciled.end()));
            }
        }
        if (!validation.valid)
        {
            instance.disposition = PrefabPropagationInstanceDisposition::Quarantined;
            instance.diagnostics.push_back(MakeDiagnostic(
                fingerprint, link, rootUuid, validation));
            plan.diagnostics.push_back(instance.diagnostics.front());
        }
        std::sort(instance.affectedEntities.begin(), instance.affectedEntities.end(),
                  [](const UUID& a, const UUID& b) { return a < b; });
        plan.instances.push_back(std::move(instance));
    }
    std::sort(plan.componentOperations.begin(), plan.componentOperations.end(),
              [](const auto& a, const auto& b) {
                  if (a.EntityUuid() != b.EntityUuid()) return a.EntityUuid() < b.EntityUuid();
                  if (a.TemplateId() != b.TemplateId()) return a.TemplateId() < b.TemplateId();
                  return a.Key().wire() < b.Key().wire();
              });
    plan.affectedEntities = plan.DerivedAffectedEntities();
    plan.syncImpact = plan.DerivedSyncImpact();
    std::sort(plan.memberSnapshots.begin(), plan.memberSnapshots.end(),
              [](const auto& a, const auto& b) { return a.entityUuid < b.entityUuid; });
    std::sort(plan.rootSnapshots.begin(), plan.rootSnapshots.end(),
              [](const auto& a, const auto& b) { return a.rootUuid < b.rootUuid; });
    std::sort(plan.diagnostics.begin(), plan.diagnostics.end());
    return Result<DiscoveredPropagationPlan>::Ok(std::move(plan));
}

} // namespace rt2::core
