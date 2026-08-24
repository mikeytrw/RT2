#include "PrefabPropagationService.h"

#include "SceneSerializer.h"
#include "SceneDocument.h"

#include <algorithm>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace rt2::core {
namespace {

bool CheckedAdd(std::uint32_t a, std::uint32_t b, std::uint32_t& result)
{
    if (a > std::numeric_limits<std::uint32_t>::max() - b) return false;
    result = a + b;
    return true;
}

template<typename T>
std::optional<T> OperationAfter(const DiscoveredPropagationPlan& plan,
                                const UUID& entity, const PrefabComponentKey& key,
                                const entt::registry& live, entt::entity liveEntity)
{
    const auto it = std::find_if(plan.componentOperations.begin(),
        plan.componentOperations.end(), [&](const auto& operation) {
            return operation.EntityUuid() == entity && operation.Key() == key;
        });
    if (it != plan.componentOperations.end())
    {
        return it->Visit([](const auto& typed) -> std::optional<T> {
            using U = typename std::decay_t<decltype(typed)>::Type;
            if constexpr (std::is_same_v<T, U>)
                return typed.After();
            else
                return std::nullopt;
        });
    }
    return ReadPropagationComponent<T>(live, liveEntity);
}

const PrefabPropagationComponentDelta* FindOperation(
    const DiscoveredPropagationPlan& plan, const UUID& entity,
    const PrefabComponentKey& key)
{
    const auto it = std::find_if(plan.componentOperations.begin(),
        plan.componentOperations.end(), [&](const auto& operation) {
            return operation.EntityUuid() == entity && operation.Key() == key;
        });
    return it == plan.componentOperations.end() ? nullptr : &*it;
}

PrefabPropagationComponentDelta* FindOperation(
    DiscoveredPropagationPlan& plan, const UUID& entity,
    const PrefabComponentKey& key)
{
    const auto it = std::find_if(plan.componentOperations.begin(),
        plan.componentOperations.end(), [&](const auto& operation) {
            return operation.EntityUuid() == entity && operation.Key() == key;
        });
    return it == plan.componentOperations.end() ? nullptr : &*it;
}

bool AddOwnership(PrefabPropagationStageEvidence& stageEvidence,
                  PrefabPropagationResourceKind kind,
                  std::uint32_t sceneBeforeExtent,
                  std::uint32_t sourceBeforeExtent,
                  const std::vector<PrefabPropagationResourcePayload>& payloads)
{
    if (payloads.empty()) return true;
    if (payloads.size() > std::numeric_limits<std::uint32_t>::max() ||
        sceneBeforeExtent > std::numeric_limits<std::uint32_t>::max() -
            static_cast<std::uint32_t>(payloads.size()))
        return false;
    PrefabPropagationResourceOwnership ownership;
    auto& rebase = ownership.rebase;
    rebase.kind = kind;
    rebase.sourceBeforeExtent = sourceBeforeExtent;
    rebase.sceneBeforeExtent = sceneBeforeExtent;
    rebase.sceneAppendBase = sceneBeforeExtent;
    rebase.sceneAfterExtent = sceneBeforeExtent + static_cast<std::uint32_t>(payloads.size());
    rebase.owned = PrefabPropagationResourceBlock::FromDecoded(kind, payloads);
    for (std::size_t i = 0; i < payloads.size(); ++i)
    {
        if (i > std::numeric_limits<std::uint32_t>::max()) return false;
        rebase.sourceSlots.push_back({static_cast<std::uint32_t>(i)});
        rebase.sceneSlots.push_back({sceneBeforeExtent + static_cast<std::uint32_t>(i)});
    }
    stageEvidence.resourceOwnership.push_back(std::move(ownership));
    return true;
}

bool RebaseMaterial(SceneMaterial& material, std::uint32_t textureBase)
{
    auto rebase = [textureBase](int& index) {
        if (index < 0) return index == -1;
        if (textureBase > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            index > std::numeric_limits<int>::max() - static_cast<int>(textureBase))
            return false;
        index += static_cast<int>(textureBase);
        return true;
    };
    return rebase(material.baseColorTextureIndex) &&
           rebase(material.normalTextureIndex) &&
           rebase(material.emissiveTextureIndex) &&
           rebase(material.metallicRoughnessTextureIndex);
}

bool RebaseMesh(MeshData& mesh, std::uint32_t materialBase)
{
    for (auto& index : mesh.materialIndices)
    {
        if (index > std::numeric_limits<std::uint32_t>::max() - materialBase)
            return false;
        index += materialBase;
    }
    return true;
}

std::optional<PrefabPropagationRootSnapshot> FindRootSnapshot(
    const DiscoveredPropagationPlan& plan, const PrefabPropagationInstancePlan& instance)
{
    const auto it = std::find_if(plan.rootSnapshots.begin(), plan.rootSnapshots.end(),
        [&](const auto& snapshot) { return snapshot.rootUuid == instance.rootUuid; });
    if (it == plan.rootSnapshots.end()) return std::nullopt;
    return *it;
}

PrefabPropagationDiagnostic MakeResolverDiagnostic(
    const DiscoveredPropagationPlan& result,
    const PrefabPropagationInstancePlan& instance,
    const AssetDiagnostic& diagnostic,
    const UUID& fallbackTemplate)
{
    PrefabPropagationDiagnostic converted;
    converted.severity = static_cast<AssetDiagnostic::Severity>(diagnostic.severity);
    converted.prefabPath = result.source.normalizedPath;
    converted.prefabAssetId = result.source.assetId;
    converted.instanceId = instance.instanceId;
    converted.rootUuid = instance.rootUuid;
    converted.templateId = fallbackTemplate;
    for (const auto& snapshot : result.memberSnapshots)
        if (snapshot.entityUuid == diagnostic.entityUuid)
        {
            converted.templateId = snapshot.templateId;
            break;
    }
    converted.reason = diagnostic.detail.empty() ? diagnostic.refPath : diagnostic.detail;
    return converted;
}

bool ApplyComponentOperation(entt::registry& registry, entt::entity entity,
                             const PrefabPropagationComponentDelta& operation)
{
    WritePropagationComponent(operation, registry, entity, true);
    return true;
}

} // namespace

Result<StageOutcome> StagePrefabPropagationResources(
    const DiscoveredPropagationPlan& durablePlan,
    const SceneDocument& live,
    const AssetResolutionContext& assets,
    const PrefabPropagationResourceHooks& hooks)
{
    if (!durablePlan.IsValid())
        return Result<StageOutcome>::Fail(
            Error::InvalidArgument, "prefab-propagation", "durable plan is invalid");

    // A canonical no-op is returned before creating a clone or asking the
    // resolver to inspect any asset. This is the isolation boundary that keeps
    // unrelated imported entities from turning a no-op into history.
    if (durablePlan.IsNoOp())
        return Result<StageOutcome>::Ok(StageOutcome::Build(
            DiscoveredPropagationPlan(durablePlan),
            PrefabPropagationStageEvidence{}, false));

    DiscoveredPropagationPlan result = durablePlan;
    PrefabPropagationStageEvidence stageEvidence;

    std::vector<PrefabPropagationResourcePayload> meshPayloads;
    std::vector<PrefabPropagationResourcePayload> materialPayloads;
    std::vector<PrefabPropagationResourcePayload> texturePayloads;
    std::unordered_set<UUID> quarantinedInstances;
    std::unordered_map<UUID, UUID> entityToInstance;
    std::unordered_map<UUID, UUID> entityToTemplate;
    for (const auto& snapshot : durablePlan.memberSnapshots)
    {
        entityToInstance[snapshot.entityUuid] = snapshot.instanceId;
        entityToTemplate[snapshot.entityUuid] = snapshot.templateId;
    }
    for (const auto& instance : result.instances)
        if (instance.disposition == PrefabPropagationInstanceDisposition::Quarantined)
            quarantinedInstances.insert(instance.instanceId);

    for (auto& instance : result.instances)
    {
        if (instance.disposition != PrefabPropagationInstanceDisposition::Propagate)
            continue;

        std::vector<UUID> entities;
        for (const auto& snapshot : durablePlan.memberSnapshots)
            if (snapshot.instanceId == instance.instanceId)
                entities.push_back(snapshot.entityUuid);
        std::sort(entities.begin(), entities.end());

        SceneDocument fragment;
        fragment.metadata.schemaVersion = live.metadata.schemaVersion;
        std::vector<UUID> resourceEntities;
        for (const UUID& uuid : entities)
        {
            const auto liveEntity = live.FindByUuid(uuid);
            if (liveEntity == entt::null || !live.ecs.registry.valid(liveEntity))
                continue;
            const auto* id = live.ecs.registry.try_get<EntityIdComponent>(liveEntity);
            if (!id) continue;

            const auto* importedOperation = FindOperation(durablePlan, uuid,
                PropagationComponentKey<ImportedMeshSourceComponent>());
            const auto* primitiveOperation = FindOperation(durablePlan, uuid,
                PropagationComponentKey<PrimitiveComponent>());
            const auto* materialOperation = FindOperation(durablePlan, uuid,
                PropagationComponentKey<MaterialOverrideComponent>());
            const auto liveImported = ReadPropagationComponent<ImportedMeshSourceComponent>(
                live.ecs.registry, liveEntity);
            // Resource staging is driven by an effective resource/provenance
            // operation.  A value-only operation must not re-resolve an
            // unchanged imported sibling in the same prefab instance.
            bool hasImported = importedOperation &&
            !importedOperation->IsNoOp() &&
                importedOperation->AfterValue().has_value();
            if (!hasImported && materialOperation &&
            !materialOperation->IsNoOp() &&
                materialOperation->AfterValue().has_value())
                hasImported = liveImported.has_value();
            const auto imported = hasImported
                ? OperationAfter<ImportedMeshSourceComponent>(
                    durablePlan, uuid, PropagationComponentKey<ImportedMeshSourceComponent>(),
                    live.ecs.registry, liveEntity)
                : std::optional<ImportedMeshSourceComponent>{};
            hasImported = imported.has_value();
            bool hasPrimitive = primitiveOperation &&
            !primitiveOperation->IsNoOp() &&
                primitiveOperation->AfterValue().has_value();
            const auto primitive = OperationAfter<PrimitiveComponent>(
                durablePlan, uuid, PropagationComponentKey<PrimitiveComponent>(),
                live.ecs.registry, liveEntity);
            hasPrimitive = hasPrimitive && primitive.has_value();
            if (!hasImported && !hasPrimitive) continue;

            const auto fragmentEntity = fragment.ecs.registry.create();
            fragment.AssignKnownUuid(fragmentEntity, uuid);
            const auto liveName = ReadPropagationComponent<NameComponent>(
                live.ecs.registry, liveEntity);
            if (liveName)
                fragment.ecs.registry.emplace<NameComponent>(fragmentEntity, *liveName);
            if (const auto* member = live.ecs.registry.try_get<PrefabMemberComponent>(liveEntity))
                fragment.ecs.registry.emplace<PrefabMemberComponent>(fragmentEntity, *member);
            if (imported)
                fragment.ecs.registry.emplace<ImportedMeshSourceComponent>(fragmentEntity, *imported);
            if (const auto material = OperationAfter<MaterialOverrideComponent>(
                    durablePlan, uuid, PropagationComponentKey<MaterialOverrideComponent>(),
                    live.ecs.registry, liveEntity))
                fragment.ecs.registry.emplace<MaterialOverrideComponent>(fragmentEntity, *material);
            if (hasPrimitive && primitive)
            {
                fragment.ecs.registry.emplace<PrimitiveComponent>(fragmentEntity, *primitive);
                const auto mesh = RegisterPrimitiveMesh(fragment.ecs.meshRegistry, *primitive);
                fragment.ecs.registry.emplace<MeshRef>(fragmentEntity, MeshRef{mesh, -1});
            }
            if (imported) resourceEntities.push_back(uuid);
        }

        std::vector<AssetDiagnostic> diagnostics;
        Error error;
        bool resolved = true;
        if (!resourceEntities.empty())
            resolved = SceneAssetResolver::ResolveAll(fragment, assets, diagnostics, error);

        bool hardFailure = !resolved && diagnostics.empty();
        for (const auto& diagnostic : diagnostics)
        {
            const auto converted = MakeResolverDiagnostic(result, instance, diagnostic,
                resourceEntities.empty() ? UUID::Nil() :
                    entityToTemplate[resourceEntities.front()]);
            instance.diagnostics.push_back(converted);
            result.diagnostics.push_back(converted);
            hardFailure |= diagnostic.severity >= AssetDiagnostic::Missing;
        }
        if (hardFailure)
        {
            quarantinedInstances.insert(instance.instanceId);
            instance.disposition = PrefabPropagationInstanceDisposition::Quarantined;
            instance.affectedEntities.clear();
            if (diagnostics.empty())
            {
                PrefabPropagationDiagnostic diagnostic;
                diagnostic.severity = AssetDiagnostic::Malformed;
                diagnostic.prefabPath = result.source.normalizedPath;
                diagnostic.prefabAssetId = result.source.assetId;
                diagnostic.instanceId = instance.instanceId;
                diagnostic.rootUuid = instance.rootUuid;
                diagnostic.templateId = resourceEntities.empty() ? UUID::Nil() :
                    entityToTemplate[resourceEntities.front()];
                diagnostic.reason = error.detail.empty() ? "candidate resource resolution failed" : error.detail;
                instance.diagnostics.push_back(diagnostic);
                result.diagnostics.push_back(std::move(diagnostic));
            }
            continue;
        }

        if (meshPayloads.size() > std::numeric_limits<std::uint32_t>::max() ||
            materialPayloads.size() > std::numeric_limits<std::uint32_t>::max() ||
            texturePayloads.size() > std::numeric_limits<std::uint32_t>::max())
            return Result<StageOutcome>::Fail(Error::InvalidArgument,
                "prefab-propagation", "resource payload count overflow while staging");
        const auto meshBase = static_cast<std::uint32_t>(meshPayloads.size());
        const auto materialBase = static_cast<std::uint32_t>(materialPayloads.size());
        const auto textureBase = static_cast<std::uint32_t>(texturePayloads.size());
        for (std::uint32_t i = 0; i < fragment.ecs.meshRegistry.GetCount(); ++i)
        {
            auto mesh = fragment.ecs.meshRegistry.GetMesh(i);
            std::uint32_t materialRebase = 0;
            std::uint32_t sceneMeshBase = 0;
            std::uint32_t sceneMeshIndex = 0;
            if (!CheckedAdd(durablePlan.materialTableExtent, materialBase, materialRebase) ||
                !CheckedAdd(durablePlan.meshTableExtent, meshBase, sceneMeshBase) ||
                !CheckedAdd(sceneMeshBase, i, sceneMeshIndex) ||
                !RebaseMesh(mesh, materialRebase))
                return Result<StageOutcome>::Fail(Error::InvalidArgument,
                    "prefab-propagation", "mesh material index overflow while staging");
            meshPayloads.push_back({"mesh:" + std::to_string(sceneMeshIndex),
                                    result.source.contentDigest, std::move(mesh)});
        }
        for (std::size_t i = 0; i < fragment.ecs.materials.size(); ++i)
        {
            auto material = fragment.ecs.materials[i];
            std::uint32_t textureRebase = 0;
            std::uint32_t sceneMaterialBase = 0;
            std::uint32_t sceneMaterialIndex = 0;
            if (i > std::numeric_limits<std::uint32_t>::max() ||
                !CheckedAdd(durablePlan.textureTableExtent, textureBase, textureRebase) ||
                !CheckedAdd(durablePlan.materialTableExtent, materialBase, sceneMaterialBase) ||
                !CheckedAdd(sceneMaterialBase, static_cast<std::uint32_t>(i), sceneMaterialIndex) ||
                !RebaseMaterial(material, textureRebase))
                return Result<StageOutcome>::Fail(Error::InvalidArgument,
                    "prefab-propagation", "material texture index overflow while staging");
            materialPayloads.push_back({"material:" + std::to_string(sceneMaterialIndex),
                result.source.contentDigest, std::move(material)});
        }
        for (std::size_t i = 0; i < fragment.ecs.textures.size(); ++i)
        {
            const auto& texture = fragment.ecs.textures[i];
            std::uint32_t sceneTextureBase = 0;
            std::uint32_t sceneTextureIndex = 0;
            if (i > std::numeric_limits<std::uint32_t>::max() ||
                !CheckedAdd(durablePlan.textureTableExtent, textureBase, sceneTextureBase) ||
                !CheckedAdd(sceneTextureBase, static_cast<std::uint32_t>(i), sceneTextureIndex))
                return Result<StageOutcome>::Fail(Error::InvalidArgument,
                    "prefab-propagation", "texture extent overflow while staging");
            texturePayloads.push_back({"texture:" + std::to_string(sceneTextureIndex),
                result.source.contentDigest, texture});
        }

        for (const UUID& uuid : entities)
        {
            const auto liveEntity = live.FindByUuid(uuid);
            const auto stagedEntity = fragment.FindByUuid(uuid);
            if (liveEntity == entt::null || stagedEntity == entt::null) continue;
            const auto* before = live.ecs.registry.try_get<MeshRef>(liveEntity);
            const auto* after = fragment.ecs.registry.try_get<MeshRef>(stagedEntity);
            std::optional<MeshRef> afterRef;
            if (after)
            {
                afterRef = *after;
                std::uint32_t meshRebase = 0;
                std::uint32_t materialRebase = 0;
                if (!CheckedAdd(durablePlan.meshTableExtent, meshBase, meshRebase) ||
                    !CheckedAdd(durablePlan.materialTableExtent, materialBase, materialRebase) ||
                    afterRef->meshIndex > std::numeric_limits<std::uint32_t>::max() - meshRebase)
                    return Result<StageOutcome>::Fail(Error::InvalidArgument,
                        "prefab-propagation", "MeshRef mesh index overflow while staging");
                afterRef->meshIndex += meshRebase;
                if (afterRef->materialIndex >= 0)
                {
                    if (materialRebase > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
                        afterRef->materialIndex > std::numeric_limits<int>::max() -
                            static_cast<int>(materialRebase))
                        return Result<StageOutcome>::Fail(Error::InvalidArgument,
                            "prefab-propagation", "MeshRef material index overflow while staging");
                    afterRef->materialIndex += static_cast<int>(materialRebase);
                }
            }
            if ((before == nullptr) != (afterRef == std::nullopt) ||
                (before && afterRef && (before->meshIndex != afterRef->meshIndex ||
                    before->materialIndex != afterRef->materialIndex)))
                stageEvidence.meshRefOperations.push_back({uuid, entityToTemplate[uuid],
                    before ? std::optional<MeshRef>(*before) : std::nullopt, afterRef});

            if (auto* material = fragment.ecs.registry.try_get<MaterialOverrideComponent>(stagedEntity))
            {
                auto repaired = *material;
                if (durablePlan.textureTableExtent >
                        std::numeric_limits<std::uint32_t>::max() - textureBase ||
                    !RebaseMaterial(repaired.material,
                        durablePlan.textureTableExtent + textureBase))
                    return Result<StageOutcome>::Fail(Error::InvalidArgument,
                        "prefab-propagation", "override texture index overflow while staging");
                if (repaired.materialIndex >= 0)
                {
                    std::uint32_t materialRebase = 0;
                    if (static_cast<std::size_t>(repaired.materialIndex) >=
                            fragment.ecs.materials.size() ||
                        !CheckedAdd(durablePlan.materialTableExtent, materialBase, materialRebase) ||
                        static_cast<std::uint32_t>(repaired.materialIndex) >
                            std::numeric_limits<std::uint32_t>::max() -
                                materialRebase)
                        return Result<StageOutcome>::Fail(Error::InvalidArgument,
                            "prefab-propagation", "override material index invalid while staging");
                    if (materialRebase > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
                        repaired.materialIndex > std::numeric_limits<int>::max() -
                            static_cast<int>(materialRebase))
                        return Result<StageOutcome>::Fail(Error::InvalidArgument,
                            "prefab-propagation", "override material index overflow while staging");
                    repaired.materialIndex += static_cast<int>(materialRebase);
                }
                if (auto* operation = FindOperation(result, uuid,
                        PropagationComponentKey<MaterialOverrideComponent>()))
                {
                    if (!operation->TryWithAfter<MaterialOverrideComponent>(repaired))
                        return Result<StageOutcome>::Fail(
                            Error::InvalidArgument, "prefab-propagation",
                            "material repair targeted a mismatched typed delta");
                }
                else
                {
                    const auto liveMaterial = hooks.readLiveMaterialOverride
                        ? hooks.readLiveMaterialOverride(live.ecs.registry, liveEntity)
                        : ReadPropagationComponent<MaterialOverrideComponent>(
                            live.ecs.registry, liveEntity);
                    const auto candidate =
                        PrefabPropagationComponentDelta::Make<MaterialOverrideComponent>(
                            uuid, entityToTemplate[uuid], liveMaterial, repaired);
                    if (!candidate.IsNoOp())
                        result.componentOperations.push_back(
                            candidate);
                }
            }
        }
    }

    // A hard resolver error quarantines the complete instance: no sibling
    // value, MeshRef, snapshot, or resource intent may survive.
    result.componentOperations.erase(std::remove_if(result.componentOperations.begin(),
        result.componentOperations.end(), [&](const auto& operation) {
            const auto it = entityToInstance.find(operation.EntityUuid());
            return it != entityToInstance.end() && quarantinedInstances.count(it->second) != 0;
        }), result.componentOperations.end());
    result.memberSnapshots.erase(std::remove_if(result.memberSnapshots.begin(),
        result.memberSnapshots.end(), [&](const auto& snapshot) {
            return quarantinedInstances.count(snapshot.instanceId) != 0;
        }), result.memberSnapshots.end());
    stageEvidence.meshRefOperations.erase(std::remove_if(stageEvidence.meshRefOperations.begin(),
        stageEvidence.meshRefOperations.end(), [&](const auto& operation) {
            const auto it = entityToInstance.find(operation.entityUuid);
            return it != entityToInstance.end() && quarantinedInstances.count(it->second) != 0;
        }), stageEvidence.meshRefOperations.end());

    if (!AddOwnership(stageEvidence, PrefabPropagationResourceKind::Mesh,
                      durablePlan.meshTableExtent,
                      static_cast<std::uint32_t>(meshPayloads.size()), meshPayloads) ||
        !AddOwnership(stageEvidence, PrefabPropagationResourceKind::Material,
                      durablePlan.materialTableExtent,
                      static_cast<std::uint32_t>(materialPayloads.size()), materialPayloads) ||
        !AddOwnership(stageEvidence, PrefabPropagationResourceKind::Texture,
                      durablePlan.textureTableExtent,
                      static_cast<std::uint32_t>(texturePayloads.size()), texturePayloads))
        return Result<StageOutcome>::Fail(Error::InvalidArgument,
            "prefab-propagation", "resource extent overflow while staging");

    for (auto& instance : result.instances)
    {
        if (instance.disposition == PrefabPropagationInstanceDisposition::Quarantined)
            continue;
        instance.affectedEntities.clear();
        for (const auto& operation : result.componentOperations)
            if (entityToInstance[operation.EntityUuid()] == instance.instanceId &&
                !operation.IsNoOp())
                instance.affectedEntities.push_back(operation.EntityUuid());
        for (const auto& operation : stageEvidence.meshRefOperations)
            if (entityToInstance[operation.entityUuid] == instance.instanceId)
                instance.affectedEntities.push_back(operation.entityUuid);
        std::sort(instance.affectedEntities.begin(), instance.affectedEntities.end());
        instance.affectedEntities.erase(std::unique(instance.affectedEntities.begin(),
            instance.affectedEntities.end()), instance.affectedEntities.end());
        if (instance.affectedEntities.empty())
            instance.disposition = PrefabPropagationInstanceDisposition::NoOp;
    }
    // Root evidence is command precondition data only for actionable
    // instances. Quarantine/no-op roots remain in the summary disposition and
    // diagnostics, never in executable evidence.
    std::unordered_set<UUID> actionableInstances;
    for (const auto& instance : result.instances)
        if (instance.disposition == PrefabPropagationInstanceDisposition::Propagate)
            actionableInstances.insert(instance.instanceId);
    result.rootSnapshots.erase(std::remove_if(result.rootSnapshots.begin(),
        result.rootSnapshots.end(), [&](const auto& snapshot) {
            return actionableInstances.count(snapshot.instanceId) == 0;
        }), result.rootSnapshots.end());
    std::sort(result.componentOperations.begin(), result.componentOperations.end(),
        [](const auto& a, const auto& b) {
            if (a.EntityUuid() != b.EntityUuid()) return a.EntityUuid() < b.EntityUuid();
            if (a.TemplateId() != b.TemplateId()) return a.TemplateId() < b.TemplateId();
            return a.Key().wire() < b.Key().wire();
        });
    result.affectedEntities = result.DerivedAffectedEntities(stageEvidence);
    result.syncImpact = result.DerivedSyncImpact(stageEvidence);
    std::sort(result.diagnostics.begin(), result.diagnostics.end());
    if (!result.IsValid(stageEvidence))
        return Result<StageOutcome>::Fail(Error::InvalidArgument,
            "prefab-propagation", "staged resource plan failed validation");
    if (result.IsNoOp(stageEvidence))
        return Result<StageOutcome>::Ok(StageOutcome::Build(
            std::move(result), std::move(stageEvidence), false));
    if (!result.IsCommandReady(stageEvidence))
        return Result<StageOutcome>::Fail(Error::InvalidArgument,
            "prefab-propagation", "staged plan lacks complete command-readiness evidence");
    return Result<StageOutcome>::Ok(StageOutcome::Build(
        std::move(result), std::move(stageEvidence), true));
}

Result<PrefabPropagationLoadReport> ReconcilePrefabPropagationForLoad(
    SceneDocument& document, const AssetResolutionContext& assets,
    const PrefabPropagationLoadHooks& hooks)
{
    struct Candidate
    {
        AssetReference reference;
        std::string canonicalPath;
        UUID effectiveId;
    };

    std::map<std::string, Candidate> candidatesByPath;
    const auto roots = document.ecs.registry.view<PrefabInstanceComponent>();
    for (const entt::entity root : roots)
    {
        const auto& link = roots.get<PrefabInstanceComponent>(root);
        // Pre-capture selection must use the shared database-first authority;
        // authored spelling and reference ID are not captured evidence.
        const auto identity = ResolveCapturedAssetIdentity(link.prefab, assets);
        if (!identity.IsOk())
            return Result<PrefabPropagationLoadReport>::Fail(
                identity.error.code, identity.error.path, identity.error.detail);
        const auto canonicalPath = identity.value.normalizedPath.generic_string();
        const UUID effectiveId = identity.value.effectiveId;
        const auto existing = candidatesByPath.find(canonicalPath);
        if (existing != candidatesByPath.end())
        {
            const UUID oldId = existing->second.effectiveId;
            if (!oldId.IsNull() && !effectiveId.IsNull() && oldId != effectiveId)
                return Result<PrefabPropagationLoadReport>::Fail(
                    Error::InvalidArgument, canonicalPath,
                    "Conflict: same prefab path has conflicting durable asset IDs");
            if (oldId.IsNull() && !effectiveId.IsNull())
                existing->second = {link.prefab, canonicalPath, effectiveId};
            else if (oldId == effectiveId &&
                     link.prefab.path < existing->second.reference.path)
                existing->second.reference = link.prefab;
            continue;
        }
        candidatesByPath.emplace(canonicalPath,
            Candidate{link.prefab, canonicalPath, effectiveId});
    }
    std::vector<Candidate> candidates;
    candidates.reserve(candidatesByPath.size());
    for (auto& item : candidatesByPath)
        candidates.push_back(std::move(item.second));
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) {
            if (a.canonicalPath != b.canonicalPath)
                return a.canonicalPath < b.canonicalPath;
            return a.effectiveId < b.effectiveId;
        });

    // Prepare every source before applying any operation. This is the
    // transaction boundary for load/recovery: a malformed later source cannot
    // leave an earlier sibling partially reconciled.
    std::vector<DiscoveredPropagationPlan> plans;
    plans.reserve(candidates.size());
    for (const auto& candidate : candidates)
    {
        PrefabPropagationDiscoveryRequest request;
        request.document = &document;
        request.assets = assets;
        request.changedSource = candidate.reference;
        request.documentGeneration = 1;
        request.resourceGeneration = 1;
        request.documentGenerationCaptured = true;
        request.resourceGenerationCaptured = true;
        request.authoringRevision = 0;
        const auto captured = hooks.capture
            ? hooks.capture(candidate.reference, assets)
            : CapturePrefabSource(candidate.reference, assets);
        if (!captured.IsOk())
            return Result<PrefabPropagationLoadReport>::Fail(
                captured.error.code, captured.error.path,
                captured.error.detail);
        request.capturedSource = captured.value;
        const auto prepared = hooks.prepare ? hooks.prepare(request)
                                            : PreparePrefabPropagation(request);
        if (!prepared.IsOk())
            return Result<PrefabPropagationLoadReport>::Fail(
                prepared.error.code, prepared.error.path, prepared.error.detail);
        plans.push_back(prepared.value);
    }

    // Validate all preconditions before writing the temporary document.
    for (const auto& plan : plans)
    {
        if (!plan.IsValid())
            return Result<PrefabPropagationLoadReport>::Fail(
                Error::InvalidArgument, plan.source.normalizedPath.string(),
                "prefab propagation plan is invalid during load");
        for (const auto& operation : plan.componentOperations)
        {
            const auto entity = document.FindByUuid(operation.EntityUuid());
            if (entity == entt::null || !document.ecs.registry.valid(entity))
                return Result<PrefabPropagationLoadReport>::Fail(
                    Error::InvalidEntity, plan.source.normalizedPath.string(),
                    "prefab propagation target entity disappeared during load");
            const auto* member = document.ecs.registry.try_get<PrefabMemberComponent>(entity);
            if (!member || member->instanceId.IsNull() ||
                member->templateId != operation.TemplateId())
                return Result<PrefabPropagationLoadReport>::Fail(
                    Error::InvalidEntity, plan.source.normalizedPath.string(),
                    "prefab propagation target membership changed during load");
            const auto current = ReadPropagationComponent(
                operation, document.ecs.registry, entity);
            const auto before = operation.BeforeValue();
            if (current.has_value() != before.has_value() ||
                (current && before &&
                 !PropagationComponentEqual(*current, *before)))
                return Result<PrefabPropagationLoadReport>::Fail(
                    Error::InvalidEntity, plan.source.normalizedPath.string(),
                    "prefab propagation before value changed during load");
        }
    }

    PrefabPropagationLoadReport report;
    for (const auto& plan : plans)
    {
        report.diagnostics.insert(report.diagnostics.end(),
            plan.diagnostics.begin(), plan.diagnostics.end());
        for (const auto& instance : plan.instances)
        {
            if (instance.disposition == PrefabPropagationInstanceDisposition::Propagate)
                ++report.propagatedInstances;
            else if (instance.disposition == PrefabPropagationInstanceDisposition::NoOp)
                ++report.noOpInstances;
            else
            {
                ++report.quarantinedInstances;
            }
        }
        for (const auto& operation : plan.componentOperations)
        {
            if (operation.IsNoOp()) continue;
            const auto entity = document.FindByUuid(operation.EntityUuid());
            if (!ApplyComponentOperation(document.ecs.registry, entity, operation))
                return Result<PrefabPropagationLoadReport>::Fail(
                    Error::InvalidArgument, plan.source.normalizedPath.string(),
                    "unsupported prefab propagation component operation");
            report.changed = true;
        }
    }
    std::sort(report.diagnostics.begin(), report.diagnostics.end());
    if (report.changed) document.metadata.dirty = true;
    return Result<PrefabPropagationLoadReport>::Ok(std::move(report));
}

Result<PrefabPropagationLoadReport> RunPrefabPropagationLoadIntegration(
    SceneDocument& document, const AssetResolutionContext& assets,
    std::vector<AssetDiagnostic>& diagnostics, Error& err,
    const PrefabPropagationLoadHooks& hooks)
{
    err = Error{};
    const auto reconciled = ReconcilePrefabPropagationForLoad(
        document, assets, hooks);
    if (!reconciled.IsOk())
    {
        err = reconciled.error;
        return Result<PrefabPropagationLoadReport>::Fail(
            err.code, err.path, err.detail);
    }
    AppendPrefabPropagationDiagnostics(reconciled.value, diagnostics);
    const auto resolveAll = hooks.resolveAll
        ? hooks.resolveAll
        : [](SceneDocument& scene, const AssetResolutionContext& context,
             std::vector<AssetDiagnostic>& output, Error& error) {
              return SceneAssetResolver::ResolveAll(scene, context, output, error);
          };
    if (!resolveAll(document, assets, diagnostics, err))
        return Result<PrefabPropagationLoadReport>::Fail(
            err.code, err.path, err.detail);
    return reconciled;
}

Result<PrefabPropagationLoadReport> RunPrefabPropagationSceneOpen(
    SceneDocument& document, const PrefabPropagationSceneOpenContext& context,
    std::vector<AssetDiagnostic>& diagnostics, Error& err,
    const PrefabPropagationLoadHooks& hooks)
{
    return RunPrefabPropagationLoadIntegration(
        document, context.Assets(), diagnostics, err, hooks);
}

void AppendPrefabPropagationDiagnostics(
    const PrefabPropagationLoadReport& report,
    std::vector<AssetDiagnostic>& diagnostics)
{
    for (const auto& source : report.diagnostics)
    {
        AssetDiagnostic diagnostic;
        diagnostic.severity = source.severity;
        diagnostic.kind = AssetKind::Prefab;
        diagnostic.refPath = source.prefabPath.string();
        diagnostic.entityUuid = source.rootUuid;
        diagnostic.sourceKey = source.templateId.ToString();
        diagnostic.detail = source.reason;
        diagnostics.push_back(std::move(diagnostic));
    }
}

} // namespace rt2::core
