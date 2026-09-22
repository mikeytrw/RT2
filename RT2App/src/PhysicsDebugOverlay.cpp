// Drawing half of the T8 physics debug visualization. Separated from the
// CPU-only DTO + capture (PhysicsDebugLines / PhysicsDebugCapture) so the
// arithmetic stays testable in projects that do not link ImGui — the same
// split as EditorViewportIcons.cpp vs EditorViewportIconsDraw.cpp.
//
// RT2App-only: includes <imgui.h>. Never add this TU to the RT2Tests,
// RT2SliceRunner, or RT2ImGuiProbe CPU source closures.
#include "PhysicsDebugOverlay.h"

#include "EditorViewportIcons.h"

#include <imgui.h>

namespace rt2::core {
namespace {

ImU32 T8LineColour(PhysicsDebugLineKind kind)
{
    switch (kind)
    {
    case PhysicsDebugLineKind::Static:     return IM_COL32(200, 200, 200, 220);
    case PhysicsDebugLineKind::Dynamic:    return IM_COL32(80, 220, 100, 230);
    case PhysicsDebugLineKind::Kinematic:  return IM_COL32(250, 210, 80, 230);
    case PhysicsDebugLineKind::Trigger:    return IM_COL32(230, 80, 230, 230);
    case PhysicsDebugLineKind::Constraint: return IM_COL32(80, 200, 250, 230);
    }
    return IM_COL32(255, 255, 255, 220);
}

} // namespace

void DrawPhysicsDebugLines(const PhysicsDebugLines& lines,
                           const glm::mat4& viewProj,
                           const glm::vec2& imageMin,
                           const glm::vec2& imageSize)
{
    if (lines.segments.empty())
        return;
    if (imageSize.x <= 1.0f || imageSize.y <= 1.0f)
        return;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (const auto& seg : lines.segments)
    {
        glm::vec2 sa{ 0.0f }, sb{ 0.0f };
        float da = 0.0f, db = 0.0f;
        if (!ProjectToViewport(seg.a, viewProj, imageMin, imageSize, sa, da))
            continue;
        if (!ProjectToViewport(seg.b, viewProj, imageMin, imageSize, sb, db))
            continue;
        const float thickness =
            (seg.kind == PhysicsDebugLineKind::Constraint) ? 2.0f : 1.5f;
        draw->AddLine(ImVec2(sa.x, sa.y), ImVec2(sb.x, sb.y),
                      T8LineColour(seg.kind), thickness);
    }
}

} // namespace rt2::core
