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
    const AssetResolutionContext& assetContext,
    std::vector<AssetDiagnostic>& assetDiagnostics,
    std::vector<FieldDiagnostic>& outDiagnostics)
{
    ScriptFieldResolutionResult resolution;
    const size_t assetDiagnosticBase = assetDiagnostics.size();

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
        const auto* nameComponent =
            ecsRegistry.try_get<NameComponent>(entity);
        const std::string entityName =
            nameComponent ? nameComponent->name : std::string{};
        if (component.asset.kind != AssetKind::Script)
        {
            (void)ResolveScriptAssetPath(
                component, assetContext, uuid, entityName, assetDiagnostics);
            ++resolution.skippedEntities;
            FieldDiagnostic diagnostic;
            diagnostic.kind = FieldDiagnostic::Kind::InvalidAssetKind;
            diagnostic.entity = uuid;
            diagnostic.message = "entity " + uuid.ToString() +
                " has a ScriptComponent whose asset kind is not Script";
            outDiagnostics.push_back(std::move(diagnostic));
            continue;
        }

        const auto resolved = ResolveScriptAssetPath(
            component, assetContext, uuid, entityName, assetDiagnostics);
        if (!resolved.success)
        {
            ++resolution.skippedEntities;
            FieldDiagnostic diagnostic;
            diagnostic.kind = FieldDiagnostic::Kind::ParseFailed;
            diagnostic.entity = uuid;
            diagnostic.message = "entity " + uuid.ToString() +
                " script declarations were not reconciled: asset resolution failed";
            outDiagnostics.push_back(std::move(diagnostic));
            continue;
        }

        const auto declarations =
            registry.GetDeclaredFields(resolved.resolvedPath);
        if (!declarations.parsed)
        {
            AssetDiagnostic assetDiagnostic;
            assetDiagnostic.severity =
                declarations.diagnostic.find("failed to read script file") == 0
                    ? AssetDiagnostic::Missing
                    : AssetDiagnostic::Malformed;
            assetDiagnostic.kind = AssetKind::Script;
            assetDiagnostic.refPath = component.asset.path;
            assetDiagnostic.resolvedPath = resolved.resolvedPath.string();
            assetDiagnostic.entityUuid = uuid;
            assetDiagnostic.entityName = entityName;
            assetDiagnostic.sourceKey = component.asset.sourceKey;
            assetDiagnostic.detail = declarations.diagnostic;
            assetDiagnostics.push_back(std::move(assetDiagnostic));

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

    std::stable_sort(
        assetDiagnostics.begin() + assetDiagnosticBase,
        assetDiagnostics.end(),
        [](const AssetDiagnostic& left, const AssetDiagnostic& right) {
            return AssetDiagnosticSortKey(left) <
                   AssetDiagnosticSortKey(right);
        });

    return resolution;
}

} // namespace rt2::core
