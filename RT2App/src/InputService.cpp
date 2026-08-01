#include "InputService.h"

#include "Walnut/Input/Input.h"
#include "Walnut/Input/KeyCodes.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <unordered_set>

namespace rt2::core {

// ============================================================================
// Defaults — mirrors the current hardcoded bindings in Camera.cpp,
// WalnutApp.cpp shortcuts, SceneEditorUI hierarchy shortcuts, and
// EditorTransformGizmo gizmo-mode hotkeys. Plus default gamepad
// mappings for the runtime context.
// ============================================================================

namespace {

InputMapping MakeKeyboardAxis(const char* name,
                               KeyCode positive, KeyCode negative)
{
    InputMapping m;
    m.name = name;
    m.isAxis = true;
    AxisBinding b;
    b.device = InputDeviceKind::KeyboardKey;
    b.positive = static_cast<uint16_t>(positive);
    b.negative = static_cast<uint16_t>(negative);
    b.deadZone = 0.0f;
    b.invert = false;
    m.axes.push_back(b);
    return m;
}

InputMapping MakeKeyboardAction(const char* name, KeyCode key,
                                 ModifierBits mods = ModifierBits::None)
{
    InputMapping m;
    m.name = name;
    m.isAxis = false;
    ActionBinding b;
    b.device = InputDeviceKind::KeyboardKey;
    b.code = static_cast<uint16_t>(key);
    b.modifiers = mods;
    m.actions.push_back(b);
    return m;
}

InputMapping MakeMouseAction(const char* name, MouseButton button)
{
    InputMapping m;
    m.name = name;
    m.isAxis = false;
    ActionBinding b;
    b.device = InputDeviceKind::MouseButton;
    b.code = static_cast<uint16_t>(button);
    b.modifiers = ModifierBits::None;
    m.actions.push_back(b);
    return m;
}

InputMapping MakeGamepadAxis(const char* name, GamepadAxis axis,
                              int slot, float deadZone, bool invert)
{
    InputMapping m;
    m.name = name;
    m.isAxis = true;
    AxisBinding b;
    b.device = InputDeviceKind::GamepadAxis;
    b.code = static_cast<uint8_t>(axis);
    b.gamepadSlot = slot;
    b.deadZone = deadZone;
    b.invert = invert;
    m.axes.push_back(b);
    return m;
}

InputMapping MakeGamepadAction(const char* name, GamepadButton button,
                                int slot)
{
    InputMapping m;
    m.name = name;
    m.isAxis = false;
    ActionBinding b;
    b.device = InputDeviceKind::GamepadButton;
    b.code = static_cast<uint8_t>(button);
    b.gamepadSlot = slot;
    m.actions.push_back(b);
    return m;
}

} // anonymous namespace

// ============================================================================
// InputService
// ============================================================================

InputService::InputService() = default;
InputService::~InputService() = default;

void InputService::LoadDefaults()
{
    // ---- Editor context: camera + shortcuts ----
    m_EditorContext.Clear();
    // Camera movement axes (Camera.cpp:75-100).
    m_EditorContext.SetMapping(MakeKeyboardAxis("move_forward", KeyCode::W, KeyCode::S));
    m_EditorContext.SetMapping(MakeKeyboardAxis("move_right",   KeyCode::D, KeyCode::A));
    m_EditorContext.SetMapping(MakeKeyboardAxis("move_up",      KeyCode::E, KeyCode::Q));
    // Camera look gate (right mouse). The actual look axis is the
    // mouse delta, not a keyboard axis; "look" is the gate action.
    m_EditorContext.SetMapping(MakeMouseAction("look", MouseButton::Right));
    // Undo / Redo (WalnutApp.cpp:1135-1140).
    m_EditorContext.SetMapping(MakeKeyboardAction("undo", KeyCode::Z, ModifierBits::Ctrl));
    m_EditorContext.SetMapping(MakeKeyboardAction("redo", KeyCode::Y, ModifierBits::Ctrl));
    m_EditorContext.SetMapping(MakeKeyboardAction("redo_shift_z", KeyCode::Z, ModifierBits::Ctrl | ModifierBits::Shift));
    // Hierarchy shortcuts (SceneEditorUI.cpp:678-688).
    m_EditorContext.SetMapping(MakeKeyboardAction("copy",       KeyCode::C, ModifierBits::Ctrl));
    m_EditorContext.SetMapping(MakeKeyboardAction("paste",      KeyCode::V, ModifierBits::Ctrl));
    m_EditorContext.SetMapping(MakeKeyboardAction("duplicate",  KeyCode::D, ModifierBits::Ctrl));
    m_EditorContext.SetMapping(MakeKeyboardAction("delete",     KeyCode::Delete));
    // Focus (WalnutApp.cpp:1098).
    m_EditorContext.SetMapping(MakeKeyboardAction("focus",  KeyCode::F));
    m_EditorContext.SetMapping(MakeKeyboardAction("focus_fit", KeyCode::F, ModifierBits::Shift));
    // Camera bookmarks 1..9 (WalnutApp.cpp:1103-1120).
    for (uint16_t i = 0; i < 9; ++i)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "camera_bookmark_slot%u", i);
        const KeyCode key = static_cast<KeyCode>(static_cast<uint16_t>(KeyCode::D1) + i);
        m_EditorContext.SetMapping(MakeKeyboardAction(name, key, ModifierBits::Ctrl));
        char nameShift[32];
        std::snprintf(nameShift, sizeof(nameShift), "camera_bookmark_slot%u_shift", i);
        m_EditorContext.SetMapping(MakeKeyboardAction(nameShift, key, ModifierBits::Ctrl | ModifierBits::Shift));
    }
    // Viewport pick (left mouse) — handled at the viewport level, but
    // the editor context owns it so it's suppressed correctly when
    // ImGui wants mouse capture.
    m_EditorContext.SetMapping(MakeMouseAction("viewport_pick", MouseButton::Left));

    // ---- Viewport context: gizmo mode hotkeys (viewport hovered,
    // no right-mouse). W/E/R toggle gizmo mode. ----
    m_ViewportContext.Clear();
    m_ViewportContext.SetMapping(MakeKeyboardAction("gizmo_translate", KeyCode::W));
    m_ViewportContext.SetMapping(MakeKeyboardAction("gizmo_rotate",    KeyCode::E));
    m_ViewportContext.SetMapping(MakeKeyboardAction("gizmo_scale",     KeyCode::R));

    // ---- Viewport.look context: right-mouse held. Claims W/S/A/D/Q/E
    // so the editor-context camera axes fire and the viewport-context
    // gizmo-mode bindings do not. ----
    m_ViewportLookContext.Clear();
    m_ViewportLookContext.SetMapping(MakeKeyboardAxis("move_forward", KeyCode::W, KeyCode::S));
    m_ViewportLookContext.SetMapping(MakeKeyboardAxis("move_right",   KeyCode::D, KeyCode::A));
    m_ViewportLookContext.SetMapping(MakeKeyboardAxis("move_up",      KeyCode::E, KeyCode::Q));
    m_ViewportLookContext.SetMapping(MakeMouseAction("look", MouseButton::Right));

    // ---- Runtime context: gameplay actions + default gamepad. ----
    m_RuntimeContext.Clear();
    // Keyboard movement (same axes as editor camera, so a script-driven
    // camera can use WASD during Play).
    m_RuntimeContext.SetMapping(MakeKeyboardAxis("move_forward", KeyCode::W, KeyCode::S));
    m_RuntimeContext.SetMapping(MakeKeyboardAxis("move_right",   KeyCode::D, KeyCode::A));
    m_RuntimeContext.SetMapping(MakeKeyboardAxis("move_up",      KeyCode::E, KeyCode::Q));
    m_RuntimeContext.SetMapping(MakeMouseAction("look", MouseButton::Right));
    m_RuntimeContext.SetMapping(MakeKeyboardAction("jump",           KeyCode::Space));
    m_RuntimeContext.SetMapping(MakeKeyboardAction("primary_action", KeyCode::F));
    // Gamepad: left stick = movement, right stick = look, A = jump,
    // X = primary_action. GLFW's Y axis is up = -1, so invert Y.
    m_RuntimeContext.SetMapping(MakeGamepadAxis("move_right", GamepadAxis::LeftX,  -1, 0.15f, false));
    m_RuntimeContext.SetMapping(MakeGamepadAxis("move_forward", GamepadAxis::LeftY, -1, 0.15f, true));
    m_RuntimeContext.SetMapping(MakeGamepadAxis("look_yaw",   GamepadAxis::RightX, -1, 0.15f, false));
    m_RuntimeContext.SetMapping(MakeGamepadAxis("look_pitch", GamepadAxis::RightY, -1, 0.15f, true));
    m_RuntimeContext.SetMapping(MakeGamepadAction("jump",           GamepadButton::A,      -1));
    m_RuntimeContext.SetMapping(MakeGamepadAction("primary_action", GamepadButton::X,      -1));
}

bool InputService::ApplyConfiguration(
    const std::vector<InputContextRecord>& projectDefaults,
    const std::vector<InputContextRecord>& userOverrides,
    Error& err)
{
    LoadDefaults();

    std::vector<InputContextRecord> builtIns;
    const auto append = [&](const InputContext& context) {
        InputContextRecord record;
        record.contextId = context.Id();
        for (const auto& [name, mapping] : context.All())
        {
            (void)name;
            record.mappings.push_back(mapping);
        }
        builtIns.push_back(std::move(record));
    };
    append(m_EditorContext);
    append(m_ViewportContext);
    append(m_ViewportLookContext);
    append(m_RuntimeContext);

    std::vector<InputContextRecord> composed;
    if (!ComposeInputContexts(builtIns, projectDefaults, userOverrides,
                              composed, err))
        return false;

    m_EditorContext.Clear();
    m_ViewportContext.Clear();
    m_ViewportLookContext.Clear();
    m_RuntimeContext.Clear();
    for (auto& record : composed)
    {
        InputContext* context = nullptr;
        if (record.contextId == "editor") context = &m_EditorContext;
        else if (record.contextId == "viewport") context = &m_ViewportContext;
        else if (record.contextId == "viewport.look")
            context = &m_ViewportLookContext;
        else if (record.contextId == "runtime") context = &m_RuntimeContext;
        else
        {
            err.code = Error::InvalidArgument;
            err.path = record.contextId;
            err.detail = "input context has no runtime owner";
            return false;
        }
        for (auto& mapping : record.mappings)
            context->SetMapping(std::move(mapping));
    }
    return true;
}

// ============================================================================
// Frame phasing
// ============================================================================

DesktopInputPollList InputService::BuildPollList() const
{
    DesktopInputPollList poll;
    std::unordered_set<uint16_t> keys, buttons;
    // std::pair<int, uint8_t> has no default hash in MSVC; use a 64-bit
    // packed key.
    struct PairHash {
        size_t operator()(const std::pair<int, uint8_t>& p) const noexcept
        {
            return std::hash<uint64_t>{}(
                (uint64_t(uint32_t(int32_t(p.first))) << 8) | p.second);
        }
    };
    std::unordered_set<std::pair<int, uint8_t>, PairHash> axes;
    bool anyGamepadButton = false;

    for (const InputContext* ctx : m_State.ContextStack())
    {
        if (!ctx) continue;
        for (const auto& kv : ctx->All())
        {
            const InputMapping& m = kv.second;
            for (const ActionBinding& b : m.actions)
            {
                if (b.device == InputDeviceKind::KeyboardKey)
                    keys.insert(b.code);
                else if (b.device == InputDeviceKind::MouseButton)
                    buttons.insert(b.code);
                else if (b.device == InputDeviceKind::GamepadButton)
                    anyGamepadButton = true;
            }
            for (const AxisBinding& b : m.axes)
            {
                if (b.device == InputDeviceKind::KeyboardKey)
                {
                    keys.insert(b.positive);
                    keys.insert(b.negative);
                }
                else if (b.device == InputDeviceKind::GamepadAxis)
                {
                    axes.insert({ b.gamepadSlot, b.code });
                }
            }
        }
    }

    poll.keys.assign(keys.begin(), keys.end());
    poll.mouseButtons.assign(buttons.begin(), buttons.end());
    poll.gamepadAxes.assign(axes.begin(), axes.end());
    poll.pollGamepadButtons = anyGamepadButton;
    return poll;
}

void InputService::SampleRaw()
{
    DesktopInputPollList poll = BuildPollList();
    RawInputSnapshot snap = m_Backend.CaptureFrame(poll);
    m_State.BeginFrame(snap);
}

void InputService::ResolveUI(bool viewportHovered, bool gizmoConsumesMouse)
{
    // Apply ImGui suppression policy. The state machine recorded the
    // ImGui capture flags in the snapshot during SampleRaw; we re-read
    // them from ImGui::GetIO() here because ResolveUI runs after
    // ImGui::NewFrame and the flags may have been updated.
    ImGuiIO& io = ImGui::GetIO();
    const bool wantText = io.WantTextInput || io.WantCaptureKeyboard;
    const bool wantMouse = io.WantCaptureMouse && !viewportHovered;
    const bool anyItemActive = ImGui::IsAnyItemActive();

    if (wantText || (anyItemActive && !viewportHovered))
        m_State.SuppressKeyboardActions();
    if (wantMouse)
        m_State.SuppressMouseActions();
    if (gizmoConsumesMouse)
        m_State.SuppressAction("viewport_pick");
}

void InputService::EndFrame()
{
    m_State.EndFrame();

    // Apply cursor capture request. The most-recent RequestCursorCapture
    // wins. On focus loss or context transitions, the state machine
    // resets mouse history; we additionally force Normal cursor for
    // one frame to avoid a locked cursor while the user is not
    // interacting.
    bool want = m_CursorCaptureRequested;
    if (m_State.IsFocusLost())
        want = false;

    if (want != m_PrevCursorCapture)
    {
        Walnut::Input::SetCursorMode(want
            ? Walnut::CursorMode::Locked
            : Walnut::CursorMode::Normal);
        m_PrevCursorCapture = want;
    }
}

} // namespace rt2::core
