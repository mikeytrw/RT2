#include "NRDIntegration.h"
#include "RTLog.h"
#include "Walnut/Application.h"

// Include NRI + NRD headers
#include "NRI.h"
#include "Extensions/NRIRayTracing.h" // AccelerationStructureBits (needed before NRIWrapperVK.h)
#include "Extensions/NRIWrapperVK.h"
#include "Extensions/NRIHelper.h"

#include "NRD.h"
#include "NRDIntegration.h"  // nrd::Integration
#include "NRDIntegration.hpp" // implementation

static nrd::Integration g_NRD;

NRDWrapper::NRDWrapper() = default;
NRDWrapper::~NRDWrapper() { Destroy(); }

void NRDWrapper::Destroy()
{
	if (!m_Initialized)
		return;

	g_NRD.Destroy();

	if (m_NRIDevice)
	{
		nri::nriDestroyDevice(m_NRIDevice);
		m_NRIDevice = nullptr;
	}

	m_Initialized = false;
}

bool NRDWrapper::Init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
                      VkQueue queue, uint32_t queueFamily, uint32_t width, uint32_t height)
{
	RT_LOG("[NRD] Init: w=%u h=%u queueFamily=%u", width, height, queueFamily);

	// Create NRI device from existing Vulkan device
	nri::QueueFamilyVKDesc queueFamilyDesc = {};
	queueFamilyDesc.queueNum = 1;
	queueFamilyDesc.queueType = nri::QueueType::GRAPHICS;
	queueFamilyDesc.familyIndex = queueFamily;

	nri::DeviceCreationVKDesc deviceDesc = {};
	deviceDesc.vkInstance = instance;
	deviceDesc.vkPhysicalDevice = physicalDevice;
	deviceDesc.vkDevice = device;
	deviceDesc.queueFamilies = &queueFamilyDesc;
	deviceDesc.queueFamilyNum = 1;
	deviceDesc.minorVersion = 2; // Vulkan 1.2
	// NRI needs to know our binding offsets to avoid collisions.
	// Our path tracer uses bindings 0-10 in set 0. NRD uses its own descriptor
	// pool and pipeline layout, so binding offsets don't matter much, but NRI
	// still needs valid values. We use high offsets to avoid collisions.
	deviceDesc.vkBindingOffsets.sRegister = 0;
	deviceDesc.vkBindingOffsets.tRegister = 0;
	deviceDesc.vkBindingOffsets.bRegister = 0;
	deviceDesc.vkBindingOffsets.uRegister = 0;
	// NRI needs to know which extensions are enabled on our VkDevice
	static const char* deviceExts[] = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
		VK_KHR_RAY_QUERY_EXTENSION_NAME,
		VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
		VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
		VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
		VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME,
		VK_KHR_SPIRV_1_4_EXTENSION_NAME,
		VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
		VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME,
		VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
		VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
		VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
		VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
		VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME,
	};
	deviceDesc.vkExtensions.deviceExtensions = deviceExts;
	deviceDesc.vkExtensions.deviceExtensionNum = (uint32_t)(sizeof(deviceExts) / sizeof(deviceExts[0]));
	deviceDesc.enableMemoryZeroInitialization = true;

	// Provide a callback that logs instead of DebugBreak()
	nri::CallbackInterface callbacks = {};
	callbacks.MessageCallback = [](nri::Message messageType, const char* file, uint32_t line, const char* message, void*) {
		const char* typeStr = "INFO";
		if (messageType == nri::Message::WARNING) typeStr = "WARNING";
		else if (messageType == nri::Message::ERROR) typeStr = "ERROR";
		RT_LOG("[NRD] NRI %s: %s:%u: %s", typeStr, file, line, message);
	};
	// Don't set AbortExecution — NRI default calls DebugBreak(), we want to
	// just log errors and handle them via return codes instead.
	callbacks.AbortExecution = [](void*) -> void { /* no-op: don't crash */ };
	deviceDesc.callbackInterface = callbacks;

	RT_LOG("[NRD] calling nriCreateDeviceFromVKDevice...");
	nri::Result devResult = nri::nriCreateDeviceFromVKDevice(deviceDesc, m_NRIDevice);
	RT_LOG("[NRD] nriCreateDeviceFromVKDevice returned %d", (int)devResult);
	if (devResult != nri::Result::SUCCESS)
	{
		RT_LOG("[NRD] Failed to create NRI device from Vulkan device");
		return false;
	}
	RT_LOG("[NRD] NRI device created successfully");

	// Create NRD instance with REBLUR_DIFFUSE_SPECULAR
	nrd::DenoiserDesc denoiserDesc = {};
	denoiserDesc.denoiser = nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;

	m_DenoiserID = 0; // identifier for this denoiser

	nrd::InstanceCreationDesc instanceDesc = {};
	instanceDesc.denoisers = &denoiserDesc;
	instanceDesc.denoisersNum = 1;

	nrd::IntegrationCreationDesc integrationDesc = {};
	strncpy(integrationDesc.name, "RT2 NRD", sizeof(integrationDesc.name));
	integrationDesc.resourceWidth = (uint16_t)width;
	integrationDesc.resourceHeight = (uint16_t)height;
	integrationDesc.queuedFrameNum = 3;
	integrationDesc.enableWholeLifetimeDescriptorCaching = false;
	integrationDesc.autoWaitForIdle = true;

	RT_LOG("[NRD] calling RecreateVK...");
	nrd::Result rr = g_NRD.RecreateVK(integrationDesc, instanceDesc, deviceDesc);
	RT_LOG("[NRD] RecreateVK returned %d", (int)rr);
	if (rr != nrd::Result::SUCCESS)
	{
		RT_LOG("[NRD] RecreateVK failed");
		nri::nriDestroyDevice(m_NRIDevice);
		m_NRIDevice = nullptr;
		return false;
	}

	m_Width = width;
	m_Height = height;
	m_Initialized = true;

	RT_LOG("[NRD] Init successful (REBLUR_DIFFUSE_SPECULAR, %ux%u)", width, height);
	return true;
}

void NRDWrapper::OnResize(uint32_t width, uint32_t height)
{
	if (!m_Initialized || (width == m_Width && height == m_Height))
		return;

	// NRD doesn't support resize — must destroy and recreate
	VkInstance instance = Walnut::Application::GetInstance();
	VkPhysicalDevice physicalDevice = Walnut::Application::GetPhysicalDevice();
	VkDevice device = Walnut::Application::GetDevice();
	VkQueue queue = Walnut::Application::GetQueue();
	uint32_t queueFamily = Walnut::Application::GetQueueFamily();

	Destroy();
	Init(instance, physicalDevice, device, queue, queueFamily, width, height);
}

void NRDWrapper::NewFrame()
{
	if (!m_Initialized)
		return;
	g_NRD.NewFrame();
}

void NRDWrapper::ResetHistory()
{
	if (!m_Initialized)
		return;
	// Setting accumulationMode to RESTART for one frame resets history
	// This is handled in SetCommonSettings via the reset parameter
}

void NRDWrapper::SetCommonSettings(const float* viewToClip, const float* viewToClipPrev,
                                   const float* worldToView, const float* worldToViewPrev,
                                   float jitterX, float jitterY,
                                   float jitterXPrev, float jitterYPrev,
                                   uint32_t frameIndex, bool reset)
{
	if (!m_Initialized)
		return;

	nrd::CommonSettings common = {};
	memcpy(common.viewToClipMatrix, viewToClip, sizeof(float) * 16);
	memcpy(common.viewToClipMatrixPrev, viewToClipPrev, sizeof(float) * 16);
	memcpy(common.worldToViewMatrix, worldToView, sizeof(float) * 16);
	memcpy(common.worldToViewMatrixPrev, worldToViewPrev, sizeof(float) * 16);
	common.motionVectorScale[0] = 1.0f;
	common.motionVectorScale[1] = 1.0f;
	common.motionVectorScale[2] = 0.0f; // 2D screen-space motion
	common.cameraJitter[0] = jitterX;
	common.cameraJitter[1] = jitterY;
	common.cameraJitterPrev[0] = jitterXPrev;
	common.cameraJitterPrev[1] = jitterYPrev;
	common.resourceSize[0] = (uint16_t)m_Width;
	common.resourceSize[1] = (uint16_t)m_Height;
	common.resourceSizePrev[0] = (uint16_t)m_Width;
	common.resourceSizePrev[1] = (uint16_t)m_Height;
	common.rectSize[0] = (uint16_t)m_Width;
	common.rectSize[1] = (uint16_t)m_Height;
	common.rectSizePrev[0] = (uint16_t)m_Width;
	common.rectSizePrev[1] = (uint16_t)m_Height;
	common.viewZScale = 1.0f;
	common.denoisingRange = 500000.0f;
	common.disocclusionThreshold = 0.01f;
	common.frameIndex = frameIndex;
	common.isMotionVectorInWorldSpace = false;
	common.accumulationMode = reset ? nrd::AccumulationMode::RESTART : nrd::AccumulationMode::CONTINUE;

	g_NRD.SetCommonSettings(common);

	// Set REBLUR settings
	nrd::ReblurSettings settings = {}; // defaults are fine
	g_NRD.SetDenoiserSettings(m_DenoiserID, &settings);
}

// Helper to create nrd::Resource from VkImage
static nrd::Resource MakeNrdResource(VkImage image, VkFormat format,
                                     nri::AccessLayoutStage state = {})
{
	nrd::Resource res = {};
	res.vk.image = (uint64_t)image;
	res.vk.format = (int32_t)format;
	res.state = state;
	return res;
}

void NRDWrapper::Denoise(VkCommandBuffer cmdBuffer,
                         VkImage inNormalRoughness, VkFormat normalRoughnessFmt,
                         VkImage inViewZ, VkFormat viewZFmt,
                         VkImage inMotion, VkFormat motionFmt,
                         VkImage inDiffRadianceHitDist, VkFormat diffFmt,
                         VkImage inSpecRadianceHitDist, VkFormat specFmt,
                         VkImage outDiffRadianceHitDist,
                         VkImage outSpecRadianceHitDist)
{
	if (!m_Initialized)
		return;

	// NRD output format is R11G11B10f (B10G11R11_UFLOAT_PACK32)
	VkFormat outFmt = VK_FORMAT_B10G11R11_UFLOAT_PACK32;

	nrd::ResourceSnapshot snapshot = {};
	snapshot.restoreInitialState = true;

	// All images stay in GENERAL layout throughout — NRD restores to this
	// state after denoising, so the next frame's raygen can imageStore.
	nri::AccessLayoutStage initialState = {};
	initialState.access = nri::AccessBits::SHADER_RESOURCE;
	initialState.layout = nri::Layout::GENERAL;
	initialState.stages = nri::StageBits::COMPUTE_SHADER;

	snapshot.SetResource(nrd::ResourceType::IN_NORMAL_ROUGHNESS,
		MakeNrdResource(inNormalRoughness, normalRoughnessFmt, initialState));
	snapshot.SetResource(nrd::ResourceType::IN_VIEWZ,
		MakeNrdResource(inViewZ, viewZFmt, initialState));
	snapshot.SetResource(nrd::ResourceType::IN_MV,
		MakeNrdResource(inMotion, motionFmt, initialState));

	// Noisy inputs
	snapshot.SetResource(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST,
		MakeNrdResource(inDiffRadianceHitDist, diffFmt, initialState));
	snapshot.SetResource(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST,
		MakeNrdResource(inSpecRadianceHitDist, specFmt, initialState));

	// Outputs
	nri::AccessLayoutStage outputState = {};
	outputState.access = nri::AccessBits::SHADER_RESOURCE_STORAGE;
	outputState.layout = nri::Layout::GENERAL;
	outputState.stages = nri::StageBits::COMPUTE_SHADER;

	snapshot.SetResource(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST,
		MakeNrdResource(outDiffRadianceHitDist, outFmt, outputState));
	snapshot.SetResource(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST,
		MakeNrdResource(outSpecRadianceHitDist, outFmt, outputState));

	// Dispatch
	nri::CommandBufferVKDesc cmdDesc = {};
	cmdDesc.vkCommandBuffer = cmdBuffer;
	cmdDesc.queueType = nri::QueueType::GRAPHICS;

	nrd::Identifier denoisers[] = { m_DenoiserID };
	g_NRD.DenoiseVK(denoisers, 1, cmdDesc, snapshot);
}