#pragma once

#ifndef SCENE_LOADER_H
#define SCENE_LOADER_H

#include "Scene.h"
#include "ECSScene.h"
#include <string>

class SceneLoader
{
public:
    // Returns false on failure. File extension determines format:
    // .gltf -> JSON glTF, .glb -> binary glTF
    static bool Save(const Scene& scene, const std::string& filepath);

    // Returns false on failure. Detects .gltf vs .glb from extension.
    // Legacy path: populates flat SceneMesh array with world-space vertices.
    static bool Load(Scene& scene, const std::string& filepath);

    // ECS path: populates ECSScene with object-space meshes + per-entity
    // transforms. Meshes are stored in the MeshRegistry (unique geometry),
    // entities get Transform + MeshRef + Hierarchy components.
    static bool LoadIntoECS(ECSScene& ecsScene, const std::string& filepath);
};

#endif // !SCENE_LOADER_H