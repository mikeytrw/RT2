#include <doctest/doctest.h>

#include "SceneTypes.h"
#include "ECSScene.h"
#include "ECSComponents.h"
#include "MeshRegistry.h"
#include "GPUSceneData.h"
#include "../shaders/shader_interface.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>
#include <cmath>
#include <vector>

// ============================================================================
// Phase 3.0: Indexed-Buffer ABI Spike
// ============================================================================
// Verifies the vec4 indexed-buffer layout contract that will replace the
// per-triangle combined buffers in Phase 3.
//
// Contract:
//   - Vertex buffer:   vec4[] (16-byte stride, .xyz = position, .w = 1.0)
//   - Index buffer:     uint[] (4-byte stride)
//   - Normal buffer:    vec4[] (16-byte stride, .xyz = normal, .w = 0.0)
//   - UV buffer:        vec4[] (16-byte stride, .xy = UV, .zw = 0.0)
//   - InstanceMeshInfo: uvec4[] (16-byte stride, .x = vertOffset, .y = idxOffset,
//                                .z = normOffset, .w = uvOffset)
// ============================================================================

static glm::vec4 packPosition(float x, float y, float z) { return glm::vec4(x, y, z, 1.0f); }
static glm::vec4 packNormal(float x, float y, float z) { return glm::vec4(x, y, z, 0.0f); }
static glm::vec4 packUV(float u, float v) { return glm::vec4(u, v, 0.0f, 0.0f); }

static glm::vec3 toVec3(const glm::vec4& v) { return glm::vec3(v.x, v.y, v.z); }
static glm::vec2 toVec2(const glm::vec4& v) { return glm::vec2(v.x, v.y); }

// ============================================================================
// Struct size assertions
// ============================================================================

TEST_CASE("vec4 is 16 bytes for std430 array stride")
{
    CHECK(sizeof(glm::vec4) == 16);
    CHECK(sizeof(glm::uvec4) == 16);
    CHECK(sizeof(uint32_t) == 4);
}

// ============================================================================
// Position packing: vec4(x, y, z, 1.0)
// ============================================================================

TEST_CASE("Position packing: vec4.xyz = position, .w = 1.0")
{
    glm::vec4 p = packPosition(1.0f, 2.0f, 3.0f);
    CHECK(p.x == 1.0f);
    CHECK(p.y == 2.0f);
    CHECK(p.z == 3.0f);
    CHECK(p.w == 1.0f);
}

TEST_CASE("Position array: contiguous vec4 elements, 16-byte stride")
{
    std::vector<glm::vec4> positions;
    positions.push_back(packPosition(0, 0, 0));
    positions.push_back(packPosition(1, 0, 0));
    positions.push_back(packPosition(0, 1, 0));

    CHECK(positions.size() == 3);
    CHECK(sizeof(positions[0]) == 16);

    const float* raw = reinterpret_cast<const float*>(positions.data());
    CHECK(raw[0] == 0.0f);   // v0.x
    CHECK(raw[3] == 1.0f);   // v0.w
    CHECK(raw[4] == 1.0f);   // v1.x
    CHECK(raw[7] == 1.0f);   // v1.w
    CHECK(raw[8] == 0.0f);   // v2.x
    CHECK(raw[9] == 1.0f);   // v2.y
    CHECK(raw[11] == 1.0f);  // v2.w
}

// ============================================================================
// Normal packing: vec4(x, y, z, 0.0)
// ============================================================================

TEST_CASE("Normal packing: vec4.xyz = normal, .w = 0.0")
{
    glm::vec4 n = packNormal(0.0f, 1.0f, 0.0f);
    CHECK(n.x == 0.0f);
    CHECK(n.y == 1.0f);
    CHECK(n.z == 0.0f);
    CHECK(n.w == 0.0f);
}

// ============================================================================
// UV packing: vec4(u, v, 0.0, 0.0)
// ============================================================================

TEST_CASE("UV packing: vec4.xy = UV, .zw = 0.0")
{
    glm::vec4 uv = packUV(0.5f, 0.25f);
    CHECK(uv.x == 0.5f);
    CHECK(uv.y == 0.25f);
    CHECK(uv.z == 0.0f);
    CHECK(uv.w == 0.0f);
}

// ============================================================================
// Index buffer: uint32_t, 4-byte stride
// ============================================================================

TEST_CASE("Index buffer: uint32_t elements, 4-byte stride")
{
    std::vector<uint32_t> indices = {0, 1, 2, 1, 2, 3};
    CHECK(sizeof(indices[0]) == 4);
    CHECK(indices.size() == 6);
    CHECK(indices[0] == 0);
    CHECK(indices[5] == 3);
}

// ============================================================================
// InstanceMeshInfo: uvec4(vertOffset, idxOffset, normOffset, uvOffset)
// ============================================================================

TEST_CASE("InstanceMeshInfo: uvec4 with 4 offsets")
{
    glm::uvec4 meshInfo(0, 0, 0, 0);
    CHECK(meshInfo.x == 0);
    CHECK(meshInfo.y == 0);
    CHECK(meshInfo.z == 0);
    CHECK(meshInfo.w == 0);
}

TEST_CASE("InstanceMeshInfo: second mesh at correct offsets")
{
    glm::uvec4 mesh1(3, 3, 3, 3);
    CHECK(mesh1.x == 3);
    CHECK(mesh1.y == 3);
    CHECK(mesh1.z == 3);
    CHECK(mesh1.w == 3);
}

// ============================================================================
// Indexed fetch: simulate the shader's closest-hit fetch pattern
// ============================================================================

TEST_CASE("Indexed fetch: gl_PrimitiveID -> indices -> vertices")
{
    std::vector<glm::vec4> vertices = {
        packPosition(0, 0, 0),
        packPosition(1, 0, 0),
        packPosition(0, 1, 0),
    };
    std::vector<uint32_t> indices = {0, 1, 2};
    glm::uvec4 meshInfo(0, 0, 0, 0);

    uint32_t primID = 0;
    uint32_t idxBase = meshInfo.y + primID * 3;
    uint32_t i0 = indices[idxBase + 0];
    uint32_t i1 = indices[idxBase + 1];
    uint32_t i2 = indices[idxBase + 2];

    glm::vec3 p0 = toVec3(vertices[meshInfo.x + i0]);
    glm::vec3 p1 = toVec3(vertices[meshInfo.x + i1]);
    glm::vec3 p2 = toVec3(vertices[meshInfo.x + i2]);

    CHECK(i0 == 0);
    CHECK(i1 == 1);
    CHECK(i2 == 2);
    CHECK(p0 == glm::vec3(0, 0, 0));
    CHECK(p1 == glm::vec3(1, 0, 0));
    CHECK(p2 == glm::vec3(0, 1, 0));
}

TEST_CASE("Indexed fetch: multi-mesh with offsets")
{
    std::vector<glm::vec4> vertices = {
        packPosition(0, 0, 0),
        packPosition(1, 0, 0),
        packPosition(0, 1, 0),
        packPosition(5, 5, 5),
        packPosition(6, 5, 5),
        packPosition(5, 6, 5),
    };
    std::vector<uint32_t> indices = {
        0, 1, 2,
        0, 1, 2,
    };
    glm::uvec4 mesh0Info(0, 0, 0, 0);
    glm::uvec4 mesh1Info(3, 3, 0, 0);

    {
        uint32_t idxBase = mesh0Info.y + 0 * 3;
        uint32_t i0 = indices[idxBase + 0];
        uint32_t i1 = indices[idxBase + 1];
        uint32_t i2 = indices[idxBase + 2];
        glm::vec3 p0 = toVec3(vertices[mesh0Info.x + i0]);
        glm::vec3 p1 = toVec3(vertices[mesh0Info.x + i1]);
        glm::vec3 p2 = toVec3(vertices[mesh0Info.x + i2]);
        CHECK(p0 == glm::vec3(0, 0, 0));
        CHECK(p1 == glm::vec3(1, 0, 0));
        CHECK(p2 == glm::vec3(0, 1, 0));
    }

    {
        uint32_t idxBase = mesh1Info.y + 0 * 3;
        uint32_t i0 = indices[idxBase + 0];
        uint32_t i1 = indices[idxBase + 1];
        uint32_t i2 = indices[idxBase + 2];
        glm::vec3 p0 = toVec3(vertices[mesh1Info.x + i0]);
        glm::vec3 p1 = toVec3(vertices[mesh1Info.x + i1]);
        glm::vec3 p2 = toVec3(vertices[mesh1Info.x + i2]);
        CHECK(p0 == glm::vec3(5, 5, 5));
        CHECK(p1 == glm::vec3(6, 5, 5));
        CHECK(p2 == glm::vec3(5, 6, 5));
    }
}

// ============================================================================
// UV indexed fetch
// ============================================================================

TEST_CASE("Indexed UV fetch: indices -> UVs")
{
    std::vector<glm::vec4> uvs = {
        packUV(0.0f, 0.0f),
        packUV(1.0f, 0.0f),
        packUV(0.0f, 1.0f),
    };
    std::vector<uint32_t> indices = {0, 1, 2};
    glm::uvec4 meshInfo(0, 0, 0, 0);

    uint32_t idxBase = meshInfo.y + 0 * 3;
    uint32_t i0 = indices[idxBase + 0];
    uint32_t i1 = indices[idxBase + 1];
    uint32_t i2 = indices[idxBase + 2];

    glm::vec2 uv0 = toVec2(uvs[meshInfo.w + i0]);
    glm::vec2 uv1 = toVec2(uvs[meshInfo.w + i1]);
    glm::vec2 uv2 = toVec2(uvs[meshInfo.w + i2]);

    CHECK(uv0 == glm::vec2(0.0f, 0.0f));
    CHECK(uv1 == glm::vec2(1.0f, 0.0f));
    CHECK(uv2 == glm::vec2(0.0f, 1.0f));
}

// ============================================================================
// Normal indexed fetch
// ============================================================================

TEST_CASE("Indexed normal fetch: indices -> normals")
{
    std::vector<glm::vec4> normals = {
        packNormal(0, 1, 0),
        packNormal(1, 0, 0),
        packNormal(0, 0, 1),
    };
    std::vector<uint32_t> indices = {0, 1, 2};
    glm::uvec4 meshInfo(0, 0, 0, 0);

    uint32_t idxBase = meshInfo.y + 0 * 3;
    uint32_t i0 = indices[idxBase + 0];
    uint32_t i1 = indices[idxBase + 1];
    uint32_t i2 = indices[idxBase + 2];

    glm::vec3 n0 = toVec3(normals[meshInfo.z + i0]);
    glm::vec3 n1 = toVec3(normals[meshInfo.z + i1]);
    glm::vec3 n2 = toVec3(normals[meshInfo.z + i2]);

    CHECK(n0 == glm::vec3(0, 1, 0));
    CHECK(n1 == glm::vec3(1, 0, 0));
    CHECK(n2 == glm::vec3(0, 0, 1));
}

// ============================================================================
// Inline tangent computation from UV gradients
// ============================================================================

static glm::vec3 computeTangent(glm::vec3 p0, glm::vec3 p1, glm::vec3 p2,
                                glm::vec2 uv0, glm::vec2 uv1, glm::vec2 uv2)
{
    glm::vec3 edge1 = p1 - p0;
    glm::vec3 edge2 = p2 - p0;
    glm::vec2 dUV1 = uv1 - uv0;
    glm::vec2 dUV2 = uv2 - uv0;
    float det = dUV1.x * dUV2.y - dUV1.y * dUV2.x;
    if (std::abs(det) < 1e-8f)
        return glm::vec3(1.0f, 0.0f, 0.0f);
    float r = 1.0f / det;
    return glm::normalize(r * (dUV2.y * edge1 - dUV1.y * edge2));
}

TEST_CASE("Inline tangent: standard triangle")
{
    glm::vec3 p0(0, 0, 0), p1(1, 0, 0), p2(0, 1, 0);
    glm::vec2 uv0(0, 0), uv1(1, 0), uv2(0, 1);

    glm::vec3 t = computeTangent(p0, p1, p2, uv0, uv1, uv2);
    CHECK(t.x == doctest::Approx(1.0f).epsilon(0.001f));
    CHECK(t.y == doctest::Approx(0.0f).epsilon(0.001f));
    CHECK(t.z == doctest::Approx(0.0f).epsilon(0.001f));
}

TEST_CASE("Inline tangent: degenerate UV fallback")
{
    glm::vec3 p0(0, 0, 0), p1(1, 0, 0), p2(0, 1, 0);
    glm::vec2 uv0(0.5f, 0.5f), uv1(0.5f, 0.5f), uv2(0.5f, 0.5f);

    glm::vec3 t = computeTangent(p0, p1, p2, uv0, uv1, uv2);
    CHECK(t == glm::vec3(1.0f, 0.0f, 0.0f));
}

TEST_CASE("Inline tangent: zero UVs fallback")
{
    glm::vec3 p0(0, 0, 0), p1(1, 0, 0), p2(0, 1, 0);
    glm::vec2 uv0(0, 0), uv1(0, 0), uv2(0, 0);

    glm::vec3 t = computeTangent(p0, p1, p2, uv0, uv1, uv2);
    CHECK(t == glm::vec3(1.0f, 0.0f, 0.0f));
}

// ============================================================================
// Light reconstruction: simulate ReSTIR/scatter emissive triangle fetch
// ============================================================================

TEST_CASE("Light reconstruction: fetch emissive triangle positions via index")
{
    std::vector<glm::vec4> vertices = {
        packPosition(0, 0, 0),
        packPosition(2, 0, 0),
        packPosition(0, 2, 0),
    };
    std::vector<uint32_t> indices = {0, 1, 2};
    std::vector<glm::uvec4> instanceMeshInfo = {
        glm::uvec4(0, 0, 0, 0),
    };

    glm::uvec4 light_ids = glm::uvec4(0, 0, 0, 0xFFFFFFFFu);
    glm::uvec4 meshInfo = instanceMeshInfo[light_ids.x];
    uint32_t idxBase = meshInfo.y + light_ids.y * 3u;
    uint32_t i0 = indices[idxBase + 0u];
    uint32_t i1 = indices[idxBase + 1u];
    uint32_t i2 = indices[idxBase + 2u];

    glm::vec3 p0 = toVec3(vertices[meshInfo.x + i0]);
    glm::vec3 p1 = toVec3(vertices[meshInfo.x + i1]);
    glm::vec3 p2 = toVec3(vertices[meshInfo.x + i2]);

    CHECK(p0 == glm::vec3(0, 0, 0));
    CHECK(p1 == glm::vec3(2, 0, 0));
    CHECK(p2 == glm::vec3(0, 2, 0));

    glm::vec3 edge1 = p1 - p0;
    glm::vec3 edge2 = p2 - p0;
    float area = 0.5f * std::abs(glm::length(glm::cross(edge1, edge2)));
    CHECK(area == doctest::Approx(2.0f).epsilon(0.001f));
}

// ============================================================================
// MeshData -> vec4 conversion (what BuildAttributeBuffers will do)
// ============================================================================

TEST_CASE("MeshData vertices (float stride-3) convert to vec4 positions")
{
    MeshData mesh;
    mesh.vertices = {0, 0, 0,  1, 0, 0,  0, 1, 0};
    mesh.indices = {0, 1, 2};

    std::vector<glm::vec4> positions;
    positions.reserve(mesh.vertices.size() / 3);
    for (size_t i = 0; i < mesh.vertices.size(); i += 3)
        positions.push_back(packPosition(mesh.vertices[i], mesh.vertices[i + 1], mesh.vertices[i + 2]));

    CHECK(positions.size() == 3);
    CHECK(positions[0] == glm::vec4(0, 0, 0, 1));
    CHECK(positions[1] == glm::vec4(1, 0, 0, 1));
    CHECK(positions[2] == glm::vec4(0, 1, 0, 1));
}

TEST_CASE("MeshData normals (float stride-3) convert to vec4 normals")
{
    MeshData mesh;
    mesh.normals = {0, 1, 0,  1, 0, 0,  0, 0, 1};

    std::vector<glm::vec4> normals;
    normals.reserve(mesh.normals.size() / 3);
    for (size_t i = 0; i < mesh.normals.size(); i += 3)
        normals.push_back(packNormal(mesh.normals[i], mesh.normals[i + 1], mesh.normals[i + 2]));

    CHECK(normals.size() == 3);
    CHECK(normals[0] == glm::vec4(0, 1, 0, 0));
    CHECK(normals[1] == glm::vec4(1, 0, 0, 0));
    CHECK(normals[2] == glm::vec4(0, 0, 1, 0));
}

TEST_CASE("MeshData UVs (float stride-2) convert to vec4 UVs")
{
    MeshData mesh;
    mesh.uvs = {0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f};

    std::vector<glm::vec4> uvs;
    uvs.reserve(mesh.uvs.size() / 2);
    for (size_t i = 0; i < mesh.uvs.size(); i += 2)
        uvs.push_back(packUV(mesh.uvs[i], mesh.uvs[i + 1]));

    CHECK(uvs.size() == 3);
    CHECK(uvs[0] == glm::vec4(0, 0, 0, 0));
    CHECK(uvs[1] == glm::vec4(1, 0, 0, 0));
    CHECK(uvs[2] == glm::vec4(0, 1, 0, 0));
}

TEST_CASE("MeshData with empty normals/UVs produces empty buffers")
{
    MeshData mesh;
    mesh.vertices = {0, 0, 0,  1, 0, 0,  0, 1, 0};
    mesh.indices = {0, 1, 2};

    std::vector<glm::vec4> normals;
    if (!mesh.normals.empty())
        for (size_t i = 0; i < mesh.normals.size(); i += 3)
            normals.push_back(packNormal(mesh.normals[i], mesh.normals[i + 1], mesh.normals[i + 2]));

    std::vector<glm::vec4> uvs;
    if (!mesh.uvs.empty())
        for (size_t i = 0; i < mesh.uvs.size(); i += 2)
            uvs.push_back(packUV(mesh.uvs[i], mesh.uvs[i + 1]));

    CHECK(normals.empty());
    CHECK(uvs.empty());
}

// ============================================================================
// Two instances at different positions (spike scenario from plan)
// ============================================================================

TEST_CASE("Two instances referencing same mesh at different transforms")
{
    std::vector<glm::vec4> vertices = {
        packPosition(0, 0, 0),
        packPosition(1, 0, 0),
        packPosition(0, 1, 0),
    };
    std::vector<uint32_t> indices = {0, 1, 2};
    glm::uvec4 meshInfo(0, 0, 0, 0);

    glm::mat4 world0 = glm::mat4(1.0f);
    glm::mat4 world1 = glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0));

    {
        uint32_t idxBase = meshInfo.y + 0 * 3;
        uint32_t i0 = indices[idxBase + 0];
        uint32_t i1 = indices[idxBase + 1];
        uint32_t i2 = indices[idxBase + 2];
        glm::vec3 p0 = toVec3(vertices[meshInfo.x + i0]);
        glm::vec3 p1 = toVec3(vertices[meshInfo.x + i1]);
        glm::vec3 p2 = toVec3(vertices[meshInfo.x + i2]);

        CHECK(p0 == glm::vec3(0, 0, 0));
        CHECK(p1 == glm::vec3(1, 0, 0));

        glm::vec3 w0 = toVec3(world0 * glm::vec4(p0, 1.0f));
        CHECK(w0 == glm::vec3(0, 0, 0));
    }

    {
        uint32_t idxBase = meshInfo.y + 0 * 3;
        uint32_t i0 = indices[idxBase + 0];
        uint32_t i1 = indices[idxBase + 1];
        uint32_t i2 = indices[idxBase + 2];
        glm::vec3 p0 = toVec3(vertices[meshInfo.x + i0]);
        glm::vec3 p1 = toVec3(vertices[meshInfo.x + i1]);
        glm::vec3 p2 = toVec3(vertices[meshInfo.x + i2]);

        glm::vec3 w0 = toVec3(world1 * glm::vec4(p0, 1.0f));
        glm::vec3 w1 = toVec3(world1 * glm::vec4(p1, 1.0f));
        CHECK(w0 == glm::vec3(10, 0, 0));
        CHECK(w1 == glm::vec3(11, 0, 0));
    }
}

// ============================================================================
// Buffer size computation (for allocation)
// ============================================================================

TEST_CASE("Buffer sizes: vertex buffer = vertCount * 16 bytes")
{
    uint32_t vertCount = 1000;
    size_t vertBufSize = vertCount * sizeof(glm::vec4);
    CHECK(vertBufSize == 16000);
}

TEST_CASE("Buffer sizes: index buffer = indexCount * 4 bytes")
{
    uint32_t indexCount = 3000;
    size_t idxBufSize = indexCount * sizeof(uint32_t);
    CHECK(idxBufSize == 12000);
}

TEST_CASE("Buffer sizes: instance mesh info = instanceCount * 16 bytes")
{
    uint32_t instanceCount = 50;
    size_t infoBufSize = instanceCount * sizeof(glm::uvec4);
    CHECK(infoBufSize == 800);
}