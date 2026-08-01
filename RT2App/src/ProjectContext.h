#pragma once

#ifndef RT2_CORE_PROJECT_CONTEXT_H
#define RT2_CORE_PROJECT_CONTEXT_H

#include "AssetResolver.h"
#include "Project.h"
#include "ProjectAssetScanner.h"

#include <memory>
#include <stdexcept>

namespace rt2::core {

struct ProjectContext
{
    ProjectDocument project;
    std::shared_ptr<const AssetDatabase> database;
    std::vector<AssetDiagnostic> scanDiagnostics;

    AssetResolutionContext Assets() const
    {
        if (!database)
            throw std::logic_error(
                "project asset context requires a database snapshot");
        return AssetResolutionContext{
            project.assetRoot, database.get() };
    }
};

// Transactional value builder: `out` is unchanged unless parse, path
// validation, and the complete sidecar scan all succeed.
bool LoadProjectContext(const std::filesystem::path& projectFile,
                        ProjectContext& out,
                        Error& err);

} // namespace rt2::core

#endif // RT2_CORE_PROJECT_CONTEXT_H
