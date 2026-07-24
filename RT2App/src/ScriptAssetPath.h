#pragma once

#ifndef RT2_CORE_SCRIPT_ASSET_PATH_H
#define RT2_CORE_SCRIPT_ASSET_PATH_H

#include <filesystem>

struct ScriptComponent;

namespace rt2::core {

class SceneDocument;

// Resolve a script reference using the same scene-relative rule in the
// editor-time field resolver and the Play-time ScriptSystem.
std::filesystem::path ResolveScriptAssetPath(
    const SceneDocument& document,
    const ::ScriptComponent& component);

} // namespace rt2::core

#endif // RT2_CORE_SCRIPT_ASSET_PATH_H
