// NormalMatrixOracle.cpp — RT4-T2 Part A GPU diagnostic oracle driver.
//
// Staged verification, all through the production upload path with an
// isolated compute dispatch comparing, per instance, the legacy in-shader
// expression transpose(inverse(mat3(world))) against the ACTUAL production
// uploaded 48-byte normal record, plus the 14 specified sample directions
// (6 unit axes + 8 normalized cube corners) transformed by each.
// Host-side double-precision gates: matrix elements rel+abs 1e-5, sample
// normals atan2(|cross|,dot) angular 1e-4 rad. Out-of-support instances are
// verified against the identity+diagnostic contract ONLY (never parity).
//
// Stages: loaded scene as found (all real test-scene transforms, when a
// scene is loaded) → synthetic 16-matrix battery → transform-only sync seam
// → reload seam → resize seam → empty seam (dummy backing). A CPU double
// inverse is never substituted: both compared quantities come from the GPU.
//
// RT2App-only (Vulkan); never linked into RT2Tests/RT2SliceRunner.
// Headless entry: --normal-matrix-oracle (see WalnutApp RunHeadless).
#include "RendererGPU.h"
#include "NormalMatrix.h"
#include "PrimitiveGeometry.h"
#include "MeshRegistry.h"
#include "CommandUtils.h"
#include "ShaderManager.h"
#include "RTLog.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
constexpr uint32_t kOracleDirs = 14;
constexpr uint32_t kOracleVecsPerInstance = 34; // 3 legacy + 3 uploaded + 14 + 14

// Synthetic battery: supported variety first, then the three rejected forms.
// Degenerate transforms (13-15) are AS-build safe (plain instance matrices,
// never traced); NaN/Inf inputs stay CPU-only (undefined for AS builds).
std::vector<glm::mat4> OracleBattery()
{
    std::vector<glm::mat4> ms;
    ms.reserve(16);
    ms.push_back(glm::mat4(1.0f)); // 0 identity
    { glm::mat4 m(1.0f); m[0][0] = 2.5f; m[1][1] = 2.5f; m[2][2] = 2.5f;
      m[3] = glm::vec4(7.0f, -3.0f, 2.0f, 1.0f); ms.push_back(m); } // 1 uniform+translation
    { glm::mat4 m(1.0f); m[0][0] = 2.0f; m[1][1] = 0.5f; m[2][2] = 4.0f;
      ms.push_back(m); } // 2 non-uniform
    { const float c = std::cos(0.7f), s = std::sin(0.7f); glm::mat4 m(1.0f);
      m[0][0] = c; m[0][2] = -s; m[2][0] = s; m[2][2] = c; ms.push_back(m); } // 3 rotY
    { const float c = std::cos(1.1f), s = std::sin(1.1f); glm::mat4 m(1.0f);
      m[1][1] = c; m[1][2] = s; m[2][1] = -s; m[2][2] = c;
      m[3] = glm::vec4(-4.0f, 1.0f, 9.0f, 1.0f); ms.push_back(m); } // 4 rotX+translation
    { glm::mat4 m(1.0f); m[0][0] = -1.0f; ms.push_back(m); } // 5 mirror
    { glm::mat4 m(1.0f); m[0][0] = -2.0f; m[1][1] = 0.5f; m[2][2] = 3.0f;
      ms.push_back(m); } // 6 mirror + non-uniform
    { glm::mat4 m(1.0f); m[1][0] = 0.5f; ms.push_back(m); } // 7 shear
    { glm::mat4 m(1.0f); m[0][0] = 1.5f; m[2][2] = 2.0f;
      m[2][0] = -0.3f; m[0][2] = 0.25f; ms.push_back(m); } // 8 shear + scale
    { glm::mat4 m(1.0f); m[0][0] = 1e-6f; m[1][1] = 1e-6f; m[2][2] = 1e-6f;
      ms.push_back(m); } // 9 tiny uniform
    { glm::mat4 m(1.0f); m[0][0] = 1e6f; m[1][1] = 1e6f; m[2][2] = 1e6f;
      ms.push_back(m); } // 10 huge uniform
    { const float c = std::cos(0.4f), s = std::sin(0.4f); glm::mat4 m(1.0f);
      m[0][0] = 0.3f * c; m[0][1] = 0.3f * s; m[1][0] = -2.0f * s; m[1][1] = 2.0f * c;
      ms.push_back(m); } // 11 rotZ + non-uniform
    { glm::mat4 m(1.0f); m[2][2] = 1e-10f; ms.push_back(m); } // 12 boundary supported
    { glm::mat4 m(1.0f); m[2][2] = 1e-13f; ms.push_back(m); } // 13 boundary reject
    { glm::mat4 m(1.0f); m[2][2] = 0.0f; ms.push_back(m); } // 14 singular reject
    ms.push_back(glm::mat4(0.0f)); // 15 zero reject
    return ms;
}

// SceneResources::SetScene takes its argument by move (m_CurrentScene =
// std::move(sceneData)), so a staged driver MUST build a fresh GPUSceneData
// for every SetScene call — reusing one across stages silently verifies an
// emptied husk (observed: zero-dispatch FAIL + downstream crash).
struct SyntheticScene
{
    MeshData cube;
    GPUSceneData data;
    std::vector<bool> support;
    uint32_t rejectCount = 0;
};

SyntheticScene BuildSyntheticScene(const std::vector<glm::mat4>& battery)
{
    SyntheticScene out;
    out.cube = PrimitiveGeometry::CreateCube(1.0f);
    GPUMeshGeometry geo;
    geo.vertices = &out.cube.vertices;
    geo.indices = &out.cube.indices;
    geo.normals = out.cube.normals.empty() ? nullptr : &out.cube.normals;
    geo.uvs = nullptr;
    out.data.meshes.push_back(geo);
    out.data.materials.emplace_back();
    for (uint32_t i = 0; i < (uint32_t)battery.size(); ++i)
    {
        GPUInstance inst;
        inst.meshIndex = 0;
        inst.materialIndex = 0;
        inst.worldMatrix = battery[i];
        inst.prevWorldMatrix = battery[i];
        inst.isTransparent = false;
        out.data.instances.push_back(inst);
        const bool s = ComputeNormalMatrixPack(battery[i]).supported;
        out.support.push_back(s);
        if (!s) out.rejectCount++;
    }
    return out;
}

// One verification stage over the CURRENT production buffers: isolated
// dispatch + readback + double-precision gates. Empty matrix list verifies
// the dummy backing directly (zero workgroups are illegal to dispatch).
bool VerifyStage(const GpuDevice& dev, const char* tag, VkPipeline pipeline,
                 VkPipelineLayout pipelineLayout, VkDescriptorSetLayout sharedLayout,
                 VkBuffer transformBuf, VkBuffer normalMatrixBuf, VkDeviceMemory normalMatrixMem,
                 const std::vector<glm::mat4>& matrices,
                 const std::vector<bool>& support,
                 uint32_t expectReject, uint32_t uploadedReject)
{
    const uint32_t instanceCount = (uint32_t)matrices.size();
    bool ok = true;
    auto fail = [&](const char* what) {
        printf("[NormalMatrixOracle][%s] FAIL: %s\n", tag, what);
        fflush(stdout);
        ok = false;
    };

    if (transformBuf == VK_NULL_HANDLE || normalMatrixBuf == VK_NULL_HANDLE)
    { fail("production buffers missing"); return false; }

    if (instanceCount == 0)
    {
        void* dummy = nullptr;
        if (vkMapMemory(dev.device, normalMatrixMem, 0, 48, 0, &dummy) != VK_SUCCESS)
        { fail("dummy map"); return false; }
        const float* df = static_cast<const float*>(dummy);
        for (int e = 0; e < 12; ++e)
        {
            if (df[e] != 0.0f)
            {
                printf("[NormalMatrixOracle][%s] FAIL: dummy float %d = %g, want 0\n",
                       tag, e, (double)df[e]);
                fflush(stdout);
                ok = false;
            }
        }
        vkUnmapMemory(dev.device, normalMatrixMem);
        if (ok && uploadedReject != 0)
        {
            printf("[NormalMatrixOracle][%s] FAIL: diagnostic count %u on empty scene\n",
                   tag, uploadedReject);
            fflush(stdout);
            ok = false;
        }
        printf("[NormalMatrixOracle][%s] %s: 0 instances, dummy backing verified\n",
               tag, ok ? "PASS" : "FAIL");
        fflush(stdout);
        return ok;
    }

    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkBuffer resultBuf = VK_NULL_HANDLE;
    VkDeviceMemory resultMem = VK_NULL_HANDLE;
    const VkDeviceSize resultSize = (VkDeviceSize)instanceCount * kOracleVecsPerInstance * sizeof(glm::vec4);

    if (!GpuResources::CreateBuffer(dev, resultSize,
             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
             resultBuf, resultMem))
    { fail("result buffer"); }

    if (ok)
    {
        VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 };
        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        if (vkCreateDescriptorPool(dev.device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
        { fail("descriptor pool"); }
    }
    if (ok)
    {
        VkDescriptorSetAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &sharedLayout;
        if (vkAllocateDescriptorSets(dev.device, &ai, &set) != VK_SUCCESS)
        { fail("descriptor set"); }
    }
    if (ok)
    {
        VkDescriptorBufferInfo infos[3] = {
            { transformBuf, 0, VK_WHOLE_SIZE },
            { normalMatrixBuf, 0, VK_WHOLE_SIZE },
            { resultBuf, 0, VK_WHOLE_SIZE },
        };
        VkWriteDescriptorSet writes[3] = {};
        for (int i = 0; i < 3; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = (uint32_t)i;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].descriptorCount = 1;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(dev.device, 3, writes, 0, nullptr);

        CommandUtils::ImmediateSubmit(dev, [&](VkCommandBuffer cmd) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            // The stage-local set is allocated from the shared oracle
            // layout, so it is exactly compatible with the pipeline layout.
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
                                    0, 1, &set, 0, nullptr);
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(uint32_t), &instanceCount);
            vkCmdDispatch(cmd, (instanceCount + 63) / 64, 1, 1);
            VkBufferMemoryBarrier barrier = {};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            barrier.buffer = resultBuf;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT,
                                 0, 0, nullptr, 1, &barrier, 0, nullptr);
        });
    }

    double worstMatrixErr = 0.0;
    double worstAngle = 0.0;
    if (ok)
    {
        void* mapped = nullptr;
        if (vkMapMemory(dev.device, resultMem, 0, resultSize, 0, &mapped) != VK_SUCCESS)
        { fail("result map"); }
        else
        {
            const float* f = static_cast<const float*>(mapped);
            // 34 vec4s per instance, 4 floats each.
            for (uint32_t i = 0; i < instanceCount && ok; ++i)
            {
                const float* base = f + (size_t)i * kOracleVecsPerInstance * 4;
                double legacyM[9], upM[9];
                for (int e = 0; e < 9; ++e)
                {
                    const int col = e / 3, row = e % 3;
                    legacyM[e] = (double)base[(col * 4) + row];
                    upM[e] = (double)base[(12 + col * 4) + row];
                }
                if (support[i])
                {
                    for (int e = 0; e < 9; ++e)
                    {
                        const double a = legacyM[e], b = upM[e];
                        const double tol = 1e-5 + 1e-5 * (std::fabs(a) > std::fabs(b) ? std::fabs(a) : std::fabs(b));
                        const double err = std::fabs(a - b);
                        if (err > worstMatrixErr) worstMatrixErr = err;
                        if (err > tol)
                        {
                            printf("[NormalMatrixOracle][%s] FAIL: instance %u matrix elem %d |%g-%g|=%g > tol %g\n",
                                   tag, i, e, a, b, err, tol);
                            fflush(stdout);
                            ok = false;
                        }
                    }
                    for (uint32_t k = 0; k < kOracleDirs && ok; ++k)
                    {
                        double ax = base[(24 + k * 4) + 0], ay = base[(24 + k * 4) + 1], az = base[(24 + k * 4) + 2];
                        double bx = base[(80 + k * 4) + 0], by = base[(80 + k * 4) + 1], bz = base[(80 + k * 4) + 2];
                        const double al = std::sqrt(ax * ax + ay * ay + az * az);
                        const double bl = std::sqrt(bx * bx + by * by + bz * bz);
                        if (!(al > 0.0) || !(bl > 0.0))
                        {
                            printf("[NormalMatrixOracle][%s] FAIL: instance %u dir %u degenerate oracle vector\n",
                                   tag, i, k);
                            fflush(stdout);
                            ok = false;
                            break;
                        }
                        ax /= al; ay /= al; az /= al;
                        bx /= bl; by /= bl; bz /= bl;
                        const double cx = ay * bz - az * by;
                        const double cy = az * bx - ax * bz;
                        const double cz = ax * by - ay * bx;
                        const double angle = std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz),
                                                        ax * bx + ay * by + az * bz);
                        if (angle > worstAngle) worstAngle = angle;
                        if (!(angle <= 1e-4))
                        {
                            printf("[NormalMatrixOracle][%s] FAIL: instance %u dir %u angle %.9f rad > 1e-4\n",
                                   tag, i, k, angle);
                            fflush(stdout);
                            ok = false;
                        }
                    }
                }
                else
                {
                    // Identity+diagnostic contract only, exact float compare.
                    static const float ident[9] = { 1,0,0, 0,1,0, 0,0,1 };
                    for (int e = 0; e < 9; ++e)
                    {
                        const int col = e / 3, row = e % 3;
                        const float got = base[(12 + col * 4) + row];
                        if (got != ident[e])
                        {
                            printf("[NormalMatrixOracle][%s] FAIL: instance %u out-of-support uploaded != identity (elem %d = %g)\n",
                                   tag, i, e, (double)got);
                            fflush(stdout);
                            ok = false;
                        }
                    }
                }
            }
            vkUnmapMemory(dev.device, resultMem);
        }
    }
    if (ok && uploadedReject != expectReject)
    {
        printf("[NormalMatrixOracle][%s] FAIL: diagnostic count %u != expected %u\n",
               tag, uploadedReject, expectReject);
        fflush(stdout);
        ok = false;
    }

    printf("[NormalMatrixOracle][%s] %s: %u instances (%u supported), worst matrix err %.3g (tol rel+abs 1e-5), worst angle %.3g rad (tol 1e-4)\n",
           tag, ok ? "PASS" : "FAIL", instanceCount, instanceCount - expectReject, worstMatrixErr, worstAngle);
    fflush(stdout);

    if (pool) vkDestroyDescriptorPool(dev.device, pool, nullptr);
    GpuResources::DestroyBuffer(dev, resultBuf, resultMem);
    return ok;
}
} // namespace

bool RendererGPU::RunNormalMatrixOracle(const Camera& camera)
{
    printf("[NormalMatrixOracle] start: staged verification via production upload path\n");
    fflush(stdout);
    bool allOk = true;

    // ---- Shared oracle pipeline (one layout + module for all stages) -------
    VkShaderModule oracleShader = ShaderManager::LoadShader("normal_matrix_oracle.spv");
    if (!oracleShader)
        oracleShader = ShaderManager::LoadShader("RT2App/shaders/normal_matrix_oracle.spv");
    if (!oracleShader)
    {
        printf("[NormalMatrixOracle] FAIL: normal_matrix_oracle.spv not found\n");
        fflush(stdout);
        return false;
    }
    VkDescriptorSetLayout sharedLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    {
        VkDescriptorSetLayoutBinding bindings[3] = {};
        for (int i = 0; i < 3; ++i)
        {
            bindings[i].binding = (uint32_t)i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li = {};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 3;
        li.pBindings = bindings;
        VkPushConstantRange push = {};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.offset = 0;
        push.size = sizeof(uint32_t);
        VkPipelineLayoutCreateInfo pli = {};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &sharedLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &push;
        bool ok = true;
        if (vkCreateDescriptorSetLayout(m_Device.device, &li, nullptr, &sharedLayout) != VK_SUCCESS)
            ok = false;
        if (ok)
        {
            pli.pSetLayouts = &sharedLayout;
            if (vkCreatePipelineLayout(m_Device.device, &pli, nullptr, &pipelineLayout) != VK_SUCCESS)
                ok = false;
        }
        if (ok)
        {
            VkComputePipelineCreateInfo pi = {};
            pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            pi.stage.module = oracleShader;
            pi.stage.pName = "main";
            pi.layout = pipelineLayout;
            if (vkCreateComputePipelines(m_Device.device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline) != VK_SUCCESS)
                ok = false;
        }
        if (!ok)
        {
            printf("[NormalMatrixOracle] FAIL: oracle pipeline setup\n");
            fflush(stdout);
            if (pipeline) vkDestroyPipeline(m_Device.device, pipeline, nullptr);
            if (pipelineLayout) vkDestroyPipelineLayout(m_Device.device, pipelineLayout, nullptr);
            if (sharedLayout) vkDestroyDescriptorSetLayout(m_Device.device, sharedLayout, nullptr);
            if (oracleShader) vkDestroyShaderModule(m_Device.device, oracleShader, nullptr);
            return false;
        }
    }

    auto currentBuffers = [&]() {
        return std::make_tuple(m_Scene.GetInstanceTransformBuffer(),
                               m_Scene.GetInstanceNormalMatrixBuffer(),
                               m_Scene.GetInstanceNormalMatrixBufferMemory());
    };
    auto classify = [](const std::vector<glm::mat4>& ms, std::vector<bool>& support, uint32_t& reject) {
        support.clear();
        reject = 0;
        for (const auto& m : ms)
        {
            const bool s = ComputeNormalMatrixPack(m).supported;
            support.push_back(s);
            if (!s) reject++;
        }
    };

    // ---- Stage 0: loaded scene as found (all real test-scene transforms) --
    {
        std::vector<glm::mat4> matrices;
        for (const auto& inst : m_Scene.GetScene().instances)
            matrices.push_back(inst.worldMatrix);
        if (!matrices.empty())
        {
            std::vector<bool> support;
            uint32_t expectReject = 0;
            classify(matrices, support, expectReject);
            auto [tBuf, nBuf, nMem] = currentBuffers();
            allOk &= VerifyStage(m_Device, "loaded-scene", pipeline, pipelineLayout, sharedLayout,
                                 tBuf, nBuf, nMem, matrices, support, expectReject,
                                 m_Scene.GetNormalMatrixOutOfSupportCount());
        }
        else
        {
            printf("[NormalMatrixOracle][loaded-scene] SKIP: no scene loaded (synthetic covers)\n");
            fflush(stdout);
        }
    }

    // ---- Stage 1: synthetic battery ----------------------------------------
    // Fresh scene object per SetScene: SetScene moves its argument.
    const std::vector<glm::mat4> battery = OracleBattery();
    {
        SyntheticScene synth = BuildSyntheticScene(battery);
        SetScene(synth.data, {});
        // A throwaway render drives the pending AS rebuild + descriptor writes
        // through the exact production flow (also pushes the new binding-19
        // write through validation layers on real dispatch state).
        { RenderOutcome warmup = Render(camera); (void)warmup; }
        auto [tBuf, nBuf, nMem] = currentBuffers();
        allOk &= VerifyStage(m_Device, "synthetic", pipeline, pipelineLayout, sharedLayout,
                             tBuf, nBuf, nMem, battery, synth.support, synth.rejectCount,
                             m_Scene.GetNormalMatrixOutOfSupportCount());
    }

    // ---- Stage 2: transform-only sync seam ----------------------------------
    {
        SyntheticScene synth = BuildSyntheticScene(battery);
        UpdateSceneInstances(synth.data, {});
        auto [tBuf, nBuf, nMem] = currentBuffers();
        allOk &= VerifyStage(m_Device, "transform-only", pipeline, pipelineLayout, sharedLayout,
                             tBuf, nBuf, nMem, battery, synth.support, synth.rejectCount,
                             m_Scene.GetNormalMatrixOutOfSupportCount());
    }

    // ---- Stage 3: reload seam ------------------------------------------------
    {
        SyntheticScene synth = BuildSyntheticScene(battery);
        SetScene(synth.data, {});
        { RenderOutcome warmup = Render(camera); (void)warmup; }
        auto [tBuf, nBuf, nMem] = currentBuffers();
        allOk &= VerifyStage(m_Device, "reload", pipeline, pipelineLayout, sharedLayout,
                             tBuf, nBuf, nMem, battery, synth.support, synth.rejectCount,
                             m_Scene.GetNormalMatrixOutOfSupportCount());
    }

    // ---- Stage 4: resize seam (instance buffers must survive realloc) -------
    OnResize(960, 540);
    {
        // Scene still holds the stage-3 reload state; only the support
        // classification is needed (buffers re-fetched fresh).
        SyntheticScene ref = BuildSyntheticScene(battery);
        auto [tBuf, nBuf, nMem] = currentBuffers();
        allOk &= VerifyStage(m_Device, "resize", pipeline, pipelineLayout, sharedLayout,
                             tBuf, nBuf, nMem, battery, ref.support, ref.rejectCount,
                             m_Scene.GetNormalMatrixOutOfSupportCount());
    }

    // ---- Stage 5: empty seam (dummy backing, logical count zero) ------------
    {
        // One mesh but zero instances: exercises the dummy-record backing
        // without depending on Render() tolerating a fully meshless scene.
        SyntheticScene full = BuildSyntheticScene(battery);
        GPUSceneData emptyScene;
        emptyScene.meshes = full.data.meshes;
        emptyScene.materials = full.data.materials;
        SetScene(emptyScene, {});
        { RenderOutcome warmup = Render(camera); (void)warmup; }
        std::vector<glm::mat4> none;
        std::vector<bool> noSupport;
        auto [tBuf, nBuf, nMem] = currentBuffers();
        allOk &= VerifyStage(m_Device, "empty", pipeline, pipelineLayout, sharedLayout,
                             tBuf, nBuf, nMem, none, noSupport, 0,
                             m_Scene.GetNormalMatrixOutOfSupportCount());
    }

    if (pipeline) vkDestroyPipeline(m_Device.device, pipeline, nullptr);
    if (pipelineLayout) vkDestroyPipelineLayout(m_Device.device, pipelineLayout, nullptr);
    if (sharedLayout) vkDestroyDescriptorSetLayout(m_Device.device, sharedLayout, nullptr);
    if (oracleShader) vkDestroyShaderModule(m_Device.device, oracleShader, nullptr);

    printf("[NormalMatrixOracle] %s: all stages complete\n", allOk ? "PASS" : "FAIL");
    fflush(stdout);
    return allOk;
}
