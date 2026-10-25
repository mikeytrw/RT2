#pragma once

#ifndef RT2_EDITOR_WIDGETS_H
#define RT2_EDITOR_WIDGETS_H

// ============================================================================
// EditorWidgets — shared presentation helpers for the Graphite panels
// (Inspector, Renderer, Performance).
//
// Presentation only: none of these own state. RowLabel and InspectorVec3Row
// keep the wrapped widget's item semantics, so callers' IsItemActivated /
// IsItemDeactivatedAfterEdit checks (preview sessions) behave unchanged.
//
// ImGui-dependent: links into RT2App only, never into CPU test targets.
// ============================================================================

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

} // namespace rt2::ui

#endif // RT2_EDITOR_WIDGETS_H
