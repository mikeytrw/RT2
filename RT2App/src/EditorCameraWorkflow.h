#pragma once

#ifndef RT2_EDITOR_CAMERA_WORKFLOW_H
#define RT2_EDITOR_CAMERA_WORKFLOW_H

#include "SceneDocument.h"
#include "CameraPresentation.h"
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
    // Camera-owned display look. Focus/frame helpers copy the whole pose so
    // they preserve it; View Through / Play adoption overwrite it with the
    // destination camera's presentation.
    CameraPresentation presentation;
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
// Transport comparator: position, normalized forward, lens and projection
// determine camera motion/cut behavior. Tone operator and exposure are
// presentation and are EXCLUDED — a presentation-only change must not reset
// temporal history. Canonicalize both sides before comparing so -0.0f EV
// cannot read as a cut.
bool EditorCameraTransportEqual(const EditorCameraPose& a, const EditorCameraPose& b);
bool TryCameraRotationFromForward(const glm::vec3& forward, glm::quat& rotation);

// Pose-application policy for editor-camera updates (bookmark recall,
// numeric edits, View Through, focus/frame results). Transport-equal
// updates — including identical poses and presentation-only changes — apply
// the look directly and must never reset temporal history; transport
// differences keep the cut path with its single reset.
enum class PoseApplyAction { PresentationOnly, Cut };
inline PoseApplyAction ResolvePoseApplyAction(const EditorCameraPose& current,
                                             const EditorCameraPose& requested)
{
    return EditorCameraTransportEqual(current, requested)
               ? PoseApplyAction::PresentationOnly
               : PoseApplyAction::Cut;
}

// Builds the complete editor-camera adoption pose from an authoring
// SceneCamera (native open, recovery, interchange import, headless load):
// position, forward, FOV, aperture, focus distance and presentation.
// SceneCamera owns no far clip, so the current far clip is retained.
// Returns false (leaving `out` untouched) when the scene camera is not a
// valid pose; callers keep the prior editor camera and fail loudly.
bool TryBuildAuthoringAdoptionPose(const SceneCamera& scene,
                                   const EditorCameraPose& current,
                                   EditorCameraPose& out);

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
