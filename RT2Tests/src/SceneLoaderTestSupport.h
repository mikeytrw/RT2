#pragma once

#include "AssetIdentity.h"
#include "SceneLoader.h"

#include "ECSComponents.h"
#include "ECSScene.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
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

// ---------------------------------------------------------------------------
// Light entities (Phase 8 step 2)
//
// Lights stopped being a flat ECSScene::lights vector and became entities
// carrying LightComponent + Transform. These mirror that conversion so tests
// can author and read lights without restating it each time. SceneLight
// survives only as a plain description of one — it is no longer stored.
// ---------------------------------------------------------------------------

inline entt::entity AddLightEntityForTest(ECSScene& scene,
                                          const SceneLight& spec,
                                          const std::string& name)
{
    const entt::entity e = scene.registry.create();

    Transform tf;
    tf.translation = spec.position;
    tf.rotation = LightDirectionToRotation(spec.direction);
    tf.dirty = true;
    scene.registry.emplace<Transform>(e, tf);

    LightComponent lc;
    lc.type = spec.type;
    lc.color = spec.color;
    lc.intensity = spec.intensity;
    lc.range = spec.range;
    lc.innerConeAngle = spec.innerConeAngle;
    lc.outerConeAngle = spec.outerConeAngle;
    scene.registry.emplace<LightComponent>(e, lc);

    scene.registry.emplace<VisibleComponent>(e);
    scene.registry.emplace<NameComponent>(e).name = name;
    return e;
}

// Read light entities back as SceneLight descriptions, ordered by name so a
// test never depends on entt's view iteration order.
inline std::vector<SceneLight> CollectLightsForTest(const ECSScene& scene)
{
    std::vector<std::pair<std::string, SceneLight>> named;
    auto view = scene.registry.view<const LightComponent, const Transform>();
    for (auto e : view)
    {
        const auto& lc = view.template get<const LightComponent>(e);
        const auto& tf = view.template get<const Transform>(e);

        SceneLight out;
        out.type = lc.type;
        out.color = lc.color;
        out.intensity = lc.intensity;
        out.range = lc.range;
        out.innerConeAngle = lc.innerConeAngle;
        out.outerConeAngle = lc.outerConeAngle;
        out.position = tf.translation;
        out.direction = LightRotationToDirection(tf.rotation);

        std::string name;
        if (const auto* nc = scene.registry.try_get<NameComponent>(e))
            name = nc->name;
        named.emplace_back(name, out);
    }

    std::sort(named.begin(), named.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<SceneLight> lights;
    lights.reserve(named.size());
    for (auto& n : named)
        lights.push_back(n.second);
    return lights;
}
