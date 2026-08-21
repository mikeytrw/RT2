#include "PrefabPropagationService.h"

#include "SceneSerializer.h"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <unordered_set>

namespace rt2::core {
namespace {

template<typename T>
void WriteOptional(entt::registry& registry, entt::entity entity,
                   const std::optional<PrefabPropagationComponentValue>& value)
{
    if (!value)
    {
        if (registry.all_of<T>(entity)) registry.remove<T>(entity);
        return;
    }
    if (const auto* typed = std::get_if<T>(&*value))
        registry.emplace_or_replace<T>(entity, *typed);
}

void WriteValue(SceneDocument& document, const PrefabPropagationComponentOperation& op)
{
    const auto entity = document.FindByUuid(op.entityUuid);
    if (entity == entt::null || !document.ecs.registry.valid(entity)) return;
    const auto wire = op.key.wire();
    if (wire == PrefabWireKeys::kName) WriteOptional<NameComponent>(document.ecs.registry, entity, op.after);
    else if (wire == PrefabWireKeys::kTransform) WriteOptional<Transform>(document.ecs.registry, entity, op.after);
    else if (wire == PrefabWireKeys::kVisible) WriteOptional<VisibleComponent>(document.ecs.registry, entity, op.after);
    else if (wire == PrefabWireKeys::kPrimitive) WriteOptional<PrimitiveComponent>(document.ecs.registry, entity, op.after);
    else if (wire == PrefabWireKeys::kImportedSource) WriteOptional<ImportedMeshSourceComponent>(document.ecs.registry, entity, op.after);
    else if (wire == PrefabWireKeys::kMaterialOverride) WriteOptional<MaterialOverrideComponent>(document.ecs.registry, entity, op.after);
    else if (wire == PrefabWireKeys::kLight) WriteOptional<LightComponent>(document.ecs.registry, entity, op.after);
    else if (wire == PrefabWireKeys::kCamera) WriteOptional<CameraComponent>(document.ecs.registry, entity, op.after);
    else if (wire == PrefabWireKeys::kMotion) WriteOptional<MotionComponent>(document.ecs.registry, entity, op.after);
    else if (wire == PrefabWireKeys::kScript) WriteOptional<ScriptComponent>(document.ecs.registry, entity, op.after);
}

bool AddOwnership(PrefabPropagationPlan& plan, PrefabPropagationResourceKind kind,
                  std::uint32_t beforeExtent, const std::vector<PrefabPropagationResourcePayload>& payloads)
{
    if (payloads.empty()) return true;
    if (payloads.size() > std::numeric_limits<std::uint32_t>::max() ||
        beforeExtent > std::numeric_limits<std::uint32_t>::max() -
            static_cast<std::uint32_t>(payloads.size()))
        return false;
    PrefabPropagationResourceOwnership ownership;
    auto& rebase = ownership.rebase;
    rebase.kind = kind;
    rebase.sourceBeforeExtent = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(payloads.size()));
    rebase.sceneBeforeExtent = beforeExtent;
    rebase.sceneAppendBase = beforeExtent;
    rebase.sceneAfterExtent = beforeExtent + static_cast<std::uint32_t>(payloads.size());
    rebase.owned = PrefabPropagationResourceBlock::FromDecoded(kind, payloads);
    for (std::size_t i = 0; i < payloads.size(); ++i)
    {
        rebase.sourceSlots.push_back({static_cast<std::uint32_t>(i)});
        rebase.sceneSlots.push_back({beforeExtent + static_cast<std::uint32_t>(i)});
    }
    plan.resourceOwnership.push_back(std::move(ownership));
    return true;
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

    SceneDocument staged;
    Error error;
    if (!SceneSerializer::CloneInMemory(live, staged, error))
        return Result<PrefabPropagationPlan>::Fail(error.code, error.path,
                                                   "could not clone staging document: " + error.detail);

    for (const auto& operation : durablePlan.componentOperations)
    {
        WriteValue(staged, operation);
    }

    for (const auto& operation : durablePlan.componentOperations)
    {
        if (operation.key.wire() != PrefabWireKeys::kPrimitive) continue;
        const auto entity = staged.FindByUuid(operation.entityUuid);
        if (entity == entt::null || !staged.ecs.registry.valid(entity))
            return Result<PrefabPropagationPlan>::Fail(Error::InvalidEntity,
                operation.entityUuid.ToString(), "primitive staging target disappeared");
        const auto* primitive = staged.ecs.registry.try_get<PrimitiveComponent>(entity);
        if (!primitive) continue;
        const auto index = RegisterPrimitiveMesh(staged.ecs.meshRegistry, *primitive);
        staged.ecs.registry.emplace_or_replace<MeshRef>(entity, MeshRef{index, -1});
    }

    std::vector<AssetDiagnostic> diagnostics;
    const bool resolved = SceneAssetResolver::ResolveAll(staged, assets, diagnostics, error);
    if (!resolved && diagnostics.empty())
        return Result<PrefabPropagationPlan>::Fail(error.code, error.path,
                                                   "staged resource resolution failed: " + error.detail);

    PrefabPropagationPlan result = durablePlan;
    std::unordered_set<UUID> failedEntities;
    for (const auto& diagnostic : diagnostics)
    {
        if (!diagnostic.entityUuid.IsNull() &&
            diagnostic.severity >= AssetDiagnostic::Missing)
            failedEntities.insert(diagnostic.entityUuid);
        PrefabPropagationDiagnostic converted;
        converted.severity = static_cast<AssetDiagnostic::Severity>(diagnostic.severity);
        converted.prefabPath = result.source.normalizedPath;
        converted.prefabAssetId = result.source.assetId;
        converted.rootUuid = diagnostic.entityUuid;
        converted.reason = diagnostic.detail.empty() ? diagnostic.refPath : diagnostic.detail;
        result.diagnostics.push_back(std::move(converted));
    }
    if (!failedEntities.empty())
    {
        result.componentOperations.erase(std::remove_if(result.componentOperations.begin(),
            result.componentOperations.end(), [&](const auto& operation) {
                return failedEntities.count(operation.entityUuid) != 0;
            }), result.componentOperations.end());
        result.meshRefOperations.erase(std::remove_if(result.meshRefOperations.begin(),
            result.meshRefOperations.end(), [&](const auto& operation) {
                return failedEntities.count(operation.entityUuid) != 0;
            }), result.meshRefOperations.end());
        for (auto& instance : result.instances)
        {
            if (std::any_of(instance.affectedEntities.begin(), instance.affectedEntities.end(),
                [&](const auto& uuid) { return failedEntities.count(uuid) != 0; }))
                instance.disposition = PrefabPropagationInstanceDisposition::Quarantined;
        }
        result.memberSnapshots.erase(std::remove_if(result.memberSnapshots.begin(),
            result.memberSnapshots.end(), [&](const auto& snapshot) {
                return failedEntities.count(snapshot.entityUuid) != 0;
            }), result.memberSnapshots.end());
    }
    result.resourceOwnership.clear();
    result.meshRefOperations.clear();
    const auto meshBefore = live.ecs.meshRegistry.GetCount();
    const auto materialBefore = static_cast<std::uint32_t>(live.ecs.materials.size());
    const auto textureBefore = static_cast<std::uint32_t>(live.ecs.textures.size());

    std::vector<PrefabPropagationResourcePayload> meshes;
    for (std::uint32_t i = meshBefore; i < staged.ecs.meshRegistry.GetCount(); ++i)
        meshes.push_back({"mesh:" + std::to_string(i), result.source.contentDigest,
                          staged.ecs.meshRegistry.GetMesh(i)});
    std::vector<PrefabPropagationResourcePayload> materials;
    for (std::uint32_t i = materialBefore; i < staged.ecs.materials.size(); ++i)
        materials.push_back({"material:" + std::to_string(i), result.source.contentDigest,
                             staged.ecs.materials[i]});
    std::vector<PrefabPropagationResourcePayload> textures;
    for (std::uint32_t i = textureBefore; i < staged.ecs.textures.size(); ++i)
        textures.push_back({"texture:" + std::to_string(i), result.source.contentDigest,
                           staged.ecs.textures[i]});
    if (!AddOwnership(result, PrefabPropagationResourceKind::Mesh, meshBefore, meshes) ||
        !AddOwnership(result, PrefabPropagationResourceKind::Material, materialBefore, materials) ||
        !AddOwnership(result, PrefabPropagationResourceKind::Texture, textureBefore, textures))
        return Result<PrefabPropagationPlan>::Fail(Error::InvalidArgument,
            "prefab-propagation", "resource extent overflow while staging");

    for (const auto& operation : durablePlan.componentOperations)
    {
        const auto entity = staged.FindByUuid(operation.entityUuid);
        if (entity == entt::null || !staged.ecs.registry.valid(entity)) continue;
        const auto before = live.ecs.registry.try_get<MeshRef>(live.FindByUuid(operation.entityUuid));
        const auto after = staged.ecs.registry.try_get<MeshRef>(entity);
        if ((before == nullptr) != (after == nullptr) ||
            (before && after && (before->meshIndex != after->meshIndex ||
                                 before->materialIndex != after->materialIndex)))
        {
            const auto existing = std::find_if(result.meshRefOperations.begin(),
                result.meshRefOperations.end(), [&](const auto& prior) {
                    return prior.entityUuid == operation.entityUuid;
                });
            if (existing == result.meshRefOperations.end())
                result.meshRefOperations.push_back({operation.entityUuid, operation.templateId,
                    before ? std::optional<MeshRef>(*before) : std::nullopt,
                    after ? std::optional<MeshRef>(*after) : std::nullopt});
            else
                existing->after = after ? std::optional<MeshRef>(*after) : std::nullopt;
        }
    }
    result.affectedEntities = result.DerivedAffectedEntities();
    result.syncImpact = result.DerivedSyncImpact();
    if (!result.IsValid())
        return Result<PrefabPropagationPlan>::Fail(Error::InvalidArgument,
            "prefab-propagation", "staged resource plan failed validation");
    return Result<PrefabPropagationPlan>::Ok(std::move(result));
}

} // namespace rt2::core
