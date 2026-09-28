---
title: "RT2 editor workspace - grounded revision 02"
kind: spec
comments: none
---

# Grounded editor workspace - revision 02

Revised on 2026-09-27 using the [current UI model](current-ui-model.md) and [gap analysis](ui-prototype-gap-analysis.md). This replaces the first draft's workflow assumptions; native implementation remains unapproved and unchanged.

[Open live prototype](http://127.0.0.1:8765/) | [HTML](../index.html) | [Guide and limits](../README.md) | [Coverage and source evidence](../grounding.md)

## Direction retained

**F**lat dark-grey surfaces with Tailwind indigo and violet accents. Outliner left, large viewport center, entity Inspector right, Content Browser/Session/Performance below. Existing tool destinations open on demand; Play controls stay visible**.**

The placement and grouping are proposed. Code-backed capabilities are distinguished from proposed new interactions and HTML-only sample behavior.

## Grounding corrections

| Previous assumption | Revised behavior |
| --- | --- |
| Asset selection takes over Inspector | Entity Inspector stays on scene selection; asset actions remain in Content Browser |
| Separate Scene/Game tabs | One viewport changes presentation with Edit/Playing/Paused |
| Environment is an entity | Environment has its own scene-wide tool destination |
| Generic Add Component picker | Specialized physics, audio, script and motion sections |
| Uniform immediate property edits | Physics/audio show working copies, Apply/Revert and local validation/conflict feedback |
| Console as existing panel | Session retains current status responsibilities; Console is listed as proposed |
| Always-available assets | Content Browser requires an open project; standalone scene state is represented |
| Simplified save and no redo | Undo/Redo, Save As and Save/Discard/Cancel replacement scenarios |
| Missing engine tools | Camera, render details, input overrides, project/session, constraints, prefab and recovery destinations restored |

## What to try

1. Select Stone arch and edit Position X. Undo/Redo, then select a browser asset: Inspector stays with the entity.
2. Select Planter. Edit Mass, Apply, Undo/Redo. Change an unapplied draft and Undo to expose conflict/Revert handling.
3. Select Ambient audio. Edit Gain or invalid distances and use Apply/Revert. Audition only changes sample state.
4. Play, Pause, Step, Stop. Authoring locks while the same viewport represents runtime.
5. File > New after editing: choose Cancel, Save and continue, or Discard.
6. Project / Session > Try standalone scene: scene editing remains; browser access is gated on a project.
7. Open Tools & views > Input Bindings, rebind a keyboard key and test a duplicate assignment.
8. Help opens deliberately simulated recovery/import/preview failures. Proposed interactions lists unimplemented design options.

## Fidelity boundaries

This is a static prototype with interactive sample state, not the running engine. Document/asset operations do not touch disk; viewport geometry stays fixed; physics, rendering, scripts and audio do not run.

Detailed renderer/camera/material/light controls, constraints, native prefab propagation, physics structural controls, collision assets and script reflection are **read-only layout references**. They retain discoverability and control coverage without implying live behavior. Native docking/persistence and complete multi-selection/subtree semantics still need native review. The simplified physics sample groups Body/Shape reversal; RT2 also has separate Apply/Revert controls.

## Proposals still open

Shared entity/asset Inspector, separate Game tab, unified Console, searchable component picker, Focus View and named workspace presets are only in the Proposed interactions dialog. None is silently promoted to an existing engine feature. Docked assets versus a drawer remains a layout choice for later iteration.

## Verification

Grounding commit: `8b7f538477ecb482946675fd1755bd5bdbe728dc`. Direct source checks supplemented Luna's analysis for component transaction semantics, prefab restrictions, native creation actions and camera/renderer controls.

JavaScript syntax check passed. Browser DOM-event checks passed for selection ownership, transform Undo/Redo, runtime gating and Step, clean draft restoration, dirty draft conflicts, audio validation, unsaved-choice branches, project gating, tool destinations and binding conflicts. Browser console reported no errors. Document width matched viewport width at 1280, 1440 and 1920 pixels. Pointer automation was unreliable and screenshot capture timed out, so captured-image or native clickability verification is not claimed. Native engine tests were not run for this HTML-only revision.

## Prototype

Open [the HTML prototype](../index.html) with its adjacent `editor.js`.


Exported from the Traycer design record on 2026-09-28. This is a dated analysis snapshot; recheck source references before native implementation.
