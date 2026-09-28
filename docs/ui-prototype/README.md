# RT2 editor workspace - grounded revision 02

Open `index.html` in a desktop browser, keeping `editor.js` beside it. No build, package installation, CDN or engine process is required. The Traycer preview inlines the script for portability.

Serve from the repository root: `python -m http.server 8765 --bind 127.0.0.1 --directory docs/ui-prototype`.

This is an interactive design prototype, not native RT2 integration or an approved implementation specification. Document edits, asset operations, playback, input capture and errors use sample data. Reload resets the sample. The SVG courtyard stays fixed. No project file is read or written; no audio is produced.

## Visual direction

Flat dark neutral-grey surfaces, with Tailwind indigo primary actions and violet secondary accents. Scene illustration and conventional XYZ colours retain their content meaning.

## Changes after the gap analysis

| Area | Grounded revision |
| --- | --- |
| Menu and transport | File / Edit / View / Help; Undo and Redo; persistent runtime controls as a proposed relocation |
| Outliner | Multi-selection, native Add menu choices, entity actions and direct-lock example |
| Inspector | Entity-only; asset selection stays in the browser; specialized script, physics, audio and motion sections |
| Viewport | One surface changes between editor and runtime; separate Game tab and invented shading modes removed |
| Environment | Scene-wide destination, no synthetic Environment entity |
| Content Browser | Project-required gate, separate asset actions, simulated model/prefab drag to viewport |
| Session | Project/scene identity, dirty/revision, roots and status |
| Tools | Camera, Environment, renderer, Input Bindings, Performance, prefab and constraints are discoverable |
| Errors | Save/discard/cancel, recovery, failed import and preview-finalization scenarios |
| New ideas | Shared Inspector, Game tab, Console, component picker and named/focused layouts appear only under Proposed interactions |

All regrouping remains a layout proposal. An existing capability does not mean its new location is already implemented.

## Try these flows

1. Edit Stone arch Position X; Undo, Redo, Save. Select a browser asset and verify Inspector remains on Stone arch.
2. Select Planter, change Mass, Apply Body + Shape, Undo and Redo. Edit a draft and Undo again: a local conflict blocks Apply until Revert.
3. Select Ambient audio, change Gain, Apply Audio, Revert a later draft, and toggle audition state. Invalid distances show a local error. No audio plays.
4. Play, Pause, Step, Stop: authoring locks, the same viewport changes mode, and authoring values survive Stop.
5. File > New after an edit: Cancel, Save and continue, or Discard. Save is an in-memory sample checkpoint.
6. Project / Session > Try standalone scene: Content Browser requires a project while the scene remains editable. Open the sample project again.
7. Drag a prefab or model asset into the viewport. The Outliner gains an entity; the SVG does not change.
8. Tools & views > Input Bindings: capture a keyboard key, then assign it to another action for conflict feedback. Editor contexts remain read-only.
9. Help exposes recovery/failure scenarios. Proposed interactions explains ideas excluded from the grounded default.

## Fidelity boundaries

**Interactive samples:** entity selection, primary-entity transforms/name/visibility, Undo/Redo, component working copies, subset validation, authoring locks, document replacement, runtime states, project gating, asset selection/filter/metadata/drag instantiation, keyboard override capture/conflicts, environment state and diagnostics.

**Read-only layout references:** Camera, Material, Light, detailed renderer/denoiser/ReSTIR/RR, real performance, constraint authoring, physics structural/detail controls, native prefab creation/overrides/propagation, collision assets, script reflection/rebind, native folder creation and reparenting. References preserve destinations and control inventories without suggesting the HTML implements the engine.

**Simplifications:** physics pairs Body and Shape and provides combined reversal; native controls also apply/revert them individually. Multi-selection edits only the primary entity. Selection changes reseed component drafts. Clipboard and subtree operations are reduced examples. Input mappings are samples. No live GPU availability, geometry, physics solver, audio or script runtime is exercised.

## Grounding and verification

See [coverage and evidence](grounding.md). Grounded at `8b7f538477ecb482946675fd1755bd5bdbe728dc` using Luna's analysis and direct checks of component, camera and renderer source. No native source changed.

Browser DOM-event checks cover separate asset/entity selection, transform Undo/Redo, runtime locks/step/return, physics clean-draft restoration and dirty-draft conflicts, audio validation, unsaved-change choices, standalone gating, tool destinations and binding conflicts. `node --check editor.js` passed. Pointer automation was unreliable; the checks establish event/state behavior, not native clickability or engine acceptance.
## Durable design record

- [Native implementation plan (draft)](planning/implementation-plan.md)
- [Workspace decisions](planning/editor-workspace-concept.md)
- [Current UI model](planning/current-ui-model.md)
- [Gap analysis](planning/ui-prototype-gap-analysis.md)
- [Workflow walkthrough](planning/walkthrough.md)
