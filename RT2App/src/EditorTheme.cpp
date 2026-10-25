#include "EditorTheme.h"

#include <imgui.h>

namespace rt2 {

namespace {

ImVec4 Hex(unsigned rgb, float a = 1.0f)
{
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f,
                  (rgb & 0xFF) / 255.0f, a);
}

} // anonymous namespace

namespace EditorPalette {
const ImVec4 Chrome        = Hex(0x111215);
const ImVec4 Panel         = Hex(0x15161A);
const ImVec4 Field         = Hex(0x1C1E23);
const ImVec4 Hover         = Hex(0x23252B);
const ImVec4 Border        = Hex(0x2A2C33);
const ImVec4 Text          = Hex(0xE6E7EA);
const ImVec4 TextSecondary = Hex(0xA4A7B0);
const ImVec4 Label         = Hex(0x858993);
const ImVec4 Selection     = Hex(0x262A4A);
const ImVec4 Indigo        = Hex(0x4F46E5);
const ImVec4 IndigoLine    = Hex(0x7C83FF);
const ImVec4 Play          = Hex(0xF5A524);
const ImVec4 Success       = Hex(0x4ADE80);
const ImVec4 Error         = Hex(0xF26D6D);
const ImVec4 AxisX         = Hex(0xF26D6D);
const ImVec4 AxisY         = Hex(0x7BD88F);
const ImVec4 AxisZ         = Hex(0x6AA6FF);
} // namespace EditorPalette

void ApplyEditorTheme(float scale)
{
    using namespace EditorPalette;

    // Base definition first so every token below fully determines the
    // result; calling this twice (or after Walnut's own setup) converges.
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;

    const ImVec4 ground = Hex(0x0E0F11);
    const ImVec4 divider = Hex(0x22242A);
    const ImVec4 selectionHover = Hex(0x2E3363);

    c[ImGuiCol_Text] = Text;
    c[ImGuiCol_TextDisabled] = Label;
    c[ImGuiCol_WindowBg] = Panel;
    c[ImGuiCol_ChildBg] = Panel;
    c[ImGuiCol_PopupBg] = Hex(0x1A1B20);
    c[ImGuiCol_Border] = Border;
    c[ImGuiCol_BorderShadow] = Hex(0x000000, 0.0f);
    c[ImGuiCol_FrameBg] = Field;
    c[ImGuiCol_FrameBgHovered] = Hover;
    c[ImGuiCol_FrameBgActive] = Hover;
    c[ImGuiCol_TitleBg] = Chrome;
    c[ImGuiCol_TitleBgActive] = Chrome;
    c[ImGuiCol_TitleBgCollapsed] = Chrome;
    c[ImGuiCol_MenuBarBg] = Chrome;
    c[ImGuiCol_ScrollbarBg] = Hex(0x000000, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = Border;
    c[ImGuiCol_ScrollbarGrabHovered] = Hex(0x3A3C44);
    c[ImGuiCol_ScrollbarGrabActive] = Hex(0x4A4D57);
    c[ImGuiCol_CheckMark] = IndigoLine;
    c[ImGuiCol_SliderGrab] = IndigoLine;
    c[ImGuiCol_SliderGrabActive] = Hex(0xA5AAFF);
    // Ordinary buttons are quiet; primary actions push Indigo themselves.
    c[ImGuiCol_Button] = Field;
    c[ImGuiCol_ButtonHovered] = Hover;
    c[ImGuiCol_ButtonActive] = Selection;
    c[ImGuiCol_Header] = Selection;
    c[ImGuiCol_HeaderHovered] = Hover;
    c[ImGuiCol_HeaderActive] = selectionHover;
    c[ImGuiCol_Separator] = divider;
    c[ImGuiCol_SeparatorHovered] = IndigoLine;
    c[ImGuiCol_SeparatorActive] = IndigoLine;
    c[ImGuiCol_ResizeGrip] = Hex(0x000000, 0.0f);
    c[ImGuiCol_ResizeGripHovered] = IndigoLine;
    c[ImGuiCol_ResizeGripActive] = IndigoLine;
    // Tabs: inactive tabs sink into the chrome, the active one joins its
    // panel, so the selected tab reads as the panel's own header.
    c[ImGuiCol_Tab] = Chrome;
    c[ImGuiCol_TabHovered] = Hover;
    c[ImGuiCol_TabActive] = Panel;
    c[ImGuiCol_TabUnfocused] = Chrome;
    c[ImGuiCol_TabUnfocusedActive] = Panel;
    c[ImGuiCol_DockingPreview] = Hex(0x7C83FF, 0.45f);
    c[ImGuiCol_DockingEmptyBg] = ground;
    c[ImGuiCol_PlotLines] = TextSecondary;
    c[ImGuiCol_PlotLinesHovered] = IndigoLine;
    c[ImGuiCol_PlotHistogram] = IndigoLine;
    c[ImGuiCol_PlotHistogramHovered] = Hex(0xA5AAFF);
    c[ImGuiCol_TableHeaderBg] = Field;
    c[ImGuiCol_TableBorderStrong] = Border;
    c[ImGuiCol_TableBorderLight] = divider;
    c[ImGuiCol_TableRowBg] = Hex(0x000000, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = Hex(0xFFFFFF, 0.02f);
    c[ImGuiCol_TextSelectedBg] = Hex(0x4F46E5, 0.45f);
    c[ImGuiCol_DragDropTarget] = IndigoLine;
    c[ImGuiCol_NavHighlight] = IndigoLine;
    c[ImGuiCol_NavWindowingHighlight] = Text;
    c[ImGuiCol_NavWindowingDimBg] = Hex(0x000000, 0.5f);
    c[ImGuiCol_ModalWindowDimBg] = Hex(0x000000, 0.55f);

    // Metrics at scale 1.0, then scaled. Assigned absolutely each call, so
    // scaling never compounds.
    style.WindowPadding = ImVec2(10.0f, 10.0f);
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.CellPadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(6.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.IndentSpacing = 16.0f;
    style.GrabMinSize = 10.0f;
    style.ScrollbarSize = 10.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 5.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    // No frame borders: ImGui would draw them on every button too. Fields
    // read against the panel by fill alone.
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    style.WindowMenuButtonPosition = ImGuiDir_None;
    if (scale > 0.0f && scale != 1.0f)
        style.ScaleAllSizes(scale);
}

} // namespace rt2
