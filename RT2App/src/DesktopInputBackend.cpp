#include "DesktopInputBackend.h"

#include "Walnut/Input/Input.h"
#include "Walnut/Input/KeyCodes.h"
#include "Walnut/Application.h"

#include <imgui.h>
#include <GLFW/glfw3.h>

#include <algorithm>

namespace rt2::core {

// ============================================================================
// DesktopInputBackend — GLFW/ImGui/Walnut snapshot collection.
//
// This is the single GLFW/ImGui/Walnut-touching code path in the input
// system. It polls; it does not register callbacks. ImGui's GLFW
// backend already installs key/mouse/scroll/window-focus callbacks,
// and competing callbacks would clobber them.
// ============================================================================

RawInputSnapshot DesktopInputBackend::CaptureFrame(
    const DesktopInputPollList& poll)
{
    RawInputSnapshot snap;

    // ---- Keyboard ----
    for (uint16_t key : poll.keys)
        if (Walnut::Input::IsKeyDown(static_cast<Walnut::KeyCode>(key)))
            snap.keysDown.insert(key);

    // ---- Mouse buttons ----
    for (uint16_t btn : poll.mouseButtons)
        if (Walnut::Input::IsMouseButtonDown(static_cast<Walnut::MouseButton>(btn)))
            snap.mouseButtonsDown.insert(btn);

    // ---- Modifiers (from ImGui IO; ImGui's GLFW backend tracks these) ----
    ImGuiIO& io = ImGui::GetIO();
    snap.ctrl  = io.KeyCtrl;
    snap.shift = io.KeyShift;
    snap.alt   = io.KeyAlt;
    snap.super = io.KeySuper;

    // ---- Mouse position + scroll ----
    snap.mousePos = Walnut::Input::GetMousePosition();
    // ImGui accumulates MouseWheel per frame in ImGui_ImplGlfw_NewFrame;
    // it is reset to 0 at the start of each ImGui frame. We read it
    // here for the current frame's value.
    snap.scrollDelta = io.MouseWheel;

    // ---- UI capture state (sampled here; InputService::ResolveUI
    // applies the editor routing policy using these flags) ----
    snap.imguiWantTextInput         = io.WantTextInput;
    snap.imguiWantCaptureKeyboard   = io.WantCaptureKeyboard;
    snap.imguiWantCaptureMouse      = io.WantCaptureMouse;
    snap.imguiAnyItemActive         = ImGui::IsAnyItemActive();
    // viewportHovered / gizmoConsumesMouse are set by InputService::
    // ResolveUI (they depend on panel state, not raw GLFW/ImGui state).

    // ---- Window focus (polled, NOT callback — see header comment) ----
    GLFWwindow* window = Walnut::Application::Get().GetWindowHandle();
    snap.windowFocused = window
        ? (glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0)
        : false;

    // ---- Gamepad state via glfwGetGamepadState ----
    // Build the slot→jid map by enumerating GLFW_JOYSTICK_1..16 in
    // ascending order. Slot 0 = first present gamepad, slot 1 =
    // second, etc. We enumerate every frame so connect/disconnect is
    // detected without a callback.
    int slot = 0;
    for (int jid = 0; jid < 16 && slot < RawInputSnapshot::kMaxGamepadSlots; ++jid)
    {
        if (!glfwJoystickIsGamepad(jid)) continue;
        GLFWgamepadstate state;
        if (!glfwGetGamepadState(jid, &state)) continue;

        snap.gamepadPresent[slot] = true;
        for (int b = 0; b < 15; ++b)
            snap.gamepadButtons[slot][b] = (state.buttons[b] == GLFW_PRESS);
        for (int a = 0; a < 6; ++a)
            snap.gamepadAxes[slot][a] = state.axes[a];
        ++slot;
    }

    (void)poll.pollGamepadButtons;   // polled uniformly above
    return snap;
}

} // namespace rt2::core