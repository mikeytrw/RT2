#include "EditorTheme.h"

#include <imgui.h>

namespace rt2 {

namespace {

// Linear-space-ish SRGB picks (ImGui consumes floats; no gamma pipeline).
// Neutrals carry no blue tint: R == G == B on every panel/border/text token.
constexpr float kNeutral900[4] = { 0.086f, 0.086f, 0.086f, 1.00f }; // main surfaces
constexpr float kNeutral850[4] = { 0.118f, 0.118f, 0.118f, 1.00f }; // raised surfaces
constexpr float kNeutral800[4] = { 0.157f, 0.157f, 0.157f, 1.00f }; // frames/inputs
constexpr float kNeutral700[4] = { 0.208f, 0.208f, 0.208f, 1.00f }; // hover fills
constexpr float kBorder[4]      = { 0.267f, 0.267f, 0.267f, 1.00f }; // neutral borders
constexpr float kText[4]        = { 0.894f, 0.894f, 0.894f, 1.00f }; // primary text
constexpr float kTextDim[4]     = { 0.635f, 0.635f, 0.635f, 1.00f }; // disabled/readable
// Indigo 500 family: focus ring, selection, primary controls.
constexpr float kIndigo[4]       = { 0.388f, 0.400f, 0.945f, 1.00f };
constexpr float kIndigoHover[4]  = { 0.470f, 0.478f, 0.957f, 1.00f };
constexpr float kIndigoActive[4] = { 0.312f, 0.329f, 0.898f, 1.00f };
// Violet 500: restrained secondary accent (selected tabs only).
constexpr float kViolet[4]       = { 0.545f, 0.361f, 0.965f, 1.00f };
constexpr float kVioletDim[4]    = { 0.545f, 0.361f, 0.965f, 0.35f };

void Set(ImVec4& slot, const float token[4])
{
    slot.x = token[0];
    slot.y = token[1];
    slot.z = token[2];
    slot.w = token[3];
}

} // anonymous namespace

void ApplyEditorTheme()
{
    // Base definition first so every token below fully determines the
    // result; calling this twice (or after Walnut's own setup) converges.
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;

    // Flat dark neutral-grey surfaces. No blue-tinted panel backgrounds.
    Set(c[ImGuiCol_Text], kText);
    Set(c[ImGuiCol_TextDisabled], kTextDim);
    Set(c[ImGuiCol_WindowBg], kNeutral900);
    Set(c[ImGuiCol_ChildBg], kNeutral900);
    Set(c[ImGuiCol_PopupBg], kNeutral850);
    Set(c[ImGuiCol_Border], kBorder);
    Set(c[ImGuiCol_BorderShadow], kNeutral900);
    Set(c[ImGuiCol_FrameBg], kNeutral800);
    Set(c[ImGuiCol_FrameBgHovered], kNeutral700);
    Set(c[ImGuiCol_FrameBgActive], kNeutral700);
    Set(c[ImGuiCol_TitleBg], kNeutral850);
    Set(c[ImGuiCol_TitleBgActive], kNeutral850);
    Set(c[ImGuiCol_TitleBgCollapsed], kNeutral900);
    Set(c[ImGuiCol_MenuBarBg], kNeutral850);
    Set(c[ImGuiCol_ScrollbarBg], kNeutral900);
    Set(c[ImGuiCol_ScrollbarGrab], kNeutral700);
    Set(c[ImGuiCol_ScrollbarGrabHovered], kBorder);
    Set(c[ImGuiCol_ScrollbarGrabActive], kBorder);
    Set(c[ImGuiCol_CheckMark], kIndigo);
    Set(c[ImGuiCol_SliderGrab], kIndigo);
    Set(c[ImGuiCol_SliderGrabActive], kIndigoActive);
    Set(c[ImGuiCol_Button], kNeutral800);
    Set(c[ImGuiCol_ButtonHovered], kIndigoHover);
    Set(c[ImGuiCol_ButtonActive], kIndigoActive);
    // Selection + focus read indigo; selected text stays full-bright.
    Set(c[ImGuiCol_Header], kIndigoActive);
    Set(c[ImGuiCol_HeaderHovered], kIndigoHover);
    Set(c[ImGuiCol_HeaderActive], kIndigo);
    Set(c[ImGuiCol_Separator], kBorder);
    Set(c[ImGuiCol_SeparatorHovered], kIndigo);
    Set(c[ImGuiCol_SeparatorActive], kIndigo);
    Set(c[ImGuiCol_ResizeGrip], kNeutral700);
    Set(c[ImGuiCol_ResizeGripHovered], kIndigo);
    Set(c[ImGuiCol_ResizeGripActive], kIndigo);
    // Tabs: neutral everywhere except the selected accent (violet, dimmed
    // unfocused wash so restraint holds with many tabs open).
    Set(c[ImGuiCol_Tab], kNeutral800);
    Set(c[ImGuiCol_TabHovered], kIndigoHover);
    Set(c[ImGuiCol_TabActive], kViolet);
    Set(c[ImGuiCol_TabUnfocused], kNeutral800);
    Set(c[ImGuiCol_TabUnfocusedActive], kVioletDim);
    Set(c[ImGuiCol_DockingPreview], kIndigo);
    Set(c[ImGuiCol_DockingEmptyBg], kNeutral900);
    // Plots keep conventional semantics (histogram blue preserved).
    Set(c[ImGuiCol_PlotLines], kTextDim);
    Set(c[ImGuiCol_PlotLinesHovered], kIndigoHover);
    Set(c[ImGuiCol_PlotHistogram], kIndigo);
    Set(c[ImGuiCol_PlotHistogramHovered], kIndigoHover);
    Set(c[ImGuiCol_TextSelectedBg], kIndigoActive);
    Set(c[ImGuiCol_DragDropTarget], kViolet);
    Set(c[ImGuiCol_NavHighlight], kIndigo);
    Set(c[ImGuiCol_NavWindowingHighlight], kText);
    Set(c[ImGuiCol_NavWindowingDimBg], kNeutral900);
    Set(c[ImGuiCol_ModalWindowDimBg], kNeutral900);

    // Compact metrics (match Walnut's setup absolutely so order is moot).
    style.WindowPadding = ImVec2(6.0f, 6.0f);
    style.FramePadding = ImVec2(3.0f, 2.0f);
    style.ItemSpacing = ImVec2(6.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 3.0f);
    style.IndentSpacing = 14.0f;
    style.GrabMinSize = 8.0f;
    style.ScrollbarSize = 12.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 2.0f;
    style.PopupRounding = 2.0f;
    style.ScrollbarRounding = 3.0f;
    style.GrabRounding = 2.0f;
    style.TabRounding = 2.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
}

} // namespace rt2
