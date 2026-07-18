#pragma once

#include "GpuDevice.h"
#include "GpuResources.h"
#include "RenderInstanceMap.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>

class GpuPickingPass
{
public:
	static constexpr uint32_t MaxFrameSlots = 2;

	struct CompletedPick
	{
		uint64_t serial = 0;
		bool hit = false;
		uint32_t instanceIndex = 0;
		glm::vec3 worldPosition{ 0.0f };
		RenderInstanceMap instanceMap;
	};

	bool Init(const GpuDevice& device, VkDescriptorSetLayout sceneSetLayout,
	          uint32_t frameSlotCount);
	void Destroy();
	bool IsAvailable() const { return m_Pipeline != VK_NULL_HANDLE; }

	// Must be called after the matching frame fence has signalled and before
	// the slot is reused.
	std::optional<CompletedPick> ReadCompletedSlot(uint32_t frameSlot);

	void Record(VkCommandBuffer cmd, uint32_t frameSlot, VkDescriptorSet sceneSet,
	            const glm::vec3& origin, const glm::vec3& direction,
	            float tMax, uint64_t serial, const RenderInstanceMap& instanceMap);
	void Invalidate();

private:
	struct alignas(16) GpuResult
	{
		uint32_t hit = 0;
		uint32_t instanceIndex = 0;
		float hitT = 0.0f;
		uint32_t padding = 0;
		glm::vec4 worldPosition{ 0.0f };
	};

	struct Slot
	{
		GpuBuffer resultBuffer;
		VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
		uint64_t serial = 0;
		bool submitted = false;
		RenderInstanceMap instanceMap;
	};

	GpuDevice m_Device;
	VkDescriptorSetLayout m_ResultSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
	VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
	VkPipeline m_Pipeline = VK_NULL_HANDLE;
	std::array<Slot, MaxFrameSlots> m_Slots;
	uint32_t m_FrameSlotCount = 0;
};
