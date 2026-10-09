// ============================================================================
// RT4-T4: alpha-traversal classification helpers.
//
// Pinned contract for MeshHasAlphaTriangle / InstanceNeedsAlphaTraversal
// (RT2App/src/GPUSceneData.h): per-triangle + instance-override
// authorities with fail-open (toward any-hit) direction. The GPU visibility
// discriminators prove end-to-end behavior; these CPU tests pin every
// boundary so a regression fails loudly here first.
// ============================================================================

#include <doctest/doctest.h>
#include "GPUSceneData.h"

namespace
{
GPUMaterial OpaqueMat()
{
    GPUMaterial m;
    m.alphaMode = 0.0f;
    return m;
}

GPUMaterial AlphaMat()
{
    GPUMaterial m;
    m.alphaMode = 2.0f; // BLEND
    return m;
}

GPUMaterial MaskMat()
{
    GPUMaterial m;
    m.alphaMode = 1.0f; // MASK
    return m;
}
} // namespace

TEST_CASE("AlphaClassification empty tri array falls back to mesh material")
{
    // Legacy behavior preserved exactly: no per-triangle data reads
    // materials[mesh.materialIndex] (0 in practice).
    std::vector<GPUMaterial> mats = { OpaqueMat() };
    GPUMeshGeometry mesh;
    mesh.materialIndex = 0;
    CHECK(!MeshHasAlphaTriangle(mesh, mats));

    mats[0] = AlphaMat();
    CHECK(MeshHasAlphaTriangle(mesh, mats));
}

TEST_CASE("AlphaClassification per-triangle scan finds mixed alpha")
{
    std::vector<GPUMaterial> mats = { OpaqueMat(), OpaqueMat(), AlphaMat() };
    std::vector<uint32_t> tris = { 0, 0, 1, 0, 0, 0 }; // all opaque
    GPUMeshGeometry mesh;
    mesh.materialIndices = &tris;
    CHECK(!MeshHasAlphaTriangle(mesh, mats));

    tris[2] = 2; // one BLEND triangle
    CHECK(MeshHasAlphaTriangle(mesh, mats));
}

TEST_CASE("AlphaClassification out-of-range tri index fails open")
{
    std::vector<GPUMaterial> mats = { OpaqueMat() };
    std::vector<uint32_t> tris = { 0, 0, 99 }; // corrupt index
    GPUMeshGeometry mesh;
    mesh.materialIndices = &tris;
    // Conservative: unknown resolves toward any-hit, never opaque.
    CHECK(MeshHasAlphaTriangle(mesh, mats));
}

TEST_CASE("AlphaClassification override resolves through override material")
{
    std::vector<GPUMaterial> mats = { OpaqueMat(), AlphaMat() };
    GPUMeshGeometry mesh; // no per-tri data; would read mats[0] = opaque
    std::vector<GPUMeshGeometry> meshes = { mesh };

    // Opaque override on any mesh: opaque.
    CHECK(!InstanceNeedsAlphaTraversal(0, 0, meshes, mats));
    // Alpha override on opaque mesh: alpha (the old BLAS rule missed this
    // because it only read the mesh side).
    CHECK(InstanceNeedsAlphaTraversal(1, 0, meshes, mats));
    // Out-of-range override: fail open.
    CHECK(InstanceNeedsAlphaTraversal(7, 0, meshes, mats));
}

TEST_CASE("AlphaClassification per-triangle instance follows mesh data")
{
    std::vector<GPUMaterial> mats = { OpaqueMat(), MaskMat() };
    std::vector<uint32_t> mixed = { 0, 1 };
    std::vector<uint32_t> solid = { 0, 0 };
    GPUMeshGeometry mixedMesh, solidMesh;
    mixedMesh.materialIndices = &mixed;
    solidMesh.materialIndices = &solid;
    std::vector<GPUMeshGeometry> meshes = { mixedMesh, solidMesh };

    // MeshRef -1 (0xFFFFFFFF): per-triangle resolution.
    CHECK(InstanceNeedsAlphaTraversal(0xFFFFFFFFu, 0, meshes, mats));
    CHECK(!InstanceNeedsAlphaTraversal(0xFFFFFFFFu, 1, meshes, mats));
    // Corrupt mesh reference: fail open.
    CHECK(InstanceNeedsAlphaTraversal(0xFFFFFFFFu, 9, meshes, mats));
}

TEST_CASE("AlphaClassification shared mesh conservative across instances")
{
    // One mixed mesh referenced three ways: the BLAS-level answer must be
    // non-opaque (any single alpha-needing instance forces it), while each
    // instance keeps its own SBT answer.
    std::vector<GPUMaterial> mats = { OpaqueMat(), AlphaMat() };
    std::vector<uint32_t> mixed = { 0, 2 };
    GPUMeshGeometry mesh;
    mesh.materialIndices = &mixed;
    std::vector<GPUMeshGeometry> meshes = { mesh };

    CHECK(MeshHasAlphaTriangle(mesh, mats)); // mesh side needs any-hit
    CHECK(InstanceNeedsAlphaTraversal(0, 0, meshes, mats) == false); // opaque override
    CHECK(InstanceNeedsAlphaTraversal(2, 0, meshes, mats) == true);  // alpha override
    CHECK(InstanceNeedsAlphaTraversal(0xFFFFFFFFu, 0, meshes, mats) == true); // per-tri
}
