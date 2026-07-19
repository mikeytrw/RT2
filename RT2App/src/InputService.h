#pragma once

#ifndef RT2_CORE_INPUT_SERVICE_H
#define RT2_CORE_INPUT_SERVICE_H

#include "InputTypes.h"
#include "InputStateMachine.h"
#include "DesktopInputBackend.h"

namespace rt2::core {

// ============================================================================
// InputService — composes InputStateMachine + DesktopInputBackend.
//
// Implements IInputService. Drives the frame phasing:
//   SampleRaw()    at the top of WalnutApp::OnUpdate (before camera)
//   ResolveUI()    at the top of WalnutApp::OnUIRender (after NewFrame)
//   EndFrame()     at the end of WalnutApp::OnUIRender
//
// Cursor capture is host-controlled: consumers call
// RequestCursorCapture(bool); EndFrame applies the most-recent request
// via Walnut::Input::SetCursorMode.
//
// Context management: PushContext / PopContext / ClearContextStack
// delegate to the state machine. The host (WalnutApp) wires editor/
// runtime/viewport context transitions.
//
// Defaults: LoadDefaults() populates the editor + viewport + runtime
// contexts with the current hardcoded bindings (WASD/QE, right-mouse
// look, Ctrl+Z/Y, Delete, F, Ctrl+C/V/D, Ctrl+1..N, W/E/R gizmo mode)
// plus default gamepad mappings for the runtime context. Called when
// EditorSettingsStore has no inputContexts field.
// ============================================================================

class InputService final : public IInputService
{
public:
    InputService();
    ~InputService() override;

    // ---- Lifecycle ----

    // Build the poll list from the active context stack and capture a
    // raw snapshot via the desktop backend. Then BeginFrame the state
    // machine. Called at the top of WalnutApp::OnUpdate.
    void SampleRaw();

    // Apply the editor routing policy (ImGui suppression, viewport
    // hover, gizmo consumption) and push/pop viewport sub-contexts.
    // Called at the top of WalnutApp::OnUIRender, after ImGui::NewFrame.
    //
    // `viewportHovered` and `gizmoConsumesMouse` are queried by the
    // host from ImGui / EditorTransformGizmo and passed in (the
    // service does not reach into panel state directly).
    void ResolveUI(bool viewportHovered, bool gizmoConsumesMouse);

    // Commit current → previous, clear per-frame deltas, apply cursor
    // capture. Called at the end of WalnutApp::OnUIRender.
    void EndFrame();

    // ---- Context management ----

    void PushContext(InputContext* ctx) { m_State.PushContext(ctx); }
    void PopContext() { m_State.PopContext(); }
    void ClearContextStack() { m_State.ClearContextStack(); }
    InputStateMachine& StateMachine() { return m_State; }

    // Access the owned contexts (LoadDefaults populates these).
    InputContext& EditorContext()   { return m_EditorContext; }
    InputContext& ViewportContext() { return m_ViewportContext; }
    InputContext& ViewportLookContext() { return m_ViewportLookContext; }
    InputContext& RuntimeContext()  { return m_RuntimeContext; }

    // Load built-in defaults (current hardcoded bindings + default
    // gamepad mappings). Called when EditorSettingsStore has no
    // inputContexts field.
    void LoadDefaults();

    // ---- IInputService ----

    ActionState GetActionState(const std::string& name) const override
    { return m_State.GetActionState(name); }
    float GetAxisValue(const std::string& name) const override
    { return m_State.GetAxisValue(name); }
    glm::vec2 GetMouseDelta() const override
    { return m_State.GetMouseDelta(); }
    float GetScrollDelta() const override
    { return m_State.GetScrollDelta(); }

    void RequestCursorCapture(bool locked) override
    { m_CursorCaptureRequested = locked; }
    bool IsCursorCaptureRequested() const override
    { return m_CursorCaptureRequested; }

private:
    // Build the poll list from the active context stack.
    DesktopInputPollList BuildPollList() const;

    InputStateMachine m_State;
    DesktopInputBackend m_Backend;

    // Owned contexts. The host pushes/pops these via PushContext /
    // PopContext. Defaults are populated by LoadDefaults().
    InputContext m_EditorContext{ "editor" };
    InputContext m_ViewportContext{ "viewport" };
    InputContext m_ViewportLookContext{ "viewport.look" };
    InputContext m_RuntimeContext{ "runtime" };

    bool m_CursorCaptureRequested = false;
    bool m_PrevCursorCapture = false;
};

} // namespace rt2::core

#endif // RT2_CORE_INPUT_SERVICE_H