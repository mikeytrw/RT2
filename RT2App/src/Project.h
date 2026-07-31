#pragma once

#ifndef RT2_CORE_PROJECT_H
#define RT2_CORE_PROJECT_H

#include "InputConfig.h"
#include "core/Error.h"
#include "core/UUID.h"

#include <filesystem>
#include <string>
#include <vector>

namespace rt2::core {

struct ProjectDocument
{
    UUID projectId;

    // Portable locators written to .rt2proj. assetRoot/cacheRoot are relative
    // to projectDirectory; startupScene is relative to assetRoot.
    std::string assetRootLocator = "Assets";
    std::string cacheRootLocator = ".rt2/cache";
    std::string startupSceneLocator;
    std::vector<InputContextRecord> inputContexts;

    // Derived runtime state. Never serialized.
    std::filesystem::path projectFile;
    std::filesystem::path projectDirectory;
    std::filesystem::path assetRoot;
    std::filesystem::path cacheRoot;
    std::filesystem::path startupScene;
};

class ProjectStore
{
public:
    static constexpr uint32_t SchemaVersion = 1;

    static bool Load(const std::filesystem::path& path,
                     ProjectDocument& out,
                     Error& err);

    static bool Save(const ProjectDocument& project,
                     const std::filesystem::path& path,
                     Error& err);

    // Recompute and validate all derived paths without doing file I/O.
    static bool ValidateAndResolve(ProjectDocument& project,
                                   const std::filesystem::path& projectFile,
                                   Error& err);
};

} // namespace rt2::core

#endif // RT2_CORE_PROJECT_H
