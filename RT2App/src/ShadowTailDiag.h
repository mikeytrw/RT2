#pragma once

#include "vulkan/vulkan.h"
#include "GpuResources.h"
#include "ShadowTailBins.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct GpuDevice;

// ============================================================================
// ShadowTailDiag — device-side SSBO for the RT4-T4 shadow-tail diagnostic
// (shadow-tail-plan contract, READY 2026-10-13). RT2App-only (Vulkan):
// NEVER add this TU to RT2Tests/RT2SliceRunner (CPU-only boundary).
//
// Layout (frozen; MUST match shaders/shadow_tail.glsl):
//   header[8]: magic, version, frameIndex, mode, edgeCountRT,
//     edgeCountGI, variantTag, reserved.
//   edgesRT[226], edgesGI[226]: frozen acceptance edges (u32 ticks,
//     written by CPU at freeze via SetEdges; zeroed before that).
//   per slot (2) per family RT=0/GI=1:
//     pilot[34], hist[226], eligible, recorded, blocked, visible,
//     strata[16], ord[16], reserved[2]  => 298 u32 per family-slot,
//     plus 2 positive-control checksums per slot.
// Total 1656 u32 = 6624 bytes. Lifecycle mirrors GpuDiagnostics
// (fill-zero + edge upload per frame, fenced copy to per-slot host
// readback); the buffer is extent-independent so no resize path exists.
// An unvalidated snapshot (bad magic/version/frame/mode, edge counts
// above capacity) is reported invalid with a reason — never silent zeros.
// ============================================================================
class ShadowTailDiag
{
public:
    // Layout mirrors shaders/shadow_tail.glsl via ShadowTailBins.h:
    // header[8] (magic, version, frameIndex, mode, edgeCountRT,
    // edgeCountGI, variantTag, reserved), edgesRT/GI[226], then per
    // slot per family: pilot[34], hist[226], eligible, recorded,
    // blocked, visible, strata[16], ord[16], reserved[2], plus 2
    // positive-control checksums per slot.
    static constexpr uint32_t kHeaderU32 = shadowtail::kHeaderU32;
    static constexpr uint32_t kEdgesPerFamily = shadowtail::kAcceptanceEdgesMax;
    static constexpr uint32_t kTotalU32 = shadowtail::kTotalU32; // 1660
    static constexpr uint32_t kMagic = 0x53484457u; // 'SHDW'
    static constexpr uint32_t kVersion = 1u;
    static constexpr uint32_t kVariantTag = 1u; // staged ON tag, see below
    static constexpr uint32_t kMaxFrameSlots = shadowtail::kSlots; // 2

    struct Snapshot
    {
        uint64_t frameIndex = 0;
        bool valid = false;
        // Non-empty whenever valid == false: "no snapshot read yet" until
        // the first fenced readback replaces it. Empty reasons are a
        // silent-failure shape — this default keeps that impossible.
        std::string invalidReason = "no snapshot read yet";
        std::array<uint32_t, kHeaderU32> head = {};
        std::vector<uint32_t> data; // kTotalU32 words when valid
        uint32_t slot = 0;      // fence slot this snapshot was read from
        uint64_t serial = 0;    // diagnostic-owned monotonic submit serial
    };

    ShadowTailDiag() = default;
    ShadowTailDiag(const ShadowTailDiag&) = delete;
    ShadowTailDiag& operator=(const ShadowTailDiag&) = delete;

    bool Init(const GpuDevice& device, uint32_t frameSlotCount);
    void Destroy(const GpuDevice& device);

    bool IsAvailable() const { return m_DeviceBuffer.buffer != VK_NULL_HANDLE; }
    VkBuffer GetBuffer() const { return m_DeviceBuffer.buffer; }
    VkDeviceSize GetBufferSize() const { return m_DeviceBuffer.size; }

    // Stage frozen acceptance edges (manifest values). Takes effect on the
    // next BeginFrame via vkCmdUpdateBuffer. Counts above capacity are
    // rejected here (loud), never clamped silently.
    bool SetEdges(uint32_t edgeCountRT, const uint32_t* edgesRT,
                  uint32_t edgeCountGI, const uint32_t* edgesGI);

    // Stage the header mode: 1 = normal ON, 2 = positive-control ON
    // (dependent work inside brackets). Anything else is rejected.
    // Takes effect on the next BeginFrame.
    bool SetMode(uint32_t mode);

    void BeginFrame(VkCommandBuffer cmd, uint32_t frameSlot, uint64_t frameIndex);
    void EndFrame(VkCommandBuffer cmd, uint32_t frameSlot);

    // Read only after the matching frame fence has signalled.
    void ReadCompletedSlot(const GpuDevice& device, uint32_t frameSlot);
    // Newest snapshot by diagnostic submit serial (not by renderer frame
    // id, which can reset — serials never go backwards).
    const Snapshot& GetLatest() const;
    // All snapshots with serials newer than the last drain, in submission
    // order (valid or not — invalid ones carry reasons). Advances the
    // delivered watermark so each snapshot is delivered exactly once.
    // This is the drain primitive the headless loop calls per frame and
    // after the final fence: BOTH pending slots are emitted, never just
    // the newest (which would overwrite/drop the other).
    std::vector<Snapshot> DrainNewSnapshots();

    // RendererGPU calls this when --shadow-tail-edges supplies a frozen
    // manifest (validated here, staged for upload). No manifest staged
    // (edge counts zero) means pilot-only output: SnapshotFrom rejects
    // every result, so an acceptance snapshot can never validate.
    static std::string SnapshotToJson(const Snapshot& snapshot);

private:
    Snapshot SnapshotFrom(const uint32_t* words, uint64_t frameIndex,
                          uint32_t expEdgeRT, uint32_t expEdgeGI) const;
    // Segment validation on a completed slot (sums, bounds, coverage).
    // Mutates snapshot.valid/invalidReason only; never passes silently.
    bool ValidateSlot(Snapshot& snapshot, uint32_t slot) const;

    static constexpr VkDeviceSize kBytesTotal = VkDeviceSize(kTotalU32) * 4u;
    static constexpr VkDeviceSize kHeaderEdgesBytes =
        VkDeviceSize(kHeaderU32 + 2u * kEdgesPerFamily) * 4u;

    struct Slot
    {
        uint64_t frameIndex = 0;
        bool submitted = false;
        uint64_t serial = 0; // assigned at EndFrame, submission order
    };

    GpuBuffer m_DeviceBuffer;
    std::array<GpuBuffer, kMaxFrameSlots> m_ReadbackBuffers = {};
    std::array<Slot, kMaxFrameSlots> m_Slots = {};
    // Per-slot snapshots + delivery state. A slot's snapshot is ALWAYS
    // overwritten by its own newer read (same segment, fence-proven) and
    // NEVER by another slot; GetLatest/Drain order by serial.
    std::array<Snapshot, kMaxFrameSlots> m_SlotSnaps = {};
    std::array<bool, kMaxFrameSlots> m_SlotHasData = {};
    uint64_t m_SubmitSerial = 1; // nonzero: 0 means "no submission yet"
    uint64_t m_DeliveredSerial = 0;
    uint32_t m_FrameSlotCount = 0;

    // CPU-side staged header+edges, uploaded on EVERY BeginFrame
    // (1840 B via vkCmdUpdateBuffer): no dirty-flag staleness is possible —
    // the manifest state on GPU always equals the last SetEdges call.
    std::array<uint32_t, kHeaderU32 + 2u * kEdgesPerFamily> m_Staged = {};
    uint32_t m_Mode = 1u;
};

static_assert(ShadowTailDiag::kTotalU32 * 4u == 6624u,
              "ShadowTailDiag SSBO must be 6624 bytes (header+edges+slots)");
