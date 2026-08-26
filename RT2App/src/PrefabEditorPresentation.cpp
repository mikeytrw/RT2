#include "PrefabEditorPresentation.h"

#include "ECSComponents.h"

#include <algorithm>

namespace rt2::core {
namespace {

std::string OverrideLabel(std::string_view wire)
{
    if (wire == "name") return "Name";
    if (wire == "transform") return "Transform";
    if (wire == "visible") return "Visibility";
    if (wire == "primitive") return "Primitive";
    if (wire == "materialOverride") return "Material";
    if (wire == "light") return "Light";
    if (wire == "camera") return "Camera";
    if (wire == "motion") return "Motion";
    if (wire == "script") return "Script";
    return std::string(wire);
}

} // namespace

PrefabEntityPresentation DescribePrefabEntity(
    const SceneDocument& document, const UUID& entityUuid)
{
    PrefabEntityPresentation out;
    const entt::entity entity = document.FindByUuid(entityUuid);
    if (entity == entt::null || !document.ecs.registry.valid(entity))
        return out;

    const auto* instance =
        document.ecs.registry.try_get<PrefabInstanceComponent>(entity);
    const auto* member =
        document.ecs.registry.try_get<PrefabMemberComponent>(entity);
    if (!instance && !member) return out;

    if (member)
    {
        out.instanceId = member->instanceId;
        out.templateId = member->templateId;
        out.overrideLabels.reserve(member->overrides.size());
        for (const auto& key : member->overrides)
            out.overrideLabels.push_back(OverrideLabel(key.wire()));
        std::sort(out.overrideLabels.begin(), out.overrideLabels.end());
        out.overrideLabels.erase(
            std::unique(out.overrideLabels.begin(), out.overrideLabels.end()),
            out.overrideLabels.end());
    }

    auto attachRoot = [&](entt::entity root,
                          const PrefabInstanceComponent& rootInstance) {
        const auto* id = document.ecs.registry.try_get<EntityIdComponent>(root);
        if (id) out.rootUuid = id->id;
        out.hasSource = true;
        out.source = rootInstance.prefab;
    };

    if (instance)
    {
        out.instanceId = instance->instanceId;
        out.kind = PrefabLinkPresentationKind::Root;
        out.hierarchyTag = "[Prefab]";
        attachRoot(entity, *instance);
        if (!member || member->instanceId != instance->instanceId ||
            instance->instanceId.IsNull())
        {
            out.kind = PrefabLinkPresentationKind::Broken;
            out.hierarchyTag = "[Prefab?]";
            out.warning = "Prefab root link is incomplete or inconsistent.";
        }
        return out;
    }

    std::vector<std::pair<entt::entity, const PrefabInstanceComponent*>> roots;
    auto rootView = document.ecs.registry.view<PrefabInstanceComponent>();
    for (const auto candidate : rootView)
    {
        const auto& rootInstance = rootView.get<PrefabInstanceComponent>(candidate);
        if (rootInstance.instanceId == member->instanceId)
            roots.emplace_back(candidate, &rootInstance);
    }
    if (roots.size() == 1 && !member->instanceId.IsNull() &&
        !member->templateId.IsNull())
    {
        out.kind = PrefabLinkPresentationKind::Member;
        out.hierarchyTag = "[Linked]";
        attachRoot(roots.front().first, *roots.front().second);
    }
    else
    {
        out.kind = PrefabLinkPresentationKind::Broken;
        out.hierarchyTag = "[Prefab?]";
        out.warning = roots.empty()
            ? "Prefab member has no matching instance root."
            : "Prefab member matches multiple instance roots.";
    }
    return out;
}

std::string PrefabOverrideTooltip(std::string_view componentLabel)
{
    return "The complete " + std::string(componentLabel) +
        " component is protected from source updates.";
}

PrefabPropagationPresentation DescribePrefabPropagation(
    const PrefabPropagationLiveReport& report)
{
    PrefabPropagationPresentation out;
    out.visible = report.accepted || report.queued || report.applied ||
        report.noOp || !report.error.IsOk() || !report.diagnostics.empty();
    if (!out.visible) return out;
    out.warning = !report.error.IsOk() || report.quarantinedInstances != 0 ||
        !report.diagnostics.empty();
    out.summary = PrefabPropagationLiveHost::FormatStatus(report);
    out.diagnostics = report.diagnostics;
    for (const auto& diagnostic : report.diagnostics)
        if (!diagnostic.instanceId.IsNull())
            out.warningInstanceIds.push_back(diagnostic.instanceId);
    std::sort(out.warningInstanceIds.begin(), out.warningInstanceIds.end());
    out.warningInstanceIds.erase(
        std::unique(out.warningInstanceIds.begin(), out.warningInstanceIds.end()),
        out.warningInstanceIds.end());
    return out;
}

} // namespace rt2::core
