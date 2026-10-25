#include "ShadowTailDiag.h"
#include "GpuDevice.h"
#include "GpuResources.h"
#include "RTLog.h"

#include <cstring>
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
    m_Mode = 1u;

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
    m_SlotSnaps = {};
    m_SlotHasData = {};
    m_SubmitSerial = 1;
    m_DeliveredSerial = 0;
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

bool ShadowTailDiag::SetMode(uint32_t mode)
{
    // Only the two specified modes exist (1 normal, 2 positive-control);
    // anything else is a loud error, never a silent default.
    if (mode != 1u && mode != 2u)
        return false;
    m_Mode = mode;
    return true;
}

void ShadowTailDiag::BeginFrame(VkCommandBuffer cmd, uint32_t frameSlot,
                                uint64_t frameIndex)
{
    if (!IsAvailable() || frameSlot >= m_FrameSlotCount)
        return;

    // Cross-frame ordering FIRST: the previous frame's readback copy
    // (transfer read of this buffer) must complete before this frame's
    // header update + counter clear (transfer writes). Same-queue submit
    // order alone does not create this memory dependency across command
    // buffers; without it the next clear/header update may race the
    // previous copy or still-running producers (plan §5: next clear
    // waits for the previous copy's transfer read).
    VkBufferMemoryBarrier orderBarrier = {};
    orderBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    orderBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    orderBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    orderBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    orderBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    orderBarrier.buffer = m_DeviceBuffer.buffer;
    orderBarrier.offset = 0;
    orderBarrier.size = kBytesTotal;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 1, &orderBarrier, 0, nullptr);

    // Header + frozen edges first (vkCmdUpdateBuffer: 1840 B, one shot),
    // then zero the counter region. Both TRANSFER_DST writes precede the
    // transfer->RT/compute barrier below.
    m_Staged[2] = static_cast<uint32_t>(frameIndex & 0xFFFFFFFFull);
    m_Staged[3] = m_Mode;
    // Slot authority: the CPU-staged slot id selects this frame's fixed
    // counter segment. Shaders read ONLY this word (never the camera
    // clock), and readback validates this same segment — one authority
    // end to end, immune to frame-index resets or negative clocks.
    m_Staged[7] = frameSlot & 1u;
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
    // Diagnostic-owned monotonic submit serial: submission order, never
    // reset (unlike renderer frame ids, which resets can move backwards).
    m_Slots[frameSlot].serial = m_SubmitSerial++;
}

ShadowTailDiag::Snapshot ShadowTailDiag::SnapshotFrom(const uint32_t* words,
                                                      uint64_t frameIndex,
                                                      uint32_t expEdgeRT,
                                                      uint32_t expEdgeGI) const
{
    // Thin wrapper over the tested core: data words are ALWAYS recovered
    // past structural checks (pilot + control carry real counters), while
    // `valid` stays reserved for acceptance-usable snapshots.
    Snapshot snapshot;
    snapshot.frameIndex = frameIndex;
    snapshot.valid = false;
    if (words == nullptr)
    {
        snapshot.invalidReason = "null buffer";
        return snapshot;
    }
    uint32_t data[shadowtail::kTotalU32] = {};
    // Belt and braces with ValidateSnapshotWords (which zeroes dataOut on
    // entry): even a future early return above the validator cannot leak
    // uninitialized stack words into snapshot.data / SnapshotToJson.
    std::string reason;
    const bool ok = shadowtail::ValidateSnapshotWords(
        words, shadowtail::kTotalU32, frameIndex, expEdgeRT, expEdgeGI,
        snapshot.head.data(), data, shadowtail::kTotalU32, reason);
    snapshot.data.assign(data, data + shadowtail::kTotalU32);
    snapshot.valid = ok;
    snapshot.invalidReason = ok ? "" : reason;
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

    // A failed map/readback INVALIDATES this slot's snapshot (with the
    // slot's frame id, serial and a reason) instead of retaining an older
    // valid one — a previous frame must never stand in for a failed
    // later frame. Other slots are untouched.
    Snapshot failed;
    failed.frameIndex = slot.frameIndex;
    failed.valid = false;
    failed.slot = frameSlot;
    failed.serial = slot.serial;

    void* mapped = nullptr;
    if (vkMapMemory(device.device, m_ReadbackBuffers[frameSlot].memory,
                    0, kBytesTotal, 0, &mapped) != VK_SUCCESS || mapped == nullptr)
    {
        RT_LOG("[ShadowTail] failed to map completed frame slot %u", frameSlot);
        failed.invalidReason = "readback map failure";
        m_SlotSnaps[frameSlot] = failed;
        m_SlotHasData[frameSlot] = true;
        return;
    }
    Snapshot snapshot = SnapshotFrom(static_cast<const uint32_t*>(mapped),
                                     slot.frameIndex, m_Staged[4], m_Staged[5]);
    vkUnmapMemory(device.device, m_ReadbackBuffers[frameSlot].memory);
    snapshot.slot = frameSlot;
    snapshot.serial = slot.serial;
    if (snapshot.valid)
        ValidateSlot(snapshot, frameSlot);
    if (!snapshot.valid)
    {
        RT_LOG("[ShadowTail] invalid snapshot (frame %llu: %s)",
               static_cast<unsigned long long>(slot.frameIndex),
               snapshot.invalidReason.c_str());
    }
    // Per-slot store only: this slot's fence proves THIS slot's data.
    // No cross-slot comparison (frame ids can reset; serials order).
    m_SlotSnaps[frameSlot] = snapshot;
    m_SlotHasData[frameSlot] = true;
}

const ShadowTailDiag::Snapshot& ShadowTailDiag::GetLatest() const
{
    // Newest by diagnostic submit serial across slots with data.
    // Referenced storage: m_SlotSnaps entries (stable member array).
    size_t best = 0;
    bool have = false;
    for (size_t i = 0; i < m_SlotSnaps.size(); ++i)
    {
        if (!m_SlotHasData[i])
            continue;
        if (!have || m_SlotSnaps[i].serial > m_SlotSnaps[best].serial)
        {
            best = i;
            have = true;
        }
    }
    // Callers must check IsShadowTail-style availability separately; with
    // no data yet this returns slot 0's default ("no snapshot read yet").
    (void)have;
    return m_SlotSnaps[best];
}

std::vector<ShadowTailDiag::Snapshot> ShadowTailDiag::DrainNewSnapshots()
{
    // Submission order across slots; each snapshot delivered exactly once
    // (watermark advances past the max delivered serial). Invalid
    // snapshots are delivered too — the analysis gate treats them as
    // inconclusive evidence, never as silent absence.
    std::vector<Snapshot> out;
    for (uint32_t i = 0; i < kMaxFrameSlots; ++i)
    {
        if (m_SlotHasData[i] && m_SlotSnaps[i].serial > m_DeliveredSerial)
            out.push_back(m_SlotSnaps[i]);
    }
    // Insertion order above is slot order, not serial order.
    for (size_t i = 0; i < out.size(); ++i)
        for (size_t j = i + 1u; j < out.size(); ++j)
            if (out[j].serial < out[i].serial)
            {
                Snapshot tmp = out[i];
                out[i] = out[j];
                out[j] = tmp;
            }
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i].serial > m_DeliveredSerial)
            m_DeliveredSerial = out[i].serial;
    return out;
}

std::string ShadowTailDiag::SnapshotToJson(const Snapshot& snapshot)
{
    // Thin wrapper over the tested core (pilot/mode/reason/edge rules in
    // one place; the golden outputs are pinned by unit tests).
    const uint32_t* dataPtr =
        snapshot.data.empty() ? nullptr : snapshot.data.data();
    // Core requires full kTotalU32 words; snapshots that failed before the
    // data copy carry an empty vector — serialize zeros explicitly so the
    // schema (not the values) stays intact. Such snapshots are invalid by
    // construction (their reason says why).
    static const std::vector<uint32_t> kZeros(shadowtail::kTotalU32, 0u);
    if (dataPtr == nullptr || snapshot.data.size() != shadowtail::kTotalU32)
        dataPtr = kZeros.data();
    return shadowtail::SnapshotJson(snapshot.frameIndex, snapshot.slot,
                                    snapshot.serial, snapshot.valid,
                                    snapshot.invalidReason, snapshot.head.data(),
                                    dataPtr);
}
