---
kind: spec
title: "Walkthrough: grounded RT2 editor prototype"
---

# Walkthrough: grounded editor prototype

Prepared 2026-09-28 for revision 02. [Open prototype](http://127.0.0.1:8765/) · [Design and embedded preview](editor-workspace-concept.md) · [Current UI model](current-ui-model.md) · [Gap analysis](ui-prototype-gap-analysis.md).

The change is a proposed organization of RT2's existing capabilities. It keeps scene editing prominent and gives occasional tools discoverable destinations. The native application has not changed. Flat dark-grey surfaces and indigo/violet accents retain the requested visual direction.

## 1. Selection and editing stay together

Select **Stone arch** in the Outliner, change **Position X**, then try **Undo**, **Redo**, and **Save**. The toolbar keeps those actions accessible while the right-hand Inspector stays on the selected entity.

Now select an asset in the Content Browser. The Inspector should still show Stone arch. This corrects the first draft's shared entity/asset Inspector assumption: scene selection and asset selection remain separate domains. Asset actions are available inside the browser. Dragging a model/prefab into the viewport creates a sample entity.

**Review:** Is maintaining entity context useful while searching for assets? Are browser actions discoverable without consuming the Inspector?

**Limit:** Geometry stays fixed. Multi-selection is represented, but property edits affect only the primary entity. Native multi-entity transforms, reparenting and complete subtree/clipboard behavior are not modeled.

## 2. Components retain their different editing rules

Select **Planter**, change **Mass**, and use **Apply Body + Shape**. Select **Ambient audio** and try **Gain**, **Apply Audio**, and **Revert Audio**. Invalid audio distances produce a local error.

These are working copies: a draft is different from an applied scene edit, and an applied edit is different from a saved document. Undo/Redo refreshes clean working copies; a dirty working copy conflicts with history changes and requires Revert before applying again. Selection changes reseed the component draft.

Specialized component sections replace the generic component picker. Linked prefab restrictions remain distinct from an editor lock: removing a lock does not make non-overridable physics/audio properties editable.

**Review:** Can the user tell whether a change is unapplied, applied but unsaved, invalid, or blocked?

**Limit:** The sample groups Body/Shape reversal; native RT2 also exposes separate Body and Shape Apply/Revert. Constraint authoring, full physics validation, prefab propagation and script reflection remain references. Audio preview changes a label, not sound.

## 3. Playtesting uses the same workspace

Use **Play → Pause → Step → Stop**. Controls stay at the top; the existing viewport changes to runtime presentation. Authoring inputs lock during Playing and Paused. Step is available only while paused; Stop returns to editing with authoring values preserved.

The separate Game tab has been removed from the default because a second view would need its own camera and switching contract.

**Review:** Is runtime state unmistakable? Is it clear why editing is unavailable?

**Limit:** This models transitions, not the runtime clone, simulation, renderer or scene camera output.

## 4. Project, scene and recovery are explicit

After an edit, use **File → New scene** to inspect **Save and continue / Discard / Cancel**. Use **Project / Session → Try standalone scene** to see that scene editing continues while Content Browser requires a project.

The Session tab holds project/scene identity, dirty state, revision and status. Input Bindings remains a per-user runtime override destination with read-only editor contexts. It is not presented as editing shared project defaults.

Help exposes recovery, import failure and preview-finalization scenarios separately. They are not reduced to a transient toast or assumed to belong in a new Console.

**Review:** Can the user identify what they are saving, whether a project is open, and how to recover from an error?

**Limit:** Save is an in-memory checkpoint. Import, rename, move, recovery and exit are sample outcomes; no project file is read or written.

## 5. Occasional tools are reachable without permanent clutter

Open **Camera**, **Environment**, **Render settings**, and **Tools & views**. Environment is scene-wide rather than a synthetic entity. Camera-owned look/navigation stays separate from renderer quality. Performance and Session share the bottom region with the Content Browser.

The important remaining gap is depth: detailed Camera, Material, Light, rendering, constraints, prefab and physics structural workflows are currently read-only control inventories. They show where features belong, but do not yet demonstrate the practical editing experience or density of real controls.

**Review:** Which of these must remain visible while adjusting the scene? A modal is a prototype navigation choice, not a settled replacement for a dockable native window.

## Decisions still open

| Question | Current prototype stance |
| --- | --- |
| Docked assets or collapsible drawer? | Docked by default; not settled for native implementation |
| How should camera/render tools open? | Dialog destinations; simultaneous adjustment workflow still needs design |
| Shared asset/entity Inspector? | Separate domains by default; unification listed as proposed |
| Separate Game view or Console? | Excluded from default; behavior requires a definition |
| Generic component picker or named layouts? | Listed as proposed; specialized controls retained |

## Evidence and practical limits

The revision was grounded against native commit `8b7f538` using Luna's source model and direct code checks. [Coverage notes](../grounding.md) map prototype destinations to source.

Recorded checks passed for JavaScript syntax and browser DOM-event behavior: selection ownership, history, component conflicts/validation, runtime gates, unsaved decisions, project gating, tool destinations, bindings and prefab sample placement/restrictions. Layout checks found no document overflow at 1280/1440/1920 pixels. Pointer automation was unreliable and screenshot capture timed out. These results do not verify native usability, docking, GPU-dependent controls or engine execution; this walkthrough does not claim additional tests.

The next useful iteration is to replace the camera/render control inventories with realistic editable mock panels and compare docked versus on-demand presentation. That tests the remaining high-impact workflow question before planning native implementation.


Exported from the Traycer design record on 2026-09-28. This is a dated analysis snapshot; recheck source references before native implementation.
