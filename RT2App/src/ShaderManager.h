#pragma once

#ifndef SHADER_MANAGER_H
#define SHADER_MANAGER_H

#include "vulkan/vulkan.h"
#include <string>
#include <vector>

class ShaderManager
{
public:
	static VkShaderModule LoadShader(const std::string& filepath);
};

#endif // !SHADER_MANAGER_H