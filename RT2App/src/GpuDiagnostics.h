#pragma once

#include "vulkan/vulkan.h"
#include "GpuResources.h"
#include "shader_interface.h"
#include <array>
#include <cstdint>

struct GpuDevice;

// Small renderer-wide GPU counter buffer with one independent segment per
// frame in flight. Each completed segment is copied into a host-visible
// readback buffer before submission completes, so reading it never stalls the
// GPU or mixes counters from overlapping frames.
class GpuDiagnostics
{
public:
	static constexpr uint32_t MaxFrameSlots = SI_DIAGNOSTIC_FRAME_SLOTS;
	static constexpr uint32_t CounterCount = SI_DIAGNOSTIC_COUNTER_COUNT;

	struct Snapshot
	{
		uint64_t frameIndex = 0;
		bool valid = false;
		std::array<uint32_t, CounterCount> counters = {};
	};

	bool Init(const GpuDevice& device, uint32_t frameSlotCount);
	void Destroy(const GpuDevice& device);

	bool IsAvailable() const { return m_DeviceBuffer.IsValid(); }
	VkBuffer GetBuffer() const { return m_DeviceBuffer.buffer; }
	VkDeviceSize GetBufferSize() const { return m_DeviceBuffer.size; }
	VkDeviceSize GetAllocatedBytes() const;
	const Snapshot& GetLatest() const { return m_Latest; }

	// Read only after the matching frame fence has signalled.
	void ReadCompletedSlot(const GpuDevice& device, uint32_t frameSlot);
	void BeginFrame(VkCommandBuffer cmd, uint32_t frameSlot, uint64_t frameIndex);
	void EndFrame(VkCommandBuffer cmd, uint32_t frameSlot);

	static const char* CounterName(uint32_t counter);

private:
	static constexpr VkDeviceSize SlotSize = VkDeviceSize(CounterCount) * sizeof(uint32_t);

	struct Slot
	{
		uint64_t frameIndex = 0;
		bool submitted = false;
	};

	GpuBuffer m_DeviceBuffer;
	std::array<GpuBuffer, MaxFrameSlots> m_ReadbackBuffers = {};
	std::array<Slot, MaxFrameSlots> m_Slots = {};
	Snapshot m_Latest = {};
	uint32_t m_FrameSlotCount = 0;
};
