# Prototype coverage and evidence

Grounded at `8b7f538477ecb482946675fd1755bd5bdbe728dc`. Source references describe native behavior; the prototype is a sample interaction model. Luna's model and gap analysis are dated source evidence, not live UI acceptance results.

| Existing workflow / source | Prototype destination | Fidelity |
| --- | --- | --- |
| File actions: `RT2App/src/WalnutApp.cpp:6655`; replacement coordinator: `:325` | File, unsaved dialog | Sample Save/Discard/Cancel and Save As; no document I/O |
| Undo/Redo: `RT2App/src/WalnutApp.cpp:6692` | Edit and toolbar | Sample history including applied component edits |
| View destinations: `RT2App/src/WalnutApp.cpp:6704` | View / Tools & views | Existing destinations regrouped; HTML positions fixed |
| Creation: `RT2App/src/SceneEditorUI.cpp:1071` | Add entity | Empty/Child Empty, light kinds, emissive sphere, primitives, import/load |
| Entity context: `RT2App/src/SceneEditorUI.cpp:1216`, `:1404` | Outliner / Entity actions | Multi-select and simplified clipboard/leaf operations; reparenting referenced |
| Inspector: `RT2App/src/SceneEditorUI.cpp:1583` | Right panel | Entity ownership retained; no asset takeover |
| Transform: `RT2App/src/SceneEditorUI.cpp:3001` | Transform section | Commit/cancel; no rendered gizmo or multi-entity transforms |
| Material/light/camera: `RT2App/src/SceneEditorUI.cpp:3244`, `:3507`, `:3675` | Entity-specific sections | Control references |
| Motion: `RT2App/src/SceneEditorUI.cpp:1643` | Motion section | Simplified presence and velocity text |
| Physics locks/body/shape: `RT2App/src/SceneEditorUI.cpp:1850`, `:2167` | Planter / Physics | Draft/history conflict/combined Apply sample; structural/details reference |
| Constraints: `RT2App/src/SceneEditorUI.cpp:2191`, `:2390` | Physics constraints | Attachment/axis/limit/motor and Apply/Revert inventory; no solver |
| Audio lifecycle/locks: `RT2App/src/SceneEditorUI.cpp:2650`; Apply/Revert: `:2854`; audition: `:2955` | Ambient audio / Audio Source | Draft/apply/revert, sample validation and audition state |
| Script: `RT2App/src/SceneEditorUI.cpp:3814` | Player / Script | Sample path/field; native reflection/rebind/diagnostics referenced |
| Prefabs: `RT2App/src/SceneEditorUI.cpp:1508`, `:1553` | Prefab section / browser | Source navigation and sample instantiation; propagation reference |
| Runtime: `RT2App/src/WalnutApp.cpp:1457` | Persistent transport / single viewport | State and gates, not runtime execution |
| Editor Camera: `RT2App/src/WalnutApp.cpp:859` | Camera destination | Pose/lens/look/frame/focus/bookmark reference |
| Environment: `RT2App/src/WalnutApp.cpp:1498` | Environment destination | Scene-owned sample HDR and intensity |
| Renderer: `RT2App/src/WalnutApp.cpp:1108` | Render settings | Sampling, raster-first, NRD/RR, ReSTIR DI/GI, debug reference; no invented quality presets |
| Performance: `RT2App/src/WalnutApp.cpp:962` | Bottom Performance | Honest unavailable values; detail levels referenced |
| Session: `RT2App/src/WalnutApp.cpp:1767` | Session / Project | Identity, roots, status, recents, refresh |
| Input: `RT2App/src/WalnutApp.cpp:1852` | Input Bindings | Per-user keyboard capture/unbind/conflicts; editor contexts read-only |
| Content Browser: `RT2App/src/WalnutApp.cpp:2025` | Bottom browser | Project gate; separate selection; sample asset operations/import outcomes |
| Recovery/failures: `RT2App/src/WalnutApp.cpp:2396`; preview recovery: `RT2App/src/SceneEditorUI.cpp:31` | Help scenarios / local diagnostics | Separate destinations; no native failure induced |

## Proposed additions excluded from default

Shared entity/asset Inspector, separate Game tab, unified Console, searchable component picker, Focus View and named presets are documented only in **Proposed interactions**. Their product decisions remain open.

The relocation of existing controls is itself the layout proposal. Keeping specialized workflows does not require preserving their old screen positions.

## Remaining simplifications

Physics sample groups Body/Shape reversal, while native controls have individual Apply/Revert and combined Apply. Multi-selection edits only the primary entity. Runtime and audition are state demonstrations. No collision resolution, script execution, asset serialization/propagation, geometry update or native docking occurs. Renderer/camera references intentionally show values without pretending that editable fields change rendered output.
