#pragma once

#include "vulkan/vulkan.h"
#include "shader_interface.h"
#include <cstdint>

struct GpuDevice;

// ReservoirResources — owns per-pixel ReSTIR DI reservoir SSBOs + surface history.
//
// Three buffers:
//   history  — previous frame's final reservoir (read by temporal, written by spatial)
//   scratch  — temporal output / spatial input (written by temporal, read by spatial)
//   surfaceHistory — per-pixel receiver metadata for temporal validation
//
// No CPU-side swapping: history is overwritten by the spatial pass each frame,
// and scratch is overwritten by the temporal pass. This avoids descriptor rewrites.
//
// Each reservoir buffer holds width*height SIReservoir entries (32 bytes/pixel).
// Surface history holds width*height SISurfaceHistory entries (16 bytes/pixel).
// Resized on viewport change; callers should check IsValid() + MatchesSize()
// before dispatching the ReSTIR passes.
//
// Memory: 2 * w * h * 32 + w * h * 16 bytes. At 1080p: ~24.9 MB total.
// Buffers are device-local (not host-visible) — written by compute shaders.
class ReservoirResources
{
public:
	ReservoirResources() = default;
	~ReservoirResources();

	ReservoirResources(const ReservoirResources&) = delete;
	ReservoirResources& operator=(const ReservoirResources&) = delete;

	void Create(const GpuDevice& dev, uint32_t width, uint32_t height);
	void Destroy();

	bool IsValid() const { return m_HistoryBuffer != VK_NULL_HANDLE; }
	bool MatchesSize(uint32_t width, uint32_t height) const
		{ return m_Width == width && m_Height == height; }

	uint32_t GetWidth()  const { return m_Width; }
	uint32_t GetHeight() const { return m_Height; }
	VkDeviceSize GetBufferSize() const { return m_BufferSize; }
	VkDeviceSize GetSurfaceHistorySize() const { return m_SurfaceHistorySize; }

	VkBuffer GetHistoryBuffer() const { return m_HistoryBuffer; }
	VkBuffer GetScratchBuffer() const { return m_ScratchBuffer; }
	VkBuffer GetSurfaceHistoryBuffer() const { return m_SurfaceHistoryBuffer; }

	// Clear history + surface history buffers (fill with zeros).
	// Called on resize, scene change, camera cut, and ReSTIR enable/disable.
	void ClearHistory(VkCommandBuffer cmd);

private:
	GpuDevice const* m_Device = nullptr;

	VkBuffer m_HistoryBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_HistoryBufferMemory = VK_NULL_HANDLE;
	VkBuffer m_ScratchBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_ScratchBufferMemory = VK_NULL_HANDLE;
	VkBuffer m_SurfaceHistoryBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_SurfaceHistoryBufferMemory = VK_NULL_HANDLE;

	uint32_t m_Width = 0;
	uint32_t m_Height = 0;
	VkDeviceSize m_BufferSize = 0;
	VkDeviceSize m_SurfaceHistorySize = 0;
};