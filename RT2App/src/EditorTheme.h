#pragma once

#ifndef RT2_EDITOR_THEME_H
#define RT2_EDITOR_THEME_H

// ============================================================================
// EditorTheme — the editor's visual policy ("Graphite").
//
// Near-neutral graphite surfaces, one indigo accent for focus and selection,
// amber reserved for Play-mode chrome (drawn by callers, not set here).
// Starts from StyleColorsDark, then assigns every token ABSOLUTELY, and
// scales metrics from fixed base values, so applying twice yields exactly
// the first result.
//
// The design reference lists every token with its ImGuiCol_ slot; keep
// EditorPalette and that sheet in step.
//
// ImGui-dependent by nature: links into RT2App and RT2ImGuiProbe, never into
// RT2Tests / RT2SliceRunner (CPU-only boundary).
// ============================================================================

struct ImVec4;

namespace rt2 {

// Named colors for widgets the theme cannot express (axis tags, Play
// chrome, type tints). Values are linear 0-1 sRGB like ImGui's own.
namespace EditorPalette {
extern const ImVec4 Chrome;       // title bar, status bar
extern const ImVec4 Panel;        // window background
extern const ImVec4 Field;        // inputs
extern const ImVec4 Hover;
extern const ImVec4 Border;
extern const ImVec4 Text;
extern const ImVec4 TextSecondary;
extern const ImVec4 Label;        // property labels, captions
extern const ImVec4 Selection;
extern const ImVec4 Indigo;       // filled primary controls
extern const ImVec4 IndigoLine;   // focus lines, accent text
extern const ImVec4 Play;         // Play-mode chrome only
extern const ImVec4 Success;
extern const ImVec4 Error;
extern const ImVec4 AxisX;
extern const ImVec4 AxisY;
extern const ImVec4 AxisZ;
} // namespace EditorPalette

// scale: the content scale the fonts were built at (EditorFonts().scale).
void ApplyEditorTheme(float scale = 1.0f);

} // namespace rt2

#endif // RT2_EDITOR_THEME_H
