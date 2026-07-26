#pragma once

#ifndef SCENE_LOADER_H
#define SCENE_LOADER_H

#include "SceneTypes.h"
#include "ECSScene.h"
#include "TextureAssetPipeline.h"
#include <string>
#include <vector>

class SceneLoader
{
public:
    // Save the ECS scene to a glTF file (.glb or .gltf).
    static bool Save(const ECSScene& ecsScene, const std::string& filepath);

    // Load a glTF file into the ECSScene. Detects .gltf vs .glb from extension.
    // Populates ECSScene with object-space meshes + per-entity transforms.
    static bool LoadIntoECS(ECSScene& ecsScene, const std::string& filepath);
    static bool LoadIntoECS(
        ECSScene& ecsScene,
        const std::string& filepath,
        const rt2::core::TextureAssetLoadContext& textureContext,
        std::vector<rt2::core::AssetDiagnostic>& diagnostics);

    // Import a glTF into an EXISTING ECSScene without clearing it.
    // Merges textures, materials (with index offset), and entities (with a
    // wrapper root entity parented to existing scene). Returns the wrapper
    // root entity, or entt::null on failure.
    static entt::entity ImportIntoECS(ECSScene& ecsScene, const std::string& filepath);
    static entt::entity ImportIntoECS(
        ECSScene& ecsScene,
        const std::string& filepath,
        const rt2::core::TextureAssetLoadContext& textureContext,
        std::vector<rt2::core::AssetDiagnostic>& diagnostics);

    // Load an OBJ file (+ .mtl + textures) into the ECS scene.
    static bool LoadObjIntoECS(ECSScene& ecsScene, const std::string& filepath);

    // Import an OBJ file into an EXISTING ECSScene without clearing it.
    // Merges textures, materials (with index offset), and entities (under a
    // wrapper root entity parented to the existing scene). Returns the
    // wrapper root entity, or entt::null on failure.
    //
    // When settings.mergeMegaMesh is true, behavior matches LoadObjIntoECS:
    // all shapes are merged into one mega-mesh with per-triangle materials
    // and a single child entity carrying sourceKey "obj:whole-model".
    //
    // When settings.mergeMegaMesh is false, each tinyobj shape with > 0
    // triangles becomes its own child entity under the wrapper root, with
    // per-triangle materials scoped to that shape's faces and sourceKey
    // "obj:shape=<index>:name=<shape_name>". Degenerate (zero-triangle)
    // shapes produce no child entity.
    static entt::entity ImportObjIntoECS(ECSScene& ecsScene,
                                         const std::string& filepath,
                                         const ImportSettings& settings);
};

#endif // !SCENE_LOADER_H
