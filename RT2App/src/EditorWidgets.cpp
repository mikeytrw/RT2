#include "EditorWidgets.h"

#include "EditorFonts.h"
#include "EditorIcons.h"
#include "EditorTheme.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

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

std::string MenuLabel(const char* icon, const char* text)
{
    return std::string(icon) + "  " + text;
}

bool BindingKeycaps(const char* id, const std::string& description, bool interactive)
{
    std::vector<std::string> caps;
    const std::string separator = " or ";
    size_t begin = 0;
    for (;;)
    {
        const size_t at = description.find(separator, begin);
        caps.push_back(description.substr(begin, at == std::string::npos ? std::string::npos : at - begin));
        if (at == std::string::npos)
            break;
        begin = at + separator.size();
    }
    const bool unbound = description == "Unbound";

    const float scale = EditorFonts().scale;
    ImFont* mono = EditorFonts().mono ? EditorFonts().mono : ImGui::GetFont();
    const float monoSize = mono->FontSize;
    const float padX = 7.0f * scale;
    const float gap = 6.0f * scale;
    const float orWidth = ImGui::CalcTextSize("or").x;
    const float height = ImGui::GetFrameHeight();

    float width = 0.0f;
    for (size_t i = 0; i < caps.size(); ++i)
    {
        if (i != 0)
            width += gap + orWidth + gap;
        width += mono->CalcTextSizeA(monoSize, FLT_MAX, 0.0f, caps[i].c_str()).x + padX * 2.0f;
    }

    const ImVec2 start = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(width, height));
    const bool hovered = interactive && ImGui::IsItemHovered();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float rounding = ImGui::GetStyle().FrameRounding;
    const float capH = height - 4.0f * scale;
    const float capY = start.y + (height - capH) * 0.5f;
    float x = start.x;
    for (size_t i = 0; i < caps.size(); ++i)
    {
        if (i != 0)
        {
            x += gap;
            draw->AddText(ImVec2(x, start.y + (height - ImGui::GetFontSize()) * 0.5f),
                ImGui::GetColorU32(EditorPalette::Label), "or");
            x += orWidth + gap;
        }
        const float textW = mono->CalcTextSizeA(monoSize, FLT_MAX, 0.0f, caps[i].c_str()).x;
        const ImVec2 a(x, capY);
        const ImVec2 b(x + textW + padX * 2.0f, capY + capH);
        if (unbound)
        {
            draw->AddRect(a, b, ImGui::GetColorU32(hovered ? EditorPalette::IndigoLine : EditorPalette::Border), rounding);
        }
        else
        {
            draw->AddRectFilled(a, b, ImGui::GetColorU32(hovered ? EditorPalette::Hover : EditorPalette::Field), rounding);
            // A darker lower lip so the cap reads as a key.
            draw->AddLine(ImVec2(a.x + rounding, b.y - 1.0f), ImVec2(b.x - rounding, b.y - 1.0f),
                ImGui::GetColorU32(EditorPalette::Border), 2.0f);
            if (hovered)
                draw->AddRect(a, b, ImGui::GetColorU32(EditorPalette::IndigoLine), rounding);
        }
        draw->AddText(mono, monoSize, ImVec2(a.x + padX, capY + (capH - monoSize) * 0.5f),
            ImGui::GetColorU32(unbound ? EditorPalette::Label : EditorPalette::Text), caps[i].c_str());
        x = b.x;
    }
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return interactive && clicked;
}

namespace {

ImVec4 ModalToneColor(ModalTone tone)
{
    switch (tone)
    {
    case ModalTone::Warning: return EditorPalette::Play;
    case ModalTone::Danger: return EditorPalette::Error;
    case ModalTone::Info:
    default: return EditorPalette::IndigoLine;
    }
}

// Padding is read at Begin and spacing while the content is submitted, so
// both stay pushed from BeginEditorModal until EndEditorModal.
constexpr int kModalStyleVars = 2;

float ButtonWidth(const DialogButton& button)
{
    const float minWidth = 84.0f * EditorFonts().scale;
    return std::max(minWidth,
        ImGui::CalcTextSize(button.label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 4.0f);
}

} // anonymous namespace

bool BeginEditorModal(const char* name, const char* icon, const char* title,
    ModalTone tone, float width, bool* p_open)
{
    const float scale = EditorFonts().scale;
    const float w = width * scale;
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.0f), ImVec2(w, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * scale, 16.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * scale, 8.0f * scale));
    if (!ImGui::BeginPopupModal(name, p_open,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::PopStyleVar(kModalStyleVars);
        return false;
    }

    ImFont* bold = EditorFonts().bold ? EditorFonts().bold : ImGui::GetFont();
    ImGui::PushStyleColor(ImGuiCol_Text, ModalToneColor(tone));
    ImGui::TextUnformatted(icon);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 10.0f * scale);
    ImGui::PushFont(bold);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 2.0f * scale));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    return true;
}

void EndEditorModal()
{
    ImGui::PopTextWrapPos();
    ImGui::EndPopup();
    ImGui::PopStyleVar(kModalStyleVars);
}

void DialogText(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, EditorPalette::TextSecondary);
    ImGui::TextWrappedV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

int DialogButtons(std::initializer_list<DialogButton> buttons)
{
    const float spacing = 8.0f * EditorFonts().scale;
    float total = 0.0f;
    for (const DialogButton& button : buttons)
        total += ButtonWidth(button);
    total += spacing * static_cast<float>(buttons.size() - 1);

    ImGui::Dummy(ImVec2(0.0f, 4.0f * EditorFonts().scale));
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - total));

    int clicked = -1;
    int index = 0;
    for (const DialogButton& button : buttons)
    {
        if (index != 0)
            ImGui::SameLine(0.0f, spacing);
        int colors = 0;
        if (button.tone == ButtonTone::Primary)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, EditorPalette::Indigo);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.36f, 0.33f, 0.93f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.26f, 0.23f, 0.82f, 1.0f));
            colors = 3;
        }
        else if (button.tone == ButtonTone::Danger)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.66f, 0.22f, 0.24f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.46f, 0.15f, 0.17f, 1.0f));
            colors = 3;
        }
        if (colors)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            ++colors;
        }
        ImGui::BeginDisabled(!button.enabled);
        if (ImGui::Button(button.label, ImVec2(ButtonWidth(button), 0.0f)) && button.enabled)
            clicked = index;
        ImGui::EndDisabled();
        if (colors)
            ImGui::PopStyleColor(colors);
        ++index;
    }
    return clicked;
}

void Spinner(float radius)
{
    const float size = ImGui::GetFontSize();
    const float r = radius > 0.0f ? radius : size * 0.5f;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float height = std::max(r * 2.0f, size);
    ImGui::Dummy(ImVec2(r * 2.0f, height));
    const ImVec2 center(start.x + r, start.y + height * 0.5f);
    const float thickness = std::max(2.0f, r * 0.22f);
    const float ring = r - thickness * 0.5f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddCircle(center, ring, ImGui::GetColorU32(EditorPalette::Border), 32, thickness);
    const float t = static_cast<float>(ImGui::GetTime());
    const float a0 = t * 6.0f;
    const float sweep = 1.6f + 0.9f * std::sin(t * 2.4f);
    draw->PathArcTo(center, ring, a0, a0 + sweep, 24);
    draw->PathStroke(ImGui::GetColorU32(EditorPalette::IndigoLine), false, thickness);
}

} // namespace rt2::ui
