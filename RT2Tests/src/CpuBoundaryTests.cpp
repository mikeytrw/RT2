#include <doctest/doctest.h>

// ============================================================================
// R1: RT2Tests CPU-only boundary guard.
//
// RT2Tests (and RT2SliceRunner) must contain no Vulkan, ImGui or Walnut
// (AGENTS.md; final plan). The ImGui gesture probe lives in the separate
// RT2ImGuiProbe target; this translation unit pins the boundary here by
// including the widest CPU header surface and failing the build if any of
// the three frameworks leak in — either as an include (IMGUI_VERSION /
// VK_HEADER_VERSION would be defined) or as a reachable path
// (__has_include probes the include directories themselves).
// ============================================================================

#include "CameraPresentation.h"
#include "CLIArgs.h"
#include "CompositePreviewSession.h"
#include "DenoiserMode.h"
#include "ECSScene.h"
#include "ECSComponents.h"
#include "EditorCameraWorkflow.h"
#include "EditorCommandHistory.h"
#include "EditorCommands.h"
#include "EditorPropertyCommands.h"
#include "EditorSceneState.h"
#include "EditorSyncRouter.h"
#include "PrefabComponentValueEquality.h"
#include "PrefabSerializer.h"
#include "RRFeatureLifecycle.h"
#include "SceneDocument.h"
#include "SceneLoader.h"
#include "SceneManager.h"
#include "SceneSerializer.h"
#include "ToneMapMath.h"
#include "TonemapPushConstants.h"
#include "TransformEditing.h"

#ifdef IMGUI_VERSION
#error "R1 boundary: RT2Tests must not import ImGui"
#endif

#ifdef VK_HEADER_VERSION
#error "R1 boundary: RT2Tests must not import Vulkan"
#endif

#if __has_include("imgui.h")
#error "R1 boundary: imgui.h must not be reachable from RT2Tests"
#endif

#if __has_include("vulkan/vulkan.h")
#error "R1 boundary: vulkan headers must not be reachable from RT2Tests"
#endif

#if __has_include("Walnut/Application.h")
#error "R1 boundary: Walnut headers must not be reachable from RT2Tests"
#endif

TEST_CASE("R1 CPU boundary holds no ImGui, Vulkan or Walnut")
{
    // The boundary is enforced at compile time above; this case exists so
    // the guarantee is visible in test listings and counts.
    CHECK(true);
}
