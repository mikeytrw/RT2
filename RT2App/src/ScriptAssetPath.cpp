#include "ScriptAssetPath.h"

#include "ECSComponents.h"

namespace rt2::core {

namespace {

std::string CandidatePath(const AssetReference& ref,
                          const AssetResolutionContext& context)
{
    if (ref.path.empty())
        return {};
    const std::filesystem::path path(ref.path);
    return (path.is_absolute() ? path : context.assetRoot / path)
        .lexically_normal().string();
}

void AppendAdapterDiagnostic(AssetDiagnostic::Severity severity,
                             const AssetReference& ref,
                             const AssetResolutionContext& context,
                             const UUID& entityUuid,
                             const std::string& entityName,
                             const std::string& resolvedPath,
                             const std::string& detail,
                             std::vector<AssetDiagnostic>& diagnostics)
{
    AssetDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.kind = AssetKind::Script;
    diagnostic.refPath = ref.path;
    diagnostic.resolvedPath = resolvedPath.empty()
        ? CandidatePath(ref, context)
        : resolvedPath;
    diagnostic.entityUuid = entityUuid;
    diagnostic.entityName = entityName;
    diagnostic.sourceKey = ref.sourceKey;
    diagnostic.detail = detail;
    diagnostics.push_back(std::move(diagnostic));
}

} // anonymous namespace

AssetResolutionResult ResolveScriptAssetPath(
    const ::ScriptComponent& component,
    const AssetResolutionContext& context,
    const UUID& entityUuid,
    const std::string& entityName,
    std::vector<AssetDiagnostic>& diagnostics)
{
    if (component.asset.kind != AssetKind::Script)
    {
        AppendAdapterDiagnostic(
            AssetDiagnostic::Malformed, component.asset, context,
            entityUuid, entityName, {},
            "ScriptComponent asset kind is not Script", diagnostics);
        return {};
    }

    AssetResolutionResult result = Resolve(
        component.asset, context, entityUuid, entityName, diagnostics);
    if (!result.success)
        return result;

    const std::string expectedSourceKey = component.asset.path.empty()
        ? std::string{}
        : "lua:asset=" + component.asset.path;
    if (component.asset.sourceKey != expectedSourceKey)
    {
        AppendAdapterDiagnostic(
            AssetDiagnostic::Unresolved, component.asset, context,
            entityUuid, entityName, result.resolvedPath.string(),
            "script source key does not match its asset path; expected '" +
                expectedSourceKey + "'",
            diagnostics);
        return {};
    }

    return result;
}

} // namespace rt2::core
