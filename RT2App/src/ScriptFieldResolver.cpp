#include "ScriptFieldResolver.h"

#include "ECSComponents.h"
#include "SceneDocument.h"
#include "ScriptAssetPath.h"
#include "ScriptFieldRegistry.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace rt2::core {

ScriptFieldResolutionResult ScriptFieldResolver::ResolveDocument(
    SceneDocument& document,
    ScriptFieldRegistry& registry,
    std::vector<FieldDiagnostic>& outDiagnostics)
{
    ScriptFieldResolutionResult resolution;

    auto& ecsRegistry = document.ecs.registry;
    std::vector<std::pair<UUID, entt::entity>> entities;
    std::vector<entt::entity> unidentified;
    auto view = ecsRegistry.view<ScriptComponent>();
    entities.reserve(view.size());
    for (const auto entity : view)
    {
        if (const auto* id = ecsRegistry.try_get<EntityIdComponent>(entity))
            entities.emplace_back(id->id, entity);
        else
            unidentified.push_back(entity);
    }
    std::sort(entities.begin(), entities.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::sort(unidentified.begin(), unidentified.end(),
              [](entt::entity a, entt::entity b) {
                  return entt::to_integral(a) < entt::to_integral(b);
              });

    for (const auto& [uuid, entity] : entities)
    {
        auto& component = ecsRegistry.get<ScriptComponent>(entity);
        if (component.asset.kind != AssetKind::Script)
        {
            ++resolution.skippedEntities;
            FieldDiagnostic diagnostic;
            diagnostic.kind = FieldDiagnostic::Kind::InvalidAssetKind;
            diagnostic.entity = uuid;
            diagnostic.message = "entity " + uuid.ToString() +
                " has a ScriptComponent whose asset kind is not Script";
            outDiagnostics.push_back(std::move(diagnostic));
            continue;
        }

        const auto path = ResolveScriptAssetPath(document, component);
        const auto declarations = registry.GetDeclaredFields(path);
        if (!declarations.parsed)
        {
            ++resolution.skippedEntities;
            FieldDiagnostic diagnostic;
            diagnostic.kind = FieldDiagnostic::Kind::ParseFailed;
            diagnostic.entity = uuid;
            diagnostic.message = "entity " + uuid.ToString() +
                " script declarations were not reconciled: " +
                declarations.diagnostic;
            outDiagnostics.push_back(std::move(diagnostic));
            continue;
        }

        ScriptFieldMap reconciled = ReconcileScriptFields(
            component.fieldValues, declarations.descriptors, uuid,
            outDiagnostics);
        if (reconciled != component.fieldValues)
        {
            component.fieldValues = std::move(reconciled);
            resolution.changed = true;
        }
        ++resolution.resolvedEntities;
    }

    for (const auto entity : unidentified)
    {
        ++resolution.skippedEntities;
        FieldDiagnostic diagnostic;
        diagnostic.kind = FieldDiagnostic::Kind::MissingEntityId;
        diagnostic.message = "ScriptComponent on registry entity " +
            std::to_string(entt::to_integral(entity)) +
            " was not reconciled because it has no EntityIdComponent";
        outDiagnostics.push_back(std::move(diagnostic));
    }

    return resolution;
}

} // namespace rt2::core
