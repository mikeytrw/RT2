#pragma once

#ifndef RT2_EDITOR_WORKSPACE_DOCKING_H
#define RT2_EDITOR_WORKSPACE_DOCKING_H

#include <imgui.h>

// ============================================================================
// EditorWorkspaceDocking — the one UI adapter allowed to touch ImGui's
// internal docking API (package 2).
//
// Builds the accepted first-use/reset arrangement into a host dockspace:
// right-hand Inspector (~23% width), bottom area (~27% height) shared by
// Session + Performance + Content Browser, Outliner (~20% of the top
// remainder), Viewport in the remaining center. Scene, Camera, Render
// Settings and Input Bindings pre-dock as right-side tabs so they open
// sensibly on first use. Window identities are the stable Begin labels and
// are never renamed here.
//
// Runs at most once per pending request (genuinely new profile or explicit
// Reset), driven by the host's pre-dock hook with a valid content size.
// Existing/loaded layouts never reach this code. Production never assigns
// SelectedTabId and never reads dock internals; first-use tab focus is a
// public one-shot SetNextWindowFocus owned by the host layer.
//
// Links into RT2App and RT2ImGuiProbe only (imgui_internal.h).
// ============================================================================

namespace rt2 {

// Stable window identities (must match the submitters' Begin labels).
inline constexpr const char* kWorkspaceViewportName = "Viewport";
inline constexpr const char* kWorkspaceOutlinerName = "Outliner";
inline constexpr const char* kWorkspaceInspectorName = "Inspector";
inline constexpr const char* kWorkspaceContentBrowserName = "Content Browser";
inline constexpr const char* kWorkspaceSessionName = "Session";
inline constexpr const char* kWorkspacePerformanceName = "Performance";
inline constexpr const char* kWorkspaceSceneName = "Scene";
inline constexpr const char* kWorkspaceCameraName = "Camera";
inline constexpr const char* kWorkspaceRenderSettingsName = "Render Settings";
inline constexpr const char* kWorkspaceInputBindingsName = "Input Bindings";

// Fraction tokens for the accepted arrangement (plan: 23% / 27% / 20%).
inline constexpr float kInspectorWidthFraction = 0.23f;
inline constexpr float kBottomHeightFraction = 0.27f;
inline constexpr float kOutlinerWidthFraction = 0.20f;

// Build the default arrangement into dockspaceId, sized by avail (logical
// dimensions). Returns false and touches nothing when avail is not valid
// (either axis <= 0); the host keeps the request pending in that case.
bool BuildDefaultDockLayout(ImGuiID dockspaceId, const ImVec2& avail);

} // namespace rt2

#endif // RT2_EDITOR_WORKSPACE_DOCKING_H
