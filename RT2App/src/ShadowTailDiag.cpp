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

    // Staged header defaults: magic/version/mode/variant set; edge counts
    // zero (manifest validation rejects a zero-count snapshot downstream:
    // pilot-only output is never a valid acceptance snapshot).
    m_Staged.fill(0u);
    m_Staged[0] = kMagic;
    m_Staged[1] = kVersion;
    m_Staged[3] = 1u; // mode ON
    m_Staged[6] = kVariantTag; // staged ON tag (see SnapshotFrom)
    m_Staged[6] = kVariantTag; // staged ON tag (see SnapshotFrom)

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
                                                      uint64_t frameIndex,
                                                      uint32_t expEdgeRT,
                                                      uint32_t expEdgeGI) const
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
    if (snapshot.head[2] != static_cast<uint32_t>(frameIndex & 0xFFFFFFFFull))
    {
        snapshot.invalidReason = "header frame differs from submitted slot";
        return snapshot;
    }
    if (snapshot.head[6] != kVariantTag)
    {
        snapshot.invalidReason = "variant tag mismatch (not staged ON data)";
        return snapshot;
    }
    if (snapshot.head[4] < 2u || snapshot.head[4] > kEdgesPerFamily ||
        snapshot.head[5] < 2u || snapshot.head[5] > kEdgesPerFamily)
    {
        snapshot.invalidReason = "edge counts outside manifest capacity";
        return snapshot;
    }
    if (snapshot.head[4] != expEdgeRT || snapshot.head[5] != expEdgeGI)
    {
        snapshot.invalidReason = "edge counts differ from staged manifest";
        return snapshot;
    }
    snapshot.data.assign(words, words + kTotalU32);
    // Per-family validation on the completed slot is done by
    // ValidateSlot below (needs the slot index, not just the words).
    snapshot.valid = true;
    return snapshot;
}

// ValidateSlot: sums, bounds and coverage on one completed slot's segment.
// All aggregates use 64-bit accumulation. Any failure writes the reason
// and returns false — the snapshot is then invalid, never passed.
bool ShadowTailDiag::ValidateSlot(Snapshot& snapshot, uint32_t slot) const
{
    constexpr uint32_t kSlotBase = kHeaderU32 + 2u * kEdgesPerFamily;
    const uint32_t seg = kSlotBase + slot * shadowtail::kSlotStride;
    bool anyShadow = false;
    for (uint32_t f = 0; f < shadowtail::kFamilies; ++f)
    {
        const uint32_t base = seg + f * shadowtail::kFamStride;
        const uint32_t eligible = snapshot.data[base + shadowtail::kOffEligible];
        const uint32_t recorded = snapshot.data[base + shadowtail::kOffRecorded];
        if (recorded > eligible)
        {
            snapshot.valid = false;
            snapshot.invalidReason = "recorded exceeds eligible";
            return false;
        }
        uint64_t histSum = 0;
        for (uint32_t i = 0; i < shadowtail::kAcceptanceEdgesMax; ++i)
            histSum += snapshot.data[base + shadowtail::kOffHist + i];
        if (histSum != recorded)
        {
            snapshot.valid = false;
            snapshot.invalidReason = "histogram sum differs from recorded";
            return false;
        }
        if (eligible > 0u)
            anyShadow = true;
    }
    if (!anyShadow)
    {
        snapshot.valid = false;
        snapshot.invalidReason = "zero shadow invocations in both families";
        return false;
    }
    return true;
}

void ShadowTailDiag::ReadCompletedSlot(const GpuDevice& device, uint32_t frameSlot)
{
    if (!IsAvailable() || frameSlot >= m_FrameSlotCount)
        return;
    Slot& slot = m_Slots[frameSlot];
    if (!slot.submitted)
        return;
    slot.submitted = false;

    // A failed map/readback INVALIDATES the latest snapshot (with the
    // slot's frame id and a reason) instead of retaining an older valid
    // one — a previous frame must never stand in for a failed later frame.
    Snapshot failed;
    failed.frameIndex = slot.frameIndex;
    failed.valid = false;

    void* mapped = nullptr;
    if (vkMapMemory(device.device, m_ReadbackBuffers[frameSlot].memory,
                    0, kBytesTotal, 0, &mapped) != VK_SUCCESS || mapped == nullptr)
    {
        RT_LOG("[ShadowTail] failed to map completed frame slot %u", frameSlot);
        failed.invalidReason = "readback map failure";
        m_Latest = failed;
        return;
    }
    Snapshot snapshot = SnapshotFrom(static_cast<const uint32_t*>(mapped),
                                     slot.frameIndex, m_Staged[4], m_Staged[5]);
    vkUnmapMemory(device.device, m_ReadbackBuffers[frameSlot].memory);
    if (snapshot.valid && !ValidateSlot(snapshot, frameSlot))
    {
        RT_LOG("[ShadowTail] invalid snapshot (frame %llu: %s)",
               static_cast<unsigned long long>(slot.frameIndex),
               snapshot.invalidReason.c_str());
    }
    if (!snapshot.valid)
    {
        RT_LOG("[ShadowTail] invalid snapshot (frame %llu: %s)",
               static_cast<unsigned long long>(slot.frameIndex),
               snapshot.invalidReason.c_str());
    }
    // Monotonic replacement only: never let an older slot overwrite newer
    // data, but a failed newer frame MUST replace an older valid one.
    if (!m_Latest.valid || snapshot.frameIndex >= m_Latest.frameIndex)
        m_Latest = snapshot;
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
