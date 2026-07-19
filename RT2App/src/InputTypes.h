#pragma once

#ifndef RT2_CORE_INPUT_TYPES_H
#define RT2_CORE_INPUT_TYPES_H

// ============================================================================
// InputTypes — CPU-only types for the Phase 5 input action system.
//
// This header defines the binding/mapping/context schema and the
// IInputService read interface. It is deliberately free of GLFW/ImGui/
// Walnut includes so it links cleanly into RT2Tests and RT2SliceRunner
// (which enforce a CPU-only boundary). Key/mouse/gamepad codes mirror
// GLFW's numeric values (see Walnut/Input/KeyCodes.h and
// GLFW/glfw3.h) but are stored as plain integers, not GLFW macros.
//
// Architecture (see docs/input-system.md):
//   InputTypes.h           this file — CPU-only types + IInputService.
//   InputStateMachine.h/.cpp
//                          CPU-only state machine: context stack, edge
//                          computation, axis computation, focus-loss.
//   DesktopInputBackend.h/.cpp
//                          GLFW/ImGui/Walnut snapshot collection.
//   InputService.h/.cpp    composes the two; implements IInputService;
//                          drives frame phasing and cursor capture.
// ============================================================================

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

namespace rt2::core {

// Integer key code mirroring GLFW numeric values (e.g. 'W' = 65, Escape
// = 256). Stored as uint16_t so the schema is portable and CPU-only.
enum class KeyCode : uint16_t
{
    None = 0,
    // A subset mirroring Walnut/Input/KeyCodes.h. New values can be
    // added as needed; the integer values match GLFW exactly so the
    // desktop backend can pass them through.
    Space       = 32,
    Apostrophe  = 39,
    Comma       = 44,
    Minus       = 45,
    Period      = 46,
    Slash       = 47,
    D0 = 48, D1 = 49, D2 = 50, D3 = 51, D4 = 52,
    D5 = 53, D6 = 54, D7 = 55, D8 = 56, D9 = 57,
    Semicolon   = 59,
    Equal       = 61,
    A = 65, B = 66, C = 67, D = 68, E = 69, F = 70, G = 71, H = 72,
    I = 73, J = 74, K = 75, L = 76, M = 77, N = 78, O = 79, P = 80,
    Q = 81, R = 82, S = 83, T = 84, U = 85, V = 86, W = 87, X = 88,
    Y = 89, Z = 90,
    LeftBracket  = 91,
    Backslash    = 92,
    RightBracket = 93,
    GraveAccent  = 96,
    Escape    = 256,
    Enter     = 257,
    Tab       = 258,
    Backspace = 259,
    Insert    = 260,
    Delete    = 261,
    Right     = 262,
    Left      = 263,
    Down      = 264,
    Up        = 265,
    PageUp    = 266,
    PageDown  = 267,
    Home      = 268,
    End       = 269,
    F1  = 290, F2  = 291, F3  = 292, F4  = 293, F5  = 294, F6  = 295,
    F7  = 296, F8  = 297, F9  = 298, F10 = 299, F11 = 300, F12 = 301,
    LeftShift    = 340,
    LeftControl  = 341,
    LeftAlt      = 342,
    LeftSuper    = 343,
    RightShift   = 344,
    RightControl = 345,
    RightAlt     = 346,
    RightSuper   = 347,
    Menu         = 348,
};

// GLFW mouse button values. Button0 = left, Button1 = right,
// Button2 = middle (matches Walnut/Input/KeyCodes.h).
enum class MouseButton : uint16_t
{
    None    = 0xffff,
    Button0 = 0,
    Button1 = 1,
    Button2 = 2,
    Button3 = 3,
    Button4 = 4,
    Button5 = 5,
    Left    = Button0,
    Right   = Button1,
    Middle  = Button2,
};

// GLFW_GAMEPAD_BUTTON_* values.
enum class GamepadButton : uint8_t
{
    None          = 0xff,
    A             = 0,
    B             = 1,
    X             = 2,
    Y             = 3,
    LeftBumper    = 4,
    RightBumper   = 5,
    Back          = 6,
    Start         = 7,
    Guide         = 8,
    LeftThumb     = 9,
    RightThumb    = 10,
    DpadUp        = 11,
    DpadRight     = 12,
    DpadDown      = 13,
    DpadLeft      = 14,
    Cross   = A,
    Circle  = B,
    Square  = X,
    Triangle = Y,
};

// GLFW_GAMEPAD_AXIS_* values.
enum class GamepadAxis : uint8_t
{
    None           = 0xff,
    LeftX          = 0,
    LeftY          = 1,
    RightX         = 2,
    RightY         = 3,
    LeftTrigger    = 4,
    RightTrigger   = 5,
};

enum class ModifierBits : uint8_t
{
    None   = 0,
    Ctrl   = 1 << 0,
    Shift  = 1 << 1,
    Alt    = 1 << 2,
    Super  = 1 << 3,
};

inline ModifierBits operator|(ModifierBits a, ModifierBits b)
{ return static_cast<ModifierBits>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b)); }
inline ModifierBits operator&(ModifierBits a, ModifierBits b)
{ return static_cast<ModifierBits>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b)); }
inline bool Any(ModifierBits a) { return static_cast<uint8_t>(a) != 0; }

// Which device a binding sources from. Required so that key=1 and
// mouseButton=1 are distinguishable in the serialized form.
enum class InputDeviceKind : uint8_t
{
    KeyboardKey   = 0,
    MouseButton   = 1,
    GamepadButton = 2,
    GamepadAxis   = 3,
};

// Edge-triggered and held state for a logical action. Computed AFTER
// combining all of an action's bindings (see InputStateMachine).
enum class ActionState : uint8_t
{
    None      = 0,
    Pressed   = 1,   // edge: up -> down this frame
    Held      = 2,   // down both this frame and last frame
    Released  = 3,   // edge: down -> up this frame
};

// One binding for a logical action. Multiple bindings per action combine
// disjunctively (any binding firing fires the action); edges are
// computed AFTER combining bindings, so releasing one binding while
// another stays held does NOT produce a spurious Released edge.
struct ActionBinding
{
    InputDeviceKind device = InputDeviceKind::KeyboardKey;
    // For device == KeyboardKey:   KeyCode (cast to uint16_t)
    // For device == MouseButton:   MouseButton (cast to uint16_t)
    // For device == GamepadButton: GamepadButton (cast to uint16_t)
    uint16_t code = 0;
    ModifierBits modifiers = ModifierBits::None;
    // Logical gamepad player slot (0..3). -1 = "any connected gamepad".
    // Raw GLFW jids are NOT persisted (not stable across reconnects).
    int gamepadSlot = -1;
};

struct AxisBinding
{
    InputDeviceKind device = InputDeviceKind::KeyboardKey;
    // For device == KeyboardKey: positive/negative are KeyCode values;
    //   code is unused.
    // For device == GamepadAxis: code is the GamepadAxis value;
    //   positive/negative are unused.
    uint16_t code = 0;
    uint16_t positive = 0;
    uint16_t negative = 0;
    int gamepadSlot = -1;
    float deadZone = 0.15f;
    bool  invert = false;
};

struct InputMapping
{
    std::string name;                         // e.g. "move_forward"
    bool isAxis = false;
    std::vector<ActionBinding> actions;
    std::vector<AxisBinding>   axes;
};

// A context owns a set of named mappings. Contexts are stacked; the
// resolution policy is physical-source consumption with lower-context
// blocking (see InputStateMachine and docs/input-system.md).
class InputContext
{
public:
    explicit InputContext(std::string id) : m_Id(std::move(id)) {}
    const std::string& Id() const { return m_Id; }

    void SetMapping(InputMapping m)
    {
        const bool isAxis = m.isAxis;
        auto name = m.name;
        m_Mappings[std::move(name)] = std::move(m);
        (void)isAxis;
    }
    const InputMapping* FindMapping(const std::string& name) const
    {
        auto it = m_Mappings.find(name);
        return it == m_Mappings.end() ? nullptr : &it->second;
    }
    bool HasMapping(const std::string& name) const
    { return m_Mappings.count(name) != 0; }
    void Clear() { m_Mappings.clear(); }
    const std::unordered_map<std::string, InputMapping>& All() const
    { return m_Mappings; }

private:
    std::string m_Id;
    std::unordered_map<std::string, InputMapping> m_Mappings;
};

// Read-only input service interface. Phase 6 scripts will receive a
// const reference to this through the runtime lifecycle observer seam
// (Phase 6 adds the reference; Phase 5 builds the service).
class IInputService
{
public:
    virtual ~IInputService() = default;

    virtual ActionState GetActionState(const std::string& name) const = 0;
    bool IsPressed(const std::string& name) const
    { return GetActionState(name) == ActionState::Pressed; }
    bool IsDown(const std::string& name) const
    {
        auto s = GetActionState(name);
        return s == ActionState::Pressed || s == ActionState::Held;
    }
    bool IsReleased(const std::string& name) const
    { return GetActionState(name) == ActionState::Released; }

    virtual float GetAxisValue(const std::string& name) const = 0;
    virtual glm::vec2 GetMouseDelta() const = 0;
    virtual float GetScrollDelta() const = 0;

    // Cursor capture is a host-controlled request. The service applies
    // the most-recent request at EndFrame via Walnut::Input::
    // SetCursorMode. Multiple consumers can request; last wins.
    virtual void RequestCursorCapture(bool locked) = 0;
    virtual bool IsCursorCaptureRequested() const = 0;
};

} // namespace rt2::core

#endif // RT2_CORE_INPUT_TYPES_H