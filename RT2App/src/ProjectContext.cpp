#include "ProjectContext.h"
#include "core/PathTransaction.h"

namespace rt2::core {

bool LoadProjectContext(const std::filesystem::path& projectFile,
                        ProjectContext& out,
                        Error& err)
{
    ProjectContext staged;
    if (!ProjectStore::Load(projectFile, staged.project, err))
        return false;
    auto recovered = PrefabFileTransaction::RecoverDirectory(staged.project.assetRoot);
    if (!recovered.IsOk())
    {
        err = recovered.error;
        return false;
    }
    ProjectAssetScanResult scan;
    if (!ScanProjectAssets(staged.project.assetRoot, scan, err))
        return false;
    staged.database = std::move(scan.database);
    staged.scanDiagnostics = std::move(scan.diagnostics);
    out = std::move(staged);
    return true;
}

} // namespace rt2::core
