#include <doctest/doctest.h>

#include "GPUSceneData.h"
#include "SceneTypes.h"
#include "ECSScene.h"
#include "ECSComponents.h"
#include "SceneGraph.h"
#include <glm/glm.hpp>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
std::string ReadShaderSource(const char* path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}

// ============================================================================
// shader_interface.h struct layout tests (shared C++/GLSL)
// ============================================================================

TEST_CASE("SICameraData is 496 bytes (7 vec4 + 6 mat4)")
{
    CHECK(sizeof(SICameraData) == 496);
}

TEST_CASE("SIMaterial is 80 bytes")
{
    CHECK(sizeof(SIMaterial) == 80);
}

TEST_CASE("SITriangleLight is 32 bytes")
{
    CHECK(sizeof(SITriangleLight) == 32);
}

TEST_CASE("SINRDUniformData is 32 bytes (RR5-T1: 16-byte prefix + roulette words)")
{
    CHECK(sizeof(SINRDUniformData) == 32);
    // Exact 32B transport: existing 16B prefix unchanged, then uint
    // depth16, float min20, float max24, reserved zero28.
    CHECK(offsetof(SINRDUniformData, nrdEnabled) == 0);
    CHECK(offsetof(SINRDUniformData, lobeDither) == 4);
    CHECK(offsetof(SINRDUniformData, restirGIEnabled) == 8);
    CHECK(offsetof(SINRDUniformData, restirGIReservoirIndex) == 12);
    CHECK(offsetof(SINRDUniformData, rrStartDepth) == 16);
    CHECK(offsetof(SINRDUniformData, rrMinP) == 20);
    CHECK(offsetof(SINRDUniformData, rrMaxP) == 24);
    CHECK(offsetof(SINRDUniformData, rrReserved) == 28);
}

TEST_CASE("SINRDUniformData zero-initializes the reserved word")
{
    SINRDUniformData d{};
    CHECK(d.rrReserved == 0u);
    CHECK(d.rrStartDepth == 0u);
}

// ============================================================================
// RR5-T0 diagnostic seed transport: 16-byte set-1 binding-17 UBO plus
// 16-byte probe slots. First word is the seed; remaining words are the
// probe selectors (default 0 = probe off).
// ============================================================================

TEST_CASE("SIDiagSeedUniform is 16 bytes with seed at byte 0")
{
    CHECK(sizeof(SIDiagSeedUniform) == 16);
    CHECK(offsetof(SIDiagSeedUniform, diagSampleSeed) == 0);
    CHECK(offsetof(SIDiagSeedUniform, diagProbeActive) == 4);
    CHECK(offsetof(SIDiagSeedUniform, diagProbeX) == 8);
    CHECK(offsetof(SIDiagSeedUniform, diagProbeY) == 12);
    SIDiagSeedUniform d{};
    CHECK(d.diagSampleSeed == 0u);
    CHECK(d.diagProbeActive == 0u);
}

TEST_CASE("SIDiagProbeSlots is 16 bytes (4 consumer slots)")
{
    CHECK(sizeof(SIDiagProbeSlots) == 16);
    CHECK(offsetof(SIDiagProbeSlots, slots) == 0);
}

// ============================================================================
// RR5-T0 review P2: mandatory diagnostic resources must be checked before
// any descriptor write, with partial-allocation rollback. CPU tests cannot
// fault-inject Vulkan allocation (CPU-only boundary), so this test pins
// the checked structure in the production source: checked creation,
// rollback, and init-gating before the descriptor-write section. No fault
// injection was performed; the success path is proven by validation-clean
// production runs.
// ============================================================================

TEST_CASE("DiagSeed: allocation failures are checked with rollback before writes")
{
    const std::string renderer = ReadShaderSource("RT2App/src/RendererGPU.cpp");
    REQUIRE(!renderer.empty());
    const size_t seedCheck = renderer.find(
        "if (!GpuResources::CreateBuffer(m_Device, sizeof(SIDiagSeedUniform)");
    CHECK(seedCheck != std::string::npos);
    const size_t probeCheck = renderer.find(
        "if (!GpuResources::CreateBuffer(m_Device, sizeof(SIDiagProbeSlots)");
    CHECK(probeCheck != std::string::npos);
    CHECK(renderer.find("failed to create seed UBO", seedCheck) != std::string::npos);
    CHECK(renderer.find("failed to create probe buffer", probeCheck) != std::string::npos);
    CHECK(renderer.find("rolling back seed UBO", probeCheck) != std::string::npos);
    // Initialization fails before any descriptor write can see null handles.
    // (CreateDiagSeedResources runs in Init right after the set-1 layout;
    // descriptor writes happen later in UpdateGBufferDescriptorSet.)
    CHECK(renderer.find("if (!CreateDiagSeedResources())") != std::string::npos);
}

// ============================================================================
// RR5-T0 review P1: a probe dump is a checked artifact. The production
// host must reject out-of-extent pixels before any frame, fail readback
// errors instead of skipping frames, and require the requested frame
// count plus at least one live slot before writing a success file.
// ============================================================================

TEST_CASE("DiagProbe: empty evidence fails loudly instead of dumping success")
{
    const std::string host = ReadShaderSource("RT2App/src/WalnutApp.cpp");
    REQUIRE(!host.empty());
    // Out-of-extent probe pixel exits before the frame loop.
    CHECK(host.find("outside render extent") != std::string::npos);
    CHECK(host.find("RNG probe pixel FAILED") != std::string::npos);
    // Readback failure is run failure, never a skipped frame.
    CHECK(host.find("diagProbeReadFailed = true") != std::string::npos);
    // Requested-count and any-live-sample gates precede the success write.
    CHECK(host.find("sampledFrames") != std::string::npos);
    CHECK(host.find("no live RNG sample") != std::string::npos);
    CHECK(host.find("discardOutput(g_CLI.diagRngDump)") != std::string::npos);
}

// RR5-T0 re-review P1: every probe failure invalidates the explicitly
// requested artifacts (never a stale success), the cleanup is verified
// loud, and no broad deletion reaches beyond the requested targets.
TEST_CASE("DiagProbe: failures discard requested artifacts with loud cleanup")
{
    const std::string host = ReadShaderSource("RT2App/src/WalnutApp.cpp");
    REQUIRE(!host.empty());
    // Early pixel rejection discards the requested dump AND the
    // requested image outputs of this run (bounded window: the printf
    // and its discards belong to the same pre-loop branch).
    const size_t gate = host.find("outside render extent");
    REQUIRE(gate != std::string::npos);
    const size_t marker = host.find("RNG probe pixel FAILED", gate);
    REQUIRE(marker != std::string::npos);
    const std::string branch = host.substr(marker, 1200);
    CHECK(branch.find("discardOutput(g_CLI.diagRngDump)") != std::string::npos);
    CHECK(branch.find("discardOutput(g_CLI.outputHDRPath)") != std::string::npos);
    CHECK(branch.find("std::exit(EXIT_FAILURE)") != std::string::npos);
    // Checked discard on evidence, open and write failures.
    CHECK(host.find("discardDumpChecked") != std::string::npos);
    CHECK(host.find("stale/partial dump discarded") != std::string::npos);
    CHECK(host.find("dump cleanup FAILED, stale file remains") != std::string::npos);
}

TEST_CASE("DiagSeed: seed term precedes pcg in all three initRNG helpers")
{
    for (const char* path : { "RT2App/shaders/pathtracer_shared.glsl",
                              "RT2App/shaders/restir_bindings.glsl",
                              "RT2App/shaders/restir_gi_bindings.glsl" })
    {
        const std::string source = ReadShaderSource(path);
        REQUIRE(!source.empty());
        const size_t term = source.find("diagSampleSeed * 2654435761u");
        CHECK(term != std::string::npos);
        // The mix lands on the initial state before the warm-up pcg call
        // of that helper (unsigned add; seed 0 is exactly 0).
        const size_t init = source.find("uint initRNG(ivec2 pixel, float frameIndex)");
        CHECK(init != std::string::npos);
        CHECK(init < term);
        const size_t pcgCall = source.find("pcg(rngState);", term);
        CHECK(pcgCall != std::string::npos);
        CHECK(source.find("diagWriteProbe") != std::string::npos);
        CHECK(source.find("SI_BINDING_DIAG_SEED") != std::string::npos);
        CHECK(source.find("SI_BINDING_DIAG_PROBE") != std::string::npos);
    }
}

TEST_CASE("DiagSeed: four consumers record raw states at their call sites")
{
    // Both raygen entries share slot 0: the SBT record runs exactly one
    // per frame (RT-primary vs raster-first), so the slot always holds the
    // primary stream that actually ran.
    const std::string primaryRgen = ReadShaderSource("RT2App/shaders/raygen.rgen");
    REQUIRE(!primaryRgen.empty());
    CHECK(primaryRgen.find("diagWriteProbe(0u, pixel, rngState)") != std::string::npos);
    const std::string raygen = ReadShaderSource("RT2App/shaders/secondary_raygen.rgen");
    REQUIRE(!raygen.empty());
    CHECK(raygen.find("diagWriteProbe(0u, pixel, rngState)") != std::string::npos);
    const std::string diTemp = ReadShaderSource("RT2App/shaders/restir_temporal.comp");
    REQUIRE(!diTemp.empty());
    CHECK(diTemp.find("diagWriteProbe(1u, pixel, rngState)") != std::string::npos);
    const std::string diSpat = ReadShaderSource("RT2App/shaders/restir_spatial.comp");
    REQUIRE(!diSpat.empty());
    CHECK(diSpat.find("diagWriteProbe(2u, pixel, rngState)") != std::string::npos);
    const std::string giTemp = ReadShaderSource("RT2App/shaders/restir_gi_temporal.comp");
    REQUIRE(!giTemp.empty());
    CHECK(giTemp.find("diagWriteProbe(3u, pixel, rngState)") != std::string::npos);
    // The DI spatial domain split that the probe samples after is intact.
    CHECK(diSpat.find("camera.position.w + 7777.0") != std::string::npos);
}

TEST_CASE("SIGIReservoir is 48 bytes (3 uvec4, std430)")
{
    CHECK(sizeof(SIGIReservoir) == 48);
}

TEST_CASE("SIGIPushConstants is 48 bytes")
{
    CHECK(sizeof(SIGIPushConstants) == 48);
}

TEST_CASE("Zeroed SIGIReservoir decodes as invalid")
{
    // vkCmdFillBuffer(..., 0) must produce an invalid reservoir: valid bit = 0
    // and M = 0. This is the contract that ClearAll relies on.
    SIGIReservoir r{};
    // data2.z packs (age << 16) | flags; zeroed → flags = 0 → valid bit clear.
    uint32_t flags = r.data2.z & 0xFFFFu;
    CHECK((flags & SI_GI_FLAG_VALID) == 0u);
    CHECK(r.data2.y == 0u);  // M == 0
}

TEST_CASE("SIReSTIRPushConstants carries both jitter samples")
{
    CHECK(sizeof(SIReSTIRPushConstants) == 60);
    CHECK(offsetof(SIReSTIRPushConstants, jitter) == 44);
}

// ============================================================================
// RR5-T1 roulette plumbing contract: tunables travel host -> set-1 NRD UBO ->
// closest-hit with identical defaults; lobe probabilities stay hard-coded.
// These source checks run from the repository root (like the script gates).
// ============================================================================

TEST_CASE("Roulette: closest-hit consumes UBO tunables, no literals remain")
{
    const std::string rchit = ReadShaderSource("RT2App/shaders/closesthit.rchit");
    REQUIRE(!rchit.empty());
    CHECK(rchit.find("nrdData.rrStartDepth") != std::string::npos);
    CHECK(rchit.find("nrdData.rrMinP") != std::string::npos);
    CHECK(rchit.find("nrdData.rrMaxP") != std::string::npos);
    // Legacy hard-codes must be gone from the roulette block.
    CHECK(rchit.find("depth >= 3u") == std::string::npos);
    CHECK(rchit.find("0.05, 0.95") == std::string::npos);
    // Unbiased survivor compensation is preserved.
    CHECK(rchit.find("newThroughput /= pContinue") != std::string::npos);
}

TEST_CASE("Roulette: GLSL NRD uniform mirrors the 32-byte C++ layout")
{
    const std::string shared = ReadShaderSource("RT2App/shaders/pathtracer_shared.glsl");
    REQUIRE(!shared.empty());
    for (const char* field : { "rrStartDepth", "rrMinP", "rrMaxP", "rrReserved" })
        CHECK(shared.find(field) != std::string::npos);
}

TEST_CASE("Roulette: lobe clamp stays hard-coded (deferred to a later ticket)")
{
    const std::string scatter = ReadShaderSource("RT2App/shaders/scatter_shared.glsl");
    REQUIRE(!scatter.empty());
    CHECK(scatter.find("0.25, 0.75") != std::string::npos);
    CHECK(scatter.find("rrMinP") == std::string::npos);
    CHECK(scatter.find("rrMaxP") == std::string::npos);
    CHECK(scatter.find("specProbMin") == std::string::npos);
    CHECK(scatter.find("specProbMax") == std::string::npos);
}

TEST_CASE("Roulette: debug view keeps the NRD prefix, GI declares no nrdData")
{
    const std::string debug = ReadShaderSource("RT2App/shaders/gbuffer_debug.comp");
    REQUIRE(!debug.empty());
    CHECK(debug.find("nrdEnabledDebug") != std::string::npos);
    CHECK(debug.find("restirGIReservoirIndexDebug") != std::string::npos);
    CHECK(debug.find("rrStartDepthDebug") != std::string::npos);
    const std::string gi = ReadShaderSource("RT2App/shaders/restir_gi_bindings.glsl");
    REQUIRE(!gi.empty());
    CHECK(gi.find("nrdData") == std::string::npos);
}

TEST_CASE("GPUMaterial matches SIMaterial layout")
{
    CHECK(sizeof(GPUMaterial) == sizeof(SIMaterial));
    CHECK(offsetof(GPUMaterial, baseColor_metallic) == offsetof(SIMaterial, baseColor_metallic));
    CHECK(offsetof(GPUMaterial, textureIndices) == offsetof(SIMaterial, textureIndices));
}

TEST_CASE("GPUTriangleLight matches SITriangleLight layout")
{
    CHECK(sizeof(GPUTriangleLight) == sizeof(SITriangleLight));
}

// ============================================================================
// GPUMaterial struct layout tests (std430 / 32-byte alignment)
// ============================================================================

TEST_CASE("GPUMaterial is 80 bytes (2 vec4 + 4 float + 2 ivec4, std430)")
{
    CHECK(sizeof(GPUMaterial) == 80);
}

TEST_CASE("GPUMaterial default values are sensible")
{
    GPUMaterial mat;
    CHECK(mat.baseColor_metallic == glm::vec4(0.8f, 0.8f, 0.8f, 0.0f));
    CHECK(mat.emissive_roughness == glm::vec4(0.0f, 0.0f, 0.0f, 0.5f));
    CHECK(mat.ior == 1.5f);
    CHECK(mat.alphaCutoff == 0.5f);
    CHECK(mat.alphaMode == 0.0f); // OPAQUE
    CHECK(mat.baseAlpha == 1.0f);
    CHECK(mat.textureIndices.x == -1); // no baseColor texture
    CHECK(mat.textureIndices.y == -1); // no normal texture
    CHECK(mat.textureIndices.z == -1); // no emissive texture
}

TEST_CASE("GPUMaterial from SceneMaterial preserves texture indices")
{
    SceneMaterial sm;
    sm.baseColorTextureIndex = 3;
    sm.normalTextureIndex = 7;
    sm.emissiveTextureIndex = 12;

    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);

    CHECK(gm.textureIndices.x == 3);
    CHECK(gm.textureIndices.y == 7);
    CHECK(gm.textureIndices.z == 12);
}

TEST_CASE("GPUMaterial from SceneMaterial preserves PBR fields")
{
    SceneMaterial sm;
    sm.type = MaterialType::Lambertian;
    sm.baseColor = {0.9f, 0.2f, 0.1f};
    sm.metallic = 0.8f;
    sm.roughness = 0.3f;
    sm.ior = 1.45f;
    sm.emissiveColor = {1.0f, 0.5f, 0.2f};
    sm.emissiveIntensity = 3.0f;

    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);

    CHECK(gm.baseColor_metallic.x == 0.9f);
    CHECK(gm.baseColor_metallic.y == 0.2f);
    CHECK(gm.baseColor_metallic.z == 0.1f);
    CHECK(gm.baseColor_metallic.w == 0.8f);
    CHECK(gm.emissive_roughness.x == 1.0f * 3.0f);
    CHECK(gm.emissive_roughness.y == 0.5f * 3.0f);
    CHECK(gm.emissive_roughness.z == 0.2f * 3.0f);
    CHECK(gm.emissive_roughness.w == 0.3f);
    CHECK(gm.ior == 1.45f);
}

TEST_CASE("GPUMaterial emissive is zero when intensity is zero")
{
    SceneMaterial sm;
    sm.emissiveColor = {1.0f, 0.5f, 0.2f};
    sm.emissiveIntensity = 0.0f;

    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);

    CHECK(gm.emissive_roughness.x == 0.0f);
    CHECK(gm.emissive_roughness.y == 0.0f);
    CHECK(gm.emissive_roughness.z == 0.0f);
}

// ============================================================================
// GPUMeshGeometry tests
// ============================================================================

TEST_CASE("GPUMeshGeometry stores vertices and indices")
{
    std::vector<float> verts = {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f};
    std::vector<uint32_t> idxs = {0, 1, 2};
    GPUMeshGeometry geo;
    geo.vertices = &verts;
    geo.indices = &idxs;
    geo.materialIndex = 3;

    CHECK(geo.vertices->size() == 9);
    CHECK(geo.indices->size() == 3);
    CHECK(geo.materialIndex == 3);
}

TEST_CASE("GPUMeshGeometry default materialIndex is 0")
{
    GPUMeshGeometry geo;
    CHECK(geo.materialIndex == 0);
}

// ============================================================================
// GPUSceneData tests
// ============================================================================

TEST_CASE("GPUSceneData starts empty")
{
    GPUSceneData scene;
    CHECK(scene.meshes.empty());
    CHECK(scene.materials.empty());
}

TEST_CASE("GPUSceneData holds multiple meshes and materials")
{
    GPUSceneData scene;

    GPUMeshGeometry geo1;
    geo1.materialIndex = 0;
    scene.meshes.push_back(geo1);

    GPUMeshGeometry geo2;
    geo2.materialIndex = 1;
    scene.meshes.push_back(geo2);

    scene.materials.push_back(GPUMaterial::fromSceneMaterial(SceneMaterial{}));
    scene.materials.push_back(GPUMaterial::fromSceneMaterial(SceneMaterial{}));

    CHECK(scene.meshes.size() == 2);
    CHECK(scene.materials.size() == 2);
    CHECK(scene.meshes[0].materialIndex == 0);
    CHECK(scene.meshes[1].materialIndex == 1);
}

// ============================================================================
// BuildGPUSceneDataFromECS from ECSScene (integration of scene → GPU data)
// ============================================================================

TEST_CASE("BuildGPUSceneDataFromECS converts ECSScene meshes and materials")
{
    ECSScene ecsScene;

    SceneMaterial mat0;
    mat0.baseColor = {1.0f, 0.0f, 0.0f};
    mat0.metallic = 0.0f;
    ecsScene.materials.push_back(mat0);

    SceneMaterial mat1;
    mat1.baseColor = {0.0f, 0.0f, 1.0f};
    mat1.metallic = 1.0f;
    ecsScene.materials.push_back(mat1);

    MeshData meshData0;
    meshData0.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    meshData0.indices = {0, 1, 2};
    uint32_t meshIdx0 = ecsScene.meshRegistry.AddMesh(std::move(meshData0));

    MeshData meshData1;
    meshData1.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    meshData1.indices = {0, 1, 2};
    uint32_t meshIdx1 = ecsScene.meshRegistry.AddMesh(std::move(meshData1));

    {
        auto entity = ecsScene.registry.create();
        ecsScene.registry.emplace<Transform>(entity);
        auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
        ref.meshIndex = meshIdx0;
        ref.materialIndex = 0;
    }
    {
        auto entity = ecsScene.registry.create();
        ecsScene.registry.emplace<Transform>(entity);
        auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
        ref.meshIndex = meshIdx1;
        ref.materialIndex = 1;
    }

    SceneGraph::UpdateWorldTransforms(ecsScene.registry);

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    CHECK(gpu.meshes.size() == 2);
    CHECK(gpu.materials.size() == 2);
    CHECK(gpu.instances.size() == 2);

    bool foundMat0 = false, foundMat1 = false;
    for (const auto& inst : gpu.instances)
    {
        if (inst.materialIndex == 0) foundMat0 = true;
        if (inst.materialIndex == 1) foundMat1 = true;
    }
    CHECK(foundMat0);
    CHECK(foundMat1);

    CHECK(gpu.materials[0].baseColor_metallic.x == 1.0f);
    CHECK(gpu.materials[0].baseColor_metallic.w == 0.0f);

    CHECK(gpu.materials[1].baseColor_metallic.z == 1.0f);
    CHECK(gpu.materials[1].baseColor_metallic.w == 1.0f);
}

TEST_CASE("BuildGPUSceneDataFromECS handles mesh with invalid materialIndex")
{
    ECSScene ecsScene;
    ecsScene.materials.push_back(SceneMaterial{});

    MeshData meshData;
    meshData.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    meshData.indices = {0, 1, 2};
    uint32_t meshIdx = ecsScene.meshRegistry.AddMesh(std::move(meshData));

    auto entity = ecsScene.registry.create();
    ecsScene.registry.emplace<Transform>(entity);
    auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
    ref.meshIndex = meshIdx;
    ref.materialIndex = 99;

    SceneGraph::UpdateWorldTransforms(ecsScene.registry);

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    CHECK(gpu.instances[0].materialIndex == 0xFFFFFFFFu);
}

TEST_CASE("BuildGPUSceneDataFromECS only creates instances for entities with MeshRef")
{
    ECSScene ecsScene;
    ecsScene.materials.push_back(SceneMaterial{});

    MeshData meshData;
    meshData.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    meshData.indices = {0, 1, 2};
    uint32_t meshIdx = ecsScene.meshRegistry.AddMesh(std::move(meshData));

    {
        auto entity = ecsScene.registry.create();
        ecsScene.registry.emplace<Transform>(entity);
        auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
        ref.meshIndex = meshIdx;
        ref.materialIndex = 0;
    }
    {
        auto entity = ecsScene.registry.create();
        ecsScene.registry.emplace<Transform>(entity);
    }

    SceneGraph::UpdateWorldTransforms(ecsScene.registry);

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    CHECK(gpu.instances.size() == 1);
}

// ============================================================================
// GPUTriangleLight struct layout tests
// ============================================================================

TEST_CASE("GPUTriangleLight is 32 bytes (vec4 + uvec4, std430)")
{
    CHECK(sizeof(GPUTriangleLight) == 32);
}

TEST_CASE("GPUTriangleLight default values")
{
    GPUTriangleLight light;
    CHECK(light.emission_area == glm::vec4(0.0f));
    CHECK(light.ids == glm::uvec4(0, 0, 0, 0xFFFFFFFFu));
}

// ============================================================================
// Light list building tests (NEE Phase 1)
// ============================================================================

TEST_CASE("BuildGPUSceneDataFromECS finds no lights when no emissive materials")
{
    ECSScene ecsScene;

    SceneMaterial mat;
    mat.emissiveColor = {0.0f, 0.0f, 0.0f};
    mat.emissiveIntensity = 0.0f;
    ecsScene.materials.push_back(mat);

    MeshData meshData;
    meshData.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    meshData.indices = {0, 1, 2};
    uint32_t meshIdx = ecsScene.meshRegistry.AddMesh(std::move(meshData));

    auto entity = ecsScene.registry.create();
    ecsScene.registry.emplace<Transform>(entity);
    auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
    ref.meshIndex = meshIdx;
    ref.materialIndex = 0;

    SceneGraph::UpdateWorldTransforms(ecsScene.registry);

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    CHECK(gpu.lights.empty());
    CHECK(gpu.totalLightArea == 0.0f);
}

TEST_CASE("BuildGPUSceneDataFromECS finds one light per emissive triangle")
{
    ECSScene ecsScene;

    SceneMaterial mat;
    mat.emissiveColor = {1.0f, 0.8f, 0.4f};
    mat.emissiveIntensity = 5.0f;
    ecsScene.materials.push_back(mat);

    MeshData meshData;
    meshData.vertices = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    meshData.indices = {0, 1, 2, 0, 2, 3};
    uint32_t meshIdx = ecsScene.meshRegistry.AddMesh(std::move(meshData));

    auto entity = ecsScene.registry.create();
    ecsScene.registry.emplace<Transform>(entity);
    auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
    ref.meshIndex = meshIdx;
    ref.materialIndex = 0;

    SceneGraph::UpdateWorldTransforms(ecsScene.registry);

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    CHECK(gpu.lights.size() == 2);
    CHECK(gpu.totalLightArea == doctest::Approx(1.0f).epsilon(0.001f));
}

TEST_CASE("GPUTriangleLight stores correct ids and emission")
{
    ECSScene ecsScene;

    SceneMaterial mat;
    mat.emissiveColor = {1.0f, 0.5f, 0.2f};
    mat.emissiveIntensity = 3.0f;
    ecsScene.materials.push_back(mat);

    MeshData meshData;
    meshData.vertices = {0, 0, 0, 2, 0, 0, 0, 2, 0};
    meshData.indices = {0, 1, 2};
    uint32_t meshIdx = ecsScene.meshRegistry.AddMesh(std::move(meshData));

    auto entity = ecsScene.registry.create();
    ecsScene.registry.emplace<Transform>(entity);
    auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
    ref.meshIndex = meshIdx;
    ref.materialIndex = 0;

    SceneGraph::UpdateWorldTransforms(ecsScene.registry);

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    REQUIRE(gpu.lights.size() == 1);
    const auto& light = gpu.lights[0];

    CHECK(light.emission_area.x == doctest::Approx(3.0f));
    CHECK(light.emission_area.y == doctest::Approx(1.5f));
    CHECK(light.emission_area.z == doctest::Approx(0.6f));

    CHECK(light.emission_area.w == doctest::Approx(2.0f).epsilon(0.001f));

    CHECK(light.ids.x == 0);
    CHECK(light.ids.y == 0);
    CHECK(light.ids.z == 0);
    CHECK(light.ids.w == 0xFFFFFFFFu);
}

TEST_CASE("BuildGPUSceneDataFromECS light ids map to correct instanceID and primitiveID")
{
    ECSScene ecsScene;

    SceneMaterial mat0;
    ecsScene.materials.push_back(mat0);

    SceneMaterial mat1;
    mat1.emissiveColor = {1.0f, 1.0f, 1.0f};
    mat1.emissiveIntensity = 2.0f;
    ecsScene.materials.push_back(mat1);

    MeshData meshData0;
    meshData0.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    meshData0.indices = {0, 1, 2};
    uint32_t meshIdx0 = ecsScene.meshRegistry.AddMesh(std::move(meshData0));

    MeshData meshData1;
    meshData1.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0,
                          1, 0, 0, 1, 1, 0, 0, 1, 0,
                          0, 0, 0, 1, 0, 0, 1, 1, 0};
    meshData1.indices = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    uint32_t meshIdx1 = ecsScene.meshRegistry.AddMesh(std::move(meshData1));

    {
        auto entity = ecsScene.registry.create();
        ecsScene.registry.emplace<Transform>(entity);
        auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
        ref.meshIndex = meshIdx0;
        ref.materialIndex = 0;
    }
    {
        auto entity = ecsScene.registry.create();
        ecsScene.registry.emplace<Transform>(entity);
        auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
        ref.meshIndex = meshIdx1;
        ref.materialIndex = 1;
    }

    SceneGraph::UpdateWorldTransforms(ecsScene.registry);

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    CHECK(gpu.lights.size() == 3);
    CHECK(gpu.meshes.size() == 2);
    CHECK(gpu.instances.size() == 2);

    uint32_t emissiveInstIdx = 0;
    for (uint32_t i = 0; i < gpu.instances.size(); i++)
    {
        if (gpu.instances[i].materialIndex == 1)
        {
            emissiveInstIdx = i;
            break;
        }
    }

    for (uint32_t i = 0; i < gpu.lights.size(); i++)
    {
        CHECK(gpu.lights[i].ids.x == emissiveInstIdx);
        CHECK(gpu.lights[i].ids.z == 1);
        CHECK(gpu.lights[i].ids.y == i);
    }
}

TEST_CASE("BuildGPUSceneDataFromECS totalLightArea sums all light areas")
{
    ECSScene ecsScene;

    SceneMaterial mat;
    mat.emissiveColor = {1.0f, 1.0f, 1.0f};
    mat.emissiveIntensity = 1.0f;
    ecsScene.materials.push_back(mat);

    MeshData meshData0;
    meshData0.vertices = {0, 0, 0, 2, 0, 0, 0, 2, 0};
    meshData0.indices = {0, 1, 2};
    uint32_t meshIdx0 = ecsScene.meshRegistry.AddMesh(std::move(meshData0));

    MeshData meshData1;
    meshData1.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    meshData1.indices = {0, 1, 2};
    uint32_t meshIdx1 = ecsScene.meshRegistry.AddMesh(std::move(meshData1));

    {
        auto entity = ecsScene.registry.create();
        ecsScene.registry.emplace<Transform>(entity);
        auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
        ref.meshIndex = meshIdx0;
        ref.materialIndex = 0;
    }
    {
        auto entity = ecsScene.registry.create();
        ecsScene.registry.emplace<Transform>(entity);
        auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
        ref.meshIndex = meshIdx1;
        ref.materialIndex = 0;
    }

    SceneGraph::UpdateWorldTransforms(ecsScene.registry);

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    CHECK(gpu.lights.size() == 2);
    CHECK(gpu.totalLightArea == doctest::Approx(2.5f).epsilon(0.001f));
}

TEST_CASE("BuildGPUSceneDataFromECS light stores emissiveTexIdx when material has emissive texture")
{
    ECSScene ecsScene;

    SceneMaterial mat;
    mat.emissiveColor = {1.0f, 1.0f, 1.0f};
    mat.emissiveIntensity = 2.0f;
    mat.emissiveTextureIndex = 5;
    ecsScene.materials.push_back(mat);

    MeshData meshData;
    meshData.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    meshData.indices = {0, 1, 2};
    uint32_t meshIdx = ecsScene.meshRegistry.AddMesh(std::move(meshData));

    auto entity = ecsScene.registry.create();
    ecsScene.registry.emplace<Transform>(entity);
    auto& ref = ecsScene.registry.emplace<MeshRef>(entity);
    ref.meshIndex = meshIdx;
    ref.materialIndex = 0;

    SceneGraph::UpdateWorldTransforms(ecsScene.registry);

    GPUSceneData gpu = BuildGPUSceneDataFromECS(ecsScene);

    REQUIRE(gpu.lights.size() == 1);
    CHECK(gpu.lights[0].ids.w == 5u);
}

// ============================================================================
// Alpha mode / translucency tests (M6 Phase 1)
// ============================================================================

TEST_CASE("GPUMaterial alphaMode defaults to OPAQUE (0.0)")
{
    GPUMaterial mat;
    CHECK(mat.alphaMode == 0.0f);
    CHECK(mat.alphaCutoff == 0.5f);
}

TEST_CASE("GPUMaterial fromSceneMaterial converts alphaMode MASK")
{
    SceneMaterial sm;
    sm.alphaMode = "MASK";
    sm.alphaCutoff = 0.3f;

    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);

    CHECK(gm.alphaMode == 1.0f);
    CHECK(gm.alphaCutoff == 0.3f);
}

TEST_CASE("GPUMaterial fromSceneMaterial converts alphaMode BLEND")
{
    SceneMaterial sm;
    sm.alphaMode = "BLEND";

    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);

    CHECK(gm.alphaMode == 2.0f);
}

TEST_CASE("GPUMaterial fromSceneMaterial converts alphaMode OPAQUE")
{
    SceneMaterial sm;
    sm.alphaMode = "OPAQUE";

    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);

    CHECK(gm.alphaMode == 0.0f);
}

TEST_CASE("GPUMaterial is 80 bytes with extraIndices for metallicRoughness")
{
    CHECK(sizeof(GPUMaterial) == 80);
}

// ============================================================================
// M7: Dielectric transmission — KHR_materials_transmission import tests
// ============================================================================

TEST_CASE("Transmission material stores transmissionFactor (not in baseAlpha)")
{
    // M7 fix: transmissionFactor is separate from baseAlpha (opacity).
    SceneMaterial sm;
    sm.alphaMode = "OPAQUE";
    sm.transmissionFactor = 0.85f;
    sm.baseAlpha = 1.0f;  // fully opaque (no alpha transparency)
    sm.ior = 1.5f;

    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);

    CHECK(gm.alphaMode == 0.0f);       // OPAQUE
    CHECK(gm.baseAlpha == 1.0f);      // opacity unchanged
    CHECK(gm.ior == 1.5f);
    // transmissionFactor packed into textureIndices.w
    CHECK(glm::intBitsToFloat(gm.textureIndices.w) == 0.85f);
}

TEST_CASE("Transmission material with transmissionFactor=1 is fully transmissive")
{
    SceneMaterial sm;
    sm.transmissionFactor = 1.0f;  // 100% transmission
    sm.baseAlpha = 1.0f;           // still opaque for alpha purposes
    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);
    CHECK(glm::intBitsToFloat(gm.textureIndices.w) == 1.0f);
    CHECK(gm.baseAlpha == 1.0f);  // opacity unchanged
    CHECK(gm.alphaMode == 0.0f);  // OPAQUE
}

TEST_CASE("Opaque material with transmissionFactor=0 is not transmissive")
{
    SceneMaterial sm;
    sm.transmissionFactor = 0.0f;
    sm.baseAlpha = 1.0f;
    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);
    CHECK(glm::intBitsToFloat(gm.textureIndices.w) == 0.0f);
    CHECK(gm.baseAlpha == 1.0f);
    CHECK(gm.alphaMode == 0.0f);
}

TEST_CASE("Alpha-blended material does NOT trigger transmission")
{
    // A BLEND alpha material (e.g. bottle) should NOT enter the refraction branch.
    // baseAlpha < 1 here means partial opacity, NOT physical transmission.
    SceneMaterial sm;
    sm.alphaMode = "BLEND";
    sm.baseAlpha = 0.5f;        // 50% transparent (alpha opacity)
    sm.transmissionFactor = 0.0f;  // NOT physical glass
    GPUMaterial gm = GPUMaterial::fromSceneMaterial(sm);
    CHECK(gm.alphaMode == 2.0f);  // BLEND
    CHECK(gm.baseAlpha == 0.5f); // opacity
    CHECK(glm::intBitsToFloat(gm.textureIndices.w) == 0.0f);  // no transmission
}

// ============================================================================
// Punctual lights must follow the transform-only sync path.
//
// A light is an entity with a Transform and no MeshRef, so it never appears in
// the renderable loop that UpdateInstancesFromECS iterates. Its position and
// aim come straight from the world matrix -- exactly what a transform edit
// changes -- so if this path does not rebuild them, dragging a light in the
// editor leaves it casting from where it used to be until some unrelated full
// sync happens to run.
// ============================================================================
TEST_CASE("UpdateInstancesFromECS moves punctual lights with their transform")
{
    ECSScene scene;
    scene.materials.push_back(SceneMaterial{});

    // One renderable, so there are instances to update at all.
    {
        MeshData mesh;
        mesh.vertices = {0,0,0, 1,0,0, 0,1,0};
        mesh.indices = {0, 1, 2};
        const uint32_t meshIdx = scene.meshRegistry.AddMesh(std::move(mesh));
        const auto e = scene.registry.create();
        scene.registry.emplace<Transform>(e);
        scene.registry.emplace<MeshRef>(e, meshIdx, 0);
        scene.registry.emplace<VisibleComponent>(e);
    }

    const auto lightEntity = scene.registry.create();
    {
        Transform tf;
        tf.translation = {1.0f, 2.0f, 3.0f};
        scene.registry.emplace<Transform>(lightEntity, tf);
        LightComponent light;
        light.type = LightType::Point;
        light.intensity = 50.0f;
        scene.registry.emplace<LightComponent>(lightEntity, light);
        scene.registry.emplace<VisibleComponent>(lightEntity);
    }

    SceneGraph::UpdateWorldTransforms(scene.registry);
    GPUSceneData gpu = BuildGPUSceneDataFromECS(scene, nullptr);
    REQUIRE(gpu.punctualLights.size() == 1);
    CHECK(gpu.punctualLights[0].position_range.x == doctest::Approx(1.0f));
    CHECK(gpu.punctualLights[0].position_range.z == doctest::Approx(3.0f));

    // Move the light, then run only the transform-sync path.
    scene.registry.get<Transform>(lightEntity).translation = {-4.0f, 5.0f, 6.0f};
    SceneGraph::MarkDirty(scene.registry, lightEntity);
    SceneGraph::UpdateWorldTransforms(scene.registry);
    UpdateInstancesFromECS(gpu, scene, nullptr);

    REQUIRE(gpu.punctualLights.size() == 1);
    CHECK(gpu.punctualLights[0].position_range.x == doctest::Approx(-4.0f));
    CHECK(gpu.punctualLights[0].position_range.y == doctest::Approx(5.0f));
    CHECK(gpu.punctualLights[0].position_range.z == doctest::Approx(6.0f));
}
