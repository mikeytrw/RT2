#pragma once

#ifndef GLTF_BUILDER_H
#define GLTF_BUILDER_H

// Test helper: programmatically builds tinygltf::Model files with known geometry
// for testing SceneLoader's glTF parsing. Writes .gltf or .glb files.
// NOTE: Do NOT define TINYGLTF_IMPLEMENTATION here — SceneLoader.cpp already does.
// Match the same defines as SceneLoader.cpp to avoid symbol mismatches.

#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include "tiny_gltf.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vector>
#include <cstring>
#include <filesystem>

class GltfBuilder
{
public:
    enum class IndexType { UInt8, UInt16, UInt32 };

    GltfBuilder()
    {
        m_Model.asset.version = "2.0";
        m_Model.asset.generator = "GltfBuilder Test";
    }

    // --- Mesh construction ---

    void AddTriangle(
        const std::vector<glm::vec3>& positions,
        const std::vector<uint32_t>& indices,
        IndexType idxType = IndexType::UInt32)
    {
        int meshIdx = AddMeshWithPositions(positions, indices, idxType);
        // Only add a node for the first primitive in a mesh
        if (m_Model.meshes[meshIdx].primitives.size() == 1)
            AddMeshNode(meshIdx);
    }

    void AddTriangleWithNormals(
        const std::vector<glm::vec3>& positions,
        const std::vector<glm::vec3>& normals,
        const std::vector<uint32_t>& indices)
    {
        int meshIdx = AddMeshWithPositions(positions, indices, IndexType::UInt32, &normals);
        if (m_Model.meshes[meshIdx].primitives.size() == 1)
            AddMeshNode(meshIdx);
    }

    void AddTriangleWithUVs(
        const std::vector<glm::vec3>& positions,
        const std::vector<glm::vec2>& uvs,
        const std::vector<uint32_t>& indices)
    {
        int meshIdx = AddMeshWithPositions(positions, indices, IndexType::UInt32, nullptr, &uvs);
        if (m_Model.meshes[meshIdx].primitives.size() == 1)
            AddMeshNode(meshIdx);
    }

    void AddTriangleWithMaterial(
        const std::vector<glm::vec3>& positions,
        const std::vector<uint32_t>& indices,
        int materialIndex)
    {
        int meshIdx = AddMeshWithPositions(positions, indices, IndexType::UInt32);
        m_Model.meshes[meshIdx].primitives.back().material = materialIndex;
        if (m_Model.meshes[meshIdx].primitives.size() == 1)
            AddMeshNode(meshIdx);
    }

    void AddTriangleNonIndexed(const std::vector<glm::vec3>& positions)
    {
        // Non-indexed: just positions, no index accessor
        int primIdx = AddPrimitivePositions(positions, -1);
        tinygltf::Primitive& prim = m_Model.meshes[0].primitives[primIdx];
        prim.indices = -1;  // non-indexed
        prim.mode = 4;      // TRIANGLES

        if (m_Model.meshes[0].primitives.size() == 1)
            AddMeshNode(0);
    }

    void AddPoints(const std::vector<glm::vec3>& positions)
    {
        // POINTS mode (0) — should be skipped by loader
        std::vector<uint32_t> indices;
        int meshIdx = AddMeshWithPositions(positions, indices, IndexType::UInt32);
        m_Model.meshes[meshIdx].primitives.back().mode = 0;  // POINTS
        if (m_Model.meshes[meshIdx].primitives.size() == 1)
            AddMeshNode(meshIdx);
    }

    // --- Node transform setters (applied to the most recently added mesh node) ---

    void SetNodeTranslation(const glm::vec3& t)
    {
        if (!m_Model.nodes.empty())
        {
            m_Model.nodes.back().translation = {t.x, t.y, t.z};
        }
    }

    void SetNodeScale(const glm::vec3& s)
    {
        if (!m_Model.nodes.empty())
        {
            m_Model.nodes.back().scale = {s.x, s.y, s.z};
        }
    }

    void SetNodeRotation(const glm::vec4& quatXYZW)
    {
        if (!m_Model.nodes.empty())
        {
            m_Model.nodes.back().rotation = {quatXYZW.x, quatXYZW.y, quatXYZW.z, quatXYZW.w};
        }
    }

    void SetNodeMatrix(const glm::mat4& m)
    {
        if (!m_Model.nodes.empty())
        {
            const float* d = glm::value_ptr(m);
            m_Model.nodes.back().matrix.assign(d, d + 16);
        }
    }

    // Set a parent node translation (creates a parent node wrapping the last child)
    void SetParentNodeTranslation(const glm::vec3& t)
    {
        if (m_Model.nodes.empty()) return;

        // Take the last node and make it a child of a new parent
        int childIdx = static_cast<int>(m_Model.nodes.size()) - 1;

        tinygltf::Node parent;
        parent.translation = {t.x, t.y, t.z};
        parent.children = {childIdx};
        m_Model.nodes.push_back(parent);
        int parentIdx = static_cast<int>(m_Model.nodes.size()) - 1;

        // Update scene to point to parent instead of child
        if (!m_Model.scenes.empty())
        {
            auto& nodes = m_Model.scenes[0].nodes;
            for (auto& n : nodes)
            {
                if (n == childIdx)
                    n = parentIdx;
            }
        }
    }

    // --- Material ---

    void AddMaterial(const std::vector<double>& baseColorFactor, double metallic, double roughness)
    {
        tinygltf::Material mat;
        mat.pbrMetallicRoughness.baseColorFactor = baseColorFactor;
        mat.pbrMetallicRoughness.metallicFactor = metallic;
        mat.pbrMetallicRoughness.roughnessFactor = roughness;
        m_Model.materials.push_back(mat);
    }

    // --- Write ---

    bool Write(const std::string& filepath)
    {
        // Ensure we have a scene
        if (m_Model.scenes.empty())
        {
            tinygltf::Scene scene;
            for (int i = 0; i < (int)m_Model.nodes.size(); i++)
                scene.nodes.push_back(i);
            m_Model.scenes.push_back(scene);
            m_Model.defaultScene = 0;
        }

        tinygltf::TinyGLTF loader;
        std::string err, warn;

        std::string ext = std::filesystem::path(filepath).extension().string();
        bool isGLB = (ext == ".glb" || ext == ".GLB");

        return loader.WriteGltfSceneToFile(&m_Model, filepath,
            false,  // embedImages
            isGLB,  // embedBuffers
            true,   // prettyPrint
            isGLB   // writeBinary
        );
    }

private:
    tinygltf::Model m_Model;

    // Helper: append raw bytes to a buffer and return the buffer index
    int AppendToBuffer(const std::vector<unsigned char>& data)
    {
        tinygltf::Buffer buffer;
        buffer.data = data;
        m_Model.buffers.push_back(buffer);
        return static_cast<int>(m_Model.buffers.size()) - 1;
    }

    // Helper: create a bufferView + accessor for float vec3 data
    int AddVec3Accessor(const std::vector<glm::vec3>& data)
    {
        std::vector<unsigned char> bytes(data.size() * 12);
        std::memcpy(bytes.data(), data.data(), bytes.size());

        int bufIdx = AppendToBuffer(bytes);

        tinygltf::BufferView bv;
        bv.buffer = bufIdx;
        bv.byteOffset = 0;
        bv.byteLength = bytes.size();
        bv.byteStride = 0;  // tightly packed
        bv.target = 34962;   // ARRAY_BUFFER
        m_Model.bufferViews.push_back(bv);
        int bvIdx = static_cast<int>(m_Model.bufferViews.size()) - 1;

        tinygltf::Accessor acc;
        acc.bufferView = bvIdx;
        acc.byteOffset = 0;
        acc.componentType = 5126;  // FLOAT
        acc.count = data.size();
        acc.type = 3;  // VEC3
        m_Model.accessors.push_back(acc);
        return static_cast<int>(m_Model.accessors.size()) - 1;
    }

    // Helper: create a bufferView + accessor for float vec2 data
    int AddVec2Accessor(const std::vector<glm::vec2>& data)
    {
        std::vector<unsigned char> bytes(data.size() * 8);
        std::memcpy(bytes.data(), data.data(), bytes.size());

        int bufIdx = AppendToBuffer(bytes);

        tinygltf::BufferView bv;
        bv.buffer = bufIdx;
        bv.byteOffset = 0;
        bv.byteLength = bytes.size();
        bv.byteStride = 0;
        bv.target = 34962;
        m_Model.bufferViews.push_back(bv);
        int bvIdx = static_cast<int>(m_Model.bufferViews.size()) - 1;

        tinygltf::Accessor acc;
        acc.bufferView = bvIdx;
        acc.byteOffset = 0;
        acc.componentType = 5126;  // FLOAT
        acc.count = data.size();
        acc.type = 2;  // VEC2
        m_Model.accessors.push_back(acc);
        return static_cast<int>(m_Model.accessors.size()) - 1;
    }

    // Helper: create index accessor
    int AddIndexAccessor(const std::vector<uint32_t>& indices, IndexType idxType)
    {
        int componentType;
        std::vector<unsigned char> bytes;

        if (idxType == IndexType::UInt8)
        {
            componentType = 5121;  // UNSIGNED_BYTE
            bytes.resize(indices.size() * 1);
            for (size_t i = 0; i < indices.size(); i++)
                bytes[i] = (uint8_t)indices[i];
        }
        else if (idxType == IndexType::UInt16)
        {
            componentType = 5123;  // UNSIGNED_SHORT
            bytes.resize(indices.size() * 2);
            for (size_t i = 0; i < indices.size(); i++)
            {
                uint16_t v = (uint16_t)indices[i];
                std::memcpy(&bytes[i * 2], &v, 2);
            }
        }
        else
        {
            componentType = 5125;  // UNSIGNED_INT
            bytes.resize(indices.size() * 4);
            std::memcpy(bytes.data(), indices.data(), bytes.size());
        }

        int bufIdx = AppendToBuffer(bytes);

        tinygltf::BufferView bv;
        bv.buffer = bufIdx;
        bv.byteOffset = 0;
        bv.byteLength = bytes.size();
        bv.byteStride = 0;
        bv.target = 34963;  // ELEMENT_ARRAY_BUFFER
        m_Model.bufferViews.push_back(bv);
        int bvIdx = static_cast<int>(m_Model.bufferViews.size()) - 1;

        tinygltf::Accessor acc;
        acc.bufferView = bvIdx;
        acc.byteOffset = 0;
        acc.componentType = componentType;
        acc.count = indices.size();
        acc.type = 65;  // SCALAR
        m_Model.accessors.push_back(acc);
        return static_cast<int>(m_Model.accessors.size()) - 1;
    }

    // Add a primitive with POSITION accessor, returns primitive index in its mesh
    int AddPrimitivePositions(
        const std::vector<glm::vec3>& positions,
        int indexAccessorIdx,
        const std::vector<glm::vec3>* normals = nullptr,
        const std::vector<glm::vec2>* uvs = nullptr)
    {
        tinygltf::Primitive prim;
        prim.attributes["POSITION"] = AddVec3Accessor(positions);
        prim.indices = indexAccessorIdx;
        prim.mode = 4;  // TRIANGLES

        if (normals && !normals->empty())
            prim.attributes["NORMAL"] = AddVec3Accessor(*normals);
        if (uvs && !uvs->empty())
            prim.attributes["TEXCOORD_0"] = AddVec2Accessor(*uvs);

        // Add to first mesh or create one
        if (m_Model.meshes.empty())
        {
            tinygltf::Mesh mesh;
            mesh.primitives.push_back(prim);
            m_Model.meshes.push_back(mesh);
        }
        else
        {
            m_Model.meshes.back().primitives.push_back(prim);
        }

        return static_cast<int>(m_Model.meshes.back().primitives.size()) - 1;
    }

    int AddMeshWithPositions(
        const std::vector<glm::vec3>& positions,
        const std::vector<uint32_t>& indices,
        IndexType idxType,
        const std::vector<glm::vec3>* normals = nullptr,
        const std::vector<glm::vec2>* uvs = nullptr)
    {
        int idxAcc = -1;
        if (!indices.empty())
            idxAcc = AddIndexAccessor(indices, idxType);

        AddPrimitivePositions(positions, idxAcc, normals, uvs);
        return static_cast<int>(m_Model.meshes.size()) - 1;
    }

    void AddMeshNode(int meshIdx)
    {
        tinygltf::Node node;
        node.mesh = meshIdx;
        m_Model.nodes.push_back(node);

        // Track this node in the scene
        if (m_Model.scenes.empty())
        {
            tinygltf::Scene scene;
            scene.nodes.push_back(static_cast<int>(m_Model.nodes.size()) - 1);
            m_Model.scenes.push_back(scene);
            m_Model.defaultScene = 0;
        }
        else
        {
            m_Model.scenes[0].nodes.push_back(static_cast<int>(m_Model.nodes.size()) - 1);
        }
    }
};

#endif // GLTF_BUILDER_H