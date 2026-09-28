---
title: "Current RT2 UI versus editor workspace prototype"
kind: review
comments: none
---

# Current RT2 UI versus editor workspace prototype

Review grounded against `8b7f538477ecb482946675fd1755bd5bdbe728dc` on 2026-09-27. The prototype is untracked in this worktree; no prototype or engine files were changed. Native behavior judgments below cite source and/or current product docs. Prototype controls were inspected in `docs/ui-prototype/index.html` and its README; browser behavior was not re-verified in this review. No running app was available, and no heavyweight build was launched.

See [current UI and workflow model](current-ui-model.md) for full panel, ownership and flow inventories. Original [editor workspace concept](editor-workspace-concept.md) calls itself an iteration surface rather than an implementation specification.

## Highest-impact gaps and incompatibilities

1. **The prototype implies a single contextual Inspector and asset selection model that current code does not have.** Current Inspector is entity/component authoring; Content Browser is a separate project-only window. Asset workflows depend on an active project and `AssetReference` identity. “Select entity or asset in the same Inspector” is a new interaction and state-boundary decision, not simple relocation. [WalnutApp.cpp:2023-2033](../../../RT2App/src/WalnutApp.cpp:2023), [SceneEditorUI.cpp:1583-1602](../../../RT2App/src/SceneEditorUI.cpp:1583), [glossary.md](../../../docs/glossary.md).
2. **A Game tab and persistent transport rearrange actual runtime transitions.** Current Play/Pause/Step/Stop controls live in Scene; Viewport is the rendered scene surface and changes to runtime presentation. Runtime play clones/isolates authoring state and gates authoring controls. Prototype terminology should preserve that distinction; “Game view” is a presentation idea, but whether RT2 exposes a distinct game-camera view is unsettled. [WalnutApp.cpp:1457-1495](../../../RT2App/src/WalnutApp.cpp:1457), [RuntimeSceneController.h:182-250](../../../RT2App/src/RuntimeSceneController.h:182).
3. **The proposed generic Add Component picker conflicts with established authoring semantics.** Current component edits vary: direct/previewed command edits, explicit physics Apply/Revert transactions, project-asset-backed script/audio assignment, and prefab-member locks/override propagation. Searchable picker categories are visually plausible, but attachability and valid component combinations cannot be inferred from it. [SceneEditorUI.cpp:1856-1944](../../../RT2App/src/SceneEditorUI.cpp:1856), [SceneEditorUI.cpp:2160-2188](../../../RT2App/src/SceneEditorUI.cpp:2160), [SceneEditorUI.cpp:596-606](../../../RT2App/src/SceneEditorUI.cpp:596).
4. **The prototype drops the bulk of real workbench capability.** Missing or collapsed out of view are separate Camera authoring/navigation, renderer settings and environment controls, input override editor, project/session roots and recents, physics debug visibility, detailed performance information, and import / asset management / prefab workflows. [WalnutApp.cpp:6704-6723](../../../RT2App/src/WalnutApp.cpp:6704), [WalnutApp.cpp:1498-1519](../../../RT2App/src/WalnutApp.cpp:1498), [WalnutApp.cpp:1765-2020](../../../RT2App/src/WalnutApp.cpp:1765).
5. **One Console tab and generic toasts are not an accurate diagnostic mapping.** There is no current Console panel; status, errors, logs, recovery prompts and component diagnostics are distributed across Session, modals, Inspector/browser, and application output. A Console would be new UI and needs defined contents/filtering/retention/selection behavior. [WalnutApp.cpp:1765-1847](../../../RT2App/src/WalnutApp.cpp:1765), [WalnutApp.cpp:2396-2600](../../../RT2App/src/WalnutApp.cpp:2396).
6. **Prototype Save is only an in-memory checkpoint, while current Save writes native documents and participates in project/asset migration and unsaved-change/recovery handling.** The label is honest inside the demo but cannot transfer to an actual editor UI without replacing the interaction contract. [WalnutApp.cpp:6363-6468](../../../RT2App/src/WalnutApp.cpp:6363), [WalnutApp.cpp:325-365](../../../RT2App/src/WalnutApp.cpp:325).

## Region/action mapping

Classification describes overlap with *current UI* and engine code. “Backend exists” does not imply prototype UI exists or that the proposed affordance is wired.

| Prototype region or action | Current UI | Engine/backend capability | Classification / consequence |
| --- | --- | --- | --- |
| Top bar project name and Project menu | Active project metadata is shown in Session; open/save/project switching is under File. | Project model, asset root/cache root, project opening, asset DB exist. | **Relocated + new grouping.** No topbar project context/actions today. Keep project vs scene identity explicit. |
| Topbar Scene menu: Save, New Empty, Load Courtyard demo | New/Open/Save/Save As under File and Scene window; no bundled courtyard demo contract. | Native scene save/open and unsaved prompts exist. | **Relocated + prototype-only demo action.** “Load demo” is fixture behavior, not engine feature. |
| Topbar View menu / workspace presets | View menu toggles windows and two viewport overlays. ImGui ini/window visibility persist; no named authoring/lighting/debugging workspace presets evidenced. | Window flags and docking layout persistence exist. | **Relocated + new workspace presets.** Presets need behavior/overwrite/reset decisions before implementation. |
| Project settings dialog | Session exposes project file/id and roots, Refresh Assets; input defaults are not edited here. | Project settings model/input configuration exists. | **New presentation and partly new UI.** Prototype text mentions startup scene, audio/runtime settings without defining actual controls. Do not claim these as existing user controls. |
| Preferences in Project Settings | Last browse directory and editor-level visibility/detail settings are per-user. Input Bindings has per-user overrides. | Persisted EditorSettings exist. | **Semantic conflict.** Personal preferences mixed with project settings in proposal; current ownership is deliberately separate. |
| Global Play/Pause/Step/Stop toolbar | Real controls are in Scene. | Runtime SceneRunState transitions and lifecycle exist. | **Relocated.** Interaction is feasible only if state gates and clone/restore semantics are retained. |
| Hierarchy / entity search | Current Outliner has hierarchical scene entities and filtering. | Stable authoring UUID selection, multi-select, hierarchy edit commands. | **Existing and relocated.** Prototype has single-selection/sample data, so not equivalent to multi-select/context commands. |
| Add entity dialog, empty/cube/camera/light/audio source | Current Outliner Add supports Empty, Child Empty, several light kinds, emissive sphere, cube/sphere/plane, import/load. No matching audio-source creation menu observed. | Primitive/light/entity creation exists. Camera component authoring exists. Audio component is authored on entities via Inspector; creation action parity not evidenced. | **Partial existing + genuinely new audio-creation affordance.** The audio card in picker overstates current creation UX. |
| Add Component searchable picker; Script, Physics, Audio groups | Components are shown/edited in selected entity Inspector; physics has grouped controls, script/audio have specialized flows. | Script, physics, audio components and serializers/runtime systems exist. | **Backend exists, proposed picker UI is new.** Picker must respect mutually exclusive physics constraints, prefab locks, asset requirements and apply/revert semantics. |
| Center Scene/Game tabs | One always-visible Viewport shows editor scene and is used for render/selection; Play enters runtime view. No separate Game tab evidenced. | Runtime scene and camera rendering exist. | **New presentation.** Distinct views, active-camera preview, and tab switching semantics are unverified. |
| Viewport Move/Rotate/Scale gizmos | Existing viewport tools and transform gizmo. | Transform edits commit through command/preview logic. | **Existing, relocated.** Prototype states controls do not manipulate actual geometry; appearance is illustrative. |
| Viewport Grid toggle | Grid/viewport drawing belongs in Viewport; exact prototype checkbox behavior differs from native interactions. | Rendering path and overlay systems exist. | **Likely existing display concern; specific toggle/capability needs live verification.** Do not claim mock toggle matches engine. |
| Viewport shading selector: Lit/Unlit/Wireframe | Render Settings supports raster/path tracing/background/denoiser and available advanced modes; viewport shading mode menu of these names not found. | Multiple renderer modes exist, but prototype's tonal filters do not configure them. | **Semantic conflict / placeholder.** “Wireframe” is explicitly a tonal placeholder; do not map to actual raster/path renderer without product decision and code. |
| Inspector selected entity; transform/name | Existing Inspector and authoring controls. | Commands/history and serialization exist. | **Existing and relocated.** Prototype changes only sample data and SVG-independent inspector state. |
| Inspector selected asset | No shared Inspector asset selection model evidenced; Content Browser is separate. | Asset records and references exist; not proof of asset details UI. | **New interaction.** Requires choosing how asset selection coexists with scene selection and what asset properties are actionable. |
| Inspector Add Component button | No generic picker evidenced. | Component systems exist. | **Backend existing, entry point new.** See component constraints above. |
| Bottom Assets tab/search/filter/folder picker | Separate Content Browser window with project gate, search, type navigation, drag/drop and asset operations. | Asset DB, import/rename/move/delete, drag instantiate, prefabs and audio imports exist. | **Mostly relocated, with capability loss.** Prototype fake folders and entries omit actual project-root/error/identity behavior. |
| Bottom Import button / simulated import | Current Content Browser import for assets; scene import/load mesh from Outliner; audio import has dedicated action. | Real import pipelines including OBJ/glTF and audio clip are present. | **Existing backend and UI elsewhere.** Prototype only adds file names, reads no files; must not inherit its behavior. |
| Place prefab into scene | Current Content Browser prefab drag/drop and Outliner Create Prefab Asset; prefab instance source/override tools in Inspector. | Create, instantiate, propagate, serialize prefabs. | **Existing, relocated.** Prototype “Place in scene” is simulated and omits real placement/selection/override outcomes. |
| Bottom Console tab | No Console window in View menu/source panel list. | Logging and typed diagnostics exist; output destinations are distributed. | **Genuinely new UI.** Placeholder UI only; content, filtering, severity, retention and click-through remain undecided. |
| Bottom Performance tab | Separate Performance window, toggle in View. | Frame/render metrics exist. | **Existing and relocated, incomplete.** Prototype stub labels/values are not connected to measured engine performance. |
| Render settings dialog / Advanced | Render Settings is a separate window, presently with many real setting controls and capability gates. | Renderer settings and GPU-dependent algorithms are available. | **Existing but substantially omitted/placeholder.** Prototype quality/background/sample/bounce fields are illustrative and not equivalent to renderer settings; denoiser/ReSTIR/RR choices are hidden. |
| Environment map controls | In Scene panel, HDR Load/Clear and intensity. | HDR load, scene environment map, renderer sync. | **Omitted existing functionality.** The prototype's render dialog mentions background only; preserve environment workflow in revision. |
| Camera controls and navigation | Separate Camera window and viewport shortcuts; camera entity view/align actions in Inspector. | Editor camera and scene camera features exist. | **Omitted existing functionality.** A general “viewport tools” treatment cannot replace editor camera movement, lens controls, bookmarks, or scene-camera authoring. |
| Input bindings | Separate Input Bindings view window; runtime map rebinding, capture, unbind, conflict warnings; editor contexts read-only. | Runtime input map composition and overrides exist. | **Omitted existing functionality.** Project Settings mockup's WASD/Space mapping lacks override/conflict/capture behavior. |
| Physics settings, constraints, debug overlay | Physics authoring controls are in Inspector; Physics Debug Lines toggled in View. | Bodies/shapes, triggers, hinges/sliders and runtime simulation exist. | **Mostly omitted.** Prototype component category is a concept label only; no authoring fields, validation, Apply/Revert, conflicts or debug overlay. |
| Audio source, preview, clip import | Inspector authors audio source; real Content Browser Import Audio; inspector audition controls; runtime audio. | Audio assets, source component, runtime backend/playback and preview controller exist. | **Mostly omitted; Add Audio Source is new.** Prototype only displays a fake “Audio source” entity kind and has no sound or clip assignment workflow. |
| Scripts and declared fields | Inspector script assignment/rebind and reflected field UI; script files are project assets; runtime errors/diagnostics. | Lua scripts, fields, lifecycle, hot reload, sandbox, runtime systems exist. | **Omitted, backend is real.** Generic “Script” category does not represent file assignment, field reflection, hot reload/errors or project-relative asset rules. |
| Save / Save As / dirty indicator | Current File/Scene actions write `.rt2scene`; dirty/revision in Scene and Session; recovery/unsaved prompts. | Native serializer, project binding, migrations and autosave recovery exist. | **Existing action, relocated; sample persistence is placeholder.** Prototype in-memory checkpoint must not be described as native save. |
| Undo | Edit menu; keyboard shortcuts; action history spans more than transforms/additions. | Command history covers authoring edits. | **Existing, relocated, prototype incomplete.** Prototype says Undo covers transform/addition sample edits only; no redo affordance. |
| Play/Pause/Step/Stop, editing disabled | Real Scene controls and editability policy. | Lifecycle, scripts, physics, audio execution, runtime clone/restore. | **Existing, relocated.** Prototype simulates state labels and disables a small set of sample controls; no actual engine runtime. |
| Empty scene and discard | New scene in File/Scene; unsaved changes modal with save/discard/cancel. | Coordinator handles New/Open/Recent/Exit. | **Existing flow, simplified in demo.** Prototype has explicit discard for sample data only; revise to native flow if representing production. |
| Recovery prompt and loading progress | Recovery Available and Loading modals; operation-specific failure modal/status. | Recovery service and background import/load work exist. | **Omitted existing UI/behavior.** No recovery or load progress/error state in prototype. |
| Keyboard shortcuts (Ctrl+S, Ctrl+Z, F5, Shift+F5, W/E/R) | Current subset overlaps save/undo and viewport tool actions; current editor supports additional editing/camera shortcuts and includes runtime input separately. | Relevant host/UI actions exist. | **Partial existing, shortcut map not validated interactively.** Prototype Escape/focus and other bindings are its own behavior; collision/focus rules need review. |
| Focus view, reset layout, tabs | No prototype-style Focus View / reset workspace action was found in File/Edit/View source. ImGui docking and saved ini layout exist. | Docking and layout persistence exist. | **New interaction.** Focus mode semantics, reset scope, and whether tab hiding should persist are unsettled. |
| Toasts/activity log | Native status/error UI varies by operation; application output/logging exists. | Diagnostics are available. | **New unified presentation.** A toast can supplement but should not replace local error details/recovery actions. |

## Presentation changes versus interaction / engine changes

### Presentation-only candidates

- Put Play/Pause/Step/Stop into a persistent toolbar while retaining exact Edit/Playing/Paused enablement and Stop restoration behavior.
- Arrange existing Outliner, Viewport, Inspector and Content Browser around one central viewport; keep camera, renderer, environment, Session and Input Bindings discoverable.
- Move an existing window's visibility toggle into a workspace menu, preserving visibility persistence.
- Present existing performance data in a tabbed lower region if all detail, empty/error states and live metrics remain available.

These still require live review for spatial density, docking, popup reachability and shortcut collisions.

### Interaction changes requiring product/technical decisions

- Asset selection in the same Inspector as entity selection and the state precedence when both exist.
- New Console model and its relationship to existing diagnostics, log output, errors, selection, and recovery actions.
- Separate Scene/Game viewport tabs and which scene camera/Game rendering means in Edit, Playing and Paused.
- Generic Add Component picker, especially physics, audio and script assignment workflows.
- New workspace presets, Focus View and Reset Layout semantics.
- Consolidating Project Settings, per-user preferences, renderer settings, input overrides and runtime defaults.

### Backend already present but prototype still placeholder

OBJ/glTF load/import; native scene save/recovery; undo/redo; runtime lifecycle; physics; audio; Lua scripting; prefab create/instantiate; project asset database; renderer settings; environment map; camera controls; performance data. Prototype surfaces only sample approximations for most. This is a design prototype, not evidence of any integration.

## Recommended faithful revision sequence

1. **Correct the information map first:** retain separate Project/Scene identity, label project-only Content Browser access, and show the actual current actions in File/Edit/View and Session.
2. **Preserve editing semantics:** represent true viewport/selection, contextual entity Inspector, add/create actions and command undo without suggesting single-selection-only or immediate-commit semantics. Mark entity/asset Inspector unification as a decision.
3. **Make the play loop faithful:** keep persistent transport as a candidate relocation, but illustrate real Edit/Playing/Paused state, Pause→Resume, Step-only-while-paused, Stop restoration, and authoring locks. Decide explicitly if Game is a separate view or the same Viewport in runtime.
4. **Restore omitted existing tool destinations:** Camera and environment; Render Settings with support/disabled states; Performance; Session/recovery; Input Bindings; Physics Debug Lines; project asset management and real prefab/script/audio/physics authoring entry points.
5. **Mark genuinely new UI as proposals:** Console, named layout presets, Focus View, searchable component picker and shared asset Inspector. Avoid sample controls that imply an engine capability not connected to the prototype.
6. **Only then assess density and visual hierarchy.** Keep the requested flat dark neutral surface with indigo/violet accents. Visual polish should not conceal which actions are existing, relocated, new or illustrative.

## Unsettled decisions (not approved)

| Decision | Recommendation for next prototype pass | Why it remains open |
| --- | --- | --- |
| Assets permanently docked, drawer, or separate window | Compare a docked Content Browser with a collapsible drawer, retaining full browser navigation and project gating. | This is a user preference question explicitly left open in the concept README; the source has a separate window. |
| One workspace vs named workspace presets | First represent the existing saved docking layout; evaluate presets only after a reset/persistence contract is defined. | No native named preset workflow was found. |
| Entity and asset details in one Inspector | Keep entity Inspector and asset browser details distinct in the faithful revision; test unification as a separate alternate. | Different selection domains and actions; sharing can alter selection and inspection focus semantics. |
| Game tab meaning | Decide whether a second tab is a game-camera preview or merely renames the existing runtime Viewport state. | Current code proves runtime rendering/state but not a separate Game window contract. |
| Console | Decide which diagnostics/output it aggregates and whether entries can navigate to objects/files or invoke recovery. | Current feedback is distributed; a tab name alone does not specify user behavior. |
| Generic component picker | Keep specialized authoring controls in place while exploring picker discoverability; do not make picker the sole interaction without parity designs. | Existing components have different validation, transaction and asset prerequisites. |

## Review limits

The source inspection is static at the stated commit. It cannot establish current interactive docking defaults, menu availability across GPU/platform configurations, keyboard focus edge cases, or whether every modal opens in a real app. Capability statements distinguish code-backed behavior from UI exposure; they are not acceptance-test claims. Prototype README explicitly marks scene art, renderer settings, camera navigation, gizmo manipulation, real import, asset editing and profiling as illustrative, and says save is an in-memory checkpoint ([prototype README](../../../docs/ui-prototype/README.md)).


Exported from the Traycer design record on 2026-09-28. This is a dated analysis snapshot; recheck source references before native implementation.
