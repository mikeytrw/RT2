#include "ShadowTailDiag.h"
#include "GpuDevice.h"
#include "GpuResources.h"
#include "RTLog.h"

#include <cstring>
#include <sstream>

bool ShadowTailDiag::Init(const GpuDevice& device, uint32_t frameSlotCount)
{
    if (IsAvailable())
        return true;
    if (frameSlotCount == 0 || frameSlotCount > kMaxFrameSlots)
        return false;

    m_FrameSlotCount = frameSlotCount;
    // Device-local counters; TRANSFER_DST for the per-frame header/edge
    // upload + zeroing, TRANSFER_SRC for the fenced readback copy.
    if (!GpuResources::CreateBuffer(device, kBytesTotal,
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                    m_DeviceBuffer))
    {
        RT_LOG("[ShadowTail] failed to create device buffer");
        return false;
    }

    for (uint32_t i = 0; i < frameSlotCount; ++i)
    {
        if (!GpuResources::CreateBuffer(device, kBytesTotal,
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                        m_ReadbackBuffers[i]))
        {
            RT_LOG("[ShadowTail] failed to create readback buffer %u", i);
            Destroy(device);
            return false;
        }
    }

    // Staged header defaults: magic/version/mode set; edge counts zero
    // (manifest validation rejects a zero-count snapshot downstream).
    m_Staged.fill(0u);
    m_Staged[0] = kMagic;
    m_Staged[1] = kVersion;
    m_Staged[3] = 1u; // mode ON

    RT_LOG("[ShadowTail] enabled (%u bytes, %u frame slots)", (unsigned)kBytesTotal,
           frameSlotCount);
    return true;
}

void ShadowTailDiag::Destroy(const GpuDevice& device)
{
    for (auto& buffer : m_ReadbackBuffers)
        GpuResources::DestroyBuffer(device, buffer);
    GpuResources::DestroyBuffer(device, m_DeviceBuffer);
    m_Slots = {};
    m_Latest = Snapshot{};
    m_FrameSlotCount = 0;
}

bool ShadowTailDiag::SetEdges(uint32_t edgeCountRT, const uint32_t* edgesRT,
                              uint32_t edgeCountGI, const uint32_t* edgesGI)
{
    // Loud rejection (never silent clamping): capacity + monotonicity.
    if (edgeCountRT < 2u || edgeCountRT > kEdgesPerFamily ||
        edgeCountGI < 2u || edgeCountGI > kEdgesPerFamily ||
        edgesRT == nullptr || edgesGI == nullptr)
        return false;
    for (uint32_t i = 0; i + 1u < edgeCountRT; ++i)
        if (!(edgesRT[i] < edgesRT[i + 1u]))
            return false;
    for (uint32_t i = 0; i + 1u < edgeCountGI; ++i)
        if (!(edgesGI[i] < edgesGI[i + 1u]))
            return false;
    if (edgesRT[0] != 0u || edgesGI[0] != 0u)
        return false;
    m_Staged[4] = edgeCountRT;
    m_Staged[5] = edgeCountGI;
    std::memcpy(m_Staged.data() + kHeaderU32, edgesRT,
                size_t(edgeCountRT) * sizeof(uint32_t));
    std::memset(m_Staged.data() + kHeaderU32 + edgeCountRT, 0,
                size_t(kEdgesPerFamily - edgeCountRT) * sizeof(uint32_t));
    std::memcpy(m_Staged.data() + kHeaderU32 + kEdgesPerFamily, edgesGI,
                size_t(edgeCountGI) * sizeof(uint32_t));
    std::memset(m_Staged.data() + kHeaderU32 + kEdgesPerFamily + edgeCountGI, 0,
                size_t(kEdgesPerFamily - edgeCountGI) * sizeof(uint32_t));
    return true;
}

void ShadowTailDiag::BeginFrame(VkCommandBuffer cmd, uint32_t frameSlot,
                                uint64_t frameIndex)
{
    if (!IsAvailable() || frameSlot >= m_FrameSlotCount)
        return;

    // Header + frozen edges first (vkCmdUpdateBuffer: 1840 B, one shot),
    // then zero the counter region. Both TRANSFER_DST writes precede the
    // transfer->RT/compute barrier below.
    m_Staged[2] = static_cast<uint32_t>(frameIndex & 0xFFFFFFFFull);
    vkCmdUpdateBuffer(cmd, m_DeviceBuffer.buffer, 0, kHeaderEdgesBytes,
                      m_Staged.data());
    vkCmdFillBuffer(cmd, m_DeviceBuffer.buffer, kHeaderEdgesBytes,
                    kBytesTotal - kHeaderEdgesBytes, 0u);

    VkBufferMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = m_DeviceBuffer.buffer;
    barrier.offset = 0;
    barrier.size = kBytesTotal;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);

    m_Slots[frameSlot].frameIndex = frameIndex;
    m_Slots[frameSlot].submitted = false;
}

void ShadowTailDiag::EndFrame(VkCommandBuffer cmd, uint32_t frameSlot)
{
    if (!IsAvailable() || frameSlot >= m_FrameSlotCount)
        return;

    VkBufferMemoryBarrier toTransfer = {};
    toTransfer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.buffer = m_DeviceBuffer.buffer;
    toTransfer.offset = 0;
    toTransfer.size = kBytesTotal;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 1, &toTransfer, 0, nullptr);

    VkBufferCopy copy = {};
    copy.srcOffset = 0;
    copy.dstOffset = 0;
    copy.size = kBytesTotal;
    vkCmdCopyBuffer(cmd, m_DeviceBuffer.buffer,
                    m_ReadbackBuffers[frameSlot].buffer, 1, &copy);

    VkBufferMemoryBarrier toHost = {};
    toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = m_ReadbackBuffers[frameSlot].buffer;
    toHost.offset = 0;
    toHost.size = kBytesTotal;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, nullptr, 1, &toHost, 0, nullptr);

    m_Slots[frameSlot].submitted = true;
}

ShadowTailDiag::Snapshot ShadowTailDiag::SnapshotFrom(const uint32_t* words,
                                                      uint64_t frameIndex) const
{
    Snapshot snapshot;
    snapshot.frameIndex = frameIndex;
    snapshot.valid = false;
    for (uint32_t i = 0; i < kHeaderU32; ++i)
        snapshot.head[i] = words[i];
    if (snapshot.head[0] != kMagic)
    {
        snapshot.invalidReason = "bad magic";
        return snapshot;
    }
    if (snapshot.head[1] != kVersion)
    {
        snapshot.invalidReason = "version mismatch";
        return snapshot;
    }
    if (snapshot.head[3] != 1u)
    {
        snapshot.invalidReason = "mode is not ON";
        return snapshot;
    }
    if (snapshot.head[4] < 2u || snapshot.head[4] > kEdgesPerFamily ||
        snapshot.head[5] < 2u || snapshot.head[5] > kEdgesPerFamily)
    {
        snapshot.invalidReason = "edge counts outside manifest capacity";
        return snapshot;
    }
    snapshot.data.assign(words, words + kTotalU32);
    snapshot.valid = true;
    return snapshot;
}

void ShadowTailDiag::ReadCompletedSlot(const GpuDevice& device, uint32_t frameSlot)
{
    if (!IsAvailable() || frameSlot >= m_FrameSlotCount)
        return;
    Slot& slot = m_Slots[frameSlot];
    if (!slot.submitted)
        return;

    void* mapped = nullptr;
    if (vkMapMemory(device.device, m_ReadbackBuffers[frameSlot].memory,
                    0, kBytesTotal, 0, &mapped) == VK_SUCCESS)
    {
        const Snapshot snapshot =
            SnapshotFrom(static_cast<const uint32_t*>(mapped), slot.frameIndex);
        vkUnmapMemory(device.device, m_ReadbackBuffers[frameSlot].memory);
        if (!snapshot.valid)
        {
            RT_LOG("[ShadowTail] invalid snapshot (frame %llu: %s)",
                   static_cast<unsigned long long>(slot.frameIndex),
                   snapshot.invalidReason.c_str());
        }
        else if (!m_Latest.valid || snapshot.frameIndex >= m_Latest.frameIndex)
            m_Latest = snapshot;
    }
    else
    {
        RT_LOG("[ShadowTail] failed to map completed frame slot %u", frameSlot);
    }
    slot.submitted = false;
}

std::string ShadowTailDiag::SnapshotToJson(const Snapshot& snapshot)
{
    std::ostringstream out;
    out << "{\"schema\":\"rt2.shadow-tail.v1\",\"frame\":" << snapshot.frameIndex
        << ",\"valid\":" << (snapshot.valid ? "true" : "false");
    if (!snapshot.valid)
    {
        out << ",\"reason\":\"" << snapshot.invalidReason << "\"}";
        return out.str();
    }
    out << ",\"head\":[";
    for (uint32_t i = 0; i < kHeaderU32; ++i)
        out << (i ? "," : "") << snapshot.head[i];
    // Counter region only (edges live in the frozen manifest, referenced
    // by frame + edge counts): slots follow the header+edges in full layout.
    constexpr uint32_t kDataBase = kHeaderU32 + 2u * kEdgesPerFamily;
    constexpr uint32_t kDataCount = kTotalU32 - kDataBase;
    out << "],\"data\":[";
    for (uint32_t i = 0; i < kDataCount; ++i)
        out << (i ? "," : "") << snapshot.data[kDataBase + i];
    out << "]}";
    return out.str();
}
