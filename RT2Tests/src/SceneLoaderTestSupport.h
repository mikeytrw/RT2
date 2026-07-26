#pragma once

#include "AssetIdentity.h"
#include "SceneLoader.h"

#include <filesystem>
#include <vector>

inline std::filesystem::path RepositoryRootForSceneLoaderTests()
{
    return std::filesystem::current_path();
}

inline rt2::core::TextureAssetLoadContext MakeSceneLoaderTestContext(
    const std::filesystem::path& absoluteRoot,
    const std::filesystem::path& modelPath)
{
    rt2::core::TextureAssetLoadContext context;
    context.resolution.assetRoot = absoluteRoot.lexically_normal();
    context.resolvedOwnerPath = modelPath.is_absolute()
        ? modelPath.lexically_normal()
        : (absoluteRoot / modelPath).lexically_normal();
    context.ownerModel.kind = AssetKind::Model;
    context.ownerModel.path = modelPath.generic_string();
    context.identityMode = rt2::core::TextureIdentityMode::ReadOnly;

    rt2::core::Error sidecarError;
    context.effectiveOwnerId = rt2::core::ReadSidecarId(
        rt2::core::AssetSidecarPath(context.resolvedOwnerPath),
        sidecarError);
    context.ownerModel.assetId = context.effectiveOwnerId;
    return context;
}

inline bool LoadGltfForTest(
    ECSScene& scene,
    const std::filesystem::path& absoluteRoot,
    const std::filesystem::path& modelPath,
    std::vector<rt2::core::AssetDiagnostic>& diagnostics)
{
    const auto context =
        MakeSceneLoaderTestContext(absoluteRoot, modelPath);
    return SceneLoader::LoadIntoECS(scene, context, diagnostics);
}

inline bool LoadGltfForTest(
    ECSScene& scene,
    const std::filesystem::path& absoluteRoot,
    const std::filesystem::path& modelPath)
{
    std::vector<rt2::core::AssetDiagnostic> diagnostics;
    return LoadGltfForTest(scene, absoluteRoot, modelPath, diagnostics);
}

inline bool LoadObjForTest(
    ECSScene& scene,
    const std::filesystem::path& absoluteRoot,
    const std::filesystem::path& modelPath,
    std::vector<rt2::core::AssetDiagnostic>& diagnostics)
{
    const auto context =
        MakeSceneLoaderTestContext(absoluteRoot, modelPath);
    return SceneLoader::LoadObjIntoECS(scene, context, diagnostics);
}

inline bool LoadObjForTest(
    ECSScene& scene,
    const std::filesystem::path& absoluteRoot,
    const std::filesystem::path& modelPath)
{
    std::vector<rt2::core::AssetDiagnostic> diagnostics;
    return LoadObjForTest(scene, absoluteRoot, modelPath, diagnostics);
}
