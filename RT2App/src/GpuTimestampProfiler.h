#pragma once

#include "vulkan/vulkan.h"
#include <array>
#include <cstdint>

struct GpuDevice;

class GpuTimestampProfiler
{
public:
	enum class Region : uint32_t
	{
		Frame,
		Raster,
		ReSTIRDITemporal,
		ReSTIRDISpatial,
		ReSTIRGITemporal,
		ReSTIRGIHistory,
		RTShading,
		NRD,
		Compose,
		Tonemap,
		Count
	};

	struct Timings
	{
		uint64_t frameIndex = 0;
		uint32_t validMask = 0;
		std::array<float, static_cast<size_t>(Region::Count)> milliseconds = {};
	};

	bool Init(const GpuDevice& device, uint32_t frameSlotCount);
	void Destroy(VkDevice device);

	bool IsAvailable() const { return m_QueryPool != VK_NULL_HANDLE; }
	const Timings& GetLatest() const { return m_Latest; }
	static const char* RegionName(Region region);

	// Call only after the slot's render fence has signaled, before re-recording it.
	void ReadCompletedSlot(VkDevice device, uint32_t frameSlot);
	void BeginFrame(VkCommandBuffer cmd, uint32_t frameSlot, uint64_t frameIndex);
	void EndFrame(uint32_t frameSlot);
	void BeginRegion(VkCommandBuffer cmd, Region region, VkPipelineStageFlagBits stage);
	void EndRegion(VkCommandBuffer cmd, Region region, VkPipelineStageFlagBits stage);

private:
	static constexpr uint32_t RegionCount = static_cast<uint32_t>(Region::Count);
	static constexpr uint32_t QueriesPerRegion = 2;
	static constexpr uint32_t QueriesPerSlot = RegionCount * QueriesPerRegion;

	struct Slot
	{
		uint64_t frameIndex = 0;
		uint64_t submitOrder = 0; // monotonically increasing; never resets
		uint32_t issuedMask = 0;
		uint32_t activeMask = 0;
		bool submitted = false;
	};

	uint32_t QueryIndex(uint32_t frameSlot, Region region, bool end) const;
	uint64_t TickDelta(uint64_t begin, uint64_t end) const;

	VkQueryPool m_QueryPool = VK_NULL_HANDLE;
	std::array<Slot, 2> m_Slots = {};
	Timings m_Latest = {};
	uint64_t m_LatestSubmitOrder = 0; // monotonic; guards merge ordering
	uint64_t m_SubmitCounter = 0;     // monotonic; assigned to each EndFrame
	uint32_t m_FrameSlotCount = 0;
	uint32_t m_ActiveFrameSlot = 0;
	uint32_t m_TimestampValidBits = 0;
	float m_TimestampPeriod = 0.0f;
};
