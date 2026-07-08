#pragma once

#include "vulkan/vulkan.h"
#include "GpuResources.h"
#include "StagingArena.h"
#include "GPUSceneData.h"
#include <vector>
#include <thread>
#include <atomic>
#include <functional>

struct GpuDevice;

// AsyncTextureLoader — decouples texture decode + GPU upload from the
// render thread so scene loading does not stall the frame loop.
//
// Lifecycle:
//   1. Begin(dev, textures)  — launches a worker thread that decodes
//      pixels, creates VkImages, records copy+mipmap commands, and
//      submits with a fence. Returns immediately.
//   2. Poll() / IsComplete() — main thread checks the fence each frame.
//   3. Adopt(out)             — on completion, moves ready GpuImages +
//      metadata into the caller. Staging arena is destroyed after.
//   4. Cancel() / Destroy()  — wait for thread, free all resources.
//
// Design notes:
//   - VkImage creation is thread-safe in Vulkan (vkCreate* calls are
//     safe from any thread as long as the device is not lost).
//   - Command buffer recording uses a dedicated TRANSIENT command pool
//     created on the worker thread.
//   - The staging arena is kept alive until the fence signals, then
//     destroyed on the main thread via Adopt().
//   - Only one upload in flight at a time. Begin() while busy is a no-op.
class AsyncTextureLoader
{
public:
	AsyncTextureLoader() = default;
	~AsyncTextureLoader();

	AsyncTextureLoader(const AsyncTextureLoader&) = delete;
	AsyncTextureLoader& operator=(const AsyncTextureLoader&) = delete;

	// Kick off async decode + upload. Returns false if busy or no textures.
	// `envMapFloatPixels` / `envMapWidth` / `envMapHeight` supply the HDR
	// env map texture that gets appended to the texture array (matches
	// the existing WalnutApp::UploadMeshToGPU convention).
	// `marginalCDF` / `conditionalCDF` / `cdfWidth` / `cdfHeight` supply
	// the CDF textures (also appended after the env map).
	bool Begin(const GpuDevice& dev,
	           const std::vector<SceneTexture>& textures,
	           const std::vector<float>& envMapFloatPixels,
	           int envMapWidth, int envMapHeight,
	           const std::vector<float>& marginalCDF,
	           const std::vector<float>& conditionalCDF,
	           int cdfWidth, int cdfHeight);

	// Check if the GPU upload fence has signalled. Safe to call every frame.
	bool IsComplete() const;

	// True if Begin() was called and the worker is still running or done.
	bool IsBusy() const { return m_Busy.load(); }

	// Move ready textures + indices into out. Only valid after IsComplete().
	// Destroys the staging arena + fence after adoption.
	void Adopt(std::vector<GpuImage>& outTextures,
	           int& envMapIndex,
	           int& marginalCDFIndex,
	           int& conditionalCDFIndex);

	// Block until the worker thread finishes (if running). Does not destroy
	// resources — use Destroy() for that.
	void Cancel();

	// Free all resources (textures, staging, fence). Blocks if busy.
	void Destroy();

private:
	void WorkerThread(const GpuDevice* dev,
	                  std::vector<SceneTexture> textures,
	                  std::vector<float> envMapFloat,
	                  std::vector<float> marginalCDF,
	                  std::vector<float> conditionalCDF);

	GpuDevice const* m_Device = nullptr;

	std::thread             m_Thread;
	std::atomic<bool>       m_Busy{false};
	std::atomic<bool>       m_Complete{false};

	VkFence       m_UploadFence    = VK_NULL_HANDLE;
	VkCommandPool m_CmdPool        = VK_NULL_HANDLE;

	// Staging arena (kept alive until fence signals, destroyed in Adopt)
	StagingArena  m_Staging;

	// Results from worker thread
	std::vector<GpuImage> m_ResultTextures;
	int m_ResultEnvMapIndex       = -1;
	int m_ResultMarginalCDFIndex  = -1;
	int m_ResultConditionalCDFIndex = -1;

	// Env map + CDF inputs (copied for the worker)
	std::vector<float> m_EnvMapFloatPixels;
	int m_EnvMapWidth  = 0;
	int m_EnvMapHeight = 0;
	std::vector<float> m_MarginalCDF;
	std::vector<float> m_ConditionalCDF;
	int m_CDFWidth  = 0;
	int m_CDFHeight = 0;
};