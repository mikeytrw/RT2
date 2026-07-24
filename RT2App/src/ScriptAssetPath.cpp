#include "ScriptAssetPath.h"

#include "ECSComponents.h"
#include "SceneDocument.h"

namespace rt2::core {

std::filesystem::path ResolveScriptAssetPath(
    const SceneDocument& document,
    const ::ScriptComponent& component)
{
    const std::filesystem::path assetPath(component.asset.path);
    if (assetPath.is_absolute() || document.metadata.sourcePath.empty())
        return assetPath;
    return document.metadata.sourcePath.parent_path() / assetPath;
}

} // namespace rt2::core
