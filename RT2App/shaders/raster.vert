#version 460
#extension GL_EXT_scalar_block_layout : require

#include "shader_interface.h"

// Bindings from set 0 (path tracer descriptor set — shared)
layout(set = 0, binding = SI_BINDING_CAMERA_UBO, std140) uniform CameraData
{
    vec4 position;
    vec4 forward;
    vec4 right;
    vec4 up;
    vec4 viewportSPP;
    vec4 apertureFocal;
    vec4 envMap;
    mat4 inverseProjection;
    mat4 inverseView;
    mat4 viewToClip;
    mat4 viewToClipPrev;
    mat4 worldToView;
    mat4 worldToViewPrev;
} camera;

layout(set = 0, binding = SI_BINDING_INSTANCE_TRANSFORMS, std430) readonly buffer InstanceTransforms
{
    mat4 instanceTransforms[];
};

// Previous frame instance transforms (binding 10 in a separate set or same set)
// For now we use a second SSBO binding — we'll add SI_BINDING_INSTANCE_TRANSFORMS_PREV
layout(set = 0, binding = 12, std430) readonly buffer InstanceTransformsPrev
{
    mat4 instanceTransformsPrev[];
};

// Vertex format: interleaved {vec3 pos, vec2 uv, vec3 normal, vec4 tangent}
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outWorldPosPrev;
layout(location = 2) out vec2 outUV;
layout(location = 3) flat out uint outInstanceIndex;
layout(location = 4) out vec3 outWorldNormal;
layout(location = 5) out vec4 outWorldTangent;

void main()
{
    uint instIdx = gl_InstanceIndex; // firstInstance from VkDrawIndirectCommand = global instance index
    mat4 world = instanceTransforms[instIdx];
    mat4 worldPrev = instanceTransformsPrev[instIdx];

    vec4 worldPos = world * vec4(inPos, 1.0);
    vec4 worldPosPrev = worldPrev * vec4(inPos, 1.0);

    outWorldPos = worldPos.xyz;
    outWorldPosPrev = worldPosPrev.xyz;
    outUV = inUV;
    outInstanceIndex = instIdx;
    mat3 normalMatrix = transpose(inverse(mat3(world)));
    outWorldNormal = normalMatrix * inNormal;
    outWorldTangent = vec4(mat3(world) * inTangent.xyz, inTangent.w);

    // Jittered clip-space position for raster (matches NRD convention).
    // NRD defines sampleUv = pixelUv + cameraJitter. Shifting geometry by
    // -jitter in clip space is equivalent to sampling at pixelUv + jitter.
    // camera.forward.w = jitter.x, camera.right.w = jitter.y (subpixel offset in [-0.5, 0.5] pixels).
    // Convert pixel offset to NDC: jitter / viewport * 2 (NDC range is [-1, 1]).
    // Multiply by clipPos.w to pre-cancel the hardware perspective divide, so the
    // post-divide NDC offset is exactly jitter * 2 / viewport regardless of depth.
    vec4 clipPos = camera.viewToClip * camera.worldToView * worldPos;
    vec2 viewport = camera.viewportSPP.xy;
    clipPos.xy -= vec2(camera.forward.w, camera.right.w) * 2.0 / viewport * clipPos.w;
    gl_Position = clipPos;
}
