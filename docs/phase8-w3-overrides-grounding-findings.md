# Phase 8 W3 — prefab overrides grounding findings

Grounded against `master` at commit `0df3175` (2026-08-04), the merge of
Phase 8 W0 and W2. This is a read-only grounding record for W3; no engine
source was changed. W1 is **not** on this commit: there is no prefab-instance
component, prefab-member component, instance-to-template map, override-set
storage, or prefab load/instantiate path in `RT2App/src`. W1's intended shape
is therefore taken only from the Phase 8 spec and amendment A1, and every such
statement below is labelled as an inference rather than a code fact.

The references below are line-level evidence against this commit. The plan is
append-only; its W0/W2 reports are period records, while the code and the
current serializer/resolver contracts are the executable evidence.

## Executive answer — the unresolved architectural choice

The central question is whether an instantiated subtree is self-sufficient in
`.rt2scene`, or is reconstructed from `.rt2prefab` at load.

| Option | Scene representation | Source-missing behaviour | W4 consequence |
|---|---|---|---|
| **A — full values plus sparse metadata** | Keep the faithful copied component values in the scene; store the prefab link, member identity, and a component-level override set as metadata. | The scene still has authored transforms/components. Asset resolution can leave only the affected render resource unresolved and report a diagnostic. | W4 must walk instances and rewrite non-overridden components when the source changes. |
| **B — overrides only** | Store the link and only component overrides; reconstruct inherited components from the prefab on every load. | A missing/unresolvable prefab is a potential data-loss event: there is no complete inherited value to show or save. A diagnostic alone cannot reconstruct it. | Propagation is nearly free, but every load depends on the prefab asset and its exact format/identity. |

**Recommendation (unsettled): choose A.** RT2 is already a value-first,
resolution-decoupled editor. `SceneSerializer` parses and instantiates the
full authored component set before external resolution
(`RT2App/src/SceneSerializer.cpp:1087-1207`), and the resolver treats missing
external models as a diagnostic while preserving the entity UUID, hierarchy,
and authored state (`RT2App/src/SceneAssetResolver.h:35-58`; `docs/scene-management.md:470-489`).
Keeping those values makes a missing prefab recoverable and keeps the scene
usable while an asset is repaired. The cost is real and should be explicit:
W4 must actively propagate source changes. Option B would be a new failure
policy, not a small optimization, because it turns an unresolved asset from a
recoverable reference into the only copy of inherited data.

This recommendation cannot be made a settled implementation fact yet. W1 is
unlanded and unverified, and the current W0 serializer refuses every non-empty
record list (`RT2App/src/PrefabSerializer.cpp:72-84,161-172`). The first W1
implementation must confirm that its faithful-copy path actually leaves the
complete authored values in the scene before W3 can finalize the load model.

## Q1 — what the existing load and resolution pipeline actually guarantees

**Verified against code.** The native scene file is not a resource cache, but
it does carry the complete authored value side of each persisted entity.

- `SceneSerializer::BuildEntityRecord` captures name, local TRS, visibility,
  mesh/material values, primitive/import provenance, material override, light,
  camera, motion, and script (`RT2App/src/SceneSerializer.cpp:564-644`).
- The JSON writer emits those component records, including the full material
  override value (`RT2App/src/SceneSerializer.cpp:647-703`); the reader creates
  the entities and those components before it wires parent UUIDs and rebuilds
  children (`RT2App/src/SceneSerializer.cpp:1099-1207,1210-1234`).
- Imported geometry is intentionally deferred: file load puts the durable
  `ImportedMeshSourceComponent` on the entity and leaves the runtime mesh index
  for the resolver to repair (`RT2App/src/SceneSerializer.cpp:1154-1174`).
- `ResolveAll` stages each model, plans all entity matches without mutating the
  target, and only commits resolved resources. If all imported entities fail,
  it returns `Error::MissingAsset` and leaves the document unchanged
  (`RT2App/src/SceneAssetResolver.cpp:555-565,581-599,673-685,772-782`). A
  partial result commits the successful resources and leaves failed entities
  without a resolved `MeshRef`, with `Missing`/`Unresolved` diagnostics
  (`RT2App/src/SceneAssetResolver.cpp:785-915`).

That behaviour is materially different from a lazy prefab reconstruction
model. In the current pipeline an unresolvable asset does not erase the
authored transform, visibility, script, light, or other component data. W3's
metadata must preserve that property if it chooses Option A.

**Inferred from the W1 spec, not verified.** The Phase 8 spec says W1
materializes an instance as a faithful copy and gives W3 an “apply overrides
on top of template values at load” job (`docs/game-engine-development-plan.md:12814-12823`).
It does not state whether those copied values remain authoritative scene data
or are discarded after the copy. That is the key missing W1 contract.

## Q2 — `MaterialOverrideComponent` is a whole-value precedence layer

**Verified against code.** The existing material path is not a property
reflection system. It is one durable, whole-value snapshot plus a transient
resolved slot:

- `MaterialOverrideComponent::material` is a complete `SceneMaterial` value;
  `authored` decides whether the snapshot wins; `sourceMaterialKey` is the
  durable source-material identity; `materialIndex` is explicitly transient
  (`RT2App/src/ECSComponents.h:241-269`). `SceneMaterial` itself carries the
  loader-minted source key alongside all scalar and texture-index fields
  (`RT2App/src/SceneTypes.h:86-116`).
- `RecordMaterialOverride` snapshots the material at the current scene slot,
  sets `authored = true`, copies `SceneMaterial::sourceKey` into
  `sourceMaterialKey`, and records the current slot only as transient state
  (`RT2App/src/SceneManager.cpp:3707-3728`).
- A global material edit writes the slot first, then records a fresh override
  for every imported entity whose `MeshRef` points at that slot
  (`RT2App/src/SceneManager.cpp:3803-3829`). Thus one material-properties edit
  may create/update several entity overrides; it is not necessarily one
  instance/one command.
- A per-entity material-index edit captures the existing override **before**
  changing the index, records the new override for imported entities, and
  returns authoritative before/after override snapshots
  (`RT2App/src/SceneManager.cpp:3832-3874`). This ordering is the repair for
  the 2026-08-03 read-after-mutate defect, recorded in the function itself at
  `RT2App/src/SceneManager.cpp:3848-3853`.
- On resolve, a non-empty new-form source key is matched against staged
  material keys; a miss falls back to the resolved slot and raises a `Stale`
  diagnostic. Legacy keys are rebased to the staged material identity only in
  the commit pass (`RT2App/src/SceneAssetResolver.cpp:688-769`). The resolver
  then copies the rebuilt source material's four texture indices into the
  authored snapshot, appends that full snapshot as a new scene material, and
  installs its transient `materialIndex` (`RT2App/src/SceneAssetResolver.cpp:857-890`).

The two Phase 8 pre-works changed this model in two important ways:

1. **Override-aware compaction (`ab3c852`, present at
   `0df3175`).** Compaction now marks all four texture indices in every
   override snapshot, and the shared rebase walker remaps those texture
   indices (`RT2App/src/SceneManager.cpp:230-300,4217-4265`). It deliberately
   does **not** retain an override's material slot: the transient
   `materialIndex` is remapped to `-1` when its live slot is swept and is
   repaired by the next resolver pass (`RT2App/src/SceneManager.cpp:212-219,288-300`).
   This closes the previously silent loss of an override-only texture, but it
   does not make resource indices durable.
2. **Source-material identity and key matching (`e7cb72d`, present at
   `0df3175`).** Loader-surfaced material identity now travels with each
   `SceneMaterial`, and overrides carry the matching key. Reorder/removal is
   no longer silent: a key miss is diagnosed before slot fallback, and legacy
   keys migrate only after a successful commit (`RT2App/src/SceneAssetResolver.cpp:688-769,840-849`).

**What W3 must not infer from this precedent:** the current override's
`materialIndex` is a live-scene index, and its texture indices are repairable
only while a materialized staged source exists. W0 explicitly requires W1 to
strip resource-table indices from prefab records (`RT2App/src/PrefabSerializer.h:21-31`).
Copying a `MaterialOverrideComponent` byte-for-byte into a prefab file would
violate the existing identity boundary.

## Q3 — stacking a prefab component override over a material override

**Verified fact:** EnTT permits only one `MaterialOverrideComponent` per entity;
there is no second component type or generic override layer in the current
tree. The existing precedence is source material → authored material snapshot
→ resolved `MeshRef` slot (`RT2App/src/SceneAssetResolver.cpp:857-905`).

So there is no verified double-count in the current runtime: the prefab layer
does not exist yet. The double-count is an implementation failure mode if W1/W3
append both the template snapshot and the instance snapshot instead of choosing
one effective component. The verified ambiguity today is only the material
component's `authored` flag; the prefab ordering below is still provisional
(`RT2App/src/ECSComponents.h:248-251`; `RT2App/src/PrefabSerializer.cpp:72-84`).

**W3 design consequence (recommendation, unsettled):** a prefab-level
component override that names `MaterialOverrideComponent` must replace the
entire durable component value for that entity. It must not append a second
snapshot or merge individual fields, because D2 has settled component-level
granularity and the current material mechanism is already a whole-value
snapshot (`RT2App/src/ECSComponents.h:241-269`; `docs/game-engine-development-plan.md:12729-12745`).
The intended effective order should be:

```
source asset material
    -> template's MaterialOverrideComponent, if authored
    -> instance's prefab component override, if marked
    -> one resolver append + MeshRef installation
```

That ordering is a recommendation, not current runtime behaviour. Applying a
prefab component override after `ResolveAll` would leave `MeshRef::materialIndex`
and the appended material slot describing the old component; applying both
snapshots independently would append two competing materials. The safe seam is
to materialize the final effective component value first, then run exactly one
source/material resolution pass, or to provide an equivalent explicit repair
pass. W1 has not landed the call ordering that would settle this.

The `authored` flag adds a second ambiguity: an instance can carry a
`MaterialOverrideComponent` whose snapshot is ignored when `authored == false`
(`RT2App/src/ECSComponents.h:248-251`). W3 must define whether an inherited
component with `authored == false` is represented as “not overridden”, and how
an instance override that intentionally restores source material is represented.
No existing code answers that question.

## Q4 — stable component identity and the key that does not exist yet

**Verified against code.** Entity identity is durable UUID, not an EnTT handle:
`EntityIdComponent` stores the UUID and the authoring document indexes it
(`RT2App/src/ECSComponents.h:144-152`; `RT2App/src/SceneDocument.h:35-36,91-116`).
W2's remapper correctly consumes a durable UUID-to-UUID map and rewrites only
typed UUID script fields (`RT2App/src/EntityReferenceRemapper.h:11-26`;
`RT2App/src/EntityReferenceRemapper.cpp:8-33`). The prefab-local identity
needed for reattachment is not in the current snapshot record: `SubtreeEntityRecord`
has a document UUID but no template ID (`RT2App/src/SubtreeSnapshot.h:43-83`).

The Phase 8 amendment A1 is therefore **spec intent, not code**: W1 is to wrap
`SubtreeEntityRecord` in a prefab-specific record carrying a frozen
`templateId`, rather than adding that field to every undo snapshot
(`docs/game-engine-development-plan.md:13066-13108`).

There is also no stable component wire ID today. `PersistedComponents::Tag<T>`
is a compile-time visitor helper; `Count` is 11, and the list is ordered C++
types, not serialized IDs (`RT2App/src/PersistedComponents.h:10-36`). The
serializer's only coverage guard is a count assertion
(`RT2App/src/SceneSerializer.cpp:38-39`). `typeid(T)`, EnTT numeric IDs, or
the visitor's order are not build-stable serialization identities.

**Recommendation (unsettled):** add an explicit, frozen prefab wire key for
each supported component — either a manually assigned enum whose numeric values
never change, or the existing JSON component names (`name`, `transform`,
`visible`, etc.) in a fixed codec table. The key must be serialized and mapped
explicitly; it must not be derived from `typeid`, EnTT storage order, or
`PersistedComponents::ForEach` order. Unknown keys should be diagnosed rather
than silently dropped, because an omitted override changes propagation
semantics. The component key and the prefab `templateId` solve different
halves of identity and must not be conflated.

## Q5 — which persisted components can be overridden

The authoritative authored-value list is exactly the 11 entries in
`PersistedComponents::ForEach` (`RT2App/src/PersistedComponents.h:20-35`).
`EntityIdComponent` and `Hierarchy` are intentionally outside it because
duplication remaps identity and relationships (`RT2App/src/PersistedComponents.h:10-14`).
D6 separately defers structural child/parent deltas
(`docs/game-engine-development-plan.md:12789-12803`). Against that boundary:

| Component | W3 status | Grounded reason |
|---|---|---|
| `NameComponent` | Plausibly overridable | Authored value; scene serializer and duplication copy it (`RT2App/src/SceneSerializer.cpp:569-571`; `RT2App/src/SceneManager.cpp:77-93`). |
| `Transform` | Plausibly overridable | Local TRS is authored and edited; world matrices/dirty flags are derived/transient (`RT2App/src/SceneManager.cpp:3177-3188`; `RT2App/src/SceneSerializer.cpp:583-588`). |
| `VisibleComponent` | Plausibly overridable | Authored visibility is a normal component edit (`RT2App/src/SceneManager.cpp:1367-1397`). |
| `PrimitiveComponent` | Plausibly overridable, with a rebuild requirement | Its fields are durable primitive recipe data, but changing them must rebuild the generated mesh; W3 cannot treat the copied `MeshRef` index as the payload (`RT2App/src/SceneSerializer.cpp:600-604,1133-1153`). |
| `ImportedMeshSourceComponent` | **Structurally suspicious; decision required** | It is the source link used to rebuild geometry, not an ordinary instance value. Overriding it means rebinding the asset and re-resolving identity, which can invalidate the member/source relationship (`RT2App/src/SceneAssetResolver.cpp:334-380`). Recommendation: exclude it from ordinary instance overrides until a rebind policy exists. |
| `MaterialOverrideComponent` | Plausibly overridable, but special | It is itself a whole-value authored override and must use the stacking/order rules in Q3; `materialIndex` and texture indices cannot be persisted as identity (`RT2App/src/ECSComponents.h:241-269`; `RT2App/src/PrefabSerializer.h:21-31`). |
| `LightComponent` | Plausibly overridable | Authored light parameters are serialized values and have a dedicated state edit (`RT2App/src/SceneSerializer.cpp:618-622`; `RT2App/src/SceneManager.cpp:3763-3780`). |
| `CameraComponent` | Plausibly overridable | Authored camera parameters are serialized and edited as component state (`RT2App/src/SceneSerializer.cpp:624-628`; `RT2App/src/SceneManager.cpp:3783-3800`). |
| `MotionComponent` | Plausibly overridable | Authored velocity is an ordinary optional component state (`RT2App/src/SceneSerializer.cpp:630-634`; `RT2App/src/SceneManager.cpp:3877-3897`). |
| `ScriptComponent` | Plausibly overridable | The binding and typed field map are durable; W2 already remaps internal UUID fields after copy (`RT2App/src/SceneSerializer.cpp:636-642`; `RT2App/src/SceneManager.cpp:1466-1487`). |
| `MeshRef` | **Structurally excluded from a prefab payload** | `meshIndex` is a transient `MeshRegistry` index and `materialIndex` is a transient scene-material index (`RT2App/src/ECSComponents.h:67-75`; `RT2App/src/SubtreeSnapshot.h:54-57`). W0 forbids such indices in prefab files (`RT2App/src/PrefabSerializer.h:21-31`). Resolver/import or primitive rebuild must derive it. |
| `Hierarchy` | Structurally excluded | Parent/children are relationship state and are explicitly handled outside `PersistedComponents` (`RT2App/src/PersistedComponents.h:10-14`; `RT2App/src/SceneSerializer.cpp:1210-1234`). |
| `EntityIdComponent` | Structurally excluded | It is the document identity that W1 must mint fresh per instance; it is not an inherited component value (`RT2App/src/ECSComponents.h:144-152`; `docs/game-engine-development-plan.md:12711-12727`). |

The root transform should be an override **only after the user edits it**, not
automatically at instantiation. The sparse set is intended to mean “this
instance deliberately diverged”; marking every root transform would prevent a
source root-transform change from propagating to untouched instances. This is a
recommendation from the sparse-set semantics in D2, not an existing behaviour
(`docs/game-engine-development-plan.md:12729-12745`; no W1 marker exists in
the code at `0df3175`).

## Q6 — when an edit becomes an override, and what undo costs

**Verified plumbing.** Existing editor mutations are not generic reflective
component writes:

- Light, camera, material, motion, and script have separate state APIs that
  apply an after value, notify authoring dirtiness, and return an authoritative
  `EditorMutationResult` (`RT2App/src/SceneManager.cpp:3763-3800,3803-3830,3877-4028`).
- Transform and visibility have their own mutation paths and dirty/sync
  handling (`RT2App/src/SceneManager.cpp:1367-1397,3177-3188`).
- The command contract requires complete before/after state at construction;
  `Execute`/`Undo` resolve UUIDs at run time and must not capture inside
  `Execute` (`RT2App/src/EditorCommand.h:12-23`).
- Property commands already store full optional override snapshots for material
  edits and restore them verbatim via `InstallMaterialOverride`
  (`RT2App/src/EditorPropertyCommands.h:43-116`; `RT2App/src/EditorPropertyCommands.cpp:132-196`).
  `InstallMaterialOverride` does not notify or bump revision, so it must remain
  paired with a state API (`RT2App/src/SceneManager.h:517-529`).
- Continuous edits deliberately capture before on activation and after on
  release. The transform session documents the ordering requirement
  (`RT2App/src/SceneEditorUI.cpp:1174-1191,1326-1349`); script fields use the
  same “mutate per frame, record final complete state” shape
  (`RT2App/src/SceneEditorUI.cpp:2075-2154`).

**Automatic-on-edit (recommendation):** the first effective edit to a component
on a prefab member should atomically change both the component value and its
override-set membership. The state API/command must capture a before state that
includes “marker absent” and an after state that includes the complete edited
component plus “marker present”. Continuous widget edits need the marker in the
activation snapshot, not added after the command has been constructed. This is
the least surprising sparse-set semantics, but it touches every component edit
path and must handle material-properties fan-out (one slot edit may update many
entities, `RT2App/src/SceneManager.cpp:3811-3822`).

**Explicit-user-action:** ordinary edits would remain inherited until a
separate “override this component” action. That can keep existing property
commands smaller, but requires a new marker command with complete before/after
membership and a clear rule for what value it captures. It also permits an edit
to be silently overwritten by W4 before the user performs the explicit action;
that risk belongs in the decision, not in UI code. W6 owns the affordance, but
W3 must expose an atomic data operation either way.

The cautionary precedent is concrete. The 2026-08-03 material-index defect
captured the “before” override after mutating the index, making both command
snapshots equal; the fix moved the capture inside `SetMaterialIndexState`
before the write and returns both snapshots (`RT2App/src/SceneManager.cpp:3848-3867`).
This is the `697d3c9` defect and `a149fd4` repair recorded by the Phase 8
prompt; the current source preserves the repaired ordering at those lines.
Any W3 automatic marker must follow the same complete-state-at-construction
contract; a read-after-mutate marker is a no-op undo bug waiting to happen.

## Q7 — schema and unknown-field policy

**Verified against current code.** `.rt2scene` is schema 4 and reads versions
3 through 4 (`RT2App/src/SceneSerializer.h:85-120`; `RT2App/src/SceneSerializer.cpp:1628-1645`).
The entity reader is presence-based and only asks for known fields; unknown
object members are ignored rather than retained
(`RT2App/src/SceneSerializer.cpp:792-885`). The writer emits a newly built
JSON object containing only known fields (`RT2App/src/SceneSerializer.cpp:1383-1408`).
The plan comments explicitly rely on older readers ignoring additive fields
(`RT2App/src/SceneSerializer.cpp:1424-1433`).
This permissiveness does not apply to an unknown asset kind with a non-empty
path: that remains a hard parse error (`RT2App/src/SceneSerializer.cpp:297-303`);
W0 makes the current build recognize `"prefab"` (`RT2App/src/AssetReference.h:29-64`).

W0 deliberately gives `.rt2prefab` its own format version 1, independent of
the scene schema (`RT2App/src/PrefabSerializer.h:41-53`). The Phase 8 W1 spec
states that W1 takes `.rt2scene` from v4 to v5 for new prefab components
(`docs/game-engine-development-plan.md:12782-12790`); this is **spec intent,
not present code** — the current tree still declares `SchemaVersion = 4`.

**Decision required:** W3 can technically add an optional override-set field
inside W1's v5 because the current parser accepts absent fields and ignores
unknown members. That is not lossless compatibility: a pre-W3 v5 reader will
ignore the set and, on save, drop it because unknown members are not preserved.
If that silent loss is unacceptable, W3 needs v6 (or W1 must reserve a
versioned metadata envelope whose unknown payload is preserved). **Recommendation:
use a new schema version for the first reader that understands override-set
semantics unless the project explicitly accepts old-reader metadata loss.**
`MinReadVersion` can remain 3 only if the v5/v6 reader still has all migrations
for v3/v4; the current policy is a numeric range, not feature negotiation
(`RT2App/src/SceneSerializer.h:123-129`; `RT2App/src/SceneSerializer.cpp:1638-1645`).

## Q8 — commitments carried into Phase 8

The Phase 7 closure records four items “Carried into Phase 8”
(`docs/game-engine-development-plan.md:11667-11685`). They must not disappear
because W3 is narrowly named “overrides”:

1. **Machine-locked tests:** sixteen absolute-path references to two large
   Downloads fixtures keep the suite local-only. This is a Phase 8 carried
   engineering debt, not evidence that W3 tests are portable
   (`docs/game-engine-development-plan.md:11667-11672`).
2. **Override-only compaction references:** this was explicitly deferred until
   Prefabs. The pre-work is now landed: override snapshots mark their own
   textures, while their transient material slots remain deliberately
   invalidatable (`RT2App/src/SceneManager.cpp:212-219,288-300,4227-4265`). W3
   must preserve this invariant when its prefab snapshots are unmirrored.
3. **File-local versus scene-global indices remain the same C++ types.** W3
   must not serialize either kind of index in prefab override data; W0's hard
   rule and the glossary's Asset/Authoring/Scene/GPU boundary are the safe
   response (`docs/game-engine-development-plan.md:11677-11682`;
   `RT2App/src/PrefabSerializer.h:21-31`; `docs/glossary.md:29-56`).
4. **One W6 negative-case test still lacks its recorded discriminating fault.**
   That is a carried test-quality commitment when W6 is touched, not a reason
   for W3 to weaken its data tests (`docs/game-engine-development-plan.md:11683-11685`).

The same closure also says Phase 7 deliberately did not deliver automatic
source-file reimport; it delivered database refresh only
(`docs/game-engine-development-plan.md:11636-11644`). W4 propagation is
therefore a new active rewrite path, not an existing watcher callback that W3
can reuse.

## Q9 — what W6 needs W3 to expose

W6 owns inspector/content-browser surfacing and the phase exit requires an
override to be visible and recoverable (`docs/game-engine-development-plan.md:12818-12823,12892-12896`).
W3 does not need to design the widgets, but its data model must expose:

- the prefab asset reference and instance identity on the root, plus each
  member's frozen template identity (the A1 shape is still provisional)
  (`docs/game-engine-development-plan.md:12711-12727,13066-13108`);
- a stable component wire key and a query that distinguishes inherited from
  overridden for a given member, without asking W6 to inspect EnTT storage or
  infer state by comparing values;
- the complete effective/template/instance component values needed to show
  what will be propagated and to make Revert/Apply/Unpack commands lossless;
- an atomic mutation API that returns complete before/after override-set state,
  diagnostics and sync impact, following the existing material helper pattern
  (`RT2App/src/SceneManager.h:517-529`);
- explicit stale/missing diagnostics for missing template members, unsupported
  component keys, missing prefab assets, and source-material/resource repair
  failures. The existing resolver diagnostic channel is the established place
  to route recoverable asset failures (`RT2App/src/SceneAssetResolver.h:74-87`).

An override marker that exists only in an opaque serializer side table would be
hard for W6 to recover and hard for the command layer to undo. Conversely,
putting transient MeshRegistry/material indices in the marker would violate the
Asset/Authoring/Scene/GPU boundary and make the inspector's state unstable after
compaction (`docs/glossary.md:29-56`; `RT2App/src/SceneManager.cpp:244-300`).

## Contradictions and provisional boundaries

The most important contradictions between the Phase 8 framing and `master`
are:

1. **The W1 machinery does not exist on the grounding commit.** W0's
   `PrefabSerializer` is an envelope with an intentionally empty entity list,
   and W2's remapper has no prefab caller (`RT2App/src/PrefabSerializer.cpp:72-84,161-172`;
   `RT2App/src/EntityReferenceRemapper.h:11-26`). Any W3 statement about
   actual instantiate/load ordering is provisional until W1 lands and is
   verified.
2. **There is no component-level override set in current scene data.** The
   only existing durable “override” is the special whole-material component,
   and its source matching/resource repair are resolver-specific
   (`RT2App/src/ECSComponents.h:241-269`; `RT2App/src/SceneAssetResolver.cpp:688-905`).
3. **`PersistedComponents` is not a serialization identity registry.** It has
   only C++ type tags and a count assertion (`RT2App/src/PersistedComponents.h:17-35`;
   `RT2App/src/SceneSerializer.cpp:38-39`). Stable component keys are net-new.
4. **The spec's v5 claim is not current code.** The serializer remains v4 and
   the W0 prefab format is independently versioned (`RT2App/src/SceneSerializer.h:123-129`;
   `RT2App/src/PrefabSerializer.h:41-53`). Whether W3 needs v6 depends on the
   compatibility/data-loss policy above.
5. **Material precedence cannot be copied as a second additive layer.** A
   prefab override of `MaterialOverrideComponent` must produce one effective
   component before resolution; otherwise resolver append/slot state can be
   doubled or stale (`RT2App/src/SceneAssetResolver.cpp:857-905`).

## What W3 cannot settle before W1 lands

These are deliberately open, not assumed defaults:

- whether W1's `.rt2scene` instance keeps full copied component values (Option
  A) or stores inherited data only through a prefab link (Option B);
- the actual prefab record codec, including how W1 strips transient indices,
  wraps `SubtreeEntityRecord`, and serializes `templateId`;
- the exact scene load order among prefab-file load, UUID/reference remapping,
  override-set application, model/material resolution, and hierarchy rebuild;
- which of `ImportedMeshSourceComponent`, `PrimitiveComponent`, and
  `MaterialOverrideComponent` W1 can materialize without inventing a new
  resource-resolution contract;
- the v5 fields W1 actually adds, and therefore whether W3 can reserve an
  additive metadata slot or must bump to v6;
- the command seam W1 exposes for editing a prefab member, including whether
  automatic-on-edit can capture the complete before/after state without a
  read-after-mutate race;
- the runtime acceptance path (“instantiate several times, override one
  material, update the source”) — W0 explicitly rejects prefab reimport until
  W4 (`RT2App/src/ContentBrowserOperations.cpp:551-560`), and no W1 runtime
  path is present on `master`.

Until W1 is landed and verified, W3 should be treated as a data-model/spec
decision pass. The recommendation to retain full scene values, use explicit
stable component keys, mark only edited components, and resolve one final
`MaterialOverrideComponent` value before resource resolution is reasoned
guidance, not an already implemented engine contract.
