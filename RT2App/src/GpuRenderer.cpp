#include "GpuRenderer.h"
#include "Walnut/Application.h"
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <vector>


static std::vector<char> DecodeBase64(const std::string& encoded)
{
    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; ++i)
        T[static_cast<unsigned char>(table[i])] = i;

    std::vector<char> out;
    int val = 0, valb = -8;
    for (unsigned char c : encoded)
    {
        if (isspace(c))
            continue;
        if (T[c] == -1)
            break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0)
        {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

static std::vector<char> ReadFileBase64(const std::string& path)
{
    std::ifstream file(path);
    if (!file)
        throw std::runtime_error("Failed to open file: " + path);

    std::stringstream ss;
    ss << file.rdbuf();
    return DecodeBase64(ss.str());
}

static uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
    {
        if ((typeBits & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    throw std::runtime_error("Failed to find suitable memory type!");
}

GpuRenderer::GpuRenderer()
{
    m_Device = Walnut::Application::GetDevice();
    m_PhysicalDevice = Walnut::Application::GetPhysicalDevice();
    m_Queue = Walnut::Application::GetGraphicsQueue();

    // Simple scene: many spheres as in CPU renderer
    auto ground_material = make_shared<LambertianMaterial>(vec3(0.5f));
    m_World.add(make_shared<Sphere>(vec3(0, -1000, 0), 1000, ground_material));
}

GpuRenderer::~GpuRenderer()
{
    if (m_Pipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
    if (m_PipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
    if (m_DescriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(m_Device, m_DescriptorSetLayout, nullptr);
    if (m_DescriptorPool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(m_Device, m_DescriptorPool, nullptr);
    if (m_ComputeShader != VK_NULL_HANDLE)
        vkDestroyShaderModule(m_Device, m_ComputeShader, nullptr);
    if (m_StorageBuffer != VK_NULL_HANDLE)
        vkDestroyBuffer(m_Device, m_StorageBuffer, nullptr);
    if (m_StorageBufferMemory != VK_NULL_HANDLE)
        vkFreeMemory(m_Device, m_StorageBufferMemory, nullptr);
}

void GpuRenderer::CreateOutputImage(uint32_t width, uint32_t height)
{
    if (m_FinalImage)
    {
        if (m_FinalImage->GetWidth() == width && m_FinalImage->GetHeight() == height)
            return;
        m_FinalImage->Resize(width, height);
    }
    else
    {
        m_FinalImage = std::make_shared<Walnut::Image>(width, height, Walnut::ImageFormat::RGBA);
    }
}

void GpuRenderer::OnResize(uint32_t width, uint32_t height)
{
    CreateOutputImage(width, height);

    if (m_Pipeline == VK_NULL_HANDLE)
        CreateRayTracingPipeline();

    m_ImageWidth = width;
    m_ImageHeight = height;

    if (m_StorageBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(m_Device, m_StorageBuffer, nullptr);
        vkFreeMemory(m_Device, m_StorageBufferMemory, nullptr);
        m_StorageBuffer = VK_NULL_HANDLE;
        m_StorageBufferMemory = VK_NULL_HANDLE;
    }

    VkDeviceSize size = (VkDeviceSize)width * height * 4;

    VkBufferCreateInfo bufferCI{};
    bufferCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferCI.size = size;
    bufferCI.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(m_Device, &bufferCI, nullptr, &m_StorageBuffer);

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(m_Device, m_StorageBuffer, &req);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = req.size;
    allocInfo.memoryTypeIndex = FindMemoryType(m_PhysicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(m_Device, &allocInfo, nullptr, &m_StorageBufferMemory);
    vkBindBufferMemory(m_Device, m_StorageBuffer, m_StorageBufferMemory, 0);

    if (m_DescriptorSet == VK_NULL_HANDLE)
    {
        VkDescriptorSetAllocateInfo allocInfoDS{};
        allocInfoDS.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfoDS.descriptorPool = m_DescriptorPool;
        allocInfoDS.descriptorSetCount = 1;
        allocInfoDS.pSetLayouts = &m_DescriptorSetLayout;
        vkAllocateDescriptorSets(m_Device, &allocInfoDS, &m_DescriptorSet);
    }

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = m_StorageBuffer;
    bufferInfo.offset = 0;
    bufferInfo.range = size;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_DescriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
}

void GpuRenderer::BuildBLAS()
{
    m_BlasGeometries.clear();
    for (const auto& object : m_World.objects)
    {
        auto sphere = std::dynamic_pointer_cast<Sphere>(object);
        if (sphere)
        {
            SphereData geom{};
            geom.Center = sphere->centre;
            geom.Radius = sphere->radius;
            m_BlasGeometries.push_back(geom);
        }
    }
}

void GpuRenderer::BuildTLAS()
{
    m_TlasInstances.resize(m_BlasGeometries.size());
    for (size_t i = 0; i < m_BlasGeometries.size(); ++i)
    {
        m_TlasInstances[i].BlasIndex = static_cast<uint32_t>(i);
    }
}

void GpuRenderer::CreateRayTracingPipeline()
{
    if (m_Pipeline != VK_NULL_HANDLE)
        return;

    auto shaderCode = ReadFileBase64("RT2App/shaders/raygen.spv.base64");

    VkShaderModuleCreateInfo moduleCI{};
    moduleCI.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleCI.codeSize = shaderCode.size();
    moduleCI.pCode = reinterpret_cast<const uint32_t*>(shaderCode.data());
    if (vkCreateShaderModule(m_Device, &moduleCI, nullptr, &m_ComputeShader) != VK_SUCCESS)
        throw std::runtime_error("failed to create shader module");

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 1;
    layoutCI.pBindings = &binding;
    vkCreateDescriptorSetLayout(m_Device, &layoutCI, nullptr, &m_DescriptorSetLayout);

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(int) * 2;

    VkPipelineLayoutCreateInfo pipelineLayoutCI{};
    pipelineLayoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutCI.setLayoutCount = 1;
    pipelineLayoutCI.pSetLayouts = &m_DescriptorSetLayout;
    pipelineLayoutCI.pushConstantRangeCount = 1;
    pipelineLayoutCI.pPushConstantRanges = &pushRange;
    vkCreatePipelineLayout(m_Device, &pipelineLayoutCI, nullptr, &m_PipelineLayout);

    VkComputePipelineCreateInfo pipelineCI{};
    pipelineCI.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineCI.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineCI.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineCI.stage.module = m_ComputeShader;
    pipelineCI.stage.pName = "main";
    pipelineCI.layout = m_PipelineLayout;

    if (vkCreateComputePipelines(m_Device, VK_NULL_HANDLE, 1, &pipelineCI, nullptr, &m_Pipeline) != VK_SUCCESS)
        throw std::runtime_error("failed to create compute pipeline");

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = 1;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes = &poolSize;
    vkCreateDescriptorPool(m_Device, &poolCI, nullptr, &m_DescriptorPool);
}

void GpuRenderer::CreateShaderBindingTable()
{
    // not used in compute implementation
}

void GpuRenderer::Render(Camera& camera)
{
    if (!m_FinalImage)
        return;

    // Ensure pipeline exists
    if (m_Pipeline == VK_NULL_HANDLE)
    {
        CreateRayTracingPipeline();
        CreateShaderBindingTable();
    }

    VkCommandBuffer cmd = Walnut::Application::GetCommandBuffer(true);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, &m_DescriptorSet, 0, nullptr);

    int push[2] = { (int)m_ImageWidth, (int)m_ImageHeight };
    vkCmdPushConstants(cmd, m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);

    uint32_t groupCountX = (m_ImageWidth + 7) / 8;
    uint32_t groupCountY = (m_ImageHeight + 7) / 8;
    vkCmdDispatch(cmd, groupCountX, groupCountY, 1);

    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.buffer = m_StorageBuffer;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);

    Walnut::Application::FlushCommandBuffer(cmd);

    void* mapped = nullptr;
    vkMapMemory(m_Device, m_StorageBufferMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
    std::vector<uint32_t> buffer(m_ImageWidth * m_ImageHeight);
    memcpy(buffer.data(), mapped, buffer.size() * sizeof(uint32_t));
    vkUnmapMemory(m_Device, m_StorageBufferMemory);

    m_FinalImage->SetData(buffer.data());
}
