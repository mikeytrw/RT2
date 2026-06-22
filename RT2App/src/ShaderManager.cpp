#include "ShaderManager.h"
#include "Walnut/Application.h"
#include <fstream>
#include <iostream>
#include <windows.h>

static std::string GetExeDirectory()
{
	char path[MAX_PATH];
	GetModuleFileNameA(nullptr, path, MAX_PATH);
	std::string fullPath(path);
	size_t pos = fullPath.find_last_of("\\/");
	return (pos != std::string::npos) ? fullPath.substr(0, pos) : ".";
}

VkShaderModule ShaderManager::LoadShader(const std::string& filepath)
{
	std::ifstream file(filepath, std::ios::binary | std::ios::ate);
	if (!file.is_open())
	{
		std::string absPath = GetExeDirectory() + "\\" + filepath;
		file.open(absPath, std::ios::binary | std::ios::ate);
		if (!file.is_open())
		{
			std::cerr << "[RT2] Failed to open shader file: " << filepath << "\n";
			std::cerr << "[RT2] Also tried: " << absPath << "\n";
			return VK_NULL_HANDLE;
		}
	}

	size_t fileSize = (size_t)file.tellg();
	file.seekg(0);
	std::vector<char> buffer(fileSize);
	file.read(buffer.data(), fileSize);
	file.close();

	VkShaderModuleCreateInfo createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.codeSize = buffer.size();
	createInfo.pCode = reinterpret_cast<const uint32_t*>(buffer.data());

	VkShaderModule shaderModule;
	VkResult err = vkCreateShaderModule(Walnut::Application::GetDevice(), &createInfo, nullptr, &shaderModule);
	if (err != VK_SUCCESS)
	{
		std::cerr << "[RT2] Failed to create shader module: " << err << "\n";
		return VK_NULL_HANDLE;
	}

	return shaderModule;
}