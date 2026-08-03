# RT2 glossary — contested terms and context boundaries

Started 2026-07-31.

**This file records only terms that have actually caused a defect or forced a
paragraph of disambiguation elsewhere.** It is deliberately not a dictionary of
the engine's vocabulary: an entry nobody has tripped over is an entry nobody
maintains, and a stale glossary is worse than none. Add a term when it collides,
with the evidence that made it collide.

Every claim carries a `file:line` so the next reader verifies rather than trusts.
The tree moves; treat a reference that no longer matches as a bug in this file.

---

## Why this file exists

Four resource-index defects were found in quick succession in July 2026. Each
one was a **translation error at a boundary between two parts of the engine
that use the same word for different things**:

| Defect | Boundary crossed |
|---|---|
| OBJ per-triangle materials numbered from 0, not `matBase` | asset file → scene |
| `MeshRef::materialIndex` not rebased by `MergeImportedECS` | imported scene → live scene |
| `KHR_lights_punctual` parsed on load but not on import | two entry points to one boundary |
| Punctual lights absent from the transform-only sync | scene → GPU |

None was a hard problem. Each was invisible because the boundary had no name,
so nothing said which side of it an index belonged to.

The same shape shows up in prose: legacy `projectRoot` has acquired three
different meanings, and the Phase 7 W4/W5 spec spends a finding (W45-F2)
disambiguating it. Two sections were numbered "Phase 8". These are cheap to
fix once named and expensive to keep rediscovering.

---

## Bounded contexts

Four parts of the engine own resources, and **identity means something
different in each**. Most confusion is a value from one context being read as
though it came from another.

| Context | Owns | Identity is | Lifetime |
|---|---|---|---|
| **Asset** | Files on disk: models, textures, scripts, environments | `AssetReference.path` + `sourceKey` (`AssetReference.h:94,96`) | Durable, survives everything |
| **Authoring** | The document the user edits and undoes | `rt2::core::UUID` | Durable, serialized to `.rt2scene` |
| **Scene** | The live ECS: entities, components, resource tables | `entt::entity`, plus array indices into `materials` / `textures` / `meshRegistry` | Transient — invalidated by import, delete and compaction |
| **GPU** | `GPUSceneData`, buffers, descriptor sets | Array index into the uploaded buffers | Rebuilt on every sync |

**The rule that matters:** an index is only meaningful inside the context that
produced it. Crossing a boundary requires an explicit translation, and every
translation is a place a defect can live. When you add a field that holds an
index, the question to answer in its comment is *whose numbering is this?*

---

## Contested terms

### `light`

Three distinct things, and the code says "light" for all three.

- **`LightComponent`** — an entity with a light (`ECSComponents.h:78`). Point,
  spot or directional. Has no geometry and never enters the acceleration
  structure.
- **Punctual light** — the GPU form of the above: `GPUPunctualLight`, uploaded
  by `SceneResources::CreatePunctualLightBuffer`. Zero area, authored in
  candela.
- **Triangle light** — an emissive *triangle*, derived from geometry whose
  material emits. `GPUTriangleLight` (`GPUSceneData.h:105`). Has area, so it
  casts soft shadows; its contribution is divided by that area, which is why an
  emissive needs a far larger authored number than a punctual light to look
  equally bright.

**Trap:** `SceneResources::CreateLightBuffer` (`SceneResources.cpp:699`) builds
**only** triangle lights, despite the unqualified name. The punctual equivalent
is `CreatePunctualLightBuffer`. Prefer "triangle light" and "punctual light" in
new code and comments; bare "light" only where all three are genuinely meant.

### `index` (material, mesh, texture)

Two numbering schemes, indistinguishable by type — both are `int` or
`uint32_t`.

- **File-local** — numbered from 0 within the asset being imported. What
  tinyobj and tinygltf hand you.
- **Scene-global** — an index into `ECSScene::materials` / `textures` /
  `meshRegistry`.

Converting file-local to scene-global means adding the relevant base
(`matBase`, `texBase`, `meshBase`). Every one of the four defects above was a
missed conversion.

Compaction adds a third case: an **old-to-new remap** after unreferenced
resources are dropped. Merge and compaction now share one walker
(`SceneManager.cpp`, `RebaseIndices`) so the field list cannot diverge between
them.

**Open item:** the two schemes are still the same type, so nothing prevents
assigning one to the other. Distinct types would make the class of defect
unrepresentable rather than merely tested for. Not done.

### `MeshRef::materialIndex == -1`

Not "no material" — it means **"use the mesh's per-triangle material indices"**
(`ECSComponents.h:73`). A mesh with `-1` and *no* per-triangle data falls back
to material 0 in the acceleration-structure build, which is whatever was
imported first. Preserve `-1` through any rebasing; never treat it as absent.

### Legacy `projectRoot` and the Phase 7 root terms

Before Phase 7 W4, one `projectRoot` setting had acquired three meanings:
dialog state, script/recovery asset base, and a misleading Session label.
W45-F2 records that historical collision. W4 removed the API and serialized
key; schema-v1/v2 settings now migrate it to `lastBrowseDirectory`.

Phase 7 D3 settles four distinct terms
(`docs/game-engine-development-plan.md:8620-8644`, amended by W45-A6):

- **`lastBrowseDirectory`** — the per-user, absolute,
  machine-specific dialog preference. It never participates in asset
  resolution.
- **`projectDirectory`** — the derived absolute parent directory of the
  portable, committed `.rt2proj` file.
- **`assetRoot`** — the derived absolute base for portable asset locators and
  `AssetDatabase::sourcePath` (`AssetResolver.h:107-116`;
  `AssetDatabase.h:61-80`).
- **`cacheRoot`** — the derived absolute location for generated,
  replaceable cache contents; the project stores only its portable relative
  locator.

Bare `projectRoot` is legacy vocabulary and must not be reintroduced. Current
project-model and host code use the four settled terms above; asset resolution
and recovery do not consult `lastBrowseDirectory`.

### ImGui `OpenPopup` and `BeginPopupModal` must share an ID scope

**Four defects in Phase 7, all identical, none visible to any test.** This is a
convention, not a curiosity.

ImGui resolves a popup name against the current ID stack. `OpenPopup` called
inside a `PushID` block or inside `BeginPopupContextItem` registers the popup
under a *nested* ID. A `BeginPopupModal` outside that scope computes the ID at
the parent level, the two never match, and **the modal silently never opens**.
Clicking a `MenuItem` also closes its containing popup, which makes the symptom
read as "the button just closed the menu".

The rule: **hoist the open out of every `PushID` and popup scope**, into the
same scope as `BeginPopupModal`. Set a flag inside the loop, open after it.

The corrected shape, in the input panel: `bool openCapture` is declared at
`RT2App/src/WalnutApp.cpp:1345`, set at `:1362` inside the per-mapping
`PushID`, and the open happens at `:1378-1379` — the same scope as the
`BeginPopupModal` at `:1419`. The content browser does the same with three
flags (`:1508` onward, opened at `:1645`).

All four instances have been fixed. Every one was found by driving the UI by
hand; none was visible to the suite:

| Modal | Cause |
|---|---|
| Rename Asset | `OpenPopup` inside per-record `PushID` **and** `BeginPopupContextItem` |
| Move Asset | same |
| Delete Asset | same |
| Capture Input (W8 rebinding) | `OpenPopup` inside per-mapping `PushID` |

The first three made the content browser's rename, move and delete
**unreachable in the application** while their CPU operations were fully
unit-tested and green. The fourth left the input rebinding dialog unable to
capture a binding, which is the entire purpose of the feature.

Why the tests cannot catch it: `RT2Tests` compiles no RT2App sources and cannot
link `WalnutApp.cpp`, so no test executes this code. The host dispatch
extraction narrowed that gap for the content-browser dispatch but does not
cover ImGui layout. **Interactive acceptance is the only cover, which is why it
is a first-class deliverable rather than a postscript.**

### `sourceKey` vs asset ID

Not interchangeable, and the distinction is load-bearing (Phase 7 finding P2).

- **`sourceKey`** answers *which subresource inside a file* — e.g.
  `gltf:scene=0:node=3:mesh=1:primitive=0`, `obj:whole-model`.
- **Asset ID** (`AssetReference.assetId`) answers *which file*.

Do not replace `sourceKey` with an asset ID; they solve different halves of
identity.

### `Import` vs `Load`

Two different operations, and the difference has produced defects.

- **Load** replaces the scene. Resources start at index 0, so a missed rebase
  is invisible.
- **Import** adds to an existing scene, so every index must be rebased.

They are also separate code paths — `SceneLoader::LoadObjIntoECS` versus
`SceneManager::ImportObj` — and a feature added to one has repeatedly been
missed in the other (`KHR_lights_punctual` was parsed on load only).

**Rule for any new import feature: exercise it twice into a non-empty scene.**
The first import always looks correct because index 0 is valid for it; defects
appear only on the second, which is the case least likely to be tested by hand.

### `Phase N`

**A number is reserved for a roadmap phase in the development plan and means
nothing else.** Work done outside the roadmap gets a name, not a number.

This was violated: the roadmap's Phase 8 is Prefabs
(`docs/game-engine-development-plan.md:586`), and the punctual-light spec was
also written as "Phase 8", leaving two in one append-only document. The lights
section has been retitled; the roadmap numbering is authoritative.

---

## Maintaining this file

- Add a term when it causes a defect or needs disambiguating in prose — not
  before.
- Carry a `file:line` for every claim.
- This file is **not** append-only; unlike the development plan it describes
  current state, so correct it in place when the tree moves.
