#pragma once

#ifndef ECS_SCENE_H
#define ECS_SCENE_H

#include <entt/entt.hpp>
#include "ECSComponents.h"
#include "MeshRegistry.h"
#include "Scene.h"  // for SceneMaterial, SceneTexture, SceneCamera ( transitional)

// ============================================================================
// ECSScene — ECS-based scene representation
//
// Replaces the flat SceneMesh array with an entity-component model:
//   - Unique mesh geometry stored in MeshRegistry (object space)
//   - Entities with Transform + MeshRef components (instancing support)
//   - Parent-child hierarchy via Hierarchy components
//   - Materials and textures in flat arrays (same as Scene)
//
// The SceneGraph system resolves the hierarchy to world matrices.
// BuildGPUSceneDataFromECS() extracts unique meshes + instance list for
// the GPU renderer.
//
// ============================================================================

struct ECSScene
{
    entt::registry registry;
    MeshRegistry meshRegistry;

    // Flat arrays (transitional — same as Scene)
    std::vector<SceneMaterial> materials;
    std::vector<SceneTexture>  textures;
    SceneCamera                camera;

    void Clear()
    {
        registry.clear();
        meshRegistry.Clear();
        materials.clear();
        textures.clear();
        camera = SceneCamera{};
    }
};

#endif // ECS_SCENE_H