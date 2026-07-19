#include <doctest/doctest.h>

#include "InputStateMachine.h"
#include "InputTypes.h"

#include <cmath>

using namespace rt2::core;

// ============================================================================
// Helpers — build small contexts and snapshots inline.
// ============================================================================

namespace {

InputMapping KeyboardAxis(const char* name, uint16_t pos, uint16_t neg)
{
    InputMapping m;
    m.name = name;
    m.isAxis = true;
    AxisBinding b;
    b.device = InputDeviceKind::KeyboardKey;
    b.positive = pos;
    b.negative = neg;
    m.axes.push_back(b);
    return m;
}

InputMapping KeyboardAction(const char* name, uint16_t key,
                             ModifierBits mods = ModifierBits::None)
{
    InputMapping m;
    m.name = name;
    m.isAxis = false;
    ActionBinding b;
    b.device = InputDeviceKind::KeyboardKey;
    b.code = key;
    b.modifiers = mods;
    m.actions.push_back(b);
    return m;
}

InputMapping MouseAction(const char* name, uint16_t button)
{
    InputMapping m;
    m.name = name;
    m.isAxis = false;
    ActionBinding b;
    b.device = InputDeviceKind::MouseButton;
    b.code = button;
    m.actions.push_back(b);
    return m;
}

InputMapping GamepadAxisMapping(const char* name, uint8_t axis,
                                 int slot, float dz, bool invert)
{
    InputMapping m;
    m.name = name;
    m.isAxis = true;
    AxisBinding b;
    b.device = InputDeviceKind::GamepadAxis;
    b.code = axis;
    b.gamepadSlot = slot;
    b.deadZone = dz;
    b.invert = invert;
    m.axes.push_back(b);
    return m;
}

RawInputSnapshot EmptySnapshot()
{
    RawInputSnapshot s;
    s.windowFocused = true;
    return s;
}

void PressKey(RawInputSnapshot& s, uint16_t key)
{ s.keysDown.insert(key); }

void PressButton(RawInputSnapshot& s, uint16_t btn)
{ s.mouseButtonsDown.insert(btn); }

} // anonymous namespace

// ============================================================================
// 1. Edge transitions
// ============================================================================

TEST_CASE("Phase 5 StateMachine: up -> down produces Pressed, then Held, then Released")
{
    InputStateMachine sm;
    InputContext editor("editor");
    editor.SetMapping(KeyboardAction("jump", static_cast<uint16_t>(KeyCode::Space)));
    sm.PushContext(&editor);

    // Frame 0: key up.
    RawInputSnapshot s0 = EmptySnapshot();
    sm.BeginFrame(s0);
    CHECK(sm.GetActionState("jump") == ActionState::None);
    sm.EndFrame();

    // Frame 1: key down (up -> down = Pressed).
    RawInputSnapshot s1 = EmptySnapshot();
    PressKey(s1, static_cast<uint16_t>(KeyCode::Space));
    sm.BeginFrame(s1);
    CHECK(sm.GetActionState("jump") == ActionState::Pressed);
    sm.EndFrame();

    // Frame 2: key still down (down -> down = Held).
    RawInputSnapshot s2 = EmptySnapshot();
    PressKey(s2, static_cast<uint16_t>(KeyCode::Space));
    sm.BeginFrame(s2);
    CHECK(sm.GetActionState("jump") == ActionState::Held);
    sm.EndFrame();

    // Frame 3: key released (down -> up = Released).
    RawInputSnapshot s3 = EmptySnapshot();
    sm.BeginFrame(s3);
    CHECK(sm.GetActionState("jump") == ActionState::Released);
    sm.EndFrame();

    // Frame 4: still up (up -> up = None).
    RawInputSnapshot s4 = EmptySnapshot();
    sm.BeginFrame(s4);
    CHECK(sm.GetActionState("jump") == ActionState::None);
    sm.EndFrame();
}

// ============================================================================
// 2. Edge computation combines bindings (false-Released guard)
// ============================================================================

TEST_CASE("Phase 5 StateMachine: releasing one binding while another stays held does NOT produce Released")
{
    InputStateMachine sm;
    InputContext ctx("ctx");
    InputMapping m = KeyboardAction("move", static_cast<uint16_t>(KeyCode::W));
    ActionBinding b2;
    b2.device = InputDeviceKind::KeyboardKey;
    b2.code = static_cast<uint16_t>(KeyCode::Up);
    m.actions.push_back(b2);
    ctx.SetMapping(m);
    sm.PushContext(&ctx);

    // Frame 0: both up.
    RawInputSnapshot s0 = EmptySnapshot();
    sm.BeginFrame(s0); sm.EndFrame();

    // Frame 1: press W (Pressed).
    RawInputSnapshot s1 = EmptySnapshot();
    PressKey(s1, static_cast<uint16_t>(KeyCode::W));
    sm.BeginFrame(s1);
    CHECK(sm.GetActionState("move") == ActionState::Pressed);
    sm.EndFrame();

    // Frame 2: W held, press Up too. Both down → Held.
    RawInputSnapshot s2 = EmptySnapshot();
    PressKey(s2, static_cast<uint16_t>(KeyCode::W));
    PressKey(s2, static_cast<uint16_t>(KeyCode::Up));
    sm.BeginFrame(s2);
    CHECK(sm.GetActionState("move") == ActionState::Held);
    sm.EndFrame();

    // Frame 3: release W, keep Up held. Action stays Held (not Released).
    RawInputSnapshot s3 = EmptySnapshot();
    PressKey(s3, static_cast<uint16_t>(KeyCode::Up));
    sm.BeginFrame(s3);
    CHECK(sm.GetActionState("move") == ActionState::Held);
    sm.EndFrame();

    // Frame 4: release Up. Now Released.
    RawInputSnapshot s4 = EmptySnapshot();
    sm.BeginFrame(s4);
    CHECK(sm.GetActionState("move") == ActionState::Released);
    sm.EndFrame();
}

// ============================================================================
// 3. Multiple bindings combine disjunctively (Pressed case)
// ============================================================================

TEST_CASE("Phase 5 StateMachine: either binding firing produces Pressed")
{
    InputStateMachine sm;
    InputContext ctx("ctx");
    InputMapping m = KeyboardAction("move", static_cast<uint16_t>(KeyCode::W));
    ActionBinding b2;
    b2.device = InputDeviceKind::KeyboardKey;
    b2.code = static_cast<uint16_t>(KeyCode::Up);
    m.actions.push_back(b2);
    ctx.SetMapping(m);
    sm.PushContext(&ctx);

    RawInputSnapshot s0 = EmptySnapshot();
    sm.BeginFrame(s0); sm.EndFrame();

    // Press only Up — action should be Pressed.
    RawInputSnapshot s1 = EmptySnapshot();
    PressKey(s1, static_cast<uint16_t>(KeyCode::Up));
    sm.BeginFrame(s1);
    CHECK(sm.GetActionState("move") == ActionState::Pressed);
    sm.EndFrame();
}

// ============================================================================
// 4. Keyboard axis clamps
// ============================================================================

TEST_CASE("Phase 5 StateMachine: keyboard axis = (down(pos) - down(neg)) clamped to [-1,1]")
{
    InputStateMachine sm;
    InputContext ctx("ctx");
    ctx.SetMapping(KeyboardAxis("move_forward",
        static_cast<uint16_t>(KeyCode::W),
        static_cast<uint16_t>(KeyCode::S)));
    sm.PushContext(&ctx);

    RawInputSnapshot s0 = EmptySnapshot();
    sm.BeginFrame(s0);
    CHECK(sm.GetAxisValue("move_forward") == 0.0f);
    sm.EndFrame();

    RawInputSnapshot s1 = EmptySnapshot();
    PressKey(s1, static_cast<uint16_t>(KeyCode::W));
    sm.BeginFrame(s1);
    CHECK(sm.GetAxisValue("move_forward") == 1.0f);
    sm.EndFrame();

    RawInputSnapshot s2 = EmptySnapshot();
    PressKey(s2, static_cast<uint16_t>(KeyCode::S));
    sm.BeginFrame(s2);
    CHECK(sm.GetAxisValue("move_forward") == -1.0f);
    sm.EndFrame();

    // Both down → 0.
    RawInputSnapshot s3 = EmptySnapshot();
    PressKey(s3, static_cast<uint16_t>(KeyCode::W));
    PressKey(s3, static_cast<uint16_t>(KeyCode::S));
    sm.BeginFrame(s3);
    CHECK(sm.GetAxisValue("move_forward") == 0.0f);
    sm.EndFrame();
}

// ============================================================================
// 5. Gamepad axis dead zone is sign-preserving
// ============================================================================

TEST_CASE("Phase 5 StateMachine: sign-preserving dead zone")
{
    InputStateMachine sm;
    InputContext ctx("ctx");
    ctx.SetMapping(GamepadAxisMapping("move_right",
        static_cast<uint8_t>(GamepadAxis::LeftX), /*slot=*/0, /*dz=*/0.15f, /*invert=*/false));
    sm.PushContext(&ctx);

    auto setAxis = [](RawInputSnapshot& s, int slot, uint8_t axis, float v)
    {
        s.gamepadPresent[slot] = true;
        s.gamepadAxes[slot][axis] = v;
    };

    // Within dead zone → 0.
    {
        RawInputSnapshot s = EmptySnapshot();
        setAxis(s, 0, static_cast<uint8_t>(GamepadAxis::LeftX), 0.10f);
        sm.BeginFrame(s);
        CHECK(sm.GetAxisValue("move_right") == 0.0f);
        sm.EndFrame();
    }

    // Positive, above dead zone.
    {
        RawInputSnapshot s = EmptySnapshot();
        setAxis(s, 0, static_cast<uint8_t>(GamepadAxis::LeftX), 0.5f);
        sm.BeginFrame(s);
        // Expected: sign(+1) * (0.5 - 0.15) / (1 - 0.15) = 0.35/0.85 ≈ 0.4118
        CHECK(doctest::Approx(sm.GetAxisValue("move_right")).epsilon(0.001f) == 0.4117647f);
        sm.EndFrame();
    }

    // Negative, above dead zone.
    {
        RawInputSnapshot s = EmptySnapshot();
        setAxis(s, 0, static_cast<uint8_t>(GamepadAxis::LeftX), -0.5f);
        sm.BeginFrame(s);
        CHECK(doctest::Approx(sm.GetAxisValue("move_right")).epsilon(0.001f) == -0.4117647f);
        sm.EndFrame();
    }

    // Saturated positive.
    {
        RawInputSnapshot s = EmptySnapshot();
        setAxis(s, 0, static_cast<uint8_t>(GamepadAxis::LeftX), 1.0f);
        sm.BeginFrame(s);
        CHECK(sm.GetAxisValue("move_right") == 1.0f);
        sm.EndFrame();
    }

    // Saturated negative.
    {
        RawInputSnapshot s = EmptySnapshot();
        setAxis(s, 0, static_cast<uint8_t>(GamepadAxis::LeftX), -1.0f);
        sm.BeginFrame(s);
        CHECK(sm.GetAxisValue("move_right") == -1.0f);
        sm.EndFrame();
    }
}

TEST_CASE("Phase 5 StateMachine: gamepad axis inversion")
{
    InputStateMachine sm;
    InputContext ctx("ctx");
    ctx.SetMapping(GamepadAxisMapping("move_forward",
        static_cast<uint8_t>(GamepadAxis::LeftY), /*slot=*/0, /*dz=*/0.15f, /*invert=*/true));
    sm.PushContext(&ctx);

    RawInputSnapshot s = EmptySnapshot();
    s.gamepadPresent[0] = true;
    s.gamepadAxes[0][static_cast<uint8_t>(GamepadAxis::LeftY)] = 0.5f;
    sm.BeginFrame(s);
    // Inverted: raw 0.5 → scaled 0.4118 → invert → -0.4118.
    CHECK(doctest::Approx(sm.GetAxisValue("move_forward")).epsilon(0.001f) == -0.4117647f);
    sm.EndFrame();
}

// ============================================================================
// 6. Context stack — physical-source consumption resolves W/E
// ============================================================================

TEST_CASE("Phase 5 StateMachine: physical-source consumption — viewport W claims, editor move_forward does not fire")
{
    InputStateMachine sm;
    InputContext editor("editor");
    editor.SetMapping(KeyboardAxis("move_forward",
        static_cast<uint16_t>(KeyCode::W),
        static_cast<uint16_t>(KeyCode::S)));
    sm.PushContext(&editor);   // stack: [editor]

    InputContext viewport("viewport");
    viewport.SetMapping(KeyboardAction("gizmo_translate",
        static_cast<uint16_t>(KeyCode::W)));
    sm.PushContext(&viewport); // stack: [editor, viewport]

    RawInputSnapshot s0 = EmptySnapshot();
    sm.BeginFrame(s0); sm.EndFrame();

    // Press W.
    RawInputSnapshot s1 = EmptySnapshot();
    PressKey(s1, static_cast<uint16_t>(KeyCode::W));
    sm.BeginFrame(s1);
    // viewport claims W → gizmo_translate Pressed.
    CHECK(sm.GetActionState("gizmo_translate") == ActionState::Pressed);
    // editor's move_forward axis sees W claimed → 0.
    CHECK(sm.GetAxisValue("move_forward") == 0.0f);
    sm.EndFrame();
}

TEST_CASE("Phase 5 StateMachine: viewport.look pushed over viewport — W claims move_forward, gizmo does not fire")
{
    InputStateMachine sm;
    InputContext editor("editor");
    editor.SetMapping(KeyboardAxis("move_forward",
        static_cast<uint16_t>(KeyCode::W),
        static_cast<uint16_t>(KeyCode::S)));
    sm.PushContext(&editor);

    InputContext viewport("viewport");
    viewport.SetMapping(KeyboardAction("gizmo_translate",
        static_cast<uint16_t>(KeyCode::W)));
    sm.PushContext(&viewport);

    InputContext viewportLook("viewport.look");
    viewportLook.SetMapping(KeyboardAxis("move_forward",
        static_cast<uint16_t>(KeyCode::W),
        static_cast<uint16_t>(KeyCode::S)));
    sm.PushContext(&viewportLook);

    RawInputSnapshot s0 = EmptySnapshot();
    sm.BeginFrame(s0); sm.EndFrame();

    RawInputSnapshot s1 = EmptySnapshot();
    PressKey(s1, static_cast<uint16_t>(KeyCode::W));
    sm.BeginFrame(s1);
    // viewport.look claims W → move_forward = 1.
    CHECK(sm.GetAxisValue("move_forward") == 1.0f);
    // viewport's gizmo_translate binding on W does not fire.
    CHECK(sm.GetActionState("gizmo_translate") == ActionState::None);
    sm.EndFrame();
}

TEST_CASE("Phase 5 StateMachine: lower context action fires when top context does not map the source")
{
    InputStateMachine sm;
    InputContext editor("editor");
    editor.SetMapping(KeyboardAction("undo",
        static_cast<uint16_t>(KeyCode::Z), ModifierBits::Ctrl));
    sm.PushContext(&editor);

    InputContext viewport("viewport");
    // viewport maps only W → gizmo_translate; it does not map Ctrl+Z.
    viewport.SetMapping(KeyboardAction("gizmo_translate",
        static_cast<uint16_t>(KeyCode::W)));
    sm.PushContext(&viewport);

    RawInputSnapshot s0 = EmptySnapshot();
    sm.BeginFrame(s0); sm.EndFrame();

    RawInputSnapshot s1 = EmptySnapshot();
    PressKey(s1, static_cast<uint16_t>(KeyCode::Z));
    s1.ctrl = true;
    sm.BeginFrame(s1);
    // Ctrl+Z is not claimed by viewport → falls through to editor.
    CHECK(sm.GetActionState("undo") == ActionState::Pressed);
    sm.EndFrame();
}

// ============================================================================
// 7. Focus loss
// ============================================================================

TEST_CASE("Phase 5 StateMachine: focus loss releases all down bindings and zeroes axes")
{
    InputStateMachine sm;
    InputContext ctx("ctx");
    ctx.SetMapping(KeyboardAxis("move_forward",
        static_cast<uint16_t>(KeyCode::W),
        static_cast<uint16_t>(KeyCode::S)));
    ctx.SetMapping(KeyboardAction("jump",
        static_cast<uint16_t>(KeyCode::Space)));
    sm.PushContext(&ctx);

    // Frame 0: press W + Space.
    RawInputSnapshot s0 = EmptySnapshot();
    PressKey(s0, static_cast<uint16_t>(KeyCode::W));
    PressKey(s0, static_cast<uint16_t>(KeyCode::Space));
    sm.BeginFrame(s0);
    CHECK(sm.GetAxisValue("move_forward") == 1.0f);
    CHECK(sm.GetActionState("jump") == ActionState::Pressed);
    sm.EndFrame();

    // Frame 1: focus lost (windowFocused = false). All down bindings →
    // Released; axes zero.
    RawInputSnapshot s1 = EmptySnapshot();
    s1.windowFocused = false;
    // Keys are still "down" in the snapshot but focus is lost; the
    // state machine treats raw samples as up while focus is lost.
    PressKey(s1, static_cast<uint16_t>(KeyCode::W));
    PressKey(s1, static_cast<uint16_t>(KeyCode::Space));
    sm.BeginFrame(s1);
    CHECK(sm.IsFocusLost());
    CHECK(sm.GetActionState("jump") == ActionState::Released);
    CHECK(sm.GetAxisValue("move_forward") == 0.0f);
    sm.EndFrame();

    // Frame 2: still unfocused. State stays None / 0.
    RawInputSnapshot s2 = EmptySnapshot();
    s2.windowFocused = false;
    sm.BeginFrame(s2);
    CHECK(sm.GetActionState("jump") == ActionState::None);
    CHECK(sm.GetAxisValue("move_forward") == 0.0f);
    sm.EndFrame();

    // Frame 3: refocus. W is still held in the snapshot. The first
    // refocus frame sets previousDown = currentDown, so no Pressed
    // spike — the action reads Held.
    RawInputSnapshot s3 = EmptySnapshot();
    s3.windowFocused = true;
    PressKey(s3, static_cast<uint16_t>(KeyCode::W));
    sm.BeginFrame(s3);
    CHECK_FALSE(sm.IsFocusLost());
    CHECK(sm.GetAxisValue("move_forward") == 1.0f);
    // jump was released before focus loss; W is held but Space is up,
    // so jump reads None (not Released or Pressed).
    CHECK(sm.GetActionState("jump") == ActionState::None);
    sm.EndFrame();

    // Frame 4: W still held. Now reads Held (not Pressed).
    RawInputSnapshot s4 = EmptySnapshot();
    s4.windowFocused = true;
    PressKey(s4, static_cast<uint16_t>(KeyCode::W));
    sm.BeginFrame(s4);
    CHECK(sm.GetAxisValue("move_forward") == 1.0f);
    sm.EndFrame();
}

TEST_CASE("Phase 5 StateMachine: gamepad axis zeroes on focus loss (glfwGetGamepadState is focus-independent)")
{
    InputStateMachine sm;
    InputContext ctx("ctx");
    ctx.SetMapping(GamepadAxisMapping("move_right",
        static_cast<uint8_t>(GamepadAxis::LeftX), /*slot=*/0, /*dz=*/0.15f, /*invert=*/false));
    sm.PushContext(&ctx);

    auto setAxis = [](RawInputSnapshot& s, int slot, uint8_t axis, float v)
    {
        s.gamepadPresent[slot] = true;
        s.gamepadAxes[slot][axis] = v;
    };

    // Frame 0: focused, stick deflected → axis value non-zero.
    {
        RawInputSnapshot s = EmptySnapshot();
        setAxis(s, 0, static_cast<uint8_t>(GamepadAxis::LeftX), 0.5f);
        sm.BeginFrame(s);
        CHECK(doctest::Approx(sm.GetAxisValue("move_right")).epsilon(0.001f) == 0.4117647f);
        sm.EndFrame();
    }

    // Frame 1: focus lost, stick still deflected. Gamepad axis must
    // read 0 (effectiveFocus gate), even though glfwGetGamepadState
    // would still return the deflection.
    {
        RawInputSnapshot s = EmptySnapshot();
        s.windowFocused = false;
        setAxis(s, 0, static_cast<uint8_t>(GamepadAxis::LeftX), 0.5f);
        sm.BeginFrame(s);
        CHECK(sm.IsFocusLost());
        CHECK(sm.GetAxisValue("move_right") == 0.0f);
        sm.EndFrame();
    }

    // Frame 2: refocus, stick still deflected. Axis reads non-zero again.
    {
        RawInputSnapshot s = EmptySnapshot();
        s.windowFocused = true;
        setAxis(s, 0, static_cast<uint8_t>(GamepadAxis::LeftX), 0.5f);
        sm.BeginFrame(s);
        CHECK_FALSE(sm.IsFocusLost());
        CHECK(doctest::Approx(sm.GetAxisValue("move_right")).epsilon(0.001f) == 0.4117647f);
        sm.EndFrame();
    }
}

// ============================================================================
// 8. Unknown action / axis names are safe
// ============================================================================

TEST_CASE("Phase 5 StateMachine: unknown action and axis names return safe defaults")
{
    InputStateMachine sm;
    InputContext ctx("ctx");
    sm.PushContext(&ctx);

    RawInputSnapshot s = EmptySnapshot();
    sm.BeginFrame(s);
    CHECK(sm.GetActionState("nonexistent") == ActionState::None);
    CHECK(sm.GetAxisValue("nonexistent") == 0.0f);
    sm.EndFrame();
}

// ============================================================================
// 9. Mouse delta and scroll
// ============================================================================

TEST_CASE("Phase 5 StateMachine: mouse delta is current - last; first frame is zero")
{
    InputStateMachine sm;
    InputContext ctx("ctx");
    sm.PushContext(&ctx);

    // First frame: mouse history reset → delta = 0.
    RawInputSnapshot s0 = EmptySnapshot();
    s0.mousePos = {100.0f, 200.0f};
    sm.BeginFrame(s0);
    CHECK(sm.GetMouseDelta().x == 0.0f);
    CHECK(sm.GetMouseDelta().y == 0.0f);
    sm.EndFrame();

    // Second frame: delta = (110 - 100, 210 - 200) = (10, 10).
    RawInputSnapshot s1 = EmptySnapshot();
    s1.mousePos = {110.0f, 210.0f};
    sm.BeginFrame(s1);
    CHECK(sm.GetMouseDelta().x == 10.0f);
    CHECK(sm.GetMouseDelta().y == 10.0f);
    sm.EndFrame();

    // Third frame: EndFrame cleared delta? No — BeginFrame computes it
    // from last; EndFrame does NOT clear mouse delta (the camera reads
    // it between BeginFrame and EndFrame). Verify delta is recomputed.
    RawInputSnapshot s2 = EmptySnapshot();
    s2.mousePos = {130.0f, 230.0f};
    sm.BeginFrame(s2);
    CHECK(sm.GetMouseDelta().x == 20.0f);
    CHECK(sm.GetMouseDelta().y == 20.0f);
    sm.EndFrame();
}

TEST_CASE("Phase 5 StateMachine: scroll delta is the snapshot value; cleared at EndFrame")
{
    InputStateMachine sm;
    InputContext ctx("ctx");
    sm.PushContext(&ctx);

    RawInputSnapshot s0 = EmptySnapshot();
    s0.scrollDelta = 1.5f;
    sm.BeginFrame(s0);
    CHECK(sm.GetScrollDelta() == 1.5f);
    sm.EndFrame();
    // After EndFrame, scroll is cleared (not visible to queries
    // between EndFrame and next BeginFrame — but the state machine
    // does not expose a query path there anyway).
}

// ============================================================================
// 10. Suppression
// ============================================================================

TEST_CASE("Phase 5 StateMachine: SuppressKeyboardActions zeroes keyboard-sourced actions and axes")
{
    InputStateMachine sm;
    InputContext ctx("ctx");
    ctx.SetMapping(KeyboardAction("undo",
        static_cast<uint16_t>(KeyCode::Z), ModifierBits::Ctrl));
    ctx.SetMapping(KeyboardAxis("move_forward",
        static_cast<uint16_t>(KeyCode::W),
        static_cast<uint16_t>(KeyCode::S)));
    ctx.SetMapping(MouseAction("look", static_cast<uint16_t>(MouseButton::Right)));
    sm.PushContext(&ctx);

    RawInputSnapshot s0 = EmptySnapshot();
    PressKey(s0, static_cast<uint16_t>(KeyCode::W));
    PressKey(s0, static_cast<uint16_t>(KeyCode::Z));
    s0.ctrl = true;
    PressButton(s0, static_cast<uint16_t>(MouseButton::Right));
    sm.BeginFrame(s0);
    CHECK(sm.GetAxisValue("move_forward") == 1.0f);
    CHECK(sm.GetActionState("undo") == ActionState::Pressed);
    CHECK(sm.GetActionState("look") == ActionState::Pressed);

    sm.SuppressKeyboardActions();
    // Keyboard action and axis are suppressed.
    CHECK(sm.GetActionState("undo") == ActionState::None);
    CHECK(sm.GetAxisValue("move_forward") == 0.0f);
    // Mouse action is NOT suppressed.
    CHECK(sm.GetActionState("look") == ActionState::Pressed);
    sm.EndFrame();
}

TEST_CASE("Phase 5 StateMachine: SuppressMouseActions zeroes mouse-sourced actions only")
{
    InputStateMachine sm;
    InputContext ctx("ctx");
    ctx.SetMapping(KeyboardAction("undo",
        static_cast<uint16_t>(KeyCode::Z), ModifierBits::Ctrl));
    ctx.SetMapping(MouseAction("look", static_cast<uint16_t>(MouseButton::Right)));
    sm.PushContext(&ctx);

    RawInputSnapshot s0 = EmptySnapshot();
    PressKey(s0, static_cast<uint16_t>(KeyCode::Z));
    s0.ctrl = true;
    PressButton(s0, static_cast<uint16_t>(MouseButton::Right));
    sm.BeginFrame(s0);
    sm.SuppressMouseActions();
    CHECK(sm.GetActionState("look") == ActionState::None);
    CHECK(sm.GetActionState("undo") == ActionState::Pressed);
    sm.EndFrame();
}

TEST_CASE("Phase 5 StateMachine: SuppressAction zeroes a single named action")
{
    InputStateMachine sm;
    InputContext ctx("ctx");
    ctx.SetMapping(KeyboardAction("undo",
        static_cast<uint16_t>(KeyCode::Z), ModifierBits::Ctrl));
    ctx.SetMapping(KeyboardAction("redo",
        static_cast<uint16_t>(KeyCode::Y), ModifierBits::Ctrl));
    sm.PushContext(&ctx);

    RawInputSnapshot s0 = EmptySnapshot();
    PressKey(s0, static_cast<uint16_t>(KeyCode::Z));
    PressKey(s0, static_cast<uint16_t>(KeyCode::Y));
    s0.ctrl = true;
    sm.BeginFrame(s0);
    CHECK(sm.GetActionState("undo") == ActionState::Pressed);
    CHECK(sm.GetActionState("redo") == ActionState::Pressed);
    sm.SuppressAction("undo");
    CHECK(sm.GetActionState("undo") == ActionState::None);
    CHECK(sm.GetActionState("redo") == ActionState::Pressed);
    sm.EndFrame();
}

// ============================================================================
// 11. Defaults loaded via LoadDefaults (exercised via InputService
// would require GLFW; here we just verify the contexts own the
// expected action names after manually building the defaults).
// ============================================================================

TEST_CASE("Phase 5 StateMachine: context stack ordering — top claims first")
{
    InputStateMachine sm;
    InputContext a("a");
    InputContext b("b");
    a.SetMapping(KeyboardAction("act", static_cast<uint16_t>(KeyCode::W)));
    b.SetMapping(KeyboardAction("act", static_cast<uint16_t>(KeyCode::W)));
    sm.PushContext(&a);
    sm.PushContext(&b);

    RawInputSnapshot s0 = EmptySnapshot();
    sm.BeginFrame(s0); sm.EndFrame();

    RawInputSnapshot s1 = EmptySnapshot();
    PressKey(s1, static_cast<uint16_t>(KeyCode::W));
    sm.BeginFrame(s1);
    // Both contexts map W → "act"; top (b) claims it. The action
    // reads Pressed (aggregated across contexts, but only b's binding
    // survives since a's binding is on a source claimed above it).
    CHECK(sm.GetActionState("act") == ActionState::Pressed);
    sm.EndFrame();
}

// ============================================================================
// 12. Modifier matching
// ============================================================================

TEST_CASE("Phase 5 StateMachine: modifier-gated action fires only when modifiers match")
{
    InputStateMachine sm;
    InputContext ctx("ctx");
    ctx.SetMapping(KeyboardAction("undo",
        static_cast<uint16_t>(KeyCode::Z), ModifierBits::Ctrl));
    sm.PushContext(&ctx);

    // Press Z without Ctrl → no fire.
    RawInputSnapshot s0 = EmptySnapshot();
    PressKey(s0, static_cast<uint16_t>(KeyCode::Z));
    sm.BeginFrame(s0);
    CHECK(sm.GetActionState("undo") == ActionState::None);
    sm.EndFrame();

    // Press Z + Ctrl → fire.
    RawInputSnapshot s1 = EmptySnapshot();
    PressKey(s1, static_cast<uint16_t>(KeyCode::Z));
    s1.ctrl = true;
    sm.BeginFrame(s1);
    CHECK(sm.GetActionState("undo") == ActionState::Pressed);
    sm.EndFrame();
}

// ============================================================================
// 13. Modifier-claim semantics — a modified binding in a higher context
// claims its base physical key, suppressing a lower context's plain
// binding on the same key even when the modifier is NOT held. This pins
// the intended "physical-source consumption" model before Phase 6/7 add
// more modified bindings.
// ============================================================================

TEST_CASE("Phase 5 StateMachine: modified binding in higher context claims base key, suppresses lower plain binding")
{
    InputStateMachine sm;
    InputContext lower("lower");
    // Lower context maps plain S → "scroll_down".
    lower.SetMapping(KeyboardAction("scroll_down",
        static_cast<uint16_t>(KeyCode::S), ModifierBits::None));
    sm.PushContext(&lower);

    InputContext upper("upper");
    // Upper context maps Ctrl+S → "save".
    upper.SetMapping(KeyboardAction("save",
        static_cast<uint16_t>(KeyCode::S), ModifierBits::Ctrl));
    sm.PushContext(&upper);

    // Frame 0: S pressed WITHOUT Ctrl. The upper context's Ctrl+S binding
    // does NOT fire (modifier mismatch), but its physical S source IS
    // claimed by the upper context. The lower context's plain-S binding
    // does not fire either (S is claimed above it). This is the documented
    // "physical-source consumption" semantics: the claim is on the physical
    // key, not the modifier-gated binding.
    RawInputSnapshot s0 = EmptySnapshot();
    sm.BeginFrame(s0); sm.EndFrame();

    RawInputSnapshot s1 = EmptySnapshot();
    PressKey(s1, static_cast<uint16_t>(KeyCode::S));
    sm.BeginFrame(s1);
    CHECK(sm.GetActionState("save") == ActionState::None);
    CHECK(sm.GetActionState("scroll_down") == ActionState::None);
    sm.EndFrame();

    // Frame 2: S pressed WITH Ctrl. Upper context's save fires (modifier
    // matches); lower context's scroll_down does not (S claimed above).
    RawInputSnapshot s2 = EmptySnapshot();
    PressKey(s2, static_cast<uint16_t>(KeyCode::S));
    s2.ctrl = true;
    sm.BeginFrame(s2);
    CHECK(sm.GetActionState("save") == ActionState::Pressed);
    CHECK(sm.GetActionState("scroll_down") == ActionState::None);
    sm.EndFrame();
}

// ============================================================================
// 14. Same-frame context push lag — actions are recomputed only in
// BeginFrame (inside SampleRaw), BEFORE the host pushes viewport
// sub-contexts. So a freshly-pushed context does not affect the same
// frame's action reads; it takes effect next frame. This pins the
// documented behavior so a future edge-sensitive action gated on a
// dynamically-pushed context is not silently misread.
// ============================================================================

TEST_CASE("Phase 5 StateMachine: freshly-pushed context does not affect same-frame action reads")
{
    InputStateMachine sm;
    InputContext editor("editor");
    editor.SetMapping(KeyboardAction("undo",
        static_cast<uint16_t>(KeyCode::Z), ModifierBits::Ctrl));
    sm.PushContext(&editor);

    InputContext viewport("viewport");
    viewport.SetMapping(KeyboardAction("gizmo_translate",
        static_cast<uint16_t>(KeyCode::W)));
    // Note: viewport is NOT pushed yet.

    // Frame 0: push viewport AFTER BeginFrame (simulating the host
    // pushing a sub-context after SampleRaw). The action reads this
    // frame do NOT see the viewport context.
    RawInputSnapshot s0 = EmptySnapshot();
    sm.BeginFrame(s0);
    // Read BEFORE push: viewport not in stack, gizmo_translate unknown.
    CHECK(sm.GetActionState("gizmo_translate") == ActionState::None);
    sm.PushContext(&viewport);
    // Read AFTER push in the same frame: still None — actions were
    // computed in BeginFrame before the push.
    CHECK(sm.GetActionState("gizmo_translate") == ActionState::None);
    sm.EndFrame();

    // Frame 1: BeginFrame now sees the viewport context. A W press
    // fires gizmo_translate.
    RawInputSnapshot s1 = EmptySnapshot();
    PressKey(s1, static_cast<uint16_t>(KeyCode::W));
    sm.BeginFrame(s1);
    CHECK(sm.GetActionState("gizmo_translate") == ActionState::Pressed);
    sm.EndFrame();
}