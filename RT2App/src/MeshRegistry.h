#pragma once

#ifndef MESH_REGISTRY_H
#define MESH_REGISTRY_H

#include <vector>
#include <cstdint>
#include <string>
#include <cmath>
#include <limits>

#include <glm/glm.hpp>

// ============================================================================
// MeshRegistry — stores unique mesh geometry in object space.
//
// Multiple entities (instances) can reference the same mesh by index.
// The AccelerationStructure builds one BLAS per registered mesh, and the
// TLAS creates instances that reference BLASes by device address with
// per-instance transforms.
//
// Mesh data is stored in object space (not world space). The per-instance
// transform (from the entity's Transform component) positions the mesh
// in the world via the TLAS instance matrix.
//
// ============================================================================

struct MeshData
{
    std::vector<float>    vertices;   // position.xyz, stride 3 (object space)
    std::vector<uint32_t> indices;    // triangle indices
    std::vector<float>    normals;    // normal.xyz, stride 3 (object space, may be empty)
    std::vector<float>    uvs;        // texcoord.xy, stride 2 (may be empty)
    std::vector<float>    tangents;   // glTF tangent.xyzw, stride 4 (object space, may be empty)
    std::vector<uint32_t> materialIndices; // per-triangle material index (may be empty)
    std::string           name;       // for debugging

    // Cached object-space bounds. MeshRegistry computes these once when the
    // mesh is registered so editor framing remains O(instances), not
    // O(vertices), for large imported scenes.
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    bool      boundsValid = false;
};

class MeshRegistry
{
public:
    uint32_t AddMesh(MeshData&& mesh)
    {
        ComputeBounds(mesh);
        uint32_t idx = static_cast<uint32_t>(m_Meshes.size());
        m_Meshes.push_back(std::move(mesh));
        return idx;
    }

    uint32_t AddMesh(const MeshData& mesh)
    {
        MeshData cached = mesh;
        ComputeBounds(cached);
        uint32_t idx = static_cast<uint32_t>(m_Meshes.size());
        m_Meshes.push_back(std::move(cached));
        return idx;
    }

    const MeshData& GetMesh(uint32_t index) const { return m_Meshes[index]; }
    MeshData& GetMesh(uint32_t index) { return m_Meshes[index]; }
    const std::vector<MeshData>& GetMeshes() const { return m_Meshes; }
    uint32_t GetCount() const { return static_cast<uint32_t>(m_Meshes.size()); }

    void Clear() { m_Meshes.clear(); }

    // Truncate the registry to `count` meshes. Used by the transactional
    // prefab-instantiate rollback to undo a partial resource merge. No-op
    // when count >= current size.
    void Truncate(uint32_t count)
    {
        if (count < m_Meshes.size())
            m_Meshes.resize(count);
    }

private:
    static void ComputeBounds(MeshData& mesh)
    {
        mesh.boundsValid = false;
        if (mesh.vertices.size() < 3)
            return;

        const float largest = std::numeric_limits<float>::max();
        glm::vec3 minimum(largest);
        glm::vec3 maximum(-largest);
        for (size_t i = 0; i + 2 < mesh.vertices.size(); i += 3)
        {
            const glm::vec3 position(mesh.vertices[i], mesh.vertices[i + 1],
                                     mesh.vertices[i + 2]);
            if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
                !std::isfinite(position.z))
                continue;
            minimum = glm::min(minimum, position);
            maximum = glm::max(maximum, position);
            mesh.boundsValid = true;
        }
        if (mesh.boundsValid)
        {
            mesh.boundsMin = minimum;
            mesh.boundsMax = maximum;
        }
    }

    std::vector<MeshData> m_Meshes;
};

#endif // MESH_REGISTRY_H
