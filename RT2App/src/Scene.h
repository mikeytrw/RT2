#pragma once

#ifndef SCENE_H
#define SCENE_H

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
    // External file reference (for OBJ-based workflow; empty for inline glTF geometry)
    std::string filepath;
    glm::vec3 position = {0.0f, 0.0f, 0.0f};
    glm::vec3 rotation = {0.0f, 0.0f, 0.0f};
    float scale = 1.0f;
    int materialIndex = 0;

    // Inline geometry (populated when loading real glTF files with mesh primitives)
    // When hasGeometry is true, filepath is ignored and geometry data is used directly
    bool hasGeometry = false;
    std::vector<float> vertices;       // position.xyz, tightly packed (stride 3)
    std::vector<float> normals;        // normal.xyz, tightly packed (stride 3), empty if none
    std::vector<float> uvs;            // texcoord.xy, tightly packed (stride 2), empty if none
    std::vector<uint32_t> indices;     // triangle indices, empty if non-indexed

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
    float metallic = 0.0f;
    float roughness = 0.5f;
    float ior = 1.5f;

    glm::vec3 emissiveColor = {0.0f, 0.0f, 0.0f};
    float emissiveIntensity = 0.0f;

    int baseColorTextureIndex = -1;
    int normalTextureIndex = -1;
    int emissiveTextureIndex = -1;
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
    std::string filepath;

    // Decoded RGBA8 pixel data (populated by SceneLoader via stb_image).
    // Empty if the image failed to decode or no image data was available.
    int width = 0;
    int height = 0;
    int channels = 4; // always RGBA8 after decode
    std::vector<unsigned char> pixels;
};

struct SceneCamera
{
    glm::vec3 position = {0.0f, 1.0f, 10.0f};
    glm::vec3 forwardDirection = {0.0f, 0.0f, -1.0f};
    float verticalFOV = 45.0f;
    float aperture = 0.0f;
    float focusDistance = 1.0f;
};

// ============================================================================
// Scene class — owns all scene data
// ============================================================================

class Scene
{
public:
    void Clear()
    {
        m_Meshes.clear();
        m_Materials.clear();
        m_Lights.clear();
        m_Textures.clear();
        m_Camera = SceneCamera{};
    }

    // --- Meshes ---
    int AddMesh(const SceneMesh& mesh)
    {
        int idx = static_cast<int>(m_Meshes.size());
        m_Meshes.push_back(mesh);
        return idx;
    }

    SceneMesh& GetMesh(int index) { return m_Meshes[index]; }
    const SceneMesh& GetMesh(int index) const { return m_Meshes[index]; }
    const std::vector<SceneMesh>& GetMeshes() const { return m_Meshes; }

    // --- Materials ---
    int AddMaterial(const SceneMaterial& material)
    {
        int idx = static_cast<int>(m_Materials.size());
        m_Materials.push_back(material);
        return idx;
    }

    SceneMaterial& GetMaterial(int index) { return m_Materials[index]; }
    const SceneMaterial& GetMaterial(int index) const { return m_Materials[index]; }
    const std::vector<SceneMaterial>& GetMaterials() const { return m_Materials; }

    // --- Lights ---
    int AddLight(const SceneLight& light)
    {
        int idx = static_cast<int>(m_Lights.size());
        m_Lights.push_back(light);
        return idx;
    }

    SceneLight& GetLight(int index) { return m_Lights[index]; }
    const SceneLight& GetLight(int index) const { return m_Lights[index]; }
    const std::vector<SceneLight>& GetLights() const { return m_Lights; }

    // --- Textures ---
    int AddTexture(const SceneTexture& texture)
    {
        int idx = static_cast<int>(m_Textures.size());
        m_Textures.push_back(texture);
        return idx;
    }

    SceneTexture& GetTexture(int index) { return m_Textures[index]; }
    const SceneTexture& GetTexture(int index) const { return m_Textures[index]; }
    const std::vector<SceneTexture>& GetTextures() const { return m_Textures; }

    // --- Camera ---
    SceneCamera& GetCamera() { return m_Camera; }
    const SceneCamera& GetCamera() const { return m_Camera; }

private:
    std::vector<SceneMesh> m_Meshes;
    std::vector<SceneMaterial> m_Materials;
    std::vector<SceneLight> m_Lights;
    std::vector<SceneTexture> m_Textures;
    SceneCamera m_Camera;
};

#endif // SCENE_H