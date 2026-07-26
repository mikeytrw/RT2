#pragma once

#ifndef RT2_CORE_SCRIPT_ASSET_PATH_H
#define RT2_CORE_SCRIPT_ASSET_PATH_H

#include "AssetResolver.h"

struct ScriptComponent;

namespace rt2::core {

// Script-specific adapter over the shared, read-only asset locator. In
// addition to the locator's existence/identity/sidecar checks, this validates
// the Script kind and the canonical lua:asset=<path> source key. A failed
// result always appends an AssetDiagnostic and has an empty resolvedPath.
AssetResolutionResult ResolveScriptAssetPath(
    const ::ScriptComponent& component,
    const AssetResolutionContext& context,
    const UUID& entityUuid,
    const std::string& entityName,
    std::vector<AssetDiagnostic>& diagnostics);

} // namespace rt2::core

#endif // RT2_CORE_SCRIPT_ASSET_PATH_H
