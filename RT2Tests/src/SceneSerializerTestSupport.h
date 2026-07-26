#pragma once

#include "AssetResolver.h"
#include "SceneSerializer.h"

#include <filesystem>
#include <vector>

inline bool SaveSceneForTest(
    const rt2::core::SceneDocument& doc,
    const std::filesystem::path& path,
    rt2::core::Error& err)
{
    std::vector<rt2::core::AssetDiagnostic> diagnostics;
    return rt2::core::SceneSerializer::Save(doc, path, diagnostics, err);
}
