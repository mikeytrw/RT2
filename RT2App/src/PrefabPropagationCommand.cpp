#include "PrefabPropagationCommand.h"

#include "SceneGraph.h"
#include "SceneSerializer.h"

#include <algorithm>
#include <limits>
#include <type_traits>

namespace
{
using namespace rt2::core;

std::uint32_t ResourceExtent(const ECSScene& ecs, PrefabPropagationResourceKind kind)
{
    switch (kind)
    {
    case PrefabPropagationResourceKind::Mesh: return ecs.meshRegistry.GetCount();
    case PrefabPropagationResourceKind::Material: return static_cast<std::uint32_t>(ecs.materials.size());
    case PrefabPropagationResourceKind::Texture: return static_cast<std::uint32_t>(ecs.textures.size());
    }
    return 0;
}

bool MeshEqual(const MeshData& a, const MeshData& b)
{ return PrefabPropagationMeshEqual(a, b); }

bool ResourceAtEquals(const ECSScene& ecs, const PrefabPropagationResourceRebase& rebase,
                      std::size_t i)
{
    const auto slot = rebase.sceneSlots[i].value;
    const auto& decoded = rebase.owned.Entries()[i].decoded;
    switch (rebase.kind)
    {
    case PrefabPropagationResourceKind::Mesh:
        if (slot >= ecs.meshRegistry.GetCount()) return false;
        {
            MeshRegistry normalized;
            normalized.AddMesh(std::get<MeshData>(decoded));
            return MeshEqual(ecs.meshRegistry.GetMesh(slot), normalized.GetMesh(0));
        }
    case PrefabPropagationResourceKind::Material:
        return slot < ecs.materials.size() &&
            PrefabCanonicalMaterialEqual(ecs.materials[slot], std::get<SceneMaterial>(decoded));
    case PrefabPropagationResourceKind::Texture:
        return slot < ecs.textures.size() &&
            PrefabPropagationTextureEqual(ecs.textures[slot], std::get<SceneTexture>(decoded));
    }
    return false;
}

EditorMutationResult Failure(Error::Code code, const std::string& path,
                             const std::string& detail)
{ return EditorMutationResult::Failure(code, path, detail); }

bool ValidateRootSnapshots(const PrefabPropagationPlan& plan,
                           const SceneDocument& doc, const ECSScene& ecs)
{
    for (const auto& snapshot : plan.rootSnapshots)
    {
        const auto entity = doc.FindByUuid(snapshot.rootUuid);
        if (entity == entt::null || !ecs.registry.valid(entity)) return false;
        const auto* root = ecs.registry.try_get<PrefabInstanceComponent>(entity);
        if (!root || root->instanceId != snapshot.instanceId ||
            !PrefabPropagationAssetReferenceEqual(root->prefab, snapshot.prefab))
            return false;
    }
    return true;
}

bool HasResourceMutation(const PrefabPropagationPlan& plan) noexcept
{ return !plan.resourceOwnership.empty() || !plan.meshRefOperations.empty(); }

bool ValidateCommitEvidence(const PrefabPropagationPlan& plan)
{
    if (!plan.source.IsValid() || plan.documentGeneration == 0 ||
        plan.resourceGeneration == 0 || !plan.authoringRevisionCaptured ||
        plan.rootSnapshots.empty())
        return false;
    for (const auto& operation : plan.componentOperations)
    {
        const auto it = std::find_if(plan.memberSnapshots.begin(), plan.memberSnapshots.end(),
            [&](const auto& snapshot) { return snapshot.entityUuid == operation.EntityUuid(); });
        if (it == plan.memberSnapshots.end() || it->templateId != operation.TemplateId())
            return false;
    }
    for (const auto& operation : plan.meshRefOperations)
    {
        const auto it = std::find_if(plan.memberSnapshots.begin(), plan.memberSnapshots.end(),
            [&](const auto& snapshot) { return snapshot.entityUuid == operation.entityUuid; });
        if (it == plan.memberSnapshots.end() || it->templateId != operation.templateId)
            return false;
    }
    for (const auto& instance : plan.instances)
    {
        if (instance.disposition == PrefabPropagationInstanceDisposition::Quarantined)
            continue;
        const auto root = std::find_if(plan.rootSnapshots.begin(), plan.rootSnapshots.end(),
            [&](const auto& snapshot) { return snapshot.rootUuid == instance.rootUuid; });
        if (root == plan.rootSnapshots.end() || root->instanceId != instance.instanceId)
            return false;
    }
    return true;
}
}

PrefabPropagationCommand::PrefabPropagationCommand(
    PrefabPropagationPlan plan, SourceFingerprintReader sourceReader)
    : m_Plan(std::move(plan)), m_SourceReader(std::move(sourceReader))
{
    m_ExpectedRevision = m_Plan.authoringRevision;
    m_ExpectedResourceGeneration = m_Plan.resourceGeneration;
}

EditorMutationResult PrefabPropagationCommand::Execute(SceneManager& scene)
{
    auto& doc = scene.m_Authoring;
    auto& ecs = scene.m_EcsScene;
    if (!m_Plan.IsValid())
        return Failure(Error::InvalidArgument, "prefab-propagation", "prepared plan is invalid");
    if (!ValidateCommitEvidence(m_Plan))
        return Failure(Error::InvalidArgument, "prefab-propagation", "prepared plan lacks complete stale-state evidence");
    if (m_Plan.documentGeneration != scene.m_DocumentGeneration)
        return Failure(Error::InvalidArgument, "prefab-propagation", "stale document generation");
    if (m_ExpectedResourceGeneration != scene.m_ResourceGeneration)
        return Failure(Error::InvalidArgument, "prefab-propagation", "stale resource generation");
    if (m_ExpectedRevision != scene.m_AuthoringRevision)
        return Failure(Error::InvalidArgument, "prefab-propagation", "stale authoring revision");
    if (!m_SourceReader)
        return Failure(Error::InvalidArgument, "prefab-propagation", "source fingerprint reader is required");
    if (!m_HasExecuted || !m_IsApplied)
    {
        if (!m_HasExecuted)
        {
            const auto current = m_SourceReader();
            if (!current.IsOk())
                return Failure(current.error.code, current.error.path,
                               current.error.detail);
            if (current.value != m_Plan.source)
                return Failure(Error::InvalidArgument, m_Plan.source.normalizedPath.string(),
                               "stale prefab source fingerprint");
        }
    }
    if (!ValidateRootSnapshots(m_Plan, doc, ecs))
        return Failure(Error::InvalidArgument, "prefab-propagation", "stale prefab root link");

    for (const auto& snapshot : m_Plan.memberSnapshots)
    {
        const auto entity = doc.FindByUuid(snapshot.entityUuid);
        if (entity == entt::null || !ecs.registry.valid(entity))
            return Failure(Error::InvalidEntity, snapshot.entityUuid.ToString(), "stale prefab member");
        const auto* member = ecs.registry.try_get<PrefabMemberComponent>(entity);
        if (!member || member->instanceId != snapshot.instanceId ||
            member->templateId != snapshot.templateId || member->overrides != snapshot.overrides)
            return Failure(Error::InvalidArgument, snapshot.entityUuid.ToString(),
                           "stale prefab link or override vector");
    }

    for (const auto& operation : m_Plan.componentOperations)
    {
        const auto entity = doc.FindByUuid(operation.EntityUuid());
        if (entity == entt::null || !ecs.registry.valid(entity))
            return Failure(Error::InvalidEntity, operation.EntityUuid().ToString(), "stale component target");
        const auto expected = operation.BeforeValue();
        const auto current = ReadPropagationComponent(operation, ecs.registry, entity);
        if (current.has_value() != expected.has_value() ||
            (current && expected && !PropagationComponentEqual(*current, *expected)))
            return Failure(Error::InvalidArgument, operation.EntityUuid().ToString(),
                           "stale component before value");
    }
    for (const auto& operation : m_Plan.meshRefOperations)
    {
        const auto entity = doc.FindByUuid(operation.entityUuid);
        if (entity == entt::null || !ecs.registry.valid(entity))
            return Failure(Error::InvalidEntity, operation.entityUuid.ToString(), "stale MeshRef target");
        const auto current = ReadPropagationComponent<MeshRef>(ecs.registry, entity);
        if (current.has_value() != operation.before.has_value() ||
            (current && (current->meshIndex != operation.before->meshIndex ||
                         current->materialIndex != operation.before->materialIndex)))
            return Failure(Error::InvalidArgument, operation.entityUuid.ToString(), "stale MeshRef before value");
    }

    for (const auto& ownership : m_Plan.resourceOwnership)
    {
        const auto& rebase = ownership.rebase;
        const auto expectedExtent = m_HasExecuted
            ? rebase.sceneAfterExtent : rebase.sceneBeforeExtent;
        if (ResourceExtent(ecs, rebase.kind) != expectedExtent)
            return Failure(Error::InvalidArgument, "prefab-propagation", "stale resource table extent");
        if (m_HasExecuted)
            for (std::size_t i = 0; i < rebase.sceneSlots.size(); ++i)
                if (!ResourceAtEquals(ecs, rebase, i))
                    return Failure(Error::InvalidArgument, "prefab-propagation", "owned resource changed before redo");
    }

    // Every possible failure has been checked above.  Appends and component
    // writes below are therefore one mutation phase, with no resolver call or
    // source-file read capable of failing halfway through it.
    if (!m_Plan.IsEffective())
        return EditorMutationResult{true, false, {}, std::nullopt,
                                    SyncImpact::None, {}};
    if (!m_HasExecuted)
        for (const auto& ownership : m_Plan.resourceOwnership)
        {
            const auto& rebase = ownership.rebase;
            for (const auto& payload : rebase.owned.Entries())
            {
                std::visit([&](const auto& decoded) {
                    using T = std::decay_t<decltype(decoded)>;
                    if constexpr (std::is_same_v<T, MeshData>) ecs.meshRegistry.AddMesh(decoded);
                    else if constexpr (std::is_same_v<T, SceneMaterial>) ecs.materials.push_back(decoded);
                    else ecs.textures.push_back(decoded);
                }, payload.decoded);
            }
        }
    for (const auto& operation : m_Plan.componentOperations)
    {
        const auto entity = doc.FindByUuid(operation.EntityUuid());
        WritePropagationComponent(operation, ecs.registry, entity, true);
        if (operation.Key().wire() == PrefabWireKeys::kTransform)
            SceneGraph::MarkDirty(ecs.registry, entity);
    }
    for (const auto& operation : m_Plan.meshRefOperations)
    {
        const auto entity = doc.FindByUuid(operation.entityUuid);
        if (operation.after) ecs.registry.emplace_or_replace<MeshRef>(entity, *operation.after);
        else if (ecs.registry.all_of<MeshRef>(entity)) ecs.registry.remove<MeshRef>(entity);
    }
    if (HasResourceMutation(m_Plan)) ++scene.m_ResourceGeneration;
    scene.NotifyAuthoringChanged();
    m_ExpectedRevision = scene.m_AuthoringRevision;
    m_ExpectedResourceGeneration = scene.m_ResourceGeneration;
    m_HasExecuted = true;
    m_IsApplied = true;
    return EditorMutationResult{true, m_Plan.IsEffective(), {}, std::nullopt,
                                m_Plan.syncImpact, m_Plan.affectedEntities};
}

EditorMutationResult PrefabPropagationCommand::Undo(SceneManager& scene)
{
    auto& doc = scene.m_Authoring;
    auto& ecs = scene.m_EcsScene;
    if (!m_Plan.IsValid())
        return Failure(Error::InvalidArgument, "prefab-propagation", "prepared plan is invalid");
    if (!ValidateCommitEvidence(m_Plan))
        return Failure(Error::InvalidArgument, "prefab-propagation", "prepared plan lacks complete stale-state evidence");
    if (m_Plan.documentGeneration != scene.m_DocumentGeneration)
        return Failure(Error::InvalidArgument, "prefab-propagation", "stale document generation");
    if (!m_HasExecuted || !m_IsApplied)
        return Failure(Error::InvalidArgument, "prefab-propagation", "command is not applied");
    if (m_ExpectedResourceGeneration != scene.m_ResourceGeneration)
        return Failure(Error::InvalidArgument, "prefab-propagation", "stale resource generation for undo");
    if (m_ExpectedRevision != scene.m_AuthoringRevision)
        return Failure(Error::InvalidArgument, "prefab-propagation", "stale authoring revision for undo");
    if (!ValidateRootSnapshots(m_Plan, doc, ecs))
        return Failure(Error::InvalidArgument, "prefab-propagation", "stale prefab root link for undo");
    for (const auto& snapshot : m_Plan.memberSnapshots)
    {
        const auto entity = doc.FindByUuid(snapshot.entityUuid);
        const auto* member = entity == entt::null ? nullptr
            : ecs.registry.try_get<PrefabMemberComponent>(entity);
        if (entity == entt::null || !ecs.registry.valid(entity) || !member ||
            member->instanceId != snapshot.instanceId || member->templateId != snapshot.templateId ||
            member->overrides != snapshot.overrides)
            return Failure(Error::InvalidArgument, snapshot.entityUuid.ToString(),
                           "stale prefab link or override vector for undo");
    }
    for (const auto& operation : m_Plan.componentOperations)
    {
        const auto entity = doc.FindByUuid(operation.EntityUuid());
        if (entity == entt::null || !ecs.registry.valid(entity) ||
            [&] {
                const auto current = ReadPropagationComponent(operation, ecs.registry, entity);
                const auto after = operation.AfterValue();
                return current.has_value() == after.has_value() &&
                    (!current || (after && PropagationComponentEqual(*current, *after)));
            }() == false)
            return Failure(Error::InvalidArgument, operation.EntityUuid().ToString(), "stale after value for undo");
    }
    for (const auto& operation : m_Plan.meshRefOperations)
    {
        const auto entity = doc.FindByUuid(operation.entityUuid);
        const auto current = entity == entt::null ? std::optional<MeshRef>{}
                                                   : ReadPropagationComponent<MeshRef>(ecs.registry, entity);
        if (entity == entt::null || !ecs.registry.valid(entity) ||
            current.has_value() != operation.after.has_value() ||
            (current && (current->meshIndex != operation.after->meshIndex ||
                         current->materialIndex != operation.after->materialIndex)))
            return Failure(Error::InvalidArgument, operation.entityUuid.ToString(), "stale MeshRef after value for undo");
    }
    for (const auto& ownership : m_Plan.resourceOwnership)
    {
        const auto& rebase = ownership.rebase;
        if (ResourceExtent(ecs, rebase.kind) != rebase.sceneAfterExtent)
            return Failure(Error::InvalidArgument, "prefab-propagation", "stale resource extent for undo");
        for (std::size_t i = 0; i < rebase.sceneSlots.size(); ++i)
            if (!ResourceAtEquals(ecs, rebase, i))
                return Failure(Error::InvalidArgument, "prefab-propagation", "owned resource changed before undo");
    }

    for (const auto& operation : m_Plan.componentOperations)
    {
        const auto entity = doc.FindByUuid(operation.EntityUuid());
        WritePropagationComponent(operation, ecs.registry, entity, false);
        if (operation.Key().wire() == PrefabWireKeys::kTransform)
            SceneGraph::MarkDirty(ecs.registry, entity);
    }
    for (const auto& operation : m_Plan.meshRefOperations)
    {
        const auto entity = doc.FindByUuid(operation.entityUuid);
        if (operation.before) ecs.registry.emplace_or_replace<MeshRef>(entity, *operation.before);
        else if (ecs.registry.all_of<MeshRef>(entity)) ecs.registry.remove<MeshRef>(entity);
    }
    // Owned resources remain resident while the command is retained by
    // history. Undo only restores durable references; it never compacts or
    // truncates an append-only table.
    if (HasResourceMutation(m_Plan)) ++scene.m_ResourceGeneration;
    scene.NotifyAuthoringChanged();
    m_ExpectedRevision = scene.m_AuthoringRevision;
    m_ExpectedResourceGeneration = scene.m_ResourceGeneration;
    m_IsApplied = false;
    return EditorMutationResult{true, m_Plan.IsEffective(), {}, std::nullopt,
                                m_Plan.syncImpact, m_Plan.affectedEntities};
}

rt2::core::Result<std::unique_ptr<PrefabPrimitiveRecipeCommand>>
PrefabPrimitiveRecipeCommand::Prepare(SceneManager& scene,
                                      const rt2::core::UUID& entity,
                                      const PrimitiveComponent& after)
{
    using namespace rt2::core;
    auto* live = scene.m_Authoring.FindByUuid(entity) == entt::null
        ? nullptr : &scene.m_EcsScene.registry;
    if (!live) return Result<std::unique_ptr<PrefabPrimitiveRecipeCommand>>::Fail(
        Error::InvalidEntity, entity.ToString(), "primitive target is absent");
    const auto e = scene.m_Authoring.FindByUuid(entity);
    if (!live->all_of<PrimitiveComponent>(e) || !live->all_of<MeshRef>(e))
        return Result<std::unique_ptr<PrefabPrimitiveRecipeCommand>>::Fail(
            Error::InvalidArgument, entity.ToString(), "primitive target has no recipe or mesh");
    auto command = std::unique_ptr<PrefabPrimitiveRecipeCommand>(new PrefabPrimitiveRecipeCommand());
    command->m_Entity = entity;
    command->m_Before = live->get<PrimitiveComponent>(e);
    command->m_After = after;
    command->m_BeforeRef = live->get<MeshRef>(e);
    command->m_ResourceGeneration = scene.m_ResourceGeneration;
    command->m_DocumentGeneration = scene.m_DocumentGeneration;
    command->m_AuthoringRevision = scene.m_AuthoringRevision;
    command->m_BeforeSchema = scene.m_Authoring.metadata.schemaVersion;
    command->m_AfterSchema = command->m_BeforeSchema;
    if (auto* member = live->try_get<PrefabMemberComponent>(e))
    {
        command->m_BeforeMarker = std::any_of(member->overrides.begin(), member->overrides.end(),
            [](const auto& key) { return key.wire() == PrefabWireKeys::kPrimitive; });
        command->m_AfterMarker = true;
        if (!command->m_BeforeMarker) command->m_AfterSchema = SceneSerializer::SchemaVersion;
    }
    command->m_ExpectedRevision = command->m_AuthoringRevision;
    command->m_ExpectedResourceGeneration = command->m_ResourceGeneration;
    MeshRegistry staged;
    RegisterPrimitiveMesh(staged, after);
    command->m_OwnedMesh = staged.GetMesh(0);
    const auto index = scene.m_EcsScene.meshRegistry.GetCount();
    command->m_AfterRef = command->m_BeforeRef;
    command->m_AfterRef.meshIndex = index;
    command->m_OwnedMeshSlot = index;
    return Result<std::unique_ptr<PrefabPrimitiveRecipeCommand>>::Ok(std::move(command));
}

EditorMutationResult PrefabPrimitiveRecipeCommand::Execute(SceneManager& scene)
{
    auto e = scene.m_Authoring.FindByUuid(m_Entity);
    if (e == entt::null || !scene.m_EcsScene.registry.valid(e) ||
        scene.m_DocumentGeneration != m_DocumentGeneration ||
        scene.m_ResourceGeneration != m_ExpectedResourceGeneration ||
        m_ExpectedRevision != scene.m_AuthoringRevision)
        return Failure(rt2::core::Error::InvalidArgument, m_Entity.ToString(), "stale primitive command");
    auto& reg = scene.m_EcsScene.registry;
    if (!reg.all_of<PrimitiveComponent, MeshRef>(e) ||
        !PrefabCanonicalComponentEqual(reg.get<PrimitiveComponent>(e), m_Before) ||
        reg.get<MeshRef>(e).meshIndex != m_BeforeRef.meshIndex ||
        reg.get<MeshRef>(e).materialIndex != m_BeforeRef.materialIndex)
        return Failure(rt2::core::Error::InvalidArgument, m_Entity.ToString(), "primitive before state changed");
    if (auto* member = reg.try_get<PrefabMemberComponent>(e))
    {
        const bool liveMarker = std::any_of(member->overrides.begin(), member->overrides.end(),
            [](const auto& key) { return key.wire() == PrefabWireKeys::kPrimitive; });
        if (liveMarker != m_BeforeMarker || scene.m_Authoring.metadata.schemaVersion != m_BeforeSchema)
            return Failure(rt2::core::Error::InvalidArgument, m_Entity.ToString(), "primitive marker/schema changed");
    }
    if (!m_HasExecuted)
    {
        if (m_AfterRef.meshIndex != scene.m_EcsScene.meshRegistry.GetCount())
            return Failure(rt2::core::Error::InvalidArgument, m_Entity.ToString(), "primitive mesh append slot changed");
        scene.m_EcsScene.meshRegistry.AddMesh(m_OwnedMesh);
    }
    else
    {
        if (m_OwnedMeshSlot >= scene.m_EcsScene.meshRegistry.GetCount())
            return Failure(rt2::core::Error::InvalidArgument, m_Entity.ToString(), "primitive owned mesh slot disappeared");
        MeshRegistry normalized;
        normalized.AddMesh(m_OwnedMesh);
        if (!PrefabPropagationMeshEqual(scene.m_EcsScene.meshRegistry.GetMesh(m_OwnedMeshSlot),
                                         normalized.GetMesh(0)))
            return Failure(rt2::core::Error::InvalidArgument, m_Entity.ToString(), "primitive owned mesh changed");
    }
    reg.replace<PrimitiveComponent>(e, m_After);
    reg.replace<MeshRef>(e, m_AfterRef);
    if (auto* member = reg.try_get<PrefabMemberComponent>(e))
    {
        auto& keys = member->overrides;
        if (m_AfterMarker && !m_BeforeMarker)
            keys.push_back(PrefabComponentKeyFor<PrimitiveComponent>::value);
        std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.wire() < b.wire(); });
        if (m_AfterSchema != m_BeforeSchema) scene.m_Authoring.metadata.schemaVersion = m_AfterSchema;
    }
    ++scene.m_ResourceGeneration;
    scene.NotifyAuthoringChanged();
    m_ExpectedRevision = scene.m_AuthoringRevision;
    m_ExpectedResourceGeneration = scene.m_ResourceGeneration;
    m_HasExecuted = true;
    m_IsApplied = true;
    return EditorMutationResult{true, true, {}, std::nullopt, SyncImpact::Structural, {m_Entity}};
}

EditorMutationResult PrefabPrimitiveRecipeCommand::Undo(SceneManager& scene)
{
    auto e = scene.m_Authoring.FindByUuid(m_Entity);
    if (e == entt::null || !scene.m_EcsScene.registry.valid(e) ||
        scene.m_DocumentGeneration != m_DocumentGeneration ||
        scene.m_ResourceGeneration != m_ExpectedResourceGeneration ||
        m_ExpectedRevision != scene.m_AuthoringRevision)
        return Failure(rt2::core::Error::InvalidArgument, m_Entity.ToString(), "stale primitive undo");
    if (!m_HasExecuted || !m_IsApplied)
        return Failure(rt2::core::Error::InvalidArgument, m_Entity.ToString(), "primitive command is not applied");
    auto& reg = scene.m_EcsScene.registry;
    if (!reg.all_of<PrimitiveComponent, MeshRef>(e) ||
        !PrefabCanonicalComponentEqual(reg.get<PrimitiveComponent>(e), m_After) ||
        reg.get<MeshRef>(e).meshIndex != m_AfterRef.meshIndex ||
        reg.get<MeshRef>(e).materialIndex != m_AfterRef.materialIndex)
        return Failure(rt2::core::Error::InvalidArgument, m_Entity.ToString(), "primitive after state changed");
    if (auto* member = reg.try_get<PrefabMemberComponent>(e))
    {
        const bool liveMarker = std::any_of(member->overrides.begin(), member->overrides.end(),
            [](const auto& key) { return key.wire() == PrefabWireKeys::kPrimitive; });
        if (liveMarker != m_AfterMarker || scene.m_Authoring.metadata.schemaVersion != m_AfterSchema)
            return Failure(rt2::core::Error::InvalidArgument, m_Entity.ToString(), "primitive marker/schema changed for undo");
    }
    if (m_AfterRef.meshIndex >= scene.m_EcsScene.meshRegistry.GetCount())
        return Failure(rt2::core::Error::InvalidArgument, m_Entity.ToString(), "primitive owned mesh slot disappeared");
    reg.replace<PrimitiveComponent>(e, m_Before);
    reg.replace<MeshRef>(e, m_BeforeRef);
    if (auto* member = reg.try_get<PrefabMemberComponent>(e))
    {
        member->overrides.erase(std::remove_if(member->overrides.begin(), member->overrides.end(),
            [](const auto& key) { return key.wire() == PrefabWireKeys::kPrimitive; }), member->overrides.end());
        if (m_BeforeMarker) member->overrides.push_back(PrefabComponentKeyFor<PrimitiveComponent>::value);
        std::sort(member->overrides.begin(), member->overrides.end(), [](const auto& a, const auto& b) { return a.wire() < b.wire(); });
        scene.m_Authoring.metadata.schemaVersion = m_BeforeSchema;
    }
    ++scene.m_ResourceGeneration;
    scene.NotifyAuthoringChanged();
    m_ExpectedRevision = scene.m_AuthoringRevision;
    m_ExpectedResourceGeneration = scene.m_ResourceGeneration;
    m_IsApplied = false;
    return EditorMutationResult{true, true, {}, std::nullopt, SyncImpact::Structural, {m_Entity}};
}
