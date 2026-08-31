---
title: "RR-neutral guide resource and semantic ledger"
---

# RR-neutral guide resource and semantic ledger

Grounding commit: `247f4f6599c12e3fba05027300a5c04a292443e7` (31 August 2026).
This is the pre-shader-change contract for W3. The five resources named
`RRGuide*` are RT2-owned; depth and motion remain shared G-buffer resources
until their contracts are validated. No NRD slot is an RR slot.

## Semantic and synchronization contract

All seven guide rows use `RenderExtent`. Values are linear and pre-tone-map.
The producer writes the resource, then the consumer reads it after the exact
barrier in the final column. `GENERAL` is used for storage-image producers;
the descriptor image layout is `GENERAL`. Raster attachments transition from
`GENERAL` to `COLOR_ATTACHMENT_OPTIMAL` before the pass and back afterward.

| Guide/resource | Producer | Consumer | Format / extent | Space and value contract | Clear / miss / reset | Initial -> final layout; stage/access barrier | Descriptor |
|---|---|---|---|---|---|---|---|
| `RRGuideNoisyHdr` | raster-first ray shading (`FrameRenderer::RecordPathTraceOrDebug`, `FrameRenderer.cpp:433-477`) | future RR evaluation only; never NRD/compose | `VK_FORMAT_R16G16B16A16_SFLOAT`, RenderExtent | finite linear HDR radiance before NRD, compose, tone map and exposure | zero clear; sky is finite sky radiance; reset on extent/backend/scene cut | `UNDEFINED -> GENERAL` (top-of-pipe/0 -> ray-tracing shader write), then ray-tracing write -> RR compute read | storage image, unique W3 index |
| `RRGuideDiffuseAlbedo` | raster material resolve (`raster.frag:157-181`) | future RR evaluation | `VK_FORMAT_R8G8B8A8_UNORM`, RenderExtent | linear RGB base reflectance, not sRGB and not lighting | sky/miss `(0,0,0,1)`; reset with guide allocation | `UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL -> GENERAL`; color-attachment write -> RR compute read | storage image, unique W3 index |
| `RRGuideSpecularAlbedo` | raster material resolve, shared `EnvBRDFApprox2` helper (`raster.frag:157-181`; helper in `rr_guide_shared.glsl`) | future RR evaluation | `VK_FORMAT_R8G8B8A8_UNORM`, RenderExtent | linear view-dependent specular reflectance using the shared pinned approximation; never metallic | sky/miss `0.5` per A0 measured contract; reset with guide allocation | same raster attachment -> RR read barrier as diffuse | storage image, unique W3 index |
| `RRGuideNormalRoughness` | raster surface resolve (`raster.frag:138-181`) | future RR evaluation | `VK_FORMAT_R16G16B16A16_SFLOAT`, RenderExtent | normalized world normal RGB in `[-1,1]`; linear roughness A in `[0,1]` | sky/miss normal `(0,1,0)`, roughness `1`; reset with guide allocation | same raster attachment -> RR read barrier as diffuse | storage image, unique W3 index |
| `RRGuideSpecularHitDistance` | ray shading before NRD normalization (`FrameRenderer.cpp:433-477`; ray payload contract `pathtracer_shared.glsl:298-315`) | future RR evaluation | `VK_FORMAT_R32_SFLOAT`, RenderExtent | world-space primary-to-first-secondary specular hit distance | exact zero on sky/miss; hit is positive finite; reset with guide allocation | `UNDEFINED -> GENERAL`, ray-tracing write -> RR compute read | storage image, unique W3 index |
| `LinearDepth` (shared) | raster G-buffer (`raster.frag:165-181`) | NRD today; RR only after validation | existing `VK_FORMAT_R32_SFLOAT`, RenderExtent | positive view-space distance `-(worldToView * hit).z`, matching A0 sample; do not reinterpret | deterministic far/miss policy remains existing NRD policy; reset on existing NRD reset | existing raster -> NRD barrier (`FrameRenderer.cpp:173-190`) | existing `SI_BINDING_G_VIEWZ` (1), no alias |
| `Motion` (shared) | raster vertex/fragment (`raster.vert:34-68`, `raster.frag:170-215`) | NRD today; RR only after dense-contract validation | existing `VK_FORMAT_R16G16_SFLOAT`, RenderExtent | current pixel back to previous pixel; includes camera and rigid-instance transforms; jitter compensation owned by W5 exactly once | deterministic zero only for truly static camera/geometry; sky/emissive must be populated by the validated path; reset on existing history reset | existing raster -> NRD barrier (`FrameRenderer.cpp:173-190`) | existing `SI_BINDING_G_MOTION` (2), no alias |

The five dedicated images are not placed in `GBufferTarget::ColorIndex` and
do not change `SI_BINDING_G_*` or `COLOR_COUNT`; W3-owned descriptors are
declared in the guide resource owner. This preserves the variable texture
binding at `SI_BINDING_TEXTURE_ARRAY=19` (`shader_interface.h:48-57`) and the
existing NRD descriptor indices (`shader_interface.h:67-82`).

## Allocation and descriptor budget

At `4096x2160` native RenderExtent, bytes below are the image payload lower
bound (`width * height * bytesPerPixel`) and must be replaced by measured
`vkGetImageMemoryRequirements().size` values in the W3 report. Each guide is a
separate image and allocation; no image is suballocated or aliased.

| Resource | Bytes/pixel | Payload lower bound |
|---|---:|---:|
| Noisy HDR | 8 | 67,108,864 |
| Diffuse albedo | 4 | 33,554,432 |
| Specular albedo | 4 | 33,554,432 |
| Normal + roughness | 8 | 67,108,864 |
| Specular hit distance | 4 | 33,554,432 |
| **Five-guide payload total** | **28** | **234,881,024 (224 MiB)** |

The acceptance ceiling is **five images and five allocations for these guides**
(six images/eight allocations including any future W3 output bookkeeping),
with measured RT2-owned device-local memory `<= 224 MiB` at 4K. The measured
Vulkan requirement sum, allocation count, image count, and NGX-private memory
are reported separately. A requirement larger than the payload is a budget
failure requiring a measured format/allocation amendment; it is never hidden.

## Ownership and reset rules

`GBufferTarget` remains the owner of the existing NRD resources
(`GBufferTarget.h:30-50`, created at `GBufferTarget.cpp:10-88`). The dedicated
guide owner must be created and destroyed with `RenderExtent` in
`RendererGPU::CreateGBufferImages` / `DestroyGBufferImages`
(`RendererGPU.cpp:1273-1280`) and refreshed on `RendererGPU::OnResize`
(`RendererGPU.cpp:261-310`). Guides are read-only diagnostics when debug/report
is disabled and must never feed NRD, compose, tone-map, or the canonical
output. Every Vulkan create, allocation, bind, view, barrier, map, write,
readback, and close result is checked and routed to the existing diagnostic
contract.

## 2026-08-31 measured correction

The pre-shader table above multiplied the 4K width by 2048 rather than the
required native height 2160; its 28-byte payload is therefore
`4096*2160*28 = 247,726,080` bytes and cannot satisfy the 224 MiB ceiling.
The production implementation corrects `RRGuideNoisyHdr` to
`VK_FORMAT_B10G11R11_UFLOAT_PACK32` (three-channel linear HDR, 4 bytes/pixel),
keeping all seven row semantics and ownership unchanged. The corrected five
guide payload is 24 bytes/pixel (`212,336,640` bytes at 4096x2160, 202.5 MiB)
before Vulkan alignment; the W3 report remains authoritative for measured
`vkGetImageMemoryRequirements().size` and rejects an over-budget allocation.
The measured raster shader also now writes positive view distance (`-viewPos.z`)
for the shared `LinearDepth` row, matching the A0 contract; this is a semantic
correction to the former negative-Z implementation, not a new RR slot.
