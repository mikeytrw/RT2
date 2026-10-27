#pragma once

#ifndef RT2_EDITOR_WIDGETS_H
#define RT2_EDITOR_WIDGETS_H

// ============================================================================
// EditorWidgets — shared presentation helpers for the Graphite panels
// (Inspector, Renderer, Performance, menus and dialogs).
//
// Presentation only: none of these own state. RowLabel and InspectorVec3Row
// keep the wrapped widget's item semantics, so callers' IsItemActivated /
// IsItemDeactivatedAfterEdit checks (preview sessions) behave unchanged.
//
// ImGui-dependent: links into RT2App only, never into CPU test targets.
// ============================================================================

#include <initializer_list>
#include <string>

namespace rt2::ui {

// A card's title: tinted icon and semibold title.
void InspectorSectionHeader(const char* icon, const char* title);

// A group inside a card (physics body, shape, constraints).
void InspectorSubHeader(const char* title);

// Width of the left label column for the current window.
float InspectorLabelWidth();

// Left-column property label; the next widget fills the rest of the row.
void InspectorRowLabel(const char* label);

// Label-left form of any labelled widget, usable inside a condition:
//     if (ImGui::DragFloat(RowLabel("Mass"), &mass, 0.01f)) ...
// Draws the visible label (text before any "##") in the left column, sets
// the next item to fill the row, and returns a hidden ID ("##" + label) for
// the widget. The returned buffer is reused by the next call; ImGui only
// reads it during the widget call.
const char* RowLabel(const char* label);

// Label-left X/Y/Z row with axis-colored tags inside each field.
bool InspectorVec3Row(const char* label, float* v, float speed,
    float vMin, float vMax, const char* format);
bool InspectorVec3Row(const char* label, float* v, float speed, const char* format = "%.3f");

// A collapsible section header: chevron, tinted icon, semibold title and a
// dim one-line summary on the right (shown whether open or closed). Open
// state lives in the window's ImGui storage under id. Returns true when open.
bool CollapsibleSection(const char* id, const char* icon, const char* title,
    const char* summary, bool defaultOpen = false);

// "icon  text" for menu items, so every item keeps the same icon column.
std::string MenuLabel(const char* icon, const char* text);

// An input binding description ("Ctrl+Z or Ctrl+Y") drawn as one keycap per
// alternative, in the mono font. "Unbound" draws as a dim outlined cap. The
// whole run is one item; returns true when clicked. A non-interactive run
// draws without hover feedback.
bool BindingKeycaps(const char* id, const std::string& description, bool interactive = true);

// Modal dialogs: untitled, centered, fixed width, with a tinted icon and a
// semibold title as the header. name is the popup ID that OpenPopup uses;
// p_open behaves as in BeginPopupModal (the dialog closes when it goes
// false). A true return must be paired with EndEditorModal().
enum class ModalTone { Info, Warning, Danger };
bool BeginEditorModal(const char* name, const char* icon, const char* title,
    ModalTone tone = ModalTone::Info, float width = 420.0f, bool* p_open = nullptr);
void EndEditorModal();

// Dialog body text, wrapped to the dialog width, in the secondary color.
void DialogText(const char* fmt, ...);

// The dialog's right-aligned button row. Returns the index of the button
// clicked this frame, or -1. Disabled buttons never report a click.
enum class ButtonTone { Secondary, Primary, Danger };
struct DialogButton
{
    const char* label;
    ButtonTone tone = ButtonTone::Secondary;
    bool enabled = true;
};
int DialogButtons(std::initializer_list<DialogButton> buttons);

// A small indeterminate spinner (rotating arc) at the cursor, one line high.
void Spinner(float radius = 0.0f);

} // namespace rt2::ui

#endif // RT2_EDITOR_WIDGETS_H
