#pragma once

#ifndef SCENE_TYPES_H
#define SCENE_TYPES_H

#include "AssetReference.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>

// ============================================================================
// Enums
// ============================================================================

enum class MaterialType
{
    Lambertian = 0,
    Metal = 1,
    Dielectric = 2,
    Emissive = 3
};

enum class LightType
{
    Point = 0,
    Spot = 1
};

// ============================================================================
// Scene data structures (POD structs — no Vulkan/CPU-renderer dependencies)
// ============================================================================

struct SceneMesh
{
    std::string filepath;
    glm::vec3 position = {0.0f, 0.0f, 0.0f};
    glm::vec3 rotation = {0.0f, 0.0f, 0.0f};
    float scale = 1.0f;
    int materialIndex = 0;

    bool hasGeometry = false;
    std::vector<float> vertices;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<uint32_t> indices;

    bool HasGeometry() const { return hasGeometry; }
    void ClearGeometry()
    {
        hasGeometry = false;
        vertices.clear();
        normals.clear();
        uvs.clear();
        indices.clear();
    }
};

struct SceneMaterial
{
    MaterialType type = MaterialType::Lambertian;

    glm::vec3 baseColor = {0.8f, 0.8f, 0.8f};
    float baseAlpha = 1.0f;
    float metallic = 0.0f;
    float roughness = 0.5f;
    float ior = 1.5f;
    float transmissionFactor = 0.0f;

    glm::vec3 emissiveColor = {0.0f, 0.0f, 0.0f};
    float emissiveIntensity = 0.0f;

    int baseColorTextureIndex = -1;
    int normalTextureIndex = -1;
    int emissiveTextureIndex = -1;
    int metallicRoughnessTextureIndex = -1;

    std::string alphaMode = "OPAQUE";
    float alphaCutoff = 0.5f;
};

struct SceneLight
{
    LightType type = LightType::Point;
    glm::vec3 position = {0.0f, 0.0f, 0.0f};
    glm::vec3 direction = {0.0f, -1.0f, 0.0f};
    glm::vec3 color = {1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 50.0f;
    float innerConeAngle = 30.0f;
    float outerConeAngle = 45.0f;
};

struct SceneTexture
{
    // Structured source identity; decoded pixels are cache data, not
    // serialized state.
    AssetReference ref;

    int width = 0;
    int height = 0;
    int channels = 4;
    std::vector<unsigned char> pixels;

    bool isHDR = false;
    std::vector<float> floatPixels;

    bool isSRGB = false;
};

struct SceneCamera
{
    glm::vec3 position = {0.0f, 1.0f, 10.0f};
    glm::vec3 forwardDirection = {0.0f, 0.0f, -1.0f};
    float verticalFOV = 45.0f;
    float aperture = 0.0f;
    float focusDistance = 1.0f;
};

#endif // SCENE_TYPES_H
