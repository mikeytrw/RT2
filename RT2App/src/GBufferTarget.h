#pragma once

#include "vulkan/vulkan.h"
#include "GpuResources.h"
#include "RenderExtents.h"
#include <cstdint>

struct GpuDevice;

// GBufferTarget — owns all G-buffer images used for raster→RT handoff + NRD.
//
// 12 color images (MRT storage + RT storage) + 1 depth image.
// Internal index matches shader_interface.h SI_BINDING_G_* constants (where
// applicable) so descriptors can be wired by index.
//
// Two index spaces exist:
//   - Shader binding index (0-10): matches SI_BINDING_G_* for descriptor set.
//     NRD outputs (NRD_DIFF_OUT, NRD_SPEC_OUT) are NOT in the descriptor set
//     (they're NRD-internal), but live in the color image array for ownership.
//   - MRT index (0-7): the 8 color attachments written by the raster pass.
//     Different order (no diff/spec radiance, no NRD outputs).
class GBufferTarget
{
public:
	// Color image index (internal storage order).
	// NOTE: indices match SI_BINDING_G_* shader bindings where applicable
	// (0-5, 7-10). Index 6 is reserved for the NRD UBO (not a color image),
	// so the color array has a gap at index 6. COLOR_COUNT=13 to accommodate
	// indices 0-12, with slot 6 left unused (null handle).
	enum ColorIndex : uint32_t
	{
		// --- Shader descriptor set bindings (SI_BINDING_G_*) ---
		NORMAL_ROUGHNESS = 0,  // A2B10G10R10 — NRD oct-packed normal + sqrt(roughness)
		VIEWZ,                 // R32_SFLOAT   — linear view-space Z
		MOTION,                // R16G16       — 2D screen-space motion vector
		DIFF_RADIANCE,         // RGBA16F      — NRD diffuse radiance + norm hit dist
		SPEC_RADIANCE,         // RGBA16F      — NRD specular radiance + norm hit dist
		ALBEDO_F0,             // RGBA16F      — albedo.rgb + F0.a
		// Index 6 = NRD UBO (not a color image — slot left empty)
		DIRECT_EMISSION = 7,   // RGBA16F      — emissive surfaces + sky
		PRIM_HIT,              // RGBA32F      — xyz = world pos, w = matIdx+1
		PRIM_GEO_NORMAL,       // RGBA8        — geometric normal (0.5+0.5 encode)
		PRIM_UV,               // RG16F        — primary hit UV
		// --- Not in shader descriptor set (NRD-internal) ---
		NRD_DIFF_OUT,          // RGBA16F      — NRD denoised diffuse output
		NRD_SPEC_OUT,          // RGBA16F      — NRD denoised specular output
		MAX_COLOR_INDEX = NRD_SPEC_OUT,
	};

	static constexpr uint32_t COLOR_COUNT = MAX_COLOR_INDEX + 1; // 13 (indices 0-12, slot 6 unused)

	// MRT color formats (8 attachments written by raster pass).
	// Order matches RasterPass MRT layout (raster.frag location 0-7).
	static constexpr VkFormat MRT_FORMATS[8] = {
		VK_FORMAT_A2B10G10R10_UNORM_PACK32, // 0: normalRoughness
		VK_FORMAT_R32_SFLOAT,                // 1: viewZ
		VK_FORMAT_R16G16_SFLOAT,             // 2: motion
		VK_FORMAT_R16G16B16A16_SFLOAT,       // 3: albedoF0
		VK_FORMAT_R16G16B16A16_SFLOAT,       // 4: directEmission
		VK_FORMAT_R32G32B32A32_SFLOAT,       // 5: primHit
		VK_FORMAT_R8G8B8A8_UNORM,            // 6: primGeoNormal
		VK_FORMAT_R16G16_SFLOAT,             // 7: primUV
	};

	// Maps MRT index (0-7) → ColorIndex.
	static constexpr ColorIndex MRT_TO_COLOR[8] = {
		NORMAL_ROUGHNESS,  // MRT 0
		VIEWZ,             // MRT 1
		MOTION,            // MRT 2
		ALBEDO_F0,         // MRT 3
		DIRECT_EMISSION,   // MRT 4
		PRIM_HIT,          // MRT 5
		PRIM_GEO_NORMAL,   // MRT 6
		PRIM_UV,           // MRT 7
	};

	static constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

	GBufferTarget() = default;
	~GBufferTarget();

	GBufferTarget(const GBufferTarget&) = delete;
	GBufferTarget& operator=(const GBufferTarget&) = delete;

	// Create all images at the given dimensions. Destroys any existing images first.
	// Color images get STORAGE | SAMPLED | TRANSFER_DST | COLOR_ATTACHMENT usage.
	// Depth image gets DEPTH_STENCIL_ATTACHMENT usage.
	// All color images are transitioned to GENERAL layout.
	void Create(const GpuDevice& dev, const RenderExtent& extent);

	// Destroy all images (color + depth). Safe to call on uncreated state.
	void Destroy();

	bool IsValid() const { return m_ColorImages[0].IsValid(); }
	RenderExtent GetExtent() const { return m_Extent; }

	// Access color image by ColorIndex.
	const GpuImage& GetColor(uint32_t index) const { return m_ColorImages[index]; }
	GpuImage& GetColor(uint32_t index) { return m_ColorImages[index]; }

	// Access depth image.
	const GpuImage& GetDepth() const { return m_DepthImage; }
	GpuImage& GetDepth() { return m_DepthImage; }

	// Fill an array of 8 VkImageViews for the raster pass MRT attachments.
	void GetMRTViews(VkImageView outViews[8]) const;

	// Fill an array of 8 VkImages for barrier operations on MRT attachments.
	void GetMRTImages(VkImage outImages[8]) const;

	// Get the format for a color image by ColorIndex.
	static VkFormat GetColorFormat(uint32_t index);

private:
	GpuImage m_ColorImages[COLOR_COUNT];
	GpuImage m_DepthImage;
	RenderExtent m_Extent;
	GpuDevice const* m_Device = nullptr;
};
