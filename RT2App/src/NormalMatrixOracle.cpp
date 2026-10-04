// NormalMatrixOracle.cpp — RT4-T2 Part A GPU diagnostic oracle driver.
//
// Isolated compute dispatch comparing, per synthetic instance, the legacy
// in-shader expression transpose(inverse(mat3(world))) against the ACTUAL
// production uploaded 48-byte normal record, plus the 14 specified sample
// directions transformed by each (6 unit axes + 8 normalized cube corners).
// Host-side double-precision gates: matrix elements rel+abs 1e-5, sample
// normals atan2(|cross|,dot) angular 1e-4 rad. Out-of-support instances are
// verified against the identity+diagnostic contract ONLY (never parity).
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
} // namespace

bool RendererGPU::RunNormalMatrixOracle(const Camera& camera)
{
    printf("[NormalMatrixOracle] start: synthetic battery via production upload path\n");
    fflush(stdout);

    // ---- Synthetic scene: one cube mesh, battery instances, one material --
    MeshData cube = PrimitiveGeometry::CreateCube(1.0f);
    GPUSceneData synth;
    GPUMeshGeometry geo;
    geo.vertices = &cube.vertices;
    geo.indices = &cube.indices;
    geo.normals = cube.normals.empty() ? nullptr : &cube.normals;
    geo.uvs = nullptr;
    synth.meshes.push_back(geo);
    synth.materials.emplace_back();

    const std::vector<glm::mat4> battery = OracleBattery();
    const uint32_t instanceCount = (uint32_t)battery.size();
    std::vector<bool> expectSupported;
    expectSupported.reserve(instanceCount);
    uint32_t expectReject = 0;
    for (uint32_t i = 0; i < instanceCount; ++i)
    {
        GPUInstance inst;
        inst.meshIndex = 0;
        inst.materialIndex = 0;
        inst.worldMatrix = battery[i];
        inst.prevWorldMatrix = battery[i];
        inst.isTransparent = false;
        synth.instances.push_back(inst);
        const bool s = ComputeNormalMatrixPack(battery[i]).supported;
        expectSupported.push_back(s);
        if (!s) expectReject++;
    }

    // ---- Production upload path (same as every scene) ----------------------
    SetScene(synth, {});
    // A throwaway render drives the pending AS rebuild + descriptor writes
    // through the exact production flow (also pushes the new binding-19
    // write through validation layers on real dispatch state).
    RenderOutcome warmup = Render(camera);
    (void)warmup;

    const VkBuffer transformBuf = m_Scene.GetInstanceTransformBuffer();
    const VkBuffer normalMatrixBuf = m_Scene.GetInstanceNormalMatrixBuffer();
    if (transformBuf == VK_NULL_HANDLE || normalMatrixBuf == VK_NULL_HANDLE)
    {
        printf("[NormalMatrixOracle] FAIL: production buffers missing after sync\n");
        fflush(stdout);
        return false;
    }
    const uint32_t uploadedReject = m_Scene.GetNormalMatrixOutOfSupportCount();
    printf("[NormalMatrixOracle] upload: %u instances, out-of-support=%u (expected %u)\n",
           instanceCount, uploadedReject, expectReject);
    fflush(stdout);

    // ---- Isolated compute dispatch ----------------------------------------
    VkShaderModule oracleShader = ShaderManager::LoadShader("normal_matrix_oracle.spv");
    if (!oracleShader)
        oracleShader = ShaderManager::LoadShader("RT2App/shaders/normal_matrix_oracle.spv");
    if (!oracleShader)
    {
        printf("[NormalMatrixOracle] FAIL: normal_matrix_oracle.spv not found\n");
        fflush(stdout);
        return false;
    }

    bool ok = true;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkBuffer resultBuf = VK_NULL_HANDLE;
    VkDeviceMemory resultMem = VK_NULL_HANDLE;
    const VkDeviceSize resultSize = (VkDeviceSize)instanceCount * kOracleVecsPerInstance * sizeof(glm::vec4);

    auto fail = [&](const char* what) {
        printf("[NormalMatrixOracle] FAIL: %s\n", what);
        fflush(stdout);
        ok = false;
    };

    // Layout: bindings 0/1/2 storage buffers + 4-byte push constant.
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
        if (vkCreateDescriptorSetLayout(m_Device.device, &li, nullptr, &setLayout) != VK_SUCCESS)
        { fail("set layout"); }

        VkPushConstantRange push = {};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.offset = 0;
        push.size = sizeof(uint32_t);
        VkPipelineLayoutCreateInfo pli = {};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &setLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &push;
        if (ok && vkCreatePipelineLayout(m_Device.device, &pli, nullptr, &pipelineLayout) != VK_SUCCESS)
        { fail("pipeline layout"); }

        VkComputePipelineCreateInfo pi = {};
        pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pi.stage.module = oracleShader;
        pi.stage.pName = "main";
        pi.layout = pipelineLayout;
        if (ok && vkCreateComputePipelines(m_Device.device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline) != VK_SUCCESS)
        { fail("compute pipeline"); }
    }

    // Result buffer (host-visible, like every other scene buffer).
    if (ok && !GpuResources::CreateBuffer(m_Device, resultSize,
             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
             resultBuf, resultMem))
    { fail("result buffer"); }

    // Pool + set + writes.
    if (ok)
    {
        VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 };
        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        if (vkCreateDescriptorPool(m_Device.device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
        { fail("descriptor pool"); }
    }
    if (ok)
    {
        VkDescriptorSetAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &setLayout;
        if (vkAllocateDescriptorSets(m_Device.device, &ai, &set) != VK_SUCCESS)
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
        vkUpdateDescriptorSets(m_Device.device, 3, writes, 0, nullptr);

        CommandUtils::ImmediateSubmit(m_Device, [&](VkCommandBuffer cmd) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
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

    // ---- Host-side double-precision verification ---------------------------
    double worstMatrixErr = 0.0;
    double worstAngle = 0.0;
    if (ok)
    {
        void* mapped = nullptr;
        if (vkMapMemory(m_Device.device, resultMem, 0, resultSize, 0, &mapped) != VK_SUCCESS)
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
                if (expectSupported[i])
                {
                    for (int e = 0; e < 9; ++e)
                    {
                        const double a = legacyM[e], b = upM[e];
                        const double tol = 1e-5 + 1e-5 * (std::fabs(a) > std::fabs(b) ? std::fabs(a) : std::fabs(b));
                        const double err = std::fabs(a - b);
                        if (err > worstMatrixErr) worstMatrixErr = err;
                        if (err > tol)
                        {
                            printf("[NormalMatrixOracle] FAIL: instance %u matrix elem %d |%g-%g|=%g > tol %g\n",
                                   i, e, a, b, err, tol);
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
                            printf("[NormalMatrixOracle] FAIL: instance %u dir %u degenerate oracle vector\n", i, k);
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
                            printf("[NormalMatrixOracle] FAIL: instance %u dir %u angle %.9f rad > 1e-4\n",
                                   i, k, angle);
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
                            printf("[NormalMatrixOracle] FAIL: instance %u out-of-support uploaded != identity (elem %d = %g)\n",
                                   i, e, (double)got);
                            fflush(stdout);
                            ok = false;
                        }
                    }
                }
            }
            vkUnmapMemory(m_Device.device, resultMem);
        }
    }
    if (ok && uploadedReject != expectReject)
    {
        printf("[NormalMatrixOracle] FAIL: diagnostic count %u != expected %u\n", uploadedReject, expectReject);
        fflush(stdout);
        ok = false;
    }

    printf("[NormalMatrixOracle] %s: %u instances (%u supported), worst matrix err %.3g (tol rel+abs 1e-5), worst angle %.3g rad (tol 1e-4)\n",
           ok ? "PASS" : "FAIL", instanceCount, instanceCount - expectReject, worstMatrixErr, worstAngle);
    fflush(stdout);

    // ---- Transient teardown -------------------------------------------------
    if (pipeline) vkDestroyPipeline(m_Device.device, pipeline, nullptr);
    if (pipelineLayout) vkDestroyPipelineLayout(m_Device.device, pipelineLayout, nullptr);
    if (setLayout) vkDestroyDescriptorSetLayout(m_Device.device, setLayout, nullptr);
    if (pool) vkDestroyDescriptorPool(m_Device.device, pool, nullptr);
    if (oracleShader) vkDestroyShaderModule(m_Device.device, oracleShader, nullptr);
    GpuResources::DestroyBuffer(m_Device, resultBuf, resultMem);
    return ok;
}
