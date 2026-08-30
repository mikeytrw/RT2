#include <vulkan/vulkan.h>

#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers_dlssd.h>
#include <nvsdk_ngx_helpers_dlssd_vk.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr const char* kProjectId = "41f7cbd8-e97e-49f0-b41d-14a4f5b547f8";
constexpr const char* kEngineVersion = "RT2-A0";
constexpr uint32_t kOutputWidth = 1280;
constexpr uint32_t kOutputHeight = 720;

struct Options
{
    std::string projectId = kProjectId;
    std::optional<std::filesystem::path> featurePath;
    std::optional<uint32_t> simulatedSupportMask;
    uint32_t vendorId = 0x10de;
    bool validation = false;
};

Options ParseOptions(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--validation")
            options.validation = true;
        else if (argument.starts_with("--project-id="))
            options.projectId = argument.substr(std::string("--project-id=").size());
        else if (argument.starts_with("--feature-path="))
            options.featurePath = std::filesystem::path(argument.substr(std::string("--feature-path=").size()));
        else if (argument.starts_with("--vendor-id="))
            options.vendorId = static_cast<uint32_t>(std::stoul(argument.substr(std::string("--vendor-id=").size()), nullptr, 0));
        else if (argument.starts_with("--simulate-support-mask="))
            options.simulatedSupportMask = static_cast<uint32_t>(
                std::stoul(argument.substr(std::string("--simulate-support-mask=").size()), nullptr, 0));
        else
            throw std::runtime_error("Unknown argument: " + argument);
    }
    return options;
}

std::atomic_uint32_t gValidationErrors{0};

VKAPI_ATTR VkBool32 VKAPI_CALL ValidationCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                   VkDebugUtilsMessageTypeFlagsEXT,
                                                   const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
                                                   void*)
{
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
        ++gValidationErrors;
    std::cerr << "[Vulkan validation] " << (callbackData && callbackData->pMessage ? callbackData->pMessage : "<no message>")
              << '\n';
    return VK_FALSE;
}

std::string Narrow(const std::wstring& value)
{
    if (value.empty())
        return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0)
        return "<wide-string conversion failed>";
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}

std::string NgxResultText(NVSDK_NGX_Result result)
{
    const wchar_t* text = GetNGXResultAsString(result);
    return text ? Narrow(text) : "unknown NGX result";
}

void CheckNgx(NVSDK_NGX_Result result, const char* operation)
{
    if (NVSDK_NGX_FAILED(result))
        throw std::runtime_error(std::string(operation) + " failed: " + NgxResultText(result));
}

void CheckVk(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed with VkResult " + std::to_string(result));
}

std::string SupportMaskText(uint32_t mask)
{
    if (mask == 0)
        return "supported";
    std::vector<std::string> reasons;
    if ((mask & NVSDK_NGX_FeatureSupportResult_CheckNotPresent) != 0)
        reasons.emplace_back("support check unavailable");
    if ((mask & NVSDK_NGX_FeatureSupportResult_DriverVersionUnsupported) != 0)
        reasons.emplace_back("driver version unsupported");
    if ((mask & NVSDK_NGX_FeatureSupportResult_AdapterUnsupported) != 0)
        reasons.emplace_back("adapter unsupported");
    if ((mask & NVSDK_NGX_FeatureSupportResult_OSVersionBelowMinimumSupported) != 0)
        reasons.emplace_back("OS version below minimum");
    if ((mask & NVSDK_NGX_FeatureSupportResult_NotImplemented) != 0)
        reasons.emplace_back("feature not implemented");
    const uint32_t known = NVSDK_NGX_FeatureSupportResult_CheckNotPresent |
                           NVSDK_NGX_FeatureSupportResult_DriverVersionUnsupported |
                           NVSDK_NGX_FeatureSupportResult_AdapterUnsupported |
                           NVSDK_NGX_FeatureSupportResult_OSVersionBelowMinimumSupported |
                           NVSDK_NGX_FeatureSupportResult_NotImplemented;
    if ((mask & ~known) != 0)
        reasons.emplace_back("unknown support bits 0x" + [&] {
            std::ostringstream stream;
            stream << std::hex << (mask & ~known);
            return stream.str();
        }());
    std::ostringstream text;
    for (size_t i = 0; i < reasons.size(); ++i)
    {
        if (i)
            text << ", ";
        text << reasons[i];
    }
    return text.str();
}

std::filesystem::path ExecutableDirectory()
{
    std::wstring path(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (count == 0 || count == path.size())
        throw std::runtime_error("GetModuleFileNameW failed");
    path.resize(count);
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path ApplicationDataDirectory()
{
    wchar_t* local = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&local, &length, L"LOCALAPPDATA") != 0 || !local || length <= 1)
        throw std::runtime_error("LOCALAPPDATA is unavailable");
    const std::filesystem::path result = std::filesystem::path(local) / L"RT2" / L"NGX" / L"a0-probe";
    std::free(local);
    std::error_code error;
    std::filesystem::create_directories(result, error);
    if (error)
        throw std::runtime_error("Cannot create NGX application-data directory: " + error.message());
    return result;
}

NVSDK_NGX_FeatureDiscoveryInfo DiscoveryInfo(const char* projectId,
                                              const NVSDK_NGX_FeatureCommonInfo* common,
                                              const std::filesystem::path& applicationData)
{
    NVSDK_NGX_FeatureDiscoveryInfo info{};
    info.SDKVersion = NVSDK_NGX_Version_API;
    info.FeatureID = NVSDK_NGX_Feature_RayReconstruction;
    info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    info.Identifier.v.ProjectDesc.ProjectId = projectId;
    info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    info.Identifier.v.ProjectDesc.EngineVersion = kEngineVersion;
    info.ApplicationDataPath = applicationData.c_str();
    info.FeatureInfo = common;
    return info;
}

std::vector<std::string> RequiredInstanceExtensions(const NVSDK_NGX_FeatureDiscoveryInfo& discovery)
{
    uint32_t count = 0;
    VkExtensionProperties* properties = nullptr;
    CheckNgx(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&discovery, &count, &properties),
             "NGX instance-extension discovery");
    std::set<std::string> unique;
    for (uint32_t i = 0; i < count; ++i)
        unique.emplace(properties[i].extensionName);
    return {unique.begin(), unique.end()};
}

std::vector<std::string> RequiredDeviceExtensions(VkInstance instance,
                                                  VkPhysicalDevice physicalDevice,
                                                  const NVSDK_NGX_FeatureDiscoveryInfo& discovery)
{
    uint32_t count = 0;
    VkExtensionProperties* properties = nullptr;
    CheckNgx(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
                 instance, physicalDevice, &discovery, &count, &properties),
             "NGX device-extension discovery");
    std::set<std::string> unique;
    for (uint32_t i = 0; i < count; ++i)
        unique.emplace(properties[i].extensionName);
    return {unique.begin(), unique.end()};
}

template <typename T>
std::vector<const char*> CStringViews(const std::vector<T>& strings)
{
    std::vector<const char*> views;
    views.reserve(strings.size());
    for (const auto& value : strings)
        views.push_back(value.c_str());
    return views;
}

void RequireInstanceExtensions(std::span<const std::string> required)
{
    uint32_t count = 0;
    CheckVk(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr), "vkEnumerateInstanceExtensionProperties(count)");
    std::vector<VkExtensionProperties> available(count);
    CheckVk(vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data()), "vkEnumerateInstanceExtensionProperties");
    for (const auto& name : required)
    {
        const bool found = std::ranges::any_of(available, [&](const auto& property) { return name == property.extensionName; });
        if (!found)
            throw std::runtime_error("Required Vulkan instance extension is unavailable: " + name);
    }
}

void RequireDeviceExtensions(VkPhysicalDevice physicalDevice, std::span<const std::string> required)
{
    uint32_t count = 0;
    CheckVk(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr),
            "vkEnumerateDeviceExtensionProperties(count)");
    std::vector<VkExtensionProperties> available(count);
    CheckVk(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, available.data()),
            "vkEnumerateDeviceExtensionProperties");
    for (const auto& name : required)
    {
        const bool found = std::ranges::any_of(available, [&](const auto& property) { return name == property.extensionName; });
        if (!found)
            throw std::runtime_error("Required Vulkan device extension is unavailable: " + name);
    }
}

uint32_t GraphicsQueueFamily(VkPhysicalDevice physicalDevice)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, families.data());
    for (uint32_t i = 0; i < count; ++i)
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
            return i;
    throw std::runtime_error("Selected Vulkan device has no graphics queue");
}

VkPhysicalDevice SelectDevice(VkInstance instance, uint32_t vendorId)
{
    uint32_t count = 0;
    CheckVk(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices(count)");
    std::vector<VkPhysicalDevice> devices(count);
    CheckVk(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
    for (VkPhysicalDevice device : devices)
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.vendorID == vendorId)
            return device;
    }
    throw std::runtime_error("No Vulkan physical device found for vendor ID 0x" + [&] {
        std::ostringstream stream;
        stream << std::hex << vendorId;
        return stream.str();
    }());
}

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
};

uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) != 0 && (properties.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    throw std::runtime_error("No compatible Vulkan memory type found");
}

Image CreateImage(VkPhysicalDevice physicalDevice, VkDevice device, VkExtent2D extent, VkFormat format)
{
    Image result{};
    result.format = format;
    result.extent = extent;
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CheckVk(vkCreateImage(device, &imageInfo, nullptr, &result.image), "vkCreateImage");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, result.image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = FindMemoryType(physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CheckVk(vkAllocateMemory(device, &allocation, nullptr, &result.memory), "vkAllocateMemory(image)");
    CheckVk(vkBindImageMemory(device, result.image, result.memory, 0), "vkBindImageMemory");

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = result.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    CheckVk(vkCreateImageView(device, &viewInfo, nullptr, &result.view), "vkCreateImageView");
    return result;
}

void DestroyImage(VkDevice device, Image& image)
{
    if (image.view)
        vkDestroyImageView(device, image.view, nullptr);
    if (image.image)
        vkDestroyImage(device, image.image, nullptr);
    if (image.memory)
        vkFreeMemory(device, image.memory, nullptr);
    image = {};
}

VkCommandBuffer BeginOneShot(VkDevice device, VkCommandPool pool)
{
    CheckVk(vkResetCommandPool(device, pool, 0), "vkResetCommandPool");
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    CheckVk(vkAllocateCommandBuffers(device, &allocation, &command), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CheckVk(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
    return command;
}

void EndSubmitWait(VkQueue queue, VkCommandBuffer command)
{
    CheckVk(vkEndCommandBuffer(command), "vkEndCommandBuffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    CheckVk(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
    CheckVk(vkQueueWaitIdle(queue), "vkQueueWaitIdle");
}

void Transition(VkCommandBuffer command,
                VkImage image,
                VkImageLayout oldLayout,
                VkImageLayout newLayout,
                VkAccessFlags sourceAccess,
                VkAccessFlags destinationAccess,
                VkPipelineStageFlags sourceStage,
                VkPipelineStageFlags destinationStage)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void PrepareInput(VkCommandBuffer command, const Image& image, const VkClearColorValue& clear)
{
    Transition(command, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vkCmdClearColorImage(command, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    Transition(command, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

void PrepareOutput(VkCommandBuffer command, const Image& image)
{
    Transition(command, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

NVSDK_NGX_Resource_VK AsNgxResource(const Image& image, bool readWrite)
{
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    return NVSDK_NGX_Create_ImageView_Resource_VK(
        image.view, image.image, range, image.format, image.extent.width, image.extent.height, readWrite);
}

struct ReadbackBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

ReadbackBuffer CreateReadback(VkPhysicalDevice physicalDevice, VkDevice device, VkDeviceSize size)
{
    ReadbackBuffer result{};
    result.size = size;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CheckVk(vkCreateBuffer(device, &info, nullptr, &result.buffer), "vkCreateBuffer(readback)");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = FindMemoryType(physicalDevice, requirements.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CheckVk(vkAllocateMemory(device, &allocation, nullptr, &result.memory), "vkAllocateMemory(readback)");
    CheckVk(vkBindBufferMemory(device, result.buffer, result.memory, 0), "vkBindBufferMemory(readback)");
    return result;
}

uint64_t ReadbackHash(VkDevice device, const ReadbackBuffer& readback, size_t& nonzeroBytes)
{
    void* mapped = nullptr;
    CheckVk(vkMapMemory(device, readback.memory, 0, readback.size, 0, &mapped), "vkMapMemory(readback)");
    const auto bytes = std::span<const uint8_t>(static_cast<const uint8_t*>(mapped), static_cast<size_t>(readback.size));
    uint64_t hash = 1469598103934665603ull;
    nonzeroBytes = 0;
    for (uint8_t byte : bytes)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
        nonzeroBytes += byte != 0;
    }
    vkUnmapMemory(device, readback.memory);
    return hash;
}

void PrintExtensions(const char* label, std::span<const std::string> extensions)
{
    std::cout << label << " (" << extensions.size() << ")\n";
    for (const auto& extension : extensions)
        std::cout << "  " << extension << '\n';
}
} // namespace

int main(int argc, char** argv)
{
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    NVSDK_NGX_Parameter* parameters = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    std::vector<Image> images;
    ReadbackBuffer readback{};
    bool ngxInitialized = false;

    try
    {
        const Options options = ParseOptions(argc, argv);
        const std::filesystem::path executableDirectory = ExecutableDirectory();
        const std::filesystem::path applicationData = ApplicationDataDirectory();
        const std::wstring featurePath = options.featurePath.value_or(executableDirectory).wstring();
        const wchar_t* featurePaths[] = {featurePath.c_str()};
        NVSDK_NGX_FeatureCommonInfo common{};
        common.PathListInfo.Path = featurePaths;
        common.PathListInfo.Length = 1;
        common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
        const NVSDK_NGX_FeatureDiscoveryInfo discovery = DiscoveryInfo(options.projectId.c_str(), &common, applicationData);

        std::vector<std::string> instanceExtensions = RequiredInstanceExtensions(discovery);
        if (options.validation && std::ranges::find(instanceExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == instanceExtensions.end())
            instanceExtensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        RequireInstanceExtensions(instanceExtensions);
        PrintExtensions("Required instance extensions", instanceExtensions);

        const std::vector<const char*> instanceExtensionViews = CStringViews(instanceExtensions);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "RT2 DLSS RR A0 Probe";
        app.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
        app.pEngineName = "RT2";
        app.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        const VkValidationFeatureEnableEXT validationEnable = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkDebugUtilsMessengerCreateInfoEXT debugInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        const char* validationLayer = "VK_LAYER_KHRONOS_validation";
        if (options.validation)
        {
            uint32_t layerCount = 0;
            CheckVk(vkEnumerateInstanceLayerProperties(&layerCount, nullptr), "vkEnumerateInstanceLayerProperties(count)");
            std::vector<VkLayerProperties> layers(layerCount);
            CheckVk(vkEnumerateInstanceLayerProperties(&layerCount, layers.data()), "vkEnumerateInstanceLayerProperties");
            if (!std::ranges::any_of(layers, [&](const auto& layer) { return std::string_view(layer.layerName) == validationLayer; }))
                throw std::runtime_error("VK_LAYER_KHRONOS_validation is unavailable");
            validationFeatures.enabledValidationFeatureCount = 1;
            validationFeatures.pEnabledValidationFeatures = &validationEnable;
            debugInfo.pNext = &validationFeatures;
            debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debugInfo.pfnUserCallback = ValidationCallback;
            instanceInfo.pNext = &debugInfo;
            instanceInfo.enabledLayerCount = 1;
            instanceInfo.ppEnabledLayerNames = &validationLayer;
        }
        instanceInfo.pApplicationInfo = &app;
        instanceInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensionViews.size());
        instanceInfo.ppEnabledExtensionNames = instanceExtensionViews.data();
        CheckVk(vkCreateInstance(&instanceInfo, nullptr, &instance), "vkCreateInstance");
        if (options.validation)
        {
            const auto createDebug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            if (!createDebug)
                throw std::runtime_error("vkCreateDebugUtilsMessengerEXT is unavailable");
            CheckVk(createDebug(instance, &debugInfo, nullptr, &debugMessenger), "vkCreateDebugUtilsMessengerEXT");
        }

        const VkPhysicalDevice physicalDevice = SelectDevice(instance, options.vendorId);
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);
        std::cout << "Physical device: " << properties.deviceName << "\n";
        std::cout << "Vulkan API: " << VK_VERSION_MAJOR(properties.apiVersion) << '.'
                  << VK_VERSION_MINOR(properties.apiVersion) << '.' << VK_VERSION_PATCH(properties.apiVersion) << "\n";

        NVSDK_NGX_FeatureRequirement requirement{};
        CheckNgx(NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physicalDevice, &discovery, &requirement),
                 "NGX Ray Reconstruction feature requirement query");
        const uint32_t supportMask = options.simulatedSupportMask.value_or(
            static_cast<uint32_t>(requirement.FeatureSupported));
        if (supportMask != NVSDK_NGX_FeatureSupportResult_Supported)
            throw std::runtime_error("Ray Reconstruction is not supported: " + SupportMaskText(supportMask) +
                                     " (mask=" + std::to_string(supportMask) + ")");
        std::cout << "Ray Reconstruction support: supported\n";

        const std::vector<std::string> deviceExtensions = RequiredDeviceExtensions(instance, physicalDevice, discovery);
        RequireDeviceExtensions(physicalDevice, deviceExtensions);
        PrintExtensions("Required device extensions", deviceExtensions);
        const std::vector<const char*> deviceExtensionViews = CStringViews(deviceExtensions);

        const uint32_t queueFamily = GraphicsQueueFamily(physicalDevice);
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        features12.pNext = &features13;
        VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        supported.pNext = &features12;
        vkGetPhysicalDeviceFeatures2(physicalDevice, &supported);
        features12.timelineSemaphore = features12.timelineSemaphore ? VK_TRUE : VK_FALSE;
        features13.synchronization2 = features13.synchronization2 ? VK_TRUE : VK_FALSE;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.pNext = &features12;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensionViews.size());
        deviceInfo.ppEnabledExtensionNames = deviceExtensionViews.data();
        CheckVk(vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device), "vkCreateDevice");
        VkQueue queue = VK_NULL_HANDLE;
        vkGetDeviceQueue(device, queueFamily, 0, &queue);

        NVSDK_NGX_FeatureCommonInfo initCommon = common;
        CheckNgx(NVSDK_NGX_VULKAN_Init_with_ProjectID(
                     options.projectId.c_str(), NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, applicationData.c_str(), instance,
                     physicalDevice, device, vkGetInstanceProcAddr, vkGetDeviceProcAddr, &initCommon),
                 "NVSDK_NGX_VULKAN_Init_with_ProjectID");
        ngxInitialized = true;
        CheckNgx(NVSDK_NGX_VULKAN_GetCapabilityParameters(&parameters), "NVSDK_NGX_VULKAN_GetCapabilityParameters");

        uint32_t renderWidth = 0;
        uint32_t renderHeight = 0;
        uint32_t maxWidth = 0;
        uint32_t maxHeight = 0;
        uint32_t minWidth = 0;
        uint32_t minHeight = 0;
        float sharpness = 0.0f;
        CheckNgx(NGX_DLSSD_GET_OPTIMAL_SETTINGS(parameters, kOutputWidth, kOutputHeight,
                                                NVSDK_NGX_PerfQuality_Value_MaxQuality, &renderWidth, &renderHeight,
                                                &maxWidth, &maxHeight, &minWidth, &minHeight, &sharpness),
                 "NGX_DLSSD_GET_OPTIMAL_SETTINGS");
        if (renderWidth == 0 || renderHeight == 0 || minWidth == 0 || minHeight == 0 || maxWidth == 0 || maxHeight == 0)
            throw std::runtime_error("NGX returned invalid render dimensions");
        std::cout << "Quality dimensions: render=" << renderWidth << 'x' << renderHeight << " output="
                  << kOutputWidth << 'x' << kOutputHeight << " min=" << minWidth << 'x' << minHeight << " max="
                  << maxWidth << 'x' << maxHeight << "\n";

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily;
        CheckVk(vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool), "vkCreateCommandPool");

        NVSDK_NGX_DLSSD_Create_Params create{};
        create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
        create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
        create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_Linear;
        create.InWidth = renderWidth;
        create.InHeight = renderHeight;
        create.InTargetWidth = kOutputWidth;
        create.InTargetHeight = kOutputHeight;
        create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        create.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxQuality;

        parameters->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,
                        NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
        VkCommandBuffer command = BeginOneShot(device, commandPool);
        CheckNgx(NGX_VULKAN_CREATE_DLSSD_EXT1(device, command, 1, 1, &feature, parameters, &create),
                 "NGX_VULKAN_CREATE_DLSSD_EXT1");
        EndSubmitWait(queue, command);
        std::cout << "Ray Reconstruction feature creation: success\n";

        const VkExtent2D renderExtent{renderWidth, renderHeight};
        const VkExtent2D outputExtent{kOutputWidth, kOutputHeight};
        images.push_back(CreateImage(physicalDevice, device, renderExtent, VK_FORMAT_R16G16B16A16_SFLOAT)); // color
        images.push_back(CreateImage(physicalDevice, device, renderExtent, VK_FORMAT_R8G8B8A8_UNORM));       // diffuse
        images.push_back(CreateImage(physicalDevice, device, renderExtent, VK_FORMAT_R8G8B8A8_UNORM));       // specular
        images.push_back(CreateImage(physicalDevice, device, renderExtent, VK_FORMAT_R16G16B16A16_SFLOAT)); // normal/roughness
        images.push_back(CreateImage(physicalDevice, device, renderExtent, VK_FORMAT_R32_SFLOAT));           // view depth
        images.push_back(CreateImage(physicalDevice, device, renderExtent, VK_FORMAT_R16G16_SFLOAT));        // motion
        images.push_back(CreateImage(physicalDevice, device, renderExtent, VK_FORMAT_R32_SFLOAT));           // specular hit distance
        images.push_back(CreateImage(physicalDevice, device, outputExtent, VK_FORMAT_R16G16B16A16_SFLOAT)); // output

        command = BeginOneShot(device, commandPool);
        PrepareInput(command, images[0], VkClearColorValue{{0.25f, 0.1f, 0.05f, 1.0f}});
        PrepareInput(command, images[1], VkClearColorValue{{0.5f, 0.3f, 0.2f, 1.0f}});
        PrepareInput(command, images[2], VkClearColorValue{{0.04f, 0.04f, 0.04f, 1.0f}});
        PrepareInput(command, images[3], VkClearColorValue{{0.0f, 0.0f, 1.0f, 0.5f}});
        PrepareInput(command, images[4], VkClearColorValue{{1.0f, 0.0f, 0.0f, 0.0f}});
        PrepareInput(command, images[5], VkClearColorValue{{0.0f, 0.0f, 0.0f, 0.0f}});
        PrepareInput(command, images[6], VkClearColorValue{{1.0f, 0.0f, 0.0f, 0.0f}});
        PrepareOutput(command, images[7]);

        auto color = AsNgxResource(images[0], false);
        auto diffuse = AsNgxResource(images[1], false);
        auto specular = AsNgxResource(images[2], false);
        auto normalRoughness = AsNgxResource(images[3], false);
        auto depth = AsNgxResource(images[4], false);
        auto motion = AsNgxResource(images[5], false);
        auto specularHitDistance = AsNgxResource(images[6], false);
        auto output = AsNgxResource(images[7], true);
        std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        NVSDK_NGX_VK_DLSSD_Eval_Params evaluate{};
        evaluate.pInColor = &color;
        evaluate.pInOutput = &output;
        evaluate.pInDiffuseAlbedo = &diffuse;
        evaluate.pInSpecularAlbedo = &specular;
        evaluate.pInNormals = &normalRoughness;
        evaluate.pInRoughness = &normalRoughness;
        evaluate.pInDepth = &depth;
        evaluate.pInMotionVectors = &motion;
        evaluate.pInSpecularHitDistance = &specularHitDistance;
        evaluate.InRenderSubrectDimensions = {renderWidth, renderHeight};
        evaluate.InReset = 1;
        evaluate.InMVScaleX = 1.0f;
        evaluate.InMVScaleY = 1.0f;
        evaluate.InPreExposure = 1.0f;
        evaluate.InExposureScale = 1.0f;
        evaluate.pInWorldToViewMatrix = identity.data();
        evaluate.pInViewToClipMatrix = identity.data();
        CheckNgx(NGX_VULKAN_EVALUATE_DLSSD_EXT(command, feature, parameters, &evaluate),
                 "NGX_VULKAN_EVALUATE_DLSSD_EXT");
        EndSubmitWait(queue, command);
        std::cout << "Ray Reconstruction evaluation: success\n";

        readback = CreateReadback(physicalDevice, device, static_cast<VkDeviceSize>(kOutputWidth) * kOutputHeight * 8u);
        command = BeginOneShot(device, commandPool);
        Transition(command, images[7].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {kOutputWidth, kOutputHeight, 1};
        vkCmdCopyImageToBuffer(command, images[7].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
        EndSubmitWait(queue, command);
        size_t nonzeroBytes = 0;
        const uint64_t hash = ReadbackHash(device, readback, nonzeroBytes);
        if (nonzeroBytes == 0)
            throw std::runtime_error("Ray Reconstruction output readback is entirely zero");
        std::cout << "Output readback: nonzeroBytes=" << nonzeroBytes << " fnv1a64=0x" << std::hex << hash << std::dec
                  << "\n";

        vkDeviceWaitIdle(device);
        const NVSDK_NGX_Result releaseResult = NVSDK_NGX_VULKAN_ReleaseFeature(feature);
        feature = nullptr;
        CheckNgx(releaseResult, "NVSDK_NGX_VULKAN_ReleaseFeature");
        std::cout << "Ray Reconstruction feature release: success\n";
        const NVSDK_NGX_Result destroyParametersResult = NVSDK_NGX_VULKAN_DestroyParameters(parameters);
        parameters = nullptr;
        CheckNgx(destroyParametersResult, "NVSDK_NGX_VULKAN_DestroyParameters");
        const NVSDK_NGX_Result shutdownResult = NVSDK_NGX_VULKAN_Shutdown1(device);
        ngxInitialized = false;
        CheckNgx(shutdownResult, "NVSDK_NGX_VULKAN_Shutdown1");
        std::cout << "NGX shutdown: success\n";
        for (Image& image : images)
            DestroyImage(device, image);
        vkDestroyBuffer(device, readback.buffer, nullptr);
        vkFreeMemory(device, readback.memory, nullptr);
        vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
        if (debugMessenger)
        {
            const auto destroyDebug = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroyDebug)
                destroyDebug(instance, debugMessenger, nullptr);
            debugMessenger = VK_NULL_HANDLE;
        }
        if (options.validation)
        {
            const uint32_t validationErrors = gValidationErrors.load();
            std::cout << "Vulkan validation errors: " << validationErrors << "\n";
            if (validationErrors != 0)
                throw std::runtime_error("Vulkan validation reported " + std::to_string(validationErrors) + " error(s)");
        }
        vkDestroyInstance(instance, nullptr);
        std::cout << "A0_RESULT=PASS\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "A0_RESULT=FAIL\n" << error.what() << '\n';
        if (device)
            vkDeviceWaitIdle(device);
        if (feature)
            NVSDK_NGX_VULKAN_ReleaseFeature(feature);
        if (parameters)
            NVSDK_NGX_VULKAN_DestroyParameters(parameters);
        if (ngxInitialized && device)
            NVSDK_NGX_VULKAN_Shutdown1(device);
        if (device)
        {
            for (Image& image : images)
                DestroyImage(device, image);
            if (readback.buffer)
                vkDestroyBuffer(device, readback.buffer, nullptr);
            if (readback.memory)
                vkFreeMemory(device, readback.memory, nullptr);
            if (commandPool)
                vkDestroyCommandPool(device, commandPool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (debugMessenger && instance)
        {
            const auto destroyDebug = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroyDebug)
                destroyDebug(instance, debugMessenger, nullptr);
        }
        if (instance)
            vkDestroyInstance(instance, nullptr);
        return 1;
    }
}
