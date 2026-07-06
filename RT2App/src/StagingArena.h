#pragma once

#include "vulkan/vulkan.h"
#include <cstdint>

struct GpuDevice;

// StagingArena — transient host-visible buffer for batched GPU uploads.
//
// Purpose: replace N individual vkCreateBuffer + vkAllocateMemory + vkMapMemory
// calls (one per texture/buffer upload) with a single large allocation that
// is persistently mapped and sub-allocated via a bump allocator.
//
// Lifecycle:
//   1. Init(device, totalSize)  — one vkAllocateMemory, one persistent map
//   2. Alloc(size, alignment)   — bump cursor, return offset into buffer
//   3. memcpy into GetMappedPointer(offset)
//   4. Record vkCmdCopyBufferToImage/vkCmdCopyBuffer using GetBuffer() + offset
//   5. Reset() between batches, or Destroy() when done
//
// The arena does NOT track individual allocations. Reset() rewinds the cursor
// to zero — caller must ensure GPU work using previous offsets has completed
// before calling Reset().
//
class StagingArena
{
public:
	StagingArena() = default;
	~StagingArena();

	// Create a host-visible, host-coherent buffer of the given size.
	// The entire buffer is persistently mapped.
	bool Init(const GpuDevice& dev, VkDeviceSize size);

	// Destroy the buffer and free memory. Safe to call on uninitialized arena.
	void Destroy();

	// Rewind the bump cursor to zero. Caller must ensure all GPU work using
	// previous allocations has completed (e.g. via ImmediateSubmit).
	void Reset() { m_Cursor = 0; }

	// Allocate `size` bytes with the given alignment. Returns offset into
	// the buffer, or VK_WHOLE_SIZE if the arena is full.
	VkDeviceSize Alloc(VkDeviceSize size, VkDeviceSize alignment = 16);

	// Accessors
	VkBuffer       GetBuffer()        const { return m_Buffer; }
	VkDeviceSize   GetCapacity()       const { return m_Capacity; }
	VkDeviceSize   GetUsed()           const { return m_Cursor; }
	VkDeviceSize   GetFree()           const { return m_Capacity - m_Cursor; }
	void*          GetMappedPointer()  const { return m_Mapped; }
	void*          GetMappedPointer(VkDeviceSize offset) const { return static_cast<char*>(m_Mapped) + offset; }

	bool IsValid() const { return m_Buffer != VK_NULL_HANDLE; }

private:
	VkDevice       m_Device  = VK_NULL_HANDLE;
	VkBuffer       m_Buffer  = VK_NULL_HANDLE;
	VkDeviceMemory m_Memory  = VK_NULL_HANDLE;
	void*          m_Mapped  = nullptr;
	VkDeviceSize   m_Capacity = 0;
	VkDeviceSize   m_Cursor   = 0;
};