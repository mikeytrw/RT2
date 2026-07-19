#pragma once

#ifndef RT2_CORE_INPUT_STATE_MACHINE_H
#define RT2_CORE_INPUT_STATE_MACHINE_H

#include "InputTypes.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

namespace rt2::core {

// ============================================================================
// InputStateMachine — CPU-only state machine for the Phase 5 input
// action system.
//
// Owns: the context stack, per-binding previous/current down state,
// edge computation (per-action after combining bindings), axis
// computation (sign-preserving dead zone), focus-loss state, mouse
// delta, scroll delta.
//
// The state machine is pure CPU: it does not call GLFW, ImGui, or
// Walnut. The desktop backend (DesktopInputBackend) collects a raw
// snapshot and hands it to BeginFrame; the state machine derives all
// edges and axis values from that snapshot.
//
// Resolution policy: physical-source consumption with lower-context
// blocking. The topmost context in the stack that maps a physical
// source (key, mouse button, gamepad button/axis) claims it; lower
// contexts' bindings on the same source do not fire. The claim is on
// the physical source, NOT on the modifier-gated binding — a higher
// context's Ctrl+S binding claims physical S and suppresses a lower
// context's plain-S binding even when Ctrl is not held.
//
// Frame phasing: actions and axes are recomputed ONLY in BeginFrame.
// A context pushed AFTER BeginFrame (in the same frame) does NOT
// affect action reads until the next BeginFrame. The host must not
// read edge-triggered actions (Pressed/Released) on a freshly-pushed
// context in the same frame; Held/axis reads are benign (one-frame
// latency for continuous consumers like the fly-camera).
//
// Test seam: SetSampleState injects a synthetic raw snapshot without
// any GLFW/ImGui call. This is the backdoor used by
// InputStateMachineTests.cpp.
// ============================================================================

// A single source-identity key in the consumption claim map. Two
// bindings claim the same source iff their SourceKey is equal.
struct SourceKey
{
    InputDeviceKind device = InputDeviceKind::KeyboardKey;
    uint16_t code = 0;
    int gamepadSlot = -1;   // -1 = "any slot"

    bool operator==(const SourceKey& o) const
    { return device == o.device && code == o.code && gamepadSlot == o.gamepadSlot; }
};

struct SourceKeyHash
{
    size_t operator()(const SourceKey& k) const noexcept
    {
        // Pack device + code + slot into a 64-bit key.
        uint64_t v = (static_cast<uint64_t>(k.device) << 48)
                   | (static_cast<uint64_t>(k.code)     << 32)
                   | static_cast<uint64_t>(uint32_t(int32_t(k.gamepadSlot)));
        return std::hash<uint64_t>{}(v);
    }
};

// Raw snapshot of one frame's input state, as collected by the desktop
// backend. The state machine does not care how the data was gathered —
// only what it contains. Test fixtures construct this directly.
struct RawInputSnapshot
{
    // Keyboard keys currently down (KeyCode values).
    std::unordered_set<uint16_t> keysDown;
    // Mouse buttons currently down (MouseButton values).
    std::unordered_set<uint16_t> mouseButtonsDown;
    // Modifier state.
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool super = false;
    // Mouse position and scroll for this frame.
    glm::vec2 mousePos = {0.0f, 0.0f};
    float scrollDelta = 0.0f;
    // Per-gamepad-slot state. gamepadPresent[slot] indicates the slot
    // has a connected gamepad this frame.
    static constexpr int kMaxGamepadSlots = 4;
    bool gamepadPresent[kMaxGamepadSlots] = {};
    // gamepadButtons[slot][GLFW_GAMEPAD_BUTTON_*] = true if pressed.
    bool gamepadButtons[kMaxGamepadSlots][15] = {};
    // gamepadAxes[slot][GLFW_GAMEPAD_AXIS_*] = raw float in [-1, 1].
    float gamepadAxes[kMaxGamepadSlots][6] = {};
    // Window focus (glfwGetWindowAttrib(GLFW_FOCUSED)).
    bool windowFocused = true;
    // UI capture state (sampled by the desktop backend from
    // ImGui::GetIO()). The state machine does not interpret these
    // directly — the InputService applies the editor routing policy
    // via SuppressKeyboardActions / SuppressMouseActions before
    // querying actions. Captured here for completeness.
    bool imguiWantTextInput = false;
    bool imguiWantCaptureKeyboard = false;
    bool imguiWantCaptureMouse = false;
    bool imguiAnyItemActive = false;
    bool viewportHovered = false;
    bool gizmoConsumesMouse = false;
};

class InputStateMachine
{
public:
    InputStateMachine() = default;

    // ---- Context stack ------------------------------------------------

    void PushContext(InputContext* ctx);
    void PopContext();
    void ClearContextStack();
    const std::vector<InputContext*>& ContextStack() const
    { return m_ContextStack; }

    // ---- Frame phasing ------------------------------------------------

    // Advance the state machine by one frame using a raw snapshot.
    // Computes per-binding current-down state, then per-action edges
    // and per-axis values. Applies the physical-source consumption
    // policy. Does NOT apply ImGui suppression — the InputService
    // does that via SuppressKeyboardActions / SuppressMouseActions
    // between BeginFrame and any consumer query.
    //
    // Mouse delta is computed as (snapshot.mousePos - m_LastMousePos)
    // unless m_ResetMouseHistory is set (focus loss / context
    // transition), in which case the delta is zero and m_LastMousePos
    // is re-seeded.
    void BeginFrame(const RawInputSnapshot& snapshot);

    // Mark a frame's worth of keyboard-sourced actions as None (e.g.
    // when io.WantTextInput is true). Called by InputService::ResolveUI.
    void SuppressKeyboardActions();

    // Mark a frame's worth of mouse-sourced actions as None (e.g. when
    // io.WantCaptureMouse is true and the viewport is not hovered).
    void SuppressMouseActions();

    // Suppress a specific action by name (e.g. viewport-pick when the
    // gizmo consumes mouse).
    void SuppressAction(const std::string& name);

    // Commit the current down-state as next frame's "previous" state,
    // clear per-frame deltas (scroll, mouse delta), and reset
    // suppression flags. Called by InputService::EndFrame.
    void EndFrame();

    // Force a mouse-history reset on the next BeginFrame (focus loss,
    // context transition). The first-frame mouse delta will be zero.
    void ResetMouseHistory();

    // ---- Queries ------------------------------------------------------

    ActionState GetActionState(const std::string& name) const;
    float GetAxisValue(const std::string& name) const;
    glm::vec2 GetMouseDelta() const { return m_MouseDelta; }
    float GetScrollDelta() const { return m_ScrollDelta; }

    // Test-only: directly inject a raw snapshot. Equivalent to
    // BeginFrame(snapshot) but also callable from tests without a
    // desktop backend. Returns *this for chaining.
    void SetSampleState(const RawInputSnapshot& snapshot)
    { BeginFrame(snapshot); }

    // True if the most recent BeginFrame saw windowFocused == false.
    bool IsFocusLost() const { return m_FocusLost; }

private:
    // Per-binding down-state, keyed by SourceKey. We track per-source
    // previous/current down, then combine per action.
    struct SourceState
    {
        bool previousDown = false;
        bool currentDown  = false;
    };

    // Compute currentDown for a source from the snapshot.
    bool IsSourceDown(const SourceKey& k, const RawInputSnapshot& s) const;
    // Resolve a gamepad slot: -1 means "any present slot" → first
    // present slot; otherwise the requested slot if present.
    int ResolveGamepadSlot(int requested, const RawInputSnapshot& s) const;

    // Apply physical-source consumption: build the set of sources
    // claimed by contexts above the lowest one that maps a given
    // action. Returns the effective InputMapping pointer + the context
    // index that owns it, or nullptr if no context in the stack maps
    // the name.
    struct ResolvedMapping
    {
        const InputMapping* mapping = nullptr;
        size_t contextIndex = 0;
    };
    ResolvedMapping ResolveMapping(const std::string& name) const;

    // Is a source claimed by a context above the given index?
    bool IsSourceClaimedAbove(const SourceKey& k, size_t ctxIndex) const;

    // Recompute per-action edges and per-axis values from the current
    // down-state and the consumption map. Called at the end of
    // BeginFrame.
    void RecomputeActionsAndAxes(const RawInputSnapshot& snapshot);

    std::vector<InputContext*> m_ContextStack;

    // Per-source down state. Rebuilt each BeginFrame.
    std::unordered_map<SourceKey, SourceState, SourceKeyHash> m_SourceStates;

    // Per-action state this frame.
    struct ActionEntry
    {
        ActionState state = ActionState::None;
        bool suppressed = false;
        // Previous-frame "down" state for this action, including modifier
        // matching. Stored per-action (not per-source) so that a modifier-
        // gated action (e.g. Ctrl+S) correctly transitions None→Pressed
        // when the base key was held last frame WITHOUT the modifier and
        // is held this frame WITH the modifier.
        bool previousActionDown = false;
        // Current-frame "down" state (computed in RecomputeActionsAndAxes,
        // committed to previousActionDown in EndFrame).
        bool currentActionDown = false;
    };
    std::unordered_map<std::string, ActionEntry> m_Actions;

    // Per-axis value this frame.
    std::unordered_map<std::string, float> m_Axes;

    // Sources claimed this frame, mapped to the context index that
    // claimed them. Built in RecomputeActionsAndAxes.
    std::unordered_map<SourceKey, size_t, SourceKeyHash> m_Claimed;

    glm::vec2 m_MouseDelta = {0.0f, 0.0f};
    float m_ScrollDelta = 0.0f;
    glm::vec2 m_LastMousePos = {0.0f, 0.0f};
    bool m_ResetMouseHistory = true;   // first frame ever: no delta

    bool m_FocusLost = false;
    bool m_PrevFocused = true;
    // Cached this frame: m_FocusLost ? false : focused. Gamepad-axis
    // computation reads this (glfwGetGamepadState is focus-independent,
    // so the gate is not implicit there).
    bool m_EffectiveFocus = true;

    // Suppression flags set by InputService::ResolveUI and cleared at
    // EndFrame.
    bool m_SuppressKeyboard = false;
    bool m_SuppressMouse = false;
    std::unordered_set<std::string> m_SuppressedActions;
};

} // namespace rt2::core

#endif // RT2_CORE_INPUT_STATE_MACHINE_H