#pragma once

#ifndef RT2_EDITOR_CAMERA_WORKFLOW_H
#define RT2_EDITOR_CAMERA_WORKFLOW_H

#include "SceneDocument.h"
#include "core/UUID.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <functional>
#include <vector>

namespace rt2::core { class ISceneRenderBridge; }

// CPU-only editor-camera data and math. This module deliberately has no
// Walnut, ImGui, Vulkan, or RendererGPU dependency so RT2Tests and the slice
// runner can exercise the complete framing contract.
struct EditorCameraPose
{
    glm::vec3 position{0.0f, 1.0f, 10.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    float verticalFOV = 45.0f;
    float aperture = 0.0f;
    float focusDistance = 1.0f;
    float farClip = 10000.0f;
};

struct EditorSelectionBounds
{
    glm::vec3 minimum{0.0f};
    glm::vec3 maximum{0.0f};
    bool valid = false;

    glm::vec3 Center() const { return (minimum + maximum) * 0.5f; }
    glm::vec3 HalfExtents() const { return (maximum - minimum) * 0.5f; }
    bool Contains(const glm::vec3& point) const;
};

struct EditorFrameSettings
{
    float viewportAspect = 16.0f / 9.0f;
    float nearClip = 0.1f;
    float margin = 1.15f;
};

bool IsValidEditorCameraPose(const EditorCameraPose& pose);
bool TryNormalizeEditorCameraPose(EditorCameraPose& pose);
bool TryCameraRotationFromForward(const glm::vec3& forward, glm::quat& rotation);

// Applies one complete editor-camera cut through the supplied host sink, then
// resets renderer history exactly once. The sink keeps this CPU-only module
// independent of Walnut's Camera/Input implementation and is recordable in
// RT2Tests. A rejected pose or sink leaves renderer history untouched.
bool ApplyEditorCameraCut(
    const EditorCameraPose& requested,
    rt2::core::ISceneRenderBridge& bridge,
    const std::function<bool(const EditorCameraPose&)>& applyPose);

rt2::core::UUID FindDeterministicCameraEntity(
    const rt2::core::SceneDocument& document);
bool TryGetCameraEntityPose(rt2::core::SceneDocument& document,
                            const rt2::core::UUID& cameraEntity,
                            const EditorCameraPose& fallback,
                            EditorCameraPose& pose);

// Includes selected roots and all descendants without visibility filtering.
// Non-renderable entities contribute a deterministic 0.5 m fallback cube.
bool ComputeEditorSelectionBounds(
    rt2::core::SceneDocument& document,
    const std::vector<rt2::core::UUID>& selectedRoots,
    EditorSelectionBounds& bounds);

// Focus keeps the camera position, points at the bounds centre, and updates
// focus distance. Frame also moves backward far enough to fit both FOV axes.
bool TryFocusEditorCamera(const EditorCameraPose& current,
                          const EditorSelectionBounds& bounds,
                          float nearClip,
                          EditorCameraPose& focused);
bool TryFrameEditorCamera(const EditorCameraPose& current,
                          const EditorSelectionBounds& bounds,
                          const EditorFrameSettings& settings,
                          EditorCameraPose& framed);

#endif // RT2_EDITOR_CAMERA_WORKFLOW_H
