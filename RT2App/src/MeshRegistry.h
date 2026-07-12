#pragma once

#ifndef MESH_REGISTRY_H
#define MESH_REGISTRY_H

#include <vector>
#include <cstdint>
#include <string>

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
    std::vector<uint32_t> materialIndices; // per-triangle material index (may be empty)
    std::string           name;       // for debugging
};

class MeshRegistry
{
public:
    uint32_t AddMesh(MeshData&& mesh)
    {
        uint32_t idx = static_cast<uint32_t>(m_Meshes.size());
        m_Meshes.push_back(std::move(mesh));
        return idx;
    }

    uint32_t AddMesh(const MeshData& mesh)
    {
        uint32_t idx = static_cast<uint32_t>(m_Meshes.size());
        m_Meshes.push_back(mesh);
        return idx;
    }

    const MeshData& GetMesh(uint32_t index) const { return m_Meshes[index]; }
    MeshData& GetMesh(uint32_t index) { return m_Meshes[index]; }
    const std::vector<MeshData>& GetMeshes() const { return m_Meshes; }
    uint32_t GetCount() const { return static_cast<uint32_t>(m_Meshes.size()); }

    void Clear() { m_Meshes.clear(); }

private:
    std::vector<MeshData> m_Meshes;
};

#endif // MESH_REGISTRY_H