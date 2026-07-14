#pragma once

#include "vulkan/vulkan.h"
#include "shader_interface.h"
#include <cstdint>

struct GpuDevice;

// ReservoirGIResources — owns the monolithic ReSTIR GI buffer.
//
// One storage buffer at SI_BINDING_GI_DATA contains four aligned regions:
//   reservoirA          pixelCount * 48 bytes (SIGIReservoir)
//   reservoirB          pixelCount * 48 bytes (SIGIReservoir)
//   receiverHistoryPrev pixelCount * 32 bytes (SISurfaceHistory)
//   receiverHistoryCur  pixelCount * 32 bytes (SISurfaceHistory)
//
// Reservoir A/B and receiver-history prev/cur ping-pong by frame parity
// (giCurrentRegion / giPreviousRegion). Two receiver-history regions are
// required because the temporal dispatch reads previous history and a
// separate second dispatch (restir_gi_history.comp) writes current history
// after all temporal reads finish — avoiding a read/write race.
//
// Storage: 160 bytes per full-resolution pixel.
//   1080p: ~316.4 MiB   |   4K: ~1.24 GiB
//
// When GI is disabled, CreateDummy allocates a 16-byte valid buffer so the
// descriptor set is still legal without paying the full cost. The buffer is
// device-local (written by compute shaders, cleared via transfer).
//
// Zero = invalid reservoir: vkCmdFillBuffer(..., 0) sets flags bit 0 = 0 and
// M = 0, so cleared regions decode as invalid by construction.
class ReservoirGIResources
{
public:
	ReservoirGIResources() = default;
	~ReservoirGIResources();

	ReservoirGIResources(const ReservoirGIResources&) = delete;
	ReservoirGIResources& operator=(const ReservoirGIResources&) = delete;

	// Full-size allocation for the given viewport.
	void Create(const GpuDevice& dev, uint32_t width, uint32_t height);

	// Dummy 16-byte allocation — used while GI is disabled so binding 11
	// points at a legal (but never read) buffer.
	void CreateDummy(const GpuDevice& dev);

	void Destroy();

	bool IsValid() const { return m_Buffer != VK_NULL_HANDLE; }
	bool IsDummy() const { return m_IsDummy; }
	bool MatchesSize(uint32_t width, uint32_t height) const
		{ return !m_IsDummy && m_Width == width && m_Height == height; }

	uint32_t GetWidth()  const { return m_Width; }
	uint32_t GetHeight() const { return m_Height; }
	VkDeviceSize GetTotalSize() const { return m_TotalSize; }

	VkBuffer GetBuffer() const { return m_Buffer; }

	// Byte offsets of each region within the monolithic buffer.
	VkDeviceSize GetReservoirRegionOffset(uint32_t region) const;
	VkDeviceSize GetReceiverHistoryRegionOffset(uint32_t region) const;
	VkDeviceSize GetReservoirRegionSize() const { return m_ReservoirRegionSize; }
	VkDeviceSize GetReceiverHistoryRegionSize() const { return m_ReceiverHistoryRegionSize; }

	// Clear all four regions (fill with zeros → invalid reservoirs/history).
	// Called on resize, scene change, camera cut, and GI enable/disable.
	void ClearAll(VkCommandBuffer cmd);

private:
	GpuDevice const* m_Device = nullptr;

	VkBuffer       m_Buffer = VK_NULL_HANDLE;
	VkDeviceMemory m_BufferMemory = VK_NULL_HANDLE;

	uint32_t m_Width = 0;
	uint32_t m_Height = 0;
	bool     m_IsDummy = false;

	VkDeviceSize m_ReservoirRegionSize = 0;        // one reservoir region (pixelCount * 48)
	VkDeviceSize m_ReceiverHistoryRegionSize = 0;  // one history region (pixelCount * 32)
	VkDeviceSize m_TotalSize = 0;
};