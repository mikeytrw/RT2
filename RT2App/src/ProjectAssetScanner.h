#pragma once

#ifndef RT2_CORE_PROJECT_ASSET_SCANNER_H
#define RT2_CORE_PROJECT_ASSET_SCANNER_H

#include "AssetDatabase.h"
#include "AssetResolver.h"
#include "core/Error.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace rt2::core {

struct ProjectAssetScanResult
{
    std::shared_ptr<const AssetDatabase> database;
    std::vector<AssetDiagnostic> diagnostics;
};

// Read-only deterministic scan. Sidecars are the source of truth; this never
// mints, repairs, decodes, or writes an asset/cache file.
bool ScanProjectAssets(const std::filesystem::path& assetRoot,
                       ProjectAssetScanResult& out,
                       Error& err);

} // namespace rt2::core

#endif // RT2_CORE_PROJECT_ASSET_SCANNER_H
