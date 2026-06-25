#pragma once

#ifndef SCENE_LOADER_H
#define SCENE_LOADER_H

#include "Scene.h"
#include <string>

class SceneLoader
{
public:
    // Returns false on failure. File extension determines format:
    // .gltf -> JSON glTF, .glb -> binary glTF
    static bool Save(const Scene& scene, const std::string& filepath);

    // Returns false on failure. Detects .gltf vs .glb from extension.
    static bool Load(Scene& scene, const std::string& filepath);
};

#endif // SCENE_LOADER_H