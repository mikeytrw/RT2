#include "EditorWorkspaceDocking.h"

#include <imgui_internal.h>

namespace rt2 {

bool BuildDefaultDockLayout(ImGuiID dockspaceId, const ImVec2& avail)
{
    if (dockspaceId == 0 || avail.x <= 0.0f || avail.y <= 0.0f)
        return false;

    // Canonical demo pattern: clear, seed a DockSpace node at the content
    // size, split by fractions, dock identities, finish. Runs once per
    // pending request — never per frame, never on resize.
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, avail);

    // 1. Right-hand Inspector area (~23% of available width).
    ImGuiID rightId = 0, remainId = dockspaceId;
    ImGui::DockBuilderSplitNode(remainId, ImGuiDir_Right,
        kInspectorWidthFraction, &rightId, &remainId);
    // 2. Bottom area from the remainder (~27% of height).
    ImGuiID bottomId = 0, topId = remainId;
    ImGui::DockBuilderSplitNode(topId, ImGuiDir_Down,
        kBottomHeightFraction, &bottomId, &topId);
    // 3. Outliner from the top remainder (~20% of that width).
    ImGuiID outlinerId = 0, centerId = topId;
    ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Left,
        kOutlinerWidthFraction, &outlinerId, &centerId);
    // 4. Viewport keeps the remaining center.

    ImGui::DockBuilderDockWindow(kWorkspaceViewportName, centerId);
    ImGui::DockBuilderDockWindow(kWorkspaceOutlinerName, outlinerId);
    ImGui::DockBuilderDockWindow(kWorkspaceInspectorName, rightId);
    ImGui::DockBuilderDockWindow(kWorkspaceSessionName, bottomId);
    ImGui::DockBuilderDockWindow(kWorkspacePerformanceName, bottomId);
    ImGui::DockBuilderDockWindow(kWorkspaceContentBrowserName, bottomId);
    // Tools without a first-use dock still open somewhere sensible: as
    // right-side tabs beside the Inspector when first shown.
    ImGui::DockBuilderDockWindow(kWorkspaceSceneName, rightId);
    ImGui::DockBuilderDockWindow(kWorkspaceCameraName, rightId);
    ImGui::DockBuilderDockWindow(kWorkspaceRenderSettingsName, rightId);
    ImGui::DockBuilderDockWindow(kWorkspaceInputBindingsName, rightId);

    ImGui::DockBuilderFinish(dockspaceId);
    return true;
}

} // namespace rt2
