#pragma once

#ifndef RT2_EDITOR_THEME_H
#define RT2_EDITOR_THEME_H

// ============================================================================
// EditorTheme — the native workspace visual policy (package 2).
//
// Flat dark neutral-grey surfaces, indigo primary/focus/selection, restrained
// violet secondary accents. Starts from StyleColorsDark, then assigns every
// token ABSOLUTELY (never scales or accumulates), so repeated application
// cannot compound: applying twice yields exactly the first result.
//
// Owner: RT2 (Walnut only creates the context, font, and compact metrics).
// The embedded Roboto font is untouched here. Engine semantics (XYZ gizmo
// colors, warning/error/runtime-state presentation, the Session histogram
// blue) are preserved: this file owns only ImGui style + color tokens.
//
// ImGui-dependent by nature: links into RT2App and RT2ImGuiProbe, never into
// RT2Tests / RT2SliceRunner (CPU-only boundary).
// ============================================================================

namespace rt2 {

void ApplyEditorTheme();

} // namespace rt2

#endif // RT2_EDITOR_THEME_H
