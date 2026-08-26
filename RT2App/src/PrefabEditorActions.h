#pragma once

#ifndef RT2_PREFAB_EDITOR_ACTIONS_H
#define RT2_PREFAB_EDITOR_ACTIONS_H

#include "AssetResolver.h"
#include "SceneMutation.h"
#include "core/UUID.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class EditorCommandHistory;
class SceneManager;

namespace rt2::core {

// CPU-only authoring actions used by the prefab UI shell.  The UI never
// recreates the create/instantiate command choreography itself: these helpers
// validate the user-facing boundary, apply the existing SceneManager
// operation, record exactly one history entry, and compensate if recording
// fails after the operation was applied.
struct PrefabEditorActionResult
{
    EditorMutationResult mutation;
    std::filesystem::path prefabPath;
    UUID selectedRoot;
    std::vector<AssetDiagnostic> diagnostics;
    std::string message;

    bool IsOk() const noexcept { return mutation.success; }
};

PrefabEditorActionResult CreatePrefabAssetFromRoot(
    SceneManager& scene, EditorCommandHistory& history,
    const UUID& selectedRoot, const std::filesystem::path& prefabPath,
    const std::filesystem::path& assetRoot);

PrefabEditorActionResult InstantiatePrefabAsset(
    SceneManager& scene, EditorCommandHistory& history,
    const std::filesystem::path& prefabPath);

} // namespace rt2::core

#endif // RT2_PREFAB_EDITOR_ACTIONS_H
