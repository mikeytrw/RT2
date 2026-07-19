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
`glfwSetErrorCallback` and `glfwSetWindowCloseCallback`.

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
unifies the two paths, adds controller support, persists mappings in
project settings, and isolates editor vs. runtime input via a context
stack. See the Phase 5 section of `docs/game-engine-development-plan.md`
for the full specification. The key contracts:

- **CPU-only binding types.** `InputTypes.h` defines `KeyCode`,
  `MouseButton`, `ModifierBits`, `ActionState`, `ActionBinding`,
  `AxisBinding`, `InputMapping`, `InputContext` with no GLFW/ImGui/
  Walnut includes. Key codes are `uint16_t` mirroring GLFW numeric
  values. This preserves the `RT2SliceRunner` and `EditorSettingsStore`
  CPU-only boundary.
- **`InputService` owns the GLFW/ImGui sampling.** `BeginFrame` samples
  `Walnut::Input` + `ImGui::GetIO()` + `glfwJoystick*`, computes
  per-binding `ActionState` edges (Pressed/Held/Released/None) and
  per-axis values (keyboard = `down(pos) - down(neg)`, controller =
  dead-zoned + inverted). `EndFrame` stashes state for next frame.
- **No new GLFW callbacks except joystick + window-focus.** Registered
  once in `InputService::Initialize`. ImGui's backend owns key/mouse/
  scroll callbacks; the service samples ImGui's IO instead of
  installing competing callbacks.
- **Context stack.** Editor and runtime contexts are mutually
  exclusive. Edit pushes `"editor"`; Play pushes `"runtime"` and pops
  `"editor"`; Stop reverses. A `"viewport"` sub-context is pushed when
  the viewport is hovered; a `"viewport.look"` sub-context is pushed
  when right-mouse is held. Top of stack wins; unhandled input falls
  through. This resolves the W/E conflict explicitly.
- **Camera refactor.** `Camera::OnUpdate(float ts, IInputService&)`
  replaces the hardcoded W/S/A/D/Q/E reads with
  `GetAxisValue("move_forward"|"move_right"|"move_up")` and
  `IsDown("look")`. Visible behavior unchanged. Camera no longer
  includes `Walnut/Input/Input.h`.
- **Editor shortcuts refactored.** `HandleEditorCameraShortcuts`,
  `HandleUndoRedoShortcuts`, viewport pick, `SceneEditorUI` hierarchy
  shortcuts, and `EditorTransformGizmo` gizmo-mode hotkeys all read
  from `IInputService` instead of `ImGui::IsKeyPressed` / `io.KeyCtrl`.
  ImGui IO is still sampled by the service (it's the only edge source),
  but shortcut code no longer touches ImGui directly.
- **Controller support from scratch.** `glfwJoystickPresent` polled per
  frame; `glfwSetJoystickCallback` registered once. Default runtime
  context maps left stick → `move_forward`/`move_right`, right stick →
  `look_yaw`/`look_pitch`, A/X → `jump`/`primary_action`. Per-binding
  `deadZone` (default 0.15) and `invert`.
- **Focus-loss reset.** `glfwSetWindowFocusCallback` registered once.
  On focus loss, all down bindings become `Released` for one frame,
  axes zero, and `m_FocusLost` gates sampling until focus returns.
- **Serialization in `EditorSettingsStore` v2.** New optional
  `inputMappings` field. Loader already ignores unknown optional
  fields, so v1 files load cleanly and the service falls back to
  built-in defaults (constructed in `LoadDefaults()`, mirroring current
  hardcoded bindings). Rebinding is verified via JSON editing of the
  settings file or a test-only `SetMapping` API; the interactive
  rebinding dialog is deferred to Phase 7's content-browser era.
- **Runtime input routing.** During Play the runtime context is
  active; editor shortcuts are unmapped in the runtime context, so
  Ctrl+Z/Delete/etc. are inert. Phase 6 adds `IInputService&` to the
  `OnSceneStart` callback signature so scripts can read input; Phase 5
  only builds the service and context switching.
- **Exit criterion.** No gameplay-facing code (and no editor camera or
  shortcut code) reads GLFW or `ImGui::IsKeyPressed` directly. The
  single GLFW-touching code path is `InputService::BeginFrame` plus
  the two callbacks registered in `Initialize`.

## Test surface

- `RT2Tests/src/InputServiceTests.cpp` — CPU-only unit tests via a
  test-only `SetSampleState` backdoor that injects synthetic frame
  state without calling GLFW/ImGui. Covers edge transitions, multi-
  binding disjunction, axis clamping, dead-zone/inversion, context
  stack, focus-loss reset, mapping serialization round-trip, unknown-
  name safety, defaults fallback.
- `RT2Tests/src/Phase5InputTests.cpp` — integration tests (or moved
  to a new `RT2AppIntegrationTests` target if RT2Tests must stay
  CPU-only). Covers editor vs. viewport W/E conflict resolution, Play
  context switching, rebinding reload, focus-loss, controller
  connect mid-Play.

## Files

- New: `RT2App/src/InputTypes.h` (CPU-only types, no GLFW/ImGui/Walnut).
- New: `RT2App/src/InputService.h/.cpp` (service + GLFW/ImGui sampling).
- New: `RT2Tests/src/InputServiceTests.cpp`,
  `RT2Tests/src/Phase5InputTests.cpp`.
- Modified: `RT2App/src/Camera.h/.cpp`, `WalnutApp.cpp`,
  `EditorTransformGizmo.cpp`, `SceneEditorUI.cpp`,
  `EditorSettings.h/.cpp`, project files.