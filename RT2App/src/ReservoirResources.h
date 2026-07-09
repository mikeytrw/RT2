#pragma once

#include "vulkan/vulkan.h"
#include "shader_interface.h"
#include <cstdint>

struct GpuDevice;

// ReservoirResources — owns the ping-pong per-pixel reservoir SSBOs used by
// RIS (Phase 1) and ReSTIR (future spatial/temporal reuse).
//
// Two buffers: `current` (written by RIS pass, read by shading pass) and
// `prev` (read by future temporal reuse pass, written by end-of-frame swap).
// Phase 1 only uses `current`; `prev` is allocated up-front to avoid a
// resize/rework churn when temporal reuse lands.
//
// Each buffer holds width*height SIReservoir entries (32 bytes/pixel).
// Resized on viewport change; callers should check IsValid() + MatchesSize()
// before dispatching the RIS pass.
//
// Memory: 2 * width * height * 32 bytes. At 1080p: ~16.6 MB total.
// Buffers are device-local (not host-visible) — written by compute shaders.
class ReservoirResources
{
public:
	ReservoirResources() = default;
	~ReservoirResources();

	ReservoirResources(const ReservoirResources&) = delete;
	ReservoirResources& operator=(const ReservoirResources&) = delete;

	// Create (or recreate) both buffers for the given viewport dimensions.
	// Destroys any existing buffers first. Safe to call with width/height=0
	// (results in IsValid()==false).
	void Create(const GpuDevice& dev, uint32_t width, uint32_t height);

	// Destroy both buffers. Safe to call on uncreated state.
	void Destroy();

	bool IsValid() const { return m_CurrentBuffer != VK_NULL_HANDLE; }
	bool MatchesSize(uint32_t width, uint32_t height) const
		{ return m_Width == width && m_Height == height; }

	uint32_t GetWidth()  const { return m_Width; }
	uint32_t GetHeight() const { return m_Height; }
	VkDeviceSize GetBufferSize() const { return m_BufferSize; }

	VkBuffer GetCurrentBuffer() const { return m_CurrentBuffer; }
	VkBuffer GetPrevBuffer()    const { return m_PrevBuffer; }

	// Swap current ↔ prev. Called at end of frame (after shading pass) so the
	// next frame's temporal reuse pass can read this frame's reservoirs as history.
	// No-op if not created.
	void Swap();

private:
	GpuDevice const* m_Device = nullptr;

	VkBuffer m_CurrentBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_CurrentBufferMemory = VK_NULL_HANDLE;
	VkBuffer m_PrevBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_PrevBufferMemory = VK_NULL_HANDLE;

	uint32_t m_Width = 0;
	uint32_t m_Height = 0;
	VkDeviceSize m_BufferSize = 0;
};