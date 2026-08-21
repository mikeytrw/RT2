#include "PrefabPropagationService.h"

#include "SceneSerializer.h"

#include <algorithm>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace rt2::core {
namespace {

template<typename T>
std::optional<T> ReadComponent(const entt::registry& registry, entt::entity entity)
{
    if (const auto* value = registry.try_get<T>(entity)) return *value;
    return std::nullopt;
}

template<typename T>
void WriteOptional(entt::registry& registry, entt::entity entity,
                   const std::optional<T>& value)
{
    if (!value)
    {
        if (registry.all_of<T>(entity)) registry.remove<T>(entity);
        return;
    }
    registry.emplace_or_replace<T>(entity, *value);
}

template<typename T>
std::optional<T> OperationAfter(const PrefabPropagationPlan& plan,
                                const UUID& entity, const PrefabComponentKey& key,
                                const entt::registry& live, entt::entity liveEntity)
{
    const auto it = std::find_if(plan.componentOperations.begin(),
        plan.componentOperations.end(), [&](const auto& operation) {
            return operation.entityUuid == entity && operation.key == key;
        });
    if (it != plan.componentOperations.end())
    {
        if (!it->after) return std::nullopt;
        if (const auto* value = std::get_if<T>(&*it->after)) return *value;
        return std::nullopt;
    }
    return ReadComponent<T>(live, liveEntity);
}

const PrefabPropagationComponentOperation* FindOperation(
    const PrefabPropagationPlan& plan, const UUID& entity,
    const PrefabComponentKey& key)
{
    const auto it = std::find_if(plan.componentOperations.begin(),
        plan.componentOperations.end(), [&](const auto& operation) {
            return operation.entityUuid == entity && operation.key == key;
        });
    return it == plan.componentOperations.end() ? nullptr : &*it;
}

PrefabPropagationComponentOperation* FindOperation(
    PrefabPropagationPlan& plan, const UUID& entity,
    const PrefabComponentKey& key)
{
    const auto it = std::find_if(plan.componentOperations.begin(),
        plan.componentOperations.end(), [&](const auto& operation) {
            return operation.entityUuid == entity && operation.key == key;
        });
    return it == plan.componentOperations.end() ? nullptr : &*it;
}

bool AddOwnership(PrefabPropagationPlan& plan, PrefabPropagationResourceKind kind,
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
    plan.resourceOwnership.push_back(std::move(ownership));
    return true;
}

void RebaseMaterial(SceneMaterial& material, std::uint32_t textureBase)
{
    auto rebase = [textureBase](int& index) {
        if (index >= 0)
            index += static_cast<int>(textureBase);
    };
    rebase(material.baseColorTextureIndex);
    rebase(material.normalTextureIndex);
    rebase(material.emissiveTextureIndex);
    rebase(material.metallicRoughnessTextureIndex);
}

void RebaseMesh(MeshData& mesh, std::uint32_t materialBase)
{
    for (auto& index : mesh.materialIndices)
        index += materialBase;
}

std::optional<PrefabPropagationRootSnapshot> FindRootSnapshot(
    const PrefabPropagationPlan& plan, const PrefabPropagationInstancePlan& instance)
{
    const auto it = std::find_if(plan.rootSnapshots.begin(), plan.rootSnapshots.end(),
        [&](const auto& snapshot) { return snapshot.rootUuid == instance.rootUuid; });
    if (it == plan.rootSnapshots.end()) return std::nullopt;
    return *it;
}

void AddResolverDiagnostic(PrefabPropagationPlan& result,
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
    result.diagnostics.push_back(std::move(converted));
}

} // namespace

Result<PrefabPropagationPlan> StagePrefabPropagationResources(
    const PrefabPropagationPlan& durablePlan,
    const SceneDocument& live,
    const AssetResolutionContext& assets)
{
    if (!durablePlan.IsValid())
        return Result<PrefabPropagationPlan>::Fail(
            Error::InvalidArgument, "prefab-propagation", "durable plan is invalid");

    // A canonical no-op is returned before creating a clone or asking the
    // resolver to inspect any asset. This is the isolation boundary that keeps
    // unrelated imported entities from turning a no-op into history.
    if (durablePlan.IsNoOp())
        return Result<PrefabPropagationPlan>::Ok(durablePlan);

    PrefabPropagationPlan result = durablePlan;
    result.resourceOwnership.clear();
    result.meshRefOperations.clear();

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

            bool hasImported = false;
            bool hasPrimitive = false;
            for (const auto& operation : durablePlan.componentOperations)
                if (operation.entityUuid == uuid)
                {
                    hasImported |= operation.key.wire() == PrefabWireKeys::kImportedSource &&
                                   operation.after.has_value();
                    hasPrimitive |= operation.key.wire() == PrefabWireKeys::kPrimitive &&
                                    operation.after.has_value();
                }
            if (!hasImported)
                hasImported = live.ecs.registry.all_of<ImportedMeshSourceComponent>(liveEntity) &&
                    FindOperation(durablePlan, uuid,
                        PrefabComponentKeyFor<ImportedMeshSourceComponent>::value) == nullptr;
            const auto imported = OperationAfter<ImportedMeshSourceComponent>(
                durablePlan, uuid, PrefabComponentKeyFor<ImportedMeshSourceComponent>::value,
                live.ecs.registry, liveEntity);
            hasImported = imported.has_value();
            const auto primitive = OperationAfter<PrimitiveComponent>(
                durablePlan, uuid, PrefabComponentKeyFor<PrimitiveComponent>::value,
                live.ecs.registry, liveEntity);
            hasPrimitive = hasPrimitive || primitive.has_value() &&
                FindOperation(durablePlan, uuid,
                    PrefabComponentKeyFor<PrimitiveComponent>::value) != nullptr;
            if (!hasImported && !hasPrimitive) continue;

            const auto fragmentEntity = fragment.ecs.registry.create();
            fragment.AssignKnownUuid(fragmentEntity, uuid);
            if (const auto* name = live.ecs.registry.try_get<NameComponent>(liveEntity))
                fragment.ecs.registry.emplace<NameComponent>(fragmentEntity, *name);
            if (const auto* member = live.ecs.registry.try_get<PrefabMemberComponent>(liveEntity))
                fragment.ecs.registry.emplace<PrefabMemberComponent>(fragmentEntity, *member);
            if (imported)
                fragment.ecs.registry.emplace<ImportedMeshSourceComponent>(fragmentEntity, *imported);
            if (const auto material = OperationAfter<MaterialOverrideComponent>(
                    durablePlan, uuid, PrefabComponentKeyFor<MaterialOverrideComponent>::value,
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
            AddResolverDiagnostic(result, instance, diagnostic,
                                  resourceEntities.empty() ? UUID::Nil() :
                                      entityToTemplate[resourceEntities.front()]);
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
                result.diagnostics.push_back(std::move(diagnostic));
            }
            continue;
        }

        const auto meshBase = static_cast<std::uint32_t>(meshPayloads.size());
        const auto materialBase = static_cast<std::uint32_t>(materialPayloads.size());
        const auto textureBase = static_cast<std::uint32_t>(texturePayloads.size());
        for (std::uint32_t i = 0; i < fragment.ecs.meshRegistry.GetCount(); ++i)
        {
            auto mesh = fragment.ecs.meshRegistry.GetMesh(i);
            RebaseMesh(mesh, durablePlan.materialTableExtent + materialBase);
            meshPayloads.push_back({"mesh:" + std::to_string(durablePlan.meshTableExtent + meshBase + i),
                                    result.source.contentDigest, std::move(mesh)});
        }
        for (std::size_t i = 0; i < fragment.ecs.materials.size(); ++i)
        {
            auto material = fragment.ecs.materials[i];
            RebaseMaterial(material, durablePlan.textureTableExtent + textureBase);
            materialPayloads.push_back({"material:" + std::to_string(
                durablePlan.materialTableExtent + materialBase + i),
                result.source.contentDigest, std::move(material)});
        }
        for (std::size_t i = 0; i < fragment.ecs.textures.size(); ++i)
        {
            const auto& texture = fragment.ecs.textures[i];
            texturePayloads.push_back({"texture:" + std::to_string(
                durablePlan.textureTableExtent + textureBase + i),
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
                afterRef->meshIndex += durablePlan.meshTableExtent + meshBase;
                if (afterRef->materialIndex >= 0)
                    afterRef->materialIndex += static_cast<int>(durablePlan.materialTableExtent + materialBase);
            }
            if ((before == nullptr) != (afterRef == std::nullopt) ||
                (before && afterRef && (before->meshIndex != afterRef->meshIndex ||
                    before->materialIndex != afterRef->materialIndex)))
                result.meshRefOperations.push_back({uuid, entityToTemplate[uuid],
                    before ? std::optional<MeshRef>(*before) : std::nullopt, afterRef});

            if (auto* material = fragment.ecs.registry.try_get<MaterialOverrideComponent>(stagedEntity))
            {
                auto repaired = *material;
                RebaseMaterial(repaired.material,
                                durablePlan.textureTableExtent + textureBase);
                if (auto* operation = FindOperation(result, uuid,
                        PrefabComponentKeyFor<MaterialOverrideComponent>::value))
                    operation->after = PrefabPropagationComponentValue{repaired};
                else
                {
                    const auto* liveMaterial = live.ecs.registry.try_get<MaterialOverrideComponent>(liveEntity);
                    if (!liveMaterial || !PrefabCanonicalComponentEqual(*liveMaterial, repaired))
                        result.componentOperations.push_back({uuid, entityToTemplate[uuid],
                            PrefabComponentKeyFor<MaterialOverrideComponent>::value,
                            liveMaterial ? std::optional<PrefabPropagationComponentValue>(
                                PrefabPropagationComponentValue{*liveMaterial}) : std::nullopt,
                            PrefabPropagationComponentValue{repaired}});
                }
            }
        }
    }

    // A hard resolver error quarantines the complete instance: no sibling
    // value, MeshRef, snapshot, or resource intent may survive.
    result.componentOperations.erase(std::remove_if(result.componentOperations.begin(),
        result.componentOperations.end(), [&](const auto& operation) {
            const auto it = entityToInstance.find(operation.entityUuid);
            return it != entityToInstance.end() && quarantinedInstances.count(it->second) != 0;
        }), result.componentOperations.end());
    result.memberSnapshots.erase(std::remove_if(result.memberSnapshots.begin(),
        result.memberSnapshots.end(), [&](const auto& snapshot) {
            return quarantinedInstances.count(snapshot.instanceId) != 0;
        }), result.memberSnapshots.end());
    result.meshRefOperations.erase(std::remove_if(result.meshRefOperations.begin(),
        result.meshRefOperations.end(), [&](const auto& operation) {
            const auto it = entityToInstance.find(operation.entityUuid);
            return it != entityToInstance.end() && quarantinedInstances.count(it->second) != 0;
        }), result.meshRefOperations.end());

    if (!AddOwnership(result, PrefabPropagationResourceKind::Mesh,
                      durablePlan.meshTableExtent,
                      static_cast<std::uint32_t>(meshPayloads.size()), meshPayloads) ||
        !AddOwnership(result, PrefabPropagationResourceKind::Material,
                      durablePlan.materialTableExtent,
                      static_cast<std::uint32_t>(materialPayloads.size()), materialPayloads) ||
        !AddOwnership(result, PrefabPropagationResourceKind::Texture,
                      durablePlan.textureTableExtent,
                      static_cast<std::uint32_t>(texturePayloads.size()), texturePayloads))
        return Result<PrefabPropagationPlan>::Fail(Error::InvalidArgument,
            "prefab-propagation", "resource extent overflow while staging");

    for (auto& instance : result.instances)
    {
        if (instance.disposition == PrefabPropagationInstanceDisposition::Quarantined)
            continue;
        instance.affectedEntities.clear();
        for (const auto& operation : result.componentOperations)
            if (entityToInstance[operation.entityUuid] == instance.instanceId &&
                !PrefabPropagationComponentOperationIsNoOp(operation))
                instance.affectedEntities.push_back(operation.entityUuid);
        for (const auto& operation : result.meshRefOperations)
            if (entityToInstance[operation.entityUuid] == instance.instanceId)
                instance.affectedEntities.push_back(operation.entityUuid);
        std::sort(instance.affectedEntities.begin(), instance.affectedEntities.end());
        instance.affectedEntities.erase(std::unique(instance.affectedEntities.begin(),
            instance.affectedEntities.end()), instance.affectedEntities.end());
        if (instance.affectedEntities.empty())
            instance.disposition = PrefabPropagationInstanceDisposition::NoOp;
    }
    std::sort(result.componentOperations.begin(), result.componentOperations.end(),
        [](const auto& a, const auto& b) {
            if (a.entityUuid != b.entityUuid) return a.entityUuid < b.entityUuid;
            if (a.templateId != b.templateId) return a.templateId < b.templateId;
            return a.key.wire() < b.key.wire();
        });
    result.affectedEntities = result.DerivedAffectedEntities();
    result.syncImpact = result.DerivedSyncImpact();
    std::sort(result.diagnostics.begin(), result.diagnostics.end());
    if (!result.IsValid())
        return Result<PrefabPropagationPlan>::Fail(Error::InvalidArgument,
            "prefab-propagation", "staged resource plan failed validation");
    return Result<PrefabPropagationPlan>::Ok(std::move(result));
}

} // namespace rt2::core
