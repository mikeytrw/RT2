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

std::string DigestBytes(const std::string& bytes, const UUID& sidecar)
{
    // A deterministic content digest is sufficient at this contract boundary;
    // the digest covers bytes and the durable sidecar identity, never mtime.
    std::uint64_t hash = 1469598103934665603ull;
    auto add = [&](unsigned char c) {
        hash ^= c;
        hash *= 1099511628211ull;
    };
    for (const unsigned char c : bytes) add(c);
    for (const unsigned char c : sidecar.bytes) add(c);
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
        if (!resolved || *resolved != key || !IsOverridable(*resolved))
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

template<typename T>
std::optional<T> CopyComponent(const entt::registry& registry, entt::entity entity)
{
    const auto* value = registry.try_get<T>(entity);
    return value ? std::optional<T>(*value) : std::nullopt;
}

bool HasOverride(const PrefabMemberComponent& member, const PrefabComponentKey& key)
{
    return std::find(member.overrides.begin(), member.overrides.end(), key) !=
           member.overrides.end();
}

template<typename T>
void AddOperation(std::vector<PrefabPropagationComponentOperation>& operations,
                  const UUID& entityUuid, const UUID& templateId,
                  const PrefabComponentKey& key,
                  const std::optional<T>& before,
                  const std::optional<T>& after)
{
    PrefabPropagationComponentOperation operation;
    operation.entityUuid = entityUuid;
    operation.templateId = templateId;
    operation.key = key;
    if (before) operation.before = PrefabPropagationComponentValue{*before};
    if (after) operation.after = PrefabPropagationComponentValue{*after};
    if (!PrefabPropagationComponentOperationIsNoOp(operation))
        operations.push_back(std::move(operation));
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
                         std::vector<PrefabPropagationComponentOperation>& output,
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

    std::vector<PrefabPropagationComponentOperation> staged;
    for (const UUID& templateId : templates)
    {
        const auto& sourceRecord = source.byTemplate.at(templateId)->record;
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
            *member, PrefabComponentKeyFor<PrimitiveComponent>::value);
        const bool markerMaterial = HasOverride(
            *member, PrefabComponentKeyFor<MaterialOverrideComponent>::value);
        const auto beforeName = CopyComponent<NameComponent>(registry, entity);
        const auto beforeTransform = CopyComponent<Transform>(registry, entity);
        const auto beforeVisible = CopyComponent<VisibleComponent>(registry, entity);
        const auto beforePrimitive = CopyComponent<PrimitiveComponent>(registry, entity);
        const auto beforeImported = CopyComponent<ImportedMeshSourceComponent>(registry, entity);
        const auto beforeMaterial = CopyComponent<MaterialOverrideComponent>(registry, entity);
        const auto beforeLight = CopyComponent<LightComponent>(registry, entity);
        const auto beforeCamera = CopyComponent<CameraComponent>(registry, entity);
        const auto beforeMotion = CopyComponent<MotionComponent>(registry, entity);
        const auto beforeScript = CopyComponent<ScriptComponent>(registry, entity);

        const std::optional<PrimitiveComponent> sourcePrimitiveValue =
            sourceRecord.hasPrimitive
                ? std::optional<PrimitiveComponent>(sourceRecord.primitive)
                : std::nullopt;
        const std::optional<ImportedMeshSourceComponent> sourceImportedValue =
            sourceRecord.hasImportedSource
                ? std::optional<ImportedMeshSourceComponent>(sourceRecord.importedSource)
                : std::nullopt;
        const std::optional<MaterialOverrideComponent> sourceMaterialValue =
            sourceRecord.hasMaterialOverride
                ? std::optional<MaterialOverrideComponent>(sourceRecord.materialOverride)
                : std::nullopt;
        const auto afterPrimitive = markerPrimitive ? beforePrimitive : sourcePrimitiveValue;
        const auto afterImported = sourceImportedValue;
        const auto afterMaterial = markerMaterial ? beforeMaterial : sourceMaterialValue;

        std::optional<NameComponent> afterName;
        if (HasOverride(*member, PrefabComponentKeyFor<NameComponent>::value))
            afterName = beforeName;
        else
            afterName = NameComponent{templateId == source.rootTemplate
                                          ? InheritedRootName(sourceRecord.name)
                                          : sourceRecord.name};
        if (!HasOverride(*member, PrefabComponentKeyFor<NameComponent>::value))
            AddOperation(staged, id->id, templateId,
                         PrefabComponentKeyFor<NameComponent>::value,
                         beforeName, afterName);

        std::optional<Transform> afterTransform;
        if (HasOverride(*member, PrefabComponentKeyFor<Transform>::value))
            afterTransform = beforeTransform;
        else
            afterTransform = Transform{sourceRecord.translation,
                                       sourceRecord.rotation,
                                       sourceRecord.scale};
        if (!HasOverride(*member, PrefabComponentKeyFor<Transform>::value))
            AddOperation(staged, id->id, templateId,
                         PrefabComponentKeyFor<Transform>::value,
                         beforeTransform, afterTransform);

        std::optional<VisibleComponent> afterVisible;
        if (HasOverride(*member, PrefabComponentKeyFor<VisibleComponent>::value))
            afterVisible = beforeVisible;
        else
            afterVisible = VisibleComponent{sourceRecord.visible};
        if (!HasOverride(*member, PrefabComponentKeyFor<VisibleComponent>::value))
            AddOperation(staged, id->id, templateId,
                         PrefabComponentKeyFor<VisibleComponent>::value,
                         beforeVisible, afterVisible);

        if (!markerPrimitive)
            AddOperation(staged, id->id, templateId,
                         PrefabComponentKeyFor<PrimitiveComponent>::value,
                         beforePrimitive, afterPrimitive);
        AddOperation(staged, id->id, templateId,
                     PrefabComponentKeyFor<ImportedMeshSourceComponent>::value,
                     beforeImported, afterImported);
        if (!markerMaterial)
            AddOperation(staged, id->id, templateId,
                         PrefabComponentKeyFor<MaterialOverrideComponent>::value,
                         beforeMaterial, afterMaterial);

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

        std::optional<LightComponent> sourceLight =
            sourceRecord.hasLight ? std::optional<LightComponent>(sourceRecord.light)
                                  : std::nullopt;
        std::optional<LightComponent> afterLight =
            HasOverride(*member, PrefabComponentKeyFor<LightComponent>::value)
                ? beforeLight : sourceLight;
        if (!HasOverride(*member, PrefabComponentKeyFor<LightComponent>::value))
            AddOperation(staged, id->id, templateId,
                         PrefabComponentKeyFor<LightComponent>::value,
                         beforeLight, afterLight);

        std::optional<CameraComponent> sourceCamera =
            sourceRecord.hasCamera ? std::optional<CameraComponent>(sourceRecord.camera)
                                   : std::nullopt;
        std::optional<CameraComponent> afterCamera =
            HasOverride(*member, PrefabComponentKeyFor<CameraComponent>::value)
                ? beforeCamera : sourceCamera;
        if (!HasOverride(*member, PrefabComponentKeyFor<CameraComponent>::value))
            AddOperation(staged, id->id, templateId,
                         PrefabComponentKeyFor<CameraComponent>::value,
                         beforeCamera, afterCamera);

        std::optional<MotionComponent> sourceMotion =
            sourceRecord.hasMotion ? std::optional<MotionComponent>(sourceRecord.motion)
                                   : std::nullopt;
        std::optional<MotionComponent> afterMotion =
            HasOverride(*member, PrefabComponentKeyFor<MotionComponent>::value)
                ? beforeMotion : sourceMotion;
        if (!HasOverride(*member, PrefabComponentKeyFor<MotionComponent>::value))
            AddOperation(staged, id->id, templateId,
                         PrefabComponentKeyFor<MotionComponent>::value,
                         beforeMotion, afterMotion);

        std::optional<ScriptComponent> sourceScript;
        if (sourceRecord.hasScript)
        {
            sourceScript = sourceRecord.script;
            std::vector<ScriptComponent*> remapped{&*sourceScript};
            RemapEntityReferences(remap, remapped);
        }
        std::optional<ScriptComponent> afterScript =
            HasOverride(*member, PrefabComponentKeyFor<ScriptComponent>::value)
                ? beforeScript : sourceScript;
        if (!HasOverride(*member, PrefabComponentKeyFor<ScriptComponent>::value))
            AddOperation(staged, id->id, templateId,
                         PrefabComponentKeyFor<ScriptComponent>::value,
                         beforeScript, afterScript);
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

} // namespace

Result<PrefabPropagationPlan> PreparePrefabPropagation(
    const PrefabPropagationDiscoveryRequest& request)
{
    if (!request.document)
        return Result<PrefabPropagationPlan>::Fail(
            Error::InvalidArgument, {}, "discovery requires a scene document");
    if (request.changedSource.kind != AssetKind::Prefab ||
        (request.changedSource.path.empty() && request.changedSource.assetId.IsNull()))
        return Result<PrefabPropagationPlan>::Fail(
            Error::InvalidArgument, {}, "changed source is not a prefab identity");

    std::vector<AssetDiagnostic> resolutionDiagnostics;
    const auto changed = Resolve(request.changedSource, request.assets,
                                 UUID::Nil(), {}, resolutionDiagnostics);
    if (!changed.success || changed.effectiveId.IsNull())
        return Result<PrefabPropagationPlan>::Fail(
            Error::MissingAsset, request.changedSource.path,
            "changed prefab source has no verified durable identity");
    const auto targetPath = CanonicalAssetPath(changed.resolvedPath);

    std::string sourceBytes;
    Error sourceError;
    const auto readBytes = request.readBytes
        ? request.readBytes
        : [](const std::filesystem::path& path, std::string& bytes, Error& error) {
              return ReadBytes(path, bytes, error);
          };
    if (!readBytes(targetPath, sourceBytes, sourceError))
        return Result<PrefabPropagationPlan>::Fail(
            sourceError.code, sourceError.path, sourceError.detail);
    Error sidecarError;
    const UUID sidecarId = ReadSidecarId(AssetSidecarPath(targetPath), sidecarError);
    if (!sidecarError.IsOk() || sidecarId.IsNull() || sidecarId != changed.effectiveId)
        return Result<PrefabPropagationPlan>::Fail(
            sidecarError.IsOk() ? Error::MissingAsset : sidecarError.code,
            targetPath.string(), "prefab source sidecar identity is missing or mismatched");

    const auto fingerprintDigest = request.fingerprint
        ? request.fingerprint(sourceBytes, sidecarId)
        : DigestBytes(sourceBytes, sidecarId);
    if (fingerprintDigest.empty())
        return Result<PrefabPropagationPlan>::Fail(
            Error::Parse, targetPath.string(),
            "prefab source fingerprint computation returned an empty digest");
    PrefabSourceFingerprint fingerprint{ targetPath, sidecarId,
                                         fingerprintDigest };

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
        const auto resolved = Resolve(link.prefab, request.assets,
                                      rootUuid,
                                      {}, rootDiagnostics);
        if (!resolved.success || resolved.effectiveId.IsNull() ||
            resolved.effectiveId != fingerprint.assetId ||
            CanonicalAssetPath(resolved.resolvedPath) != fingerprint.normalizedPath)
            continue;
        roots.push_back(root);
    }

    PrefabPropagationPlan plan;
    plan.source = fingerprint;
    plan.documentGeneration = request.documentGeneration;
    plan.resourceGeneration = request.resourceGeneration;
    plan.authoringRevision = request.authoringRevision;
    plan.authoringRevisionCaptured = true;
    plan.sourceSchemaVersion = PrefabSerializer::FormatVersion;
    plan.meshTableExtent = request.document->ecs.meshRegistry.GetCount();
    plan.materialTableExtent = static_cast<std::uint32_t>(request.document->ecs.materials.size());
    plan.textureTableExtent = static_cast<std::uint32_t>(request.document->ecs.textures.size());

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

    // Parse the exact immutable bytes already fingerprinted. The legacy path
    // seam remains available to hosts that explicitly inject it, but the
    // production default never reopens the source after the snapshot read.
    PrefabDocument document;
    bool parsed = false;
    if (request.parseBytes)
        parsed = request.parseBytes(document, sourceBytes, targetPath, sourceError);
    else if (request.load)
        parsed = request.load(document, targetPath, sourceError);
    else
        parsed = PrefabSerializer::LoadBytes(document, sourceBytes, targetPath, sourceError);
    if (!parsed)
        return Result<PrefabPropagationPlan>::Fail(
            sourceError.code, sourceError.path, sourceError.detail);
    SourceModel model;
    if (!BuildSourceModel(std::move(document), model, sourceError))
        return Result<PrefabPropagationPlan>::Fail(
            sourceError.code, targetPath.string(), sourceError.detail);
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
            std::vector<PrefabPropagationComponentOperation> reconciled;
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
                                  operation.entityUuid) == instance.affectedEntities.end())
                        instance.affectedEntities.push_back(operation.entityUuid);
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
                  if (a.entityUuid != b.entityUuid) return a.entityUuid < b.entityUuid;
                  if (a.templateId != b.templateId) return a.templateId < b.templateId;
                  return a.key.wire() < b.key.wire();
              });
    plan.affectedEntities = plan.DerivedAffectedEntities();
    plan.syncImpact = plan.DerivedSyncImpact();
    std::sort(plan.memberSnapshots.begin(), plan.memberSnapshots.end(),
              [](const auto& a, const auto& b) { return a.entityUuid < b.entityUuid; });
    std::sort(plan.rootSnapshots.begin(), plan.rootSnapshots.end(),
              [](const auto& a, const auto& b) { return a.rootUuid < b.rootUuid; });
    std::sort(plan.diagnostics.begin(), plan.diagnostics.end());
    return Result<PrefabPropagationPlan>::Ok(std::move(plan));
}

} // namespace rt2::core
