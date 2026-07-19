# Input system (Phase 5, planned)

## Current state (pre-Phase-5)

RT2 has two parallel, uncoordinated input paths and no runtime/gameplay
input layer:

1. **`Walnut::Input`** (`Walnut/Walnut/src/Walnut/Input/Input.cpp`) —
   GLFW polling, held-only. `IsKeyDown`, `IsMouseButtonDown`,
   `GetMousePosition`, `SetCursorMode`. No edge detection, no scroll,
   no modifiers, no joystick. Sole consumer: `RT2App/src/Camera.cpp:52-126`
   (`Camera::OnUpdate`), which hardcodes W/S/A/D/Q/E + right-mouse-look.
2. **ImGui IO** — `ImGui::IsKeyPressed`, `ImGui::IsMouseClicked`,
   `io.KeyCtrl/Shift/Alt`. Used by every editor shortcut (Undo/Redo,
   Delete, Copy/Paste/Duplicate, F, Ctrl+number camera bookmarks), the
   gizmo mode hotkeys (`EditorTransformGizmo.cpp:101-103`, W/E/R), and
   viewport mouse picking (`WalnutApp.cpp:733-743`).

ImGui's GLFW backend (`ImGui_ImplGlfw_InitForVulkan` at
`Walnut/Application.cpp:635`) is the only event source. Walnut itself
registers no GLFW key/mouse/scroll/joystick callbacks — only
`glfwSetErrorCallback` and `glfwSetWindowCloseCallback`. ImGui's
backend also installs `glfwSetWindowFocusCallback`
(`imgui_impl_glfw.cpp:430`) and chains the previous callback via
`PrevUserCallbackWindowFocus`.

**Walnut frame ordering is load-bearing for this design.**
`Application::Run` (see `Walnut/Walnut/src/Walnut/Application.cpp:745-854`)
orders the frame as: `glfwPollEvents` → timestep → `Layer::OnUpdate`
→ `ImGui::NewFrame` → `Layer::OnUIRender` → `ImGui::Render`. So
`OnUpdate` runs BEFORE `ImGui::NewFrame`, and viewport hover / widget
capture are not yet known during `OnUpdate`.

Latent conflict: W/E are overloaded for gizmo mode (viewport-hovered,
no right-mouse) and camera translate/up-down (right-mouse held). They
do not collide today only because the two gates are disjoint.

Runtime Play runs the *same* `Camera::OnUpdate` against `m_RuntimeCam`
(`WalnutApp.cpp:977-980`), so during Play the editor flycam bindings
are still active and there is no gameplay input consumer.
`RuntimeSceneController` reads no input. `RT2SliceRunner` is headless
and links no GLFW/Walnut/ImGui.

`EditorSettingsStore` is CPU-only (no GLFW/ImGui/Walnut) and versioned
(`SettingsVersion = 1`); its loader ignores unknown optional fields,
giving a clean v2 upgrade path for serialized input bindings.

## Phase 5 completion contracts

The Phase 5 completion slice introduces a single `InputService` that
unifies the two paths, adds gamepad support via GLFW's standardized
gamepad API, persists mappings in project settings, and isolates
editor vs. runtime input via a context stack with physical-source
consumption. See the Phase 5 section of
`docs/game-engine-development-plan.md` for the full specification. The
key contracts:

- **Architecture: state machine + desktop backend + service.**
  `InputStateMachine` (CPU-only, no GLFW/ImGui/Walnut) owns mappings,
  contexts, edge computation, axis computation, and focus-loss state.
  `DesktopInputBackend` (links GLFW/ImGui/Walnut) only collects a raw
  snapshot. `InputService` composes the two. `RT2Tests` links and tests
  the state machine directly via a `SetSampleState` backdoor; a new
  `RT2AppIntegrationTests` target tests the desktop backend and frame
  phasing.
- **Frame phasing spans `OnUpdate` and `OnUIRender`.**
  `SampleRaw` at the top of `OnUpdate` snapshots GLFW/ImGui/gamepad and
  advances raw-down state so the camera can read actions in `OnUpdate`.
  `ResolveUI` at the top of `OnUIRender` (after `ImGui::NewFrame`)
  applies ImGui suppression and pushes/pops viewport sub-contexts.
  `EndFrame` at the end of `OnUIRender` commits down-state, clears
  per-frame deltas, and applies cursor capture. Edges are derived from
  raw-down snapshots — `ImGui::IsKeyPressed` is NOT used as an edge
  source.
- **CPU-only binding types with device and context identity.**
  `InputTypes.h` defines `KeyCode`, `MouseButton`, `GamepadButton`,
  `GamepadAxis`, `ModifierBits`, `InputDeviceKind` (KeyboardKey /
  MouseButton / GamepadButton), `ActionState`, `ActionBinding`,
  `AxisBinding`, `InputMapping`, `InputContext` with no GLFW/ImGui/
  Walnut includes. `ActionBinding::device` disambiguates `code = 1`
  for keyboard key 1 vs. mouse button 1. Serialized mappings are
  grouped by `contextId` so `"editor"`, `"runtime"` and `"viewport"`
  can have overlapping action names.
- **Edge computation combines bindings.** `previousActionDown = any
  binding was down last frame`; `currentActionDown = any binding is
  down this frame`; state = (Pressed if up→down, Held if down→down,
  Released if down→up, None otherwise). Avoids false Released when one
  binding releases while another stays held.
- **Axis computation with sign-preserving dead zone.** Keyboard axis =
  `(down(neg) ? -1 : 0) + (down(pos) ? 1 : 0)`, clamped. Gamepad axis =
  `sign(raw) * (abs(raw) - dz) / (1 - dz)` if `abs(raw) > dz` else 0,
  with optional inversion. Final axis value = sum of bindings, clamped.
- **Context stack with physical-source consumption.** Topmost context
  that maps a physical source (key, mouse button, gamepad button/axis)
  claims it; lower contexts' bindings on the same source do not fire.
  This resolves W/E: with `"viewport"` top mapping W →
  `gizmo_translate`, W is claimed and `"editor"`'s W → `move_forward`
  does not fire. Pushing `"viewport.look"` (W → `move_forward`) claims
  W and suppresses `gizmo_translate`.
- **Editor routing policy preserves ImGui suppression.** Contexts alone
  do not replace `io.WantTextInput`, `WantCaptureKeyboard`,
  `WantCaptureMouse`, active-widget and gizmo-consumption checks.
  `ResolveUI` applies a policy that suppresses keyboard-sourced actions
  when ImGui wants text input or a widget is active; suppresses
  mouse-sourced actions when ImGui wants mouse capture and the viewport
  is not hovered; suppresses viewport-pick when the gizmo consumes
  mouse. Suppression is per-action so `IsDown("move_forward")` returns
  false while typing in a text box.
- **Camera refactor.** `Camera::OnUpdate(float ts, IInputService&)`
  replaces hardcoded W/S/A/D/Q/E reads with `GetAxisValue` /
  `IsDown("look")`. Camera no longer includes `Walnut/Input/Input.h`
  and no longer calls `Input::SetCursorMode` — it calls
  `input.RequestCursorCapture(true/false)` and the service applies the
  request at `EndFrame`. Visible behavior unchanged.
- **Editor shortcuts refactored.** `HandleEditorCameraShortcuts`,
  `HandleUndoRedoShortcuts`, viewport pick, `SceneEditorUI` hierarchy
  shortcuts, and `EditorTransformGizmo` gizmo-mode hotkeys all read
  from `IInputService` instead of `ImGui::IsKeyPressed` / `io.KeyCtrl`.
- **Gamepad via GLFW's standardized API.** `glfwGetGamepadState` +
  `GLFW_GAMEPAD_BUTTON_*` + `GLFW_GAMEPAD_AXIS_*`. Player slots (0..3)
  are positional, assigned by ascending jid; raw jids are NOT
  persisted. `gamepadSlot = -1` = "any connected gamepad". No
  `glfwSetJoystickCallback` (polling is uniform with the rest of the
  design). GUID-based persistence is out of scope.
- **Focus loss via polling, not callback.** ImGui's GLFW backend
  already installs `glfwSetWindowFocusCallback`; Phase 5 does NOT
  install a competing callback. Instead it polls
  `glfwGetWindowAttrib(window, GLFW_FOCUSED)` per frame. On focus
  loss: all down bindings → `Released` for one frame, axes zero,
  `m_FocusLost` gates sampling until refocus. First refocus frame's
  down-state becomes "previous" (no `Pressed` spikes). Mouse position
  history is reset on focus loss/regain/context transitions to
  prevent large first-frame look deltas.
- **Cursor ownership on `IInputService`.** `RequestCursorCapture(bool
  locked)` records the request; `EndFrame` applies the most-recent
  request via `Walnut::Input::SetCursorMode`. The camera no longer
  calls `SetCursorMode` directly. Focus loss / context transitions
  force `CursorMode::Normal` for one frame.
- **Serialization in `EditorSettingsStore` v2.** New optional
  `inputContexts` field: a list of contexts, each with `contextId` and
  a `mappings` list. `device` field disambiguates keyboard/mouse/
  gamepad. Loader already ignores unknown optional fields, so v1
  files load cleanly and the service falls back to built-in defaults
  (`LoadDefaults()`, mirroring current hardcoded bindings + default
  runtime gamepad mappings). Rebinding is verified via JSON editing
  or the test-only `SetMapping` API; the interactive rebinding dialog
  is deferred to Phase 7's content-browser era.
- **Runtime input routing.** During Play the runtime context is
  active; editor shortcuts are unmapped in the runtime context, so
  Ctrl+Z/Delete/etc. are inert. Phase 6 adds `IInputService&` to the
  `OnSceneStart` callback signature so scripts can read input; Phase 5
  only builds the service and context switching.
- **Exit criterion.** No gameplay or editor code reads GLFW or
  `ImGui::IsKeyPressed` directly — only `DesktopInputBackend::
  CaptureFrame` does. No new GLFW callbacks are registered. The
  single `SetCursorMode` call is in `InputService::EndFrame`.

## Test surface

- `RT2Tests/src/InputStateMachineTests.cpp` — CPU-only unit tests via
  `SetSampleState`. Covers edge transitions, multi-binding disjunction,
  the false-Released edge case, axis clamping, sign-preserving dead-
  zone, inversion, physical-source consumption (W/E conflict resolved),
  focus-loss reset, mapping serialization round-trip with `device` and
  `contextId`, unknown-name safety, defaults fallback.
- `RT2AppIntegrationTests/src/InputServiceFramePhaseTests.cpp` — new
  integration target linking Walnut + ImGui + GLFW. Covers frame
  phasing across `OnUpdate` / `OnUIRender`, ImGui suppression policy,
  viewport hover context transitions, Play/Stop context switching,
  focus-loss polling, cursor capture request, gamepad connect mid-Play.

## Files

- New: `RT2App/src/InputTypes.h` (CPU-only types, no GLFW/ImGui/Walnut).
- New: `RT2App/src/InputStateMachine.h/.cpp` (CPU-only state machine).
- New: `RT2App/src/DesktopInputBackend.h/.cpp` (GLFW/ImGui/Walnut
  snapshot collection).
- New: `RT2App/src/InputService.h/.cpp` (composes the two; implements
  `IInputService`; frame phasing; cursor capture).
- New: `RT2Tests/src/InputStateMachineTests.cpp`.
- New: `RT2AppIntegrationTests/` project (premake5.lua, vcxproj,
  `src/InputServiceFramePhaseTests.cpp`, `src/main.cpp`).
- Modified: `RT2App/src/Camera.h/.cpp`, `WalnutApp.cpp`,
  `EditorTransformGizmo.cpp`, `SceneEditorUI.cpp`,
  `EditorSettings.h/.cpp`, project files.