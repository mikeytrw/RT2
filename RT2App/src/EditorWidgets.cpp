#include "EditorWidgets.h"

#include "EditorFonts.h"
#include "EditorIcons.h"
#include "EditorTheme.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>

namespace rt2::ui {

namespace {

// Draws [begin, end) as the row's label, clipped to the label column (the
// full text becomes a tooltip when clipped), and moves to the value column.
void DrawRowLabelText(const char* begin, const char* end)
{
    const float column = InspectorLabelWidth();
    const float room = column - ImGui::GetStyle().ItemSpacing.x;
    ImGui::AlignTextToFramePadding();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const bool clipped = ImGui::CalcTextSize(begin, end).x > room;
    ImGui::PushStyleColor(ImGuiCol_Text, EditorPalette::Label);
    if (clipped)
        ImGui::PushClipRect(start, ImVec2(start.x + room, start.y + ImGui::GetFrameHeight()), true);
    ImGui::TextUnformatted(begin, end);
    if (clipped)
    {
        ImGui::PopClipRect();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%.*s", static_cast<int>(end - begin), begin);
    }
    ImGui::PopStyleColor();
    ImGui::SameLine(column);
}

} // anonymous namespace

void InspectorSectionHeader(const char* icon, const char* title)
{
    ImGui::Spacing();
    ImFont* bold = EditorFonts().bold;
    if (bold) ImGui::PushFont(bold);
    ImGui::PushStyleColor(ImGuiCol_Text, EditorPalette::IndigoLine);
    ImGui::TextUnformatted(icon);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextUnformatted(title);
    if (bold) ImGui::PopFont();
    ImGui::Spacing();
}

void InspectorSubHeader(const char* title)
{
    ImGui::Spacing();
    ImFont* medium = EditorFonts().medium;
    if (medium) ImGui::PushFont(medium);
    ImGui::PushStyleColor(ImGuiCol_Text, EditorPalette::TextSecondary);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    if (medium) ImGui::PopFont();
}

// A share of the panel width, clamped so short panels keep usable fields and
// wide ones don't strand labels far from their values.
float InspectorLabelWidth()
{
    const float panel = ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x;
    const float scale = ImGui::GetFontSize() / 15.0f;
    return std::clamp(panel * 0.36f, 90.0f * scale, 160.0f * scale);
}

void InspectorRowLabel(const char* label)
{
    DrawRowLabelText(label, label + std::strlen(label));
    ImGui::SetNextItemWidth(-FLT_MIN);
}

const char* RowLabel(const char* label)
{
    static char hiddenId[256];
    const char* hash = std::strstr(label, "##");
    DrawRowLabelText(label, hash ? hash : label + std::strlen(label));
    ImGui::SetNextItemWidth(-FLT_MIN);
    std::snprintf(hiddenId, sizeof(hiddenId), "##%s", label);
    return hiddenId;
}

bool InspectorVec3Row(const char* label, float* v, float speed,
    float vMin, float vMax, const char* format)
{
    DrawRowLabelText(label, label + std::strlen(label));

    bool changed = false;
    ImGui::BeginGroup();
    ImGui::PushID(label);
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const float width = (ImGui::GetContentRegionAvail().x - spacing * 2.0f) / 3.0f;
    static const char* const kAxisNames[3] = { "X", "Y", "Z" };
    const ImVec4 axisColors[3] = { EditorPalette::AxisX, EditorPalette::AxisY, EditorPalette::AxisZ };
    for (int i = 0; i < 3; ++i)
    {
        ImGui::PushID(i);
        if (i > 0) ImGui::SameLine(0.0f, spacing);
        ImGui::SetNextItemWidth(width);
        changed |= ImGui::DragFloat("##v", &v[i], speed, vMin, vMax, format);
        const ImVec2 min = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(min.x + 6.0f, min.y + ImGui::GetStyle().FramePadding.y),
            ImGui::GetColorU32(axisColors[i]), kAxisNames[i]);
        ImGui::PopID();
    }
    ImGui::PopID();
    ImGui::EndGroup();
    return changed;
}

bool InspectorVec3Row(const char* label, float* v, float speed, const char* format)
{
    return InspectorVec3Row(label, v, speed, 0.0f, 0.0f, format);
}

bool CollapsibleSection(const char* id, const char* icon, const char* title,
    const char* summary, bool defaultOpen)
{
    ImGui::Spacing();
    ImGui::PushID(id);
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetID("open");
    bool open = storage->GetBool(key, defaultOpen);

    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetFrameHeight();
    if (ImGui::InvisibleButton("##header", ImVec2(width, height)))
    {
        open = !open;
        storage->SetBool(key, open);
    }
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered)
        draw->AddRectFilled(start, ImVec2(start.x + width, start.y + height),
            ImGui::GetColorU32(EditorPalette::Hover), ImGui::GetStyle().FrameRounding);
    ImFont* font = ImGui::GetFont();
    ImFont* bold = EditorFonts().bold ? EditorFonts().bold : font;
    const float size = ImGui::GetFontSize();
    const float textY = start.y + (height - size) * 0.5f;
    float x = start.x + 4.0f;
    draw->AddText(font, size, ImVec2(x, textY), ImGui::GetColorU32(EditorPalette::Label),
        open ? RT2_ICON_CHEVRON_DOWN : RT2_ICON_CHEVRON_RIGHT);
    x += size + 6.0f;
    draw->AddText(font, size, ImVec2(x, textY), ImGui::GetColorU32(EditorPalette::IndigoLine), icon);
    x += size + 8.0f;
    draw->AddText(bold, size, ImVec2(x, textY), ImGui::GetColorU32(EditorPalette::Text), title);
    const float titleEnd = x + bold->CalcTextSizeA(size, FLT_MAX, 0.0f, title).x;
    if (summary && *summary)
    {
        const float summaryW = font->CalcTextSizeA(size, FLT_MAX, 0.0f, summary).x;
        const float summaryX = start.x + width - 6.0f - summaryW;
        if (summaryX > titleEnd + 12.0f)
            draw->AddText(font, size, ImVec2(summaryX, textY),
                ImGui::GetColorU32(EditorPalette::Label), summary);
    }
    ImGui::PopID();
    return open;
}

} // namespace rt2::ui
