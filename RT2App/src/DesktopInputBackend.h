#pragma once

#ifndef RT2_CORE_DESKTOP_INPUT_BACKEND_H
#define RT2_CORE_DESKTOP_INPUT_BACKEND_H

#include "InputStateMachine.h"

namespace rt2::core {

// ============================================================================
// DesktopInputBackend — collects a RawInputSnapshot per frame by
// polling Walnut::Input, ImGui::GetIO(), glfwGetGamepadState, and
// glfwGetWindowAttrib(GLFW_FOCUSED).
//
// The backend has NO state and NO context logic. It is the single
// GLFW/ImGui/Walnut-touching code path in the input system. It does
// not register any GLFW callbacks (ImGui's backend already installs
// key/mouse/scroll/window-focus callbacks; competing callbacks would
// clobber them — see docs/input-system.md §"Focus loss").
//
// The backend collects only the raw-down state of keys/buttons/axes
// referenced by the active context stack. The caller (InputService)
// passes the set of keys/buttons/axes to poll so the backend does not
// poll the entire keyboard every frame.
// ============================================================================

struct DesktopInputPollList
{
    // Keys (KeyCode values) to poll via Walnut::Input::IsKeyDown.
    std::vector<uint16_t> keys;
    // Mouse buttons (MouseButton values) to poll via
    // Walnut::Input::IsMouseButtonDown.
    std::vector<uint16_t> mouseButtons;
    // Gamepad axes to poll (per slot). Each entry is (slot, axisIndex).
    std::vector<std::pair<int, uint8_t>> gamepadAxes;
    // True if any gamepad buttons are bound (so we call
    // glfwGetGamepadState for the slot).
    bool pollGamepadButtons = false;
};

class DesktopInputBackend
{
public:
    // Collect a raw snapshot for this frame. The poll list is built by
    // InputService from the active context stack.
    RawInputSnapshot CaptureFrame(const DesktopInputPollList& poll);
};

} // namespace rt2::core

#endif // RT2_CORE_DESKTOP_INPUT_BACKEND_H