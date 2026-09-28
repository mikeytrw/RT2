#pragma once

#ifndef RT2_EDITOR_TOOLBAR_H
#define RT2_EDITOR_TOOLBAR_H

#include "EditorActionQueue.h"
#include "SceneRunState.h"

#include <imgui.h>

#include <functional>
#include <string>

// ============================================================================
// EditorToolbar — the persistent native toolbar (workspace package 3).
//
// Rendered by the host in its pre-dock hook, above the dockspace: full-width
// strip, present even with Scene closed or the viewport floating. Reads a
// snapshot, emits action requests, owns no scene state and no history. Real
// effects happen at dispatch in RT2 owner callbacks.
//
// Overflow is measured, not guessed: Save + transport stay visible;
// Undo/Redo/Add collapse into an overflow popup at narrow widths. Buttons
// carry text tooltips (no invented icon font). The shared Add contents
// render here and in the Outliner popup from one function; each caller owns
// its popup scope (glossary popup rule: no PushID/popup wrapping around the
// OpenPopup/BeginPopup pair).
//
// ImGui-only: links into RT2App and RT2ImGuiProbe, never CPU closures.
// ============================================================================

namespace rt2 {

// Probe recorder for synthesized clicks (see SharedAddMenuContext::hooks).
// Production passes nullptr; the widget code path is identical either way.
struct EditorToolbarProbeHooks
{
    std::function<void(const char* label, bool enabled,
        const ImVec2& min, const ImVec2& max)> onItem;
};

struct EditorToolbarSnapshot
{
    core::EditorActionAvailability availability;
    core::SceneRunState runState = core::SceneRunState::Edit;
    bool documentDirty = false;
    std::string documentLabel; // scene filename, "(untitled)", or project tag
};

// One shared Add menu body. Call with an emit context from inside the
// caller's OWN popup (BeginPopup already open): Outliner "AddEntity" and
// the toolbar's "ToolbarAddEntity"/overflow popups. Mirrors the legacy
// Outliner items, separators, Light submenu, and gating exactly; clicks
// enqueue instead of executing (dialogs defer to dispatch in the owner).
void RenderSharedAddMenuContents(const core::SharedAddMenuContext& context,
    const EditorToolbarProbeHooks* hooks = nullptr);

// Persistent strip. No-op when headless (the host skips the call); safe to
// call every frame — emission is event-driven, persistence untouched.
void RenderEditorToolbar(
    const EditorToolbarSnapshot& snapshot, core::EditorActionQueue& queue,
    const EditorToolbarProbeHooks* hooks = nullptr);

} // namespace rt2

#endif // RT2_EDITOR_TOOLBAR_H
