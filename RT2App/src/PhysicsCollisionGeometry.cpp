// ============================================================================
// PhysicsCollisionGeometry — shared CPU OBJ/glTF collision decode (T4).
//
// See header for the contract. This file must NOT define
// TINYGLTF_IMPLEMENTATION or TINYOBJLOADER_IMPLEMENTATION: SceneLoader.cpp
// owns both implementations and this translation unit uses declaration-only
// includes so the symbols are not duplicated. It links into RT2Tests and
// RT2SliceRunner (CPU-only: filesystem + tinyobj/tinygltf + core/Error only).
// ============================================================================

#include "PhysicsCollisionGeometry.h"

// Match SceneLoader.cpp: stb image I/O is disabled in tinygltf (geometry
// parsing is unaffected; embedded images are skipped, not failed). This also
// keeps the LoadImageData/WriteImageData symbols out of this TU so the link
// does not require them (no TU in the CPU targets defines them).
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include "tiny_gltf.h"
#include "tinyobjloader/tiny_obj_loader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace rt2::core {
namespace {

bool IsFiniteFloat(float v) { return std::isfinite(v); }

uint64_t HashCombine(uint64_t seed, uint64_t v)
{
    // FNV-1a mix step.
    seed ^= v;
    seed *= 1099511628211ULL;
    return seed;
}

Result<CollisionGeometry> FailWith(Error::Code code, const std::string& path,
                                   const std::string& detail)
{
    return Result<CollisionGeometry>::Fail(code, path, detail);
}

std::string LowerExtension(const std::filesystem::path& p)
{
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

// ---- OBJ ---------------------------------------------------------------

Result<CollisionGeometry> DecodeObj(const std::filesystem::path& absolutePath,
                                    const std::string& sourceKey,
                                    const ImportSettings& settings)
{
    if (sourceKey != "obj:whole-model")
    {
        return FailWith(Error::InvalidArgument, absolutePath.string(),
                        "DecodeCollisionGeometry: OBJ sourceKey mismatch (expected "
                        "'obj:whole-model', got '" + sourceKey + "')");
    }

    tinyobj::ObjReader reader;
    tinyobj::ObjReaderConfig config;
    config.triangulate = settings.triangulate;
    if (!reader.ParseFromFile(absolutePath.string(), config))
    {
        return FailWith(Error::Parse, absolutePath.string(),
                        "DecodeCollisionGeometry: tinyobj parse failed for '" +
                            absolutePath.string() +
                            "': " + reader.Error());
    }
    if (!reader.Warning().empty())
    {
        // Warnings (e.g. missing .mtl) do not fail collision decode; geometry
        // without materials is still collidable. No diagnostic sink here —
        // the provider names the file on real failures only.
    }

    const tinyobj::attrib_t& attrib = reader.GetAttrib();
    const std::vector<tinyobj::shape_t>& shapes = reader.GetShapes();

    CollisionGeometry out;
    out.vertices = attrib.vertices; // xyz triplets, authoring scale.
    if (out.vertices.size() % 3 != 0)
    {
        return FailWith(Error::Parse, absolutePath.string(),
                        "DecodeCollisionGeometry: OBJ vertex array is not xyz triplets");
    }
    const size_t vertexCount = out.vertices.size() / 3;
    if (vertexCount == 0)
    {
        return FailWith(Error::Parse, absolutePath.string(),
                        "DecodeCollisionGeometry: OBJ '" + absolutePath.string() +
                            "' contains no vertices");
    }
    if (vertexCount > kMaxCollisionVertices)
    {
        return FailWith(Error::InvalidArgument, absolutePath.string(),
                        "DecodeCollisionGeometry: OBJ '" + absolutePath.string() +
                            "' has " + std::to_string(vertexCount) +
                            " vertices (cap " +
                            std::to_string(kMaxCollisionVertices) + ")");
    }
    for (float v : out.vertices)
    {
        if (!IsFiniteFloat(v))
        {
            return FailWith(Error::Parse, absolutePath.string(),
                            "DecodeCollisionGeometry: OBJ '" +
                                absolutePath.string() +
                                "' contains non-finite vertex data");
        }
    }

    size_t indexCount = 0;
    for (const auto& shape : shapes)
        indexCount += shape.mesh.indices.size();
    if (indexCount == 0)
    {
        return FailWith(Error::Parse, absolutePath.string(),
                        "DecodeCollisionGeometry: OBJ '" + absolutePath.string() +
                            "' contains no triangle indices");
    }
    if (indexCount > kMaxCollisionIndices)
    {
        return FailWith(Error::InvalidArgument, absolutePath.string(),
                        "DecodeCollisionGeometry: OBJ '" + absolutePath.string() +
                            "' has " + std::to_string(indexCount) +
                            " indices (cap " +
                            std::to_string(kMaxCollisionIndices) + ")");
    }
    out.indices.reserve(indexCount);
    for (const auto& shape : shapes)
    {
        for (const tinyobj::index_t& idx : shape.mesh.indices)
        {
            if (idx.vertex_index < 0 ||
                (size_t)idx.vertex_index >= vertexCount)
            {
                return FailWith(Error::Parse, absolutePath.string(),
                                "DecodeCollisionGeometry: OBJ index out of range");
            }
            out.indices.push_back((uint32_t)idx.vertex_index);
        }
    }

    uint64_t h = FnV1a64(sourceKey.data(), sourceKey.size());
    h = FnV1a64(out.vertices.data(),
                out.vertices.size() * sizeof(float), h);
    h = FnV1a64(out.indices.data(),
                out.indices.size() * sizeof(uint32_t), h);
    out.contentHash = h;
    return Result<CollisionGeometry>::Ok(std::move(out));
}

// ---- glTF ---------------------------------------------------------------

struct GltfSubresource
{
    int scene = -1, node = -1, mesh = -1, primitive = -1;
};

bool ParseGltfSourceKey(const std::string& key, GltfSubresource& sub)
{
    // Exact format: gltf:scene=<s>:node=<n>:mesh=<m>:primitive=<p> with
    // non-negative integers and no trailing content (strict: sscanf-free so
    // no CRT deprecation warning; mirrors SceneAssetResolver::ParseGltfKey).
    auto readInt = [](const char* p, const char* end, int& dst) -> const char* {
        if (p >= end || *p < '0' || *p > '9')
            return nullptr;
        long v = 0;
        while (p < end && *p >= '0' && *p <= '9')
        {
            v = v * 10 + (*p - '0');
            if (v > 1000000000L)
                return nullptr;
            ++p;
        }
        dst = (int)v;
        return p;
    };
    auto expect = [](const char* p, const char* end, const char* tag) -> const char* {
        const size_t n = std::strlen(tag);
        if ((size_t)(end - p) < n || std::memcmp(p, tag, n) != 0)
            return nullptr;
        return p + n;
    };
    const char* p = key.c_str();
    const char* end = p + key.size();
    int s = -1, n = -1, m = -1, prim = -1;
    p = expect(p, end, "gltf:scene=");
    if (p == nullptr)
        return false;
    p = readInt(p, end, s);
    if (p == nullptr)
        return false;
    p = expect(p, end, ":node=");
    if (p == nullptr)
        return false;
    p = readInt(p, end, n);
    if (p == nullptr)
        return false;
    p = expect(p, end, ":mesh=");
    if (p == nullptr)
        return false;
    p = readInt(p, end, m);
    if (p == nullptr)
        return false;
    p = expect(p, end, ":primitive=");
    if (p == nullptr)
        return false;
    p = readInt(p, end, prim);
    if (p == nullptr || p != end)
        return false;
    sub.scene = s;
    sub.node = n;
    sub.mesh = m;
    sub.primitive = prim;
    return true;
}

bool ReadGltfVec3(const tinygltf::Model& model, int accessorIdx,
                  std::vector<float>& out, std::string& detail)
{
    if (accessorIdx < 0 || accessorIdx >= (int)model.accessors.size())
    {
        detail = "POSITION accessor index out of range";
        return false;
    }
    const tinygltf::Accessor& acc = model.accessors[(size_t)accessorIdx];
    if (acc.type != TINYGLTF_TYPE_VEC3 ||
        acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
    {
        detail = "POSITION accessor must be VEC3 float";
        return false;
    }
    if (acc.bufferView < 0 ||
        acc.bufferView >= (int)model.bufferViews.size())
    {
        detail = "POSITION bufferView out of range";
        return false;
    }
    const tinygltf::BufferView& bv =
        model.bufferViews[(size_t)acc.bufferView];
    if (bv.buffer < 0 || bv.buffer >= (int)model.buffers.size())
    {
        detail = "POSITION buffer out of range";
        return false;
    }
    const tinygltf::Buffer& buf = model.buffers[(size_t)bv.buffer];
    const size_t stride =
        bv.byteStride != 0 ? (size_t)bv.byteStride : 3 * sizeof(float);
    if (stride < 3 * sizeof(float))
    {
        detail = "POSITION byteStride too small";
        return false;
    }
    if (acc.count <= 0)
    {
        detail = "POSITION accessor is empty";
        return false;
    }
    const size_t base = (size_t)bv.byteOffset + (size_t)acc.byteOffset;
    const size_t need = base + (size_t)(acc.count - 1) * stride + 3 * sizeof(float);
    if (need > buf.data.size())
    {
        detail = "POSITION accessor overruns its buffer";
        return false;
    }
    out.resize((size_t)acc.count * 3);
    for (int i = 0; i < acc.count; ++i)
    {
        float x, y, z;
        std::memcpy(&x, &buf.data[base + (size_t)i * stride], sizeof(float));
        std::memcpy(&y, &buf.data[base + (size_t)i * stride + 4], sizeof(float));
        std::memcpy(&z, &buf.data[base + (size_t)i * stride + 8], sizeof(float));
        if (!IsFiniteFloat(x) || !IsFiniteFloat(y) || !IsFiniteFloat(z))
        {
            detail = "POSITION contains non-finite data";
            return false;
        }
        out[(size_t)i * 3 + 0] = x;
        out[(size_t)i * 3 + 1] = y;
        out[(size_t)i * 3 + 2] = z;
    }
    return true;
}

bool ReadGltfIndices(const tinygltf::Model& model,
                     const tinygltf::Primitive& prim, size_t vertexCount,
                     std::vector<uint32_t>& out, std::string& detail)
{
    if (prim.indices < 0)
    {
        // Non-indexed primitive: sequential triangles.
        if (vertexCount % 3 != 0)
        {
            detail = "non-indexed primitive vertex count is not a multiple of 3";
            return false;
        }
        out.resize(vertexCount);
        for (size_t i = 0; i < vertexCount; ++i)
            out[i] = (uint32_t)i;
        return true;
    }
    if (prim.indices >= (int)model.accessors.size())
    {
        detail = "index accessor out of range";
        return false;
    }
    const tinygltf::Accessor& acc =
        model.accessors[(size_t)prim.indices];
    if (acc.type != TINYGLTF_TYPE_SCALAR ||
        (acc.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
         acc.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT &&
         acc.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT))
    {
        detail = "index accessor must be SCALAR uint8/uint16/uint32";
        return false;
    }
    if (acc.bufferView < 0 ||
        acc.bufferView >= (int)model.bufferViews.size())
    {
        detail = "index bufferView out of range";
        return false;
    }
    const tinygltf::BufferView& bv =
        model.bufferViews[(size_t)acc.bufferView];
    if (bv.buffer < 0 || bv.buffer >= (int)model.buffers.size())
    {
        detail = "index buffer out of range";
        return false;
    }
    const tinygltf::Buffer& buf = model.buffers[(size_t)bv.buffer];
    const size_t elemSize =
        acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE
            ? 1
        : acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT
            ? 2
            : 4;
    const size_t stride =
        bv.byteStride != 0 ? (size_t)bv.byteStride : elemSize;
    const size_t base = (size_t)bv.byteOffset + (size_t)acc.byteOffset;
    if (acc.count <= 0)
    {
        detail = "index accessor is empty";
        return false;
    }
    const size_t need =
        base + (size_t)(acc.count - 1) * stride + elemSize;
    if (need > buf.data.size())
    {
        detail = "index accessor overruns its buffer";
        return false;
    }
    out.resize((size_t)acc.count);
    for (int i = 0; i < acc.count; ++i)
    {
        uint32_t v = 0;
        const unsigned char* p = &buf.data[base + (size_t)i * stride];
        if (elemSize == 1)
            v = *p;
        else if (elemSize == 2)
        {
            uint16_t t;
            std::memcpy(&t, p, 2);
            v = t;
        }
        else
            std::memcpy(&v, p, 4);
        if ((size_t)v >= vertexCount)
        {
            detail = "index out of range";
            return false;
        }
        out[(size_t)i] = v;
    }
    return true;
}

Result<CollisionGeometry> DecodeGltf(const std::filesystem::path& absolutePath,
                                     const std::string& sourceKey)
{
    GltfSubresource sub;
    if (!ParseGltfSourceKey(sourceKey, sub))
    {
        return FailWith(Error::InvalidArgument, absolutePath.string(),
                        "DecodeCollisionGeometry: glTF sourceKey mismatch (expected "
                        "'gltf:scene=<s>:node=<n>:mesh=<m>:primitive=<p>', got '" +
                            sourceKey + "')");
    }

    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;
    const std::string ext = LowerExtension(absolutePath);
    bool ok = false;
    if (ext == ".glb")
        ok = loader.LoadBinaryFromFile(&model, &err, &warn,
                                       absolutePath.string());
    else
        ok = loader.LoadASCIIFromFile(&model, &err, &warn,
                                      absolutePath.string());
    if (!ok)
    {
        return FailWith(Error::Parse, absolutePath.string(),
                        "DecodeCollisionGeometry: tinygltf parse failed for '" +
                            absolutePath.string() + "': " + err);
    }

    if (sub.scene >= (int)model.scenes.size())
    {
        return FailWith(Error::InvalidArgument, absolutePath.string(),
                        "DecodeCollisionGeometry: glTF scene index out of range");
    }
    // Node identity: validate the node index exists. Node hierarchy transforms
    // are NOT baked here — the entity's uniform world scale applies at
    // PhysicsWorld build (single scale owner, plan section 3).
    if (sub.node < 0 || sub.node >= (int)model.nodes.size())
    {
        return FailWith(Error::InvalidArgument, absolutePath.string(),
                        "DecodeCollisionGeometry: glTF node index out of range");
    }
    if (sub.mesh < 0 || sub.mesh >= (int)model.meshes.size())
    {
        return FailWith(Error::InvalidArgument, absolutePath.string(),
                        "DecodeCollisionGeometry: glTF mesh index out of range");
    }
    const tinygltf::Mesh& mesh = model.meshes[(size_t)sub.mesh];
    if (sub.primitive < 0 || sub.primitive >= (int)mesh.primitives.size())
    {
        return FailWith(Error::InvalidArgument, absolutePath.string(),
                        "DecodeCollisionGeometry: glTF primitive index out of range");
    }
    const tinygltf::Primitive& prim =
        mesh.primitives[(size_t)sub.primitive];
    const auto posIt = prim.attributes.find("POSITION");
    if (posIt == prim.attributes.end())
    {
        return FailWith(Error::Parse, absolutePath.string(),
                        "DecodeCollisionGeometry: glTF primitive has no POSITION");
    }

    CollisionGeometry out;
    std::string detail;
    if (!ReadGltfVec3(model, posIt->second, out.vertices, detail))
    {
        return FailWith(Error::Parse, absolutePath.string(),
                        "DecodeCollisionGeometry: glTF POSITION invalid (" +
                            detail + ")");
    }
    const size_t vertexCount = out.vertices.size() / 3;
    if (vertexCount == 0 || vertexCount > kMaxCollisionVertices)
    {
        return FailWith(
            vertexCount == 0 ? Error::Parse : Error::InvalidArgument,
            absolutePath.string(),
            "DecodeCollisionGeometry: glTF vertex count " +
                std::to_string(vertexCount) + " (cap " +
                std::to_string(kMaxCollisionVertices) + ")");
    }
    if (!ReadGltfIndices(model, prim, vertexCount, out.indices, detail))
    {
        return FailWith(Error::Parse, absolutePath.string(),
                        "DecodeCollisionGeometry: glTF indices invalid (" +
                            detail + ")");
    }
    if (out.indices.empty() || out.indices.size() > kMaxCollisionIndices)
    {
        return FailWith(
            out.indices.empty() ? Error::Parse : Error::InvalidArgument,
            absolutePath.string(),
            "DecodeCollisionGeometry: glTF index count " +
                std::to_string(out.indices.size()) + " (cap " +
                std::to_string(kMaxCollisionIndices) + ")");
    }

    uint64_t h = FnV1a64(sourceKey.data(), sourceKey.size());
    h = FnV1a64(out.vertices.data(),
                out.vertices.size() * sizeof(float), h);
    h = FnV1a64(out.indices.data(),
                out.indices.size() * sizeof(uint32_t), h);
    out.contentHash = h;
    return Result<CollisionGeometry>::Ok(std::move(out));
}

} // namespace

uint64_t FnV1a64(const void* data, size_t bytes, uint64_t seed)
{
    const auto* p = (const unsigned char*)data;
    uint64_t h = seed;
    for (size_t i = 0; i < bytes; ++i)
    {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

Result<CollisionGeometry> DecodeCollisionGeometry(
    const std::filesystem::path& absolutePath,
    const std::string& sourceKey,
    const ImportSettings& settings)
{
    if (absolutePath.empty() || sourceKey.empty())
    {
        return FailWith(Error::InvalidArgument, absolutePath.string(),
                        "DecodeCollisionGeometry: empty path or sourceKey");
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(absolutePath, ec) || ec)
    {
        return FailWith(Error::MissingAsset, absolutePath.string(),
                        "DecodeCollisionGeometry: file not found '" +
                            absolutePath.string() + "'");
    }
    const std::string ext = LowerExtension(absolutePath);
    if (ext == ".obj")
        return DecodeObj(absolutePath, sourceKey, settings);
    if (ext == ".gltf" || ext == ".glb")
        return DecodeGltf(absolutePath, sourceKey);
    return FailWith(Error::InvalidArgument, absolutePath.string(),
                    "DecodeCollisionGeometry: unsupported collision extension '" +
                        ext + "' (OBJ and glTF only)");
}

} // namespace rt2::core
