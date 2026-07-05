#pragma once

#include "vulkan/vulkan.h"
#include "RTLog.h"
#include <cstdlib>

// VK_CHECK — call a Vulkan function, check VkResult, log + abort on failure.
// Use for one-time setup calls where failure is unrecoverable.
// For per-frame calls where failure is expected (e.g. out of memory), check manually.
#define VK_CHECK(call) do { \
    VkResult _vk_result = (call); \
    if (_vk_result != VK_SUCCESS) { \
        RT_LOG("[VK_CHECK] %s failed: VkResult=%d (%s:%d)", #call, _vk_result, __FILE__, __LINE__); \
        std::abort(); \
    } \
} while (0)

// VK_CHECK_SOFT — call a Vulkan function, log on failure but don't abort.
// Returns the VkResult so the caller can handle it.
#define VK_CHECK_SOFT(call) \
    [&]() { VkResult _r = (call); if (_r != VK_SUCCESS) RT_LOG("[VK] %s failed: %d (%s:%d)", #call, _r, __FILE__, __LINE__); return _r; }()