#pragma once

#ifndef RT2_EDITOR_FONTS_H
#define RT2_EDITOR_FONTS_H

#include <string>

struct ImFont;

// ============================================================================
// EditorFonts — the editor's font set (Graphite design).
//
// Inter for UI text, JetBrains Mono for numbers, and Lucide icons merged into
// every Inter font so RT2_ICON_* strings (EditorIcons.h) render inline. Loaded
// once from <exe dir>/fonts/ by Walnut's fontLoader hook, before the atlas is
// uploaded; sizes are multiplied by the monitor content scale.
//
// A missing or unreadable font file is logged and the whole set falls back to
// ImGui's built-in font (every pointer below then refers to it), so the editor
// still starts; EditorFonts().error says why.
//
// ImGui-dependent: links into RT2App only, never into CPU test targets.
// ============================================================================

namespace rt2 {

struct EditorFontSet
{
    ImFont* body = nullptr;   // Inter Regular, default UI font
    ImFont* medium = nullptr; // Inter Medium, labels and tabs
    ImFont* bold = nullptr;   // Inter SemiBold, card headers and titles
    ImFont* mono = nullptr;   // JetBrains Mono, numeric fields and readouts
    ImFont* icons = nullptr;  // Lucide alone at a larger size, toolbars
    float scale = 1.0f;       // content scale the sizes were built at
    bool fallback = false;    // true when the built-in font is in use
    std::string error;        // why fallback is true
};

// Adds the font set to ImGui::GetIO().Fonts and sets FontDefault. Returns
// false (and falls back) when any file is missing or fails to load.
bool LoadEditorFonts(float contentScale);

const EditorFontSet& EditorFonts();

} // namespace rt2

#endif // RT2_EDITOR_FONTS_H
