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
#include <exception>
#include <fstream>
#include <new>

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

    // Caps before copies: the parser-owned arrays are measured first so an
    // over-cap hostile file is refused without duplicating its storage.
    if (attrib.vertices.size() / 3 > kMaxCollisionVertices)
    {
        return FailWith(Error::InvalidArgument, absolutePath.string(),
                        "DecodeCollisionGeometry: OBJ '" + absolutePath.string() +
                            "' has " +
                            std::to_string(attrib.vertices.size() / 3) +
                            " vertices (cap " +
                            std::to_string(kMaxCollisionVertices) + ")");
    }
    uint64_t indexTotal = 0;
    for (const auto& shape : shapes)
    {
        indexTotal += (uint64_t)shape.mesh.indices.size();
        if (indexTotal > (uint64_t)kMaxCollisionIndices)
        {
            return FailWith(Error::InvalidArgument, absolutePath.string(),
                            "DecodeCollisionGeometry: OBJ '" +
                                absolutePath.string() +
                                "' has more than " +
                                std::to_string(kMaxCollisionIndices) +
                                " indices (cap refused before staging)");
        }
    }

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
    // (The vertex cap was enforced on the parser-owned array above, before
    // the copy landed here.)
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

    // indexTotal was capped above before any staging allocation.
    if (indexTotal == 0)
    {
        return FailWith(Error::Parse, absolutePath.string(),
                        "DecodeCollisionGeometry: OBJ '" + absolutePath.string() +
                            "' contains no triangle indices");
    }
    out.indices.reserve((size_t)indexTotal);
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

// Shared accessor-span validation. All metadata is attacker-influenced, so
// every bound is checked with overflow-safe arithmetic BEFORE any allocation
// or byte is touched:
//   - non-negative offsets/counts and a known component layout,
//   - sparse accessors are rejected (unsupported, never silently ignored),
//   - count is capped before out.resize (allocation follows validation),
//   - the buffer view lies inside its buffer, and the accessor span lies
//     inside BOTH the view and the buffer,
//   - stride covers at least one element and base/stride honor component
//     alignment (glTF aligns accessors to the component width — 4 for float,
//     2 for uint16 — not to the whole-element width; unaligned component
//     reads are refused, not reinterpreted).
struct GltfSpan
{
    const unsigned char* data = nullptr; // first element byte
    size_t count = 0;                    // validated element count
    size_t stride = 0;                   // validated bytes per element
    size_t elemSize = 0;                 // bytes per tightly-packed element
};

size_t ComponentWidth(int componentType)
{
    switch (componentType)
    {
        case TINYGLTF_COMPONENT_TYPE_BYTE:
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: return 1;
        case TINYGLTF_COMPONENT_TYPE_SHORT:
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return 2;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        case TINYGLTF_COMPONENT_TYPE_FLOAT: return 4;
        default: return 0;
    }
}

size_t TypeComponents(int type)
{
    switch (type)
    {
        case TINYGLTF_TYPE_SCALAR: return 1;
        case TINYGLTF_TYPE_VEC3: return 3;
        default: return 0;
    }
}

bool ValidateGltfSpan(const tinygltf::Model& model, int accessorIdx,
                      int expectedType, const int* allowedComponents,
                      size_t allowedCount, size_t maxCount,
                      const char* what, GltfSpan& span, Error::Code& codeOut, std::string& detail)
{
    codeOut = Error::Parse;
    if (accessorIdx < 0 || accessorIdx >= (int)model.accessors.size())
    {
        detail = std::string(what) + " accessor index out of range";
        return false;
    }
    const tinygltf::Accessor& acc = model.accessors[(size_t)accessorIdx];
    if (acc.type != expectedType || TypeComponents(expectedType) == 0)
    {
        detail = std::string(what) + " accessor has the wrong type";
        return false;
    }
    bool componentOk = false;
    for (size_t i = 0; i < allowedCount; ++i)
    {
        if (acc.componentType == allowedComponents[i])
            componentOk = true;
    }
    const size_t elemSize =
        ComponentWidth(acc.componentType) * TypeComponents(expectedType);
    if (!componentOk || elemSize == 0)
    {
        detail = std::string(what) + " accessor has an unsupported component type";
        return false;
    }
    if (acc.sparse.isSparse)
    {
        codeOut = Error::InvalidArgument;
        detail = std::string(what) +
                 " uses a sparse accessor (unsupported for collision decode)";
        return false;
    }
    if (acc.count <= 0)
    {
        detail = std::string(what) + " accessor is empty";
        return false;
    }
    // Cap before allocation: attacker-controlled count never sizes a vector.
    if ((uint64_t)acc.count > (uint64_t)maxCount)
    {
        codeOut = Error::InvalidArgument;
        detail = std::string(what) + " accessor count exceeds the collision cap";
        return false;
    }
    if (acc.bufferView < 0 ||
        acc.bufferView >= (int)model.bufferViews.size())
    {
        detail = std::string(what) + " bufferView out of range";
        return false;
    }
    const tinygltf::BufferView& bv =
        model.bufferViews[(size_t)acc.bufferView];
    if (bv.buffer < 0 || bv.buffer >= (int)model.buffers.size())
    {
        detail = std::string(what) + " buffer out of range";
        return false;
    }
    const tinygltf::Buffer& buf = model.buffers[(size_t)bv.buffer];
    if (bv.byteOffset < 0 || bv.byteLength < 0 || acc.byteOffset < 0)
    {
        detail = std::string(what) + " has a negative offset/length";
        return false;
    }
    const size_t stride = bv.byteStride != 0 ? (size_t)bv.byteStride : elemSize;
    if (bv.byteStride != 0 && (bv.byteStride < 0 || (size_t)bv.byteStride < elemSize))
    {
        detail = std::string(what) + " byteStride is smaller than one element";
        return false;
    }
    // View inside buffer, then span inside view and buffer. Subtraction form
    // (remaining >= need) is overflow-safe for arbitrary hostile metadata.
    const size_t bufSize = buf.data.size();
    const size_t viewOff = (size_t)bv.byteOffset;
    const size_t viewLen = (size_t)bv.byteLength;
    const size_t accOff = (size_t)acc.byteOffset;
    if (viewOff > bufSize || viewLen > bufSize - viewOff)
    {
        detail = std::string(what) + " buffer view escapes its buffer";
        return false;
    }
    if (accOff > viewLen)
    {
        detail = std::string(what) + " accessor offset escapes its view";
        return false;
    }
    const uint64_t count = (uint64_t)acc.count;
    const uint64_t spanNeed =
        (count - 1) * (uint64_t)stride + (uint64_t)elemSize;
    if (spanNeed > (uint64_t)(viewLen - accOff) ||
        spanNeed > (uint64_t)(bufSize - (viewOff + accOff)))
    {
        detail = std::string(what) + " accessor span escapes its view/buffer";
        return false;
    }
    const size_t base = viewOff + accOff;
    const size_t compWidth = ComponentWidth(acc.componentType);
    if (base % compWidth != 0 || stride % compWidth != 0)
    {
        detail = std::string(what) + " accessor is misaligned for its component type";
        return false;
    }
    span.data = buf.data.data() + base;
    span.count = (size_t)count;
    span.stride = stride;
    span.elemSize = elemSize;
    return true;
}

bool ReadGltfVec3(const tinygltf::Model& model, int accessorIdx,
                  std::vector<float>& out, Error::Code& codeOut, std::string& detail)
{
    static const int kFloat[] = {TINYGLTF_COMPONENT_TYPE_FLOAT};
    GltfSpan span;
    if (!ValidateGltfSpan(model, accessorIdx, TINYGLTF_TYPE_VEC3, kFloat, 1,
                           kMaxCollisionVertices, "POSITION", span, codeOut,
                           detail))
        return false;
    out.resize(span.count * 3);
    for (size_t i = 0; i < span.count; ++i)
    {
        float x, y, z;
        const unsigned char* p = span.data + i * span.stride;
        std::memcpy(&x, p, sizeof(float));
        std::memcpy(&y, p + 4, sizeof(float));
        std::memcpy(&z, p + 8, sizeof(float));
        if (!IsFiniteFloat(x) || !IsFiniteFloat(y) || !IsFiniteFloat(z))
        {
            detail = "POSITION contains non-finite data";
            return false;
        }
        out[i * 3 + 0] = x;
        out[i * 3 + 1] = y;
        out[i * 3 + 2] = z;
    }
    return true;
}

bool ReadGltfIndices(const tinygltf::Model& model,
                     const tinygltf::Primitive& prim, size_t vertexCount,
                     std::vector<uint32_t>& out, Error::Code& codeOut, std::string& detail)
{
    if (prim.indices < 0)
    {
        // Non-indexed primitive: sequential triangles.
        if (vertexCount % 3 != 0)
        {
            detail = "non-indexed primitive vertex count is not a multiple of 3";
            return false;
        }
        if (vertexCount > kMaxCollisionIndices)
        {
            codeOut = Error::InvalidArgument;
            detail = "non-indexed primitive exceeds the collision index cap";
            return false;
        }
        out.resize(vertexCount);
        for (size_t i = 0; i < vertexCount; ++i)
            out[i] = (uint32_t)i;
        return true;
    }
    static const int kUint[] = {TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE,
                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT,
                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT};
    GltfSpan span;
    if (!ValidateGltfSpan(model, prim.indices, TINYGLTF_TYPE_SCALAR, kUint, 3,
                           kMaxCollisionIndices, "index", span, codeOut,
                           detail))
        return false;
    // Element width follows the validated component type.
    const tinygltf::Accessor& acc =
        model.accessors[(size_t)prim.indices];
    const size_t elemSize =
        acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE
            ? 1
        : acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT
            ? 2
            : 4;
    if (span.elemSize != elemSize)
    {
        detail = "index accessor width mismatch";
        return false;
    }
    out.resize(span.count);
    for (size_t i = 0; i < span.count; ++i)
    {
        uint32_t v = 0;
        const unsigned char* p = span.data + i * span.stride;
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
        out[i] = v;
    }
    // Triangle cardinality: indexed triangle soup comes in triples.
    if (out.size() % 3 != 0)
    {
        detail = "indexed triangle count is not a multiple of 3";
        return false;
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

    if (sub.scene < 0 || sub.scene >= (int)model.scenes.size())
    {
        return FailWith(Error::InvalidArgument, absolutePath.string(),
                        "DecodeCollisionGeometry: glTF scene index out of range");
    }
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
    // Exact source identity: the named node must be reachable from the named
    // scene (roots plus transitive children) AND carry the named mesh. A
    // forged but in-range key that recombines unrelated scene/node/mesh
    // indices selects geometry the importer never identified, so it is an
    // unresolved-identity refusal, not a best-effort decode.
    {
        const tinygltf::Scene& scene = model.scenes[(size_t)sub.scene];
        std::vector<int> stack = scene.nodes;
        std::vector<char> seen(model.nodes.size(), 0);
        bool reachable = false;
        while (!stack.empty())
        {
            const int current = stack.back();
            stack.pop_back();
            if (current < 0 || current >= (int)model.nodes.size())
                continue;
            if (seen[(size_t)current])
                continue;
            seen[(size_t)current] = 1;
            if (current == sub.node)
            {
                reachable = true;
                break;
            }
            for (int child : model.nodes[(size_t)current].children)
                stack.push_back(child);
        }
        if (!reachable)
        {
            return FailWith(Error::InvalidArgument, absolutePath.string(),
                            "DecodeCollisionGeometry: glTF node is not reachable "
                            "from the named scene (forged source relationship)");
        }
        if (model.nodes[(size_t)sub.node].mesh != sub.mesh)
        {
            return FailWith(Error::InvalidArgument, absolutePath.string(),
                            "DecodeCollisionGeometry: glTF node does not carry "
                            "the named mesh (forged source relationship)");
        }
    }
    // Node hierarchy transforms are NOT baked here — the entity's uniform
    // world scale applies at PhysicsWorld build (single scale owner, plan
    // section 3).
    const tinygltf::Mesh& mesh = model.meshes[(size_t)sub.mesh];
    if (sub.primitive < 0 || sub.primitive >= (int)mesh.primitives.size())
    {
        return FailWith(Error::InvalidArgument, absolutePath.string(),
                        "DecodeCollisionGeometry: glTF primitive index out of range");
    }
    const tinygltf::Primitive& prim =
        mesh.primitives[(size_t)sub.primitive];
    // Triangle soup only. Mode -1 is tinygltf's absent-field value, which the
    // glTF spec defines as TRIANGLES; every other non-triangle mode is a loud
    // refusal (points/lines/strips/fans have no collision meaning).
    if (prim.mode != TINYGLTF_MODE_TRIANGLES && prim.mode != -1)
    {
        return FailWith(Error::InvalidArgument, absolutePath.string(),
                        "DecodeCollisionGeometry: glTF primitive is not triangles "
                        "(collision decode requires TRIANGLES mode)");
    }
    const auto posIt = prim.attributes.find("POSITION");
    if (posIt == prim.attributes.end())
    {
        return FailWith(Error::Parse, absolutePath.string(),
                        "DecodeCollisionGeometry: glTF primitive has no POSITION");
    }

    CollisionGeometry out;
    std::string detail;
    Error::Code readerCode = Error::Parse;
    if (!ReadGltfVec3(model, posIt->second, out.vertices, readerCode, detail))
    {
        return FailWith(readerCode, absolutePath.string(),
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
    if (!ReadGltfIndices(model, prim, vertexCount, out.indices, readerCode,
                          detail))
    {
        return FailWith(readerCode, absolutePath.string(),
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

float CollisionMinHalf(const CollisionGeometry& geom)
{
    if (geom.vertices.size() < 3 || geom.vertices.size() % 3 != 0)
        return 0.0f;
    float minX = geom.vertices[0], maxX = minX;
    float minY = geom.vertices[1], maxY = minY;
    float minZ = geom.vertices[2], maxZ = minZ;
    for (size_t i = 3; i < geom.vertices.size(); i += 3)
    {
        const float x = geom.vertices[i];
        const float y = geom.vertices[i + 1];
        const float z = geom.vertices[i + 2];
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            return 0.0f;
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
        minZ = std::min(minZ, z);
        maxZ = std::max(maxZ, z);
    }
    return std::min({(maxX - minX) * 0.5f, (maxY - minY) * 0.5f,
                     (maxZ - minZ) * 0.5f});
}

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

namespace {
bool s_TestThrowBadAlloc = false;
} // namespace

void SetCollisionDecodeTestThrow(bool fail)
{
    s_TestThrowBadAlloc = fail;
}

bool CollisionDecodeTestThrow()
{
    return s_TestThrowBadAlloc;
}

Result<CollisionGeometry> DecodeCollisionGeometryChecked(
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

Result<CollisionGeometry> DecodeCollisionGeometry(
    const std::filesystem::path& absolutePath,
    const std::string& sourceKey,
    const ImportSettings& settings)
{
    // Allocation/parser translation boundary (review P2): resource
    // exhaustion anywhere below — vendor parser internals, vector growth,
    // Bullet-adjacent staging buffers — surfaces as a typed file diagnostic,
    // never an escaped exception. Error::Io is the resource-exhaustion code.
    // The deterministic injection hook fires INSIDE the boundary so tests
    // prove translation rather than escape.
    try
    {
        if (s_TestThrowBadAlloc)
            throw std::bad_alloc();
        return DecodeCollisionGeometryChecked(absolutePath, sourceKey,
                                              settings);
    }
    catch (const std::bad_alloc&)
    {
        return Result<CollisionGeometry>::Fail(
            Error::Io, absolutePath.string(),
            "DecodeCollisionGeometry: allocation failure while decoding '" +
                absolutePath.string() + "' (no geometry produced)");
    }
    catch (const std::exception& e)
    {
        return Result<CollisionGeometry>::Fail(
            Error::Parse, absolutePath.string(),
            std::string("DecodeCollisionGeometry: parser exception for '") +
                absolutePath.string() + "': " + e.what());
    }
}

} // namespace rt2::core
