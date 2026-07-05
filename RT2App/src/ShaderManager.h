#pragma once

#ifndef SHADER_MANAGER_H
#define SHADER_MANAGER_H

#include "vulkan/vulkan.h"
#include <string>
#include <vector>

class ShaderManager
{
public:
	static void Init(VkDevice device);
	static VkShaderModule LoadShader(const std::string& filepath);
private:
	static VkDevice s_Device;
};

#endif // !SHADER_MANAGER_H