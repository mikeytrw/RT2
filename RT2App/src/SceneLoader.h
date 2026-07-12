#pragma once

#ifndef SCENE_LOADER_H
#define SCENE_LOADER_H

#include "SceneTypes.h"
#include "ECSScene.h"
#include <string>

class SceneLoader
{
public:
    // Save the ECS scene to a glTF file (.glb or .gltf).
    static bool Save(const ECSScene& ecsScene, const std::string& filepath);

    // Load a glTF file into the ECSScene. Detects .gltf vs .glb from extension.
    // Populates ECSScene with object-space meshes + per-entity transforms.
    static bool LoadIntoECS(ECSScene& ecsScene, const std::string& filepath);

    // Import a glTF into an EXISTING ECSScene without clearing it.
    // Merges textures, materials (with index offset), and entities (with a
    // wrapper root entity parented to existing scene). Returns the wrapper
    // root entity, or entt::null on failure.
    static entt::entity ImportIntoECS(ECSScene& ecsScene, const std::string& filepath);

    // Load an OBJ file (+ .mtl + textures) into the ECS scene.
    static bool LoadObjIntoECS(ECSScene& ecsScene, const std::string& filepath);
};

#endif // !SCENE_LOADER_H