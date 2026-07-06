#pragma once

#ifndef ECS_COMPONENTS_H
#define ECS_COMPONENTS_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <string>
#include <cstdint>

// ============================================================================
// ECS Components
//
// Components are plain data structs stored in the entt registry. Systems
// operate on entities that have the relevant components. An entity is just
// an entt::entity (uint32_t ID).
//
// Design principles:
// - Components are POD (plain old data) — no logic, no Vulkan/CPU-renderer deps
// - Transforms use TRS (translation/rotation/scale) for local space
// - World transforms are computed by the SceneGraph system
// - Mesh data (vertices, indices, UVs, tangents) lives in a separate
//   MeshRegistry keyed by mesh index, and MeshRef components point into it
//
// ============================================================================

// Transform — local TRS relative to parent entity (or world if no parent)
struct Transform
{
    glm::vec3 translation = {0.0f, 0.0f, 0.0f};
    glm::quat rotation = {1.0f, 0.0f, 0.0f, 0.0f};  // identity = no rotation
    glm::vec3 scale = {1.0f, 1.0f, 1.0f};

    // Computed world matrix (updated by SceneGraph system)
    glm::mat4 worldMatrix = glm::mat4(1.0f);
    glm::mat4 prevWorldMatrix = glm::mat4(1.0f);  // for motion vectors
    bool dirty = true;  // needs world matrix recomputation

    // Convenience: compute local TRS matrix
    glm::mat4 localMatrix() const
    {
        glm::mat4 t = glm::translate(glm::mat4(1.0f), translation);
        glm::mat4 r = glm::mat4(rotation);
        glm::mat4 s = glm::scale(glm::mat4(1.0f), scale);
        return t * r * s;
    }
};

// Parent-child relationship for scene graph hierarchy.
// The Hierarchy component is attached to entities that have children.
struct Hierarchy
{
    entt::entity parent = entt::null;
    std::vector<entt::entity> children;
};

// Reference to a unique mesh stored in the MeshRegistry.
// Multiple entities can reference the same meshIndex (instancing).
struct MeshRef
{
    uint32_t meshIndex = 0;      // index into MeshRegistry
    int materialIndex = 0;       // material index for this instance
};

// Light component for point/spot lights (CPU-side, not emissive triangles)
struct LightComponent
{
    glm::vec3 color = {1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 50.0f;
    float innerConeAngle = 30.0f;
    float outerConeAngle = 45.0f;
    bool isSpot = false;
};

// Camera component (only one entity should have this)
struct CameraComponent
{
    float verticalFOV = 45.0f;
    float aperture = 0.0f;
    float focusDistance = 1.0f;
    glm::vec3 forwardDirection = {0.0f, 0.0f, -1.0f};
};

// Name tag for debugging/UI
struct NameComponent
{
    std::string name;
};

// Marks an entity as visible for rendering
struct VisibleComponent
{
    bool visible = true;
};

#endif // ECS_COMPONENTS_H