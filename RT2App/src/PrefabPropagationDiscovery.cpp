#include "PrefabPropagationDiscovery.h"

#include "AssetIdentity.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
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

std::filesystem::path CanonicalPath(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : canonical.lexically_normal();
}

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

bool HasEntity(const SceneDocument& document, entt::entity entity)
{
    return entity != entt::null && document.ecs.registry.valid(entity) &&
           document.ecs.registry.all_of<EntityIdComponent>(entity);
}

bool ValidateOverrides(const PrefabMemberComponent& member, std::string& reason)
{
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
};

InstanceValidation ValidateInstance(const SceneDocument& document,
                                    entt::entity root,
                                    const PrefabInstanceComponent& link,
                                    const SourceModel& source)
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
        if (!HasEntity(document, entity))
        {
            result.reason = "instance member has no durable entity UUID";
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
        if (!ValidateOverrides(member, overrideReason))
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
            allMembers[member.instanceId].push_back(entity);
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
    return result;
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
    const auto targetPath = CanonicalPath(changed.resolvedPath);

    std::string sourceBytes;
    Error sourceError;
    if (!ReadBytes(targetPath, sourceBytes, sourceError))
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
    std::vector<entt::entity> roots;
    auto rootView = request.document->ecs.registry.view<PrefabInstanceComponent,
                                                        EntityIdComponent>();
    for (const entt::entity root : rootView)
    {
        const auto& link = rootView.get<PrefabInstanceComponent>(root);
        std::vector<AssetDiagnostic> rootDiagnostics;
        const auto resolved = Resolve(link.prefab, request.assets,
                                      rootView.get<EntityIdComponent>(root).id,
                                      {}, rootDiagnostics);
        if (!resolved.success || resolved.effectiveId.IsNull() ||
            resolved.effectiveId != fingerprint.assetId ||
            CanonicalPath(resolved.resolvedPath) != fingerprint.normalizedPath)
            continue;
        roots.push_back(root);
    }

    PrefabPropagationPlan plan;
    plan.source = fingerprint;
    plan.documentGeneration = request.documentGeneration;
    plan.resourceGeneration = request.resourceGeneration;
    plan.sourceSchemaVersion = PrefabSerializer::FormatVersion;

    std::sort(roots.begin(), roots.end(), [&](entt::entity a, entt::entity b) {
        const auto& am = rootView.get<PrefabInstanceComponent>(a);
        const auto& bm = rootView.get<PrefabInstanceComponent>(b);
        if (am.instanceId != bm.instanceId) return am.instanceId < bm.instanceId;
        return rootView.get<EntityIdComponent>(a).id < rootView.get<EntityIdComponent>(b).id;
    });

    std::unordered_set<UUID> duplicateInstanceIds;
    for (std::size_t i = 1; i < roots.size(); ++i)
    {
        const auto& previous = rootView.get<PrefabInstanceComponent>(roots[i - 1]);
        const auto& current = rootView.get<PrefabInstanceComponent>(roots[i]);
        if (!previous.instanceId.IsNull() && previous.instanceId == current.instanceId)
            duplicateInstanceIds.insert(current.instanceId);
    }

    const auto loader = request.load
        ? request.load
        : [](PrefabDocument& value, const std::filesystem::path& path, Error& error) {
              return PrefabSerializer::Load(value, path, error);
          };
    std::vector<LoadedSource> loaded;
    if (!roots.empty())
    {
        PrefabDocument document;
        if (!loader(document, targetPath, sourceError))
            return Result<PrefabPropagationPlan>::Fail(
                sourceError.code, sourceError.path, sourceError.detail);
        SourceModel model;
        if (!BuildSourceModel(std::move(document), model, sourceError))
            return Result<PrefabPropagationPlan>::Fail(
                sourceError.code, targetPath.string(), sourceError.detail);
        loaded.push_back({ fingerprint, std::move(model) });
    }

    for (const entt::entity root : roots)
    {
        const auto& link = rootView.get<PrefabInstanceComponent>(root);
        const UUID rootUuid = rootView.get<EntityIdComponent>(root).id;
        InstanceValidation validation;
        if (duplicateInstanceIds.count(link.instanceId) != 0)
            validation.reason = "instance has multiple PrefabInstanceComponent roots";
        else
            validation = ValidateInstance(*request.document, root, link,
                                          loaded.front().model);
        PrefabPropagationInstancePlan instance;
        instance.instanceId = link.instanceId;
        instance.rootUuid = rootUuid;
        if (validation.valid)
        {
            instance.disposition = PrefabPropagationInstanceDisposition::Propagate;
            instance.affectedEntities = validation.entities;
        }
        else
        {
            instance.disposition = PrefabPropagationInstanceDisposition::Quarantined;
            instance.diagnostics.push_back(MakeDiagnostic(
                fingerprint, link, rootUuid, validation));
            plan.diagnostics.push_back(instance.diagnostics.front());
        }
        plan.instances.push_back(std::move(instance));
    }
    std::sort(plan.diagnostics.begin(), plan.diagnostics.end());
    return Result<PrefabPropagationPlan>::Ok(std::move(plan));
}

} // namespace rt2::core
