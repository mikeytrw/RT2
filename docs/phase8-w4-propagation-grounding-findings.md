# Phase 8 W4 — prefab propagation grounding findings

Grounded read-only against commit `62d0eb4` (`docs: ground Phase 8 W3 prefab
overrides`, 2026-08-04), forked from the merged W0/W2 tree at `0df3175`.
W1 is not present on this branch. Every statement below is labelled
**verified against code** or **inferred from the Phase 8 spec/W1 intent**.
The plan is append-only; completed phase reports are period records. Current
behaviour below is taken from the tree, not from an old count or status claim.

## Executive verdict

**Verified against code: RT2 has no live source-asset → dependent-entity
propagation pathway today.** The briefing premise that imported-mesh
reimport already updates existing dependants is false. `ResolveAll` is called
in the production app only while preparing a temporary document for scene
open (`RT2App/src/WalnutApp.cpp:4360-4383`) and recovery
(`RT2App/src/SceneRecoveryService.cpp:524-546`); the slice runner has a
separate load-time call (`RT2SliceRunner/src/Main.cpp:893-911`). There is no
call that re-resolves the currently adopted authoring document after a model
or prefab file changes.

The event-delivery half does exist. The project-root watcher classifies
`.rt2prefab` as a database-refresh event (`RT2App/src/AssetWatchPolicy.cpp:21-30`),
queues it, and drains it into `RefreshProjectAssets()`
(`RT2App/src/WalnutApp.cpp:3693-3771`). That refresh replaces the immutable
asset-database snapshot and updates the resolver context
(`RT2App/src/WalnutApp.cpp:3845-3878`); it does not read the prefab, walk
instances, mutate ECS values, notify authoring, or sync the renderer.

The explicit content-browser “Reimport” path is not an update-in-place
precedent. For models it calls `SceneManager::ImportGltf`/`ImportObj`
(`RT2App/src/WalnutApp.cpp:1589-1622`), whose importer appends entities and
resources to the live ECS (`RT2App/src/SceneManager.cpp:680-734,
736-789`), then marks a full GPU sync pending. For `.rt2prefab`, W0 rejects
the operation explicitly (`RT2App/src/ContentBrowserOperations.cpp:552-559`).
The code does not establish whether the model behaviour was deliberate or a
latent mismatch with the W6 prose; it does establish that it cannot be used
as W4's propagation mechanism.

**Scope finding:** W4 must create RT2's first source-to-dependent propagation
mechanism. The two-sentence roadmap entry substantially understates the
workstream. W4 can extend the existing watcher/event seam and the existing
staged resolver/key-matching machinery, but it must add the dependent
traversal, plan/apply transaction, diagnostics, authoring notifications, and
GPU/history integration.

**Recommendation (inferred, unsettled until W1/W3 land): retain W3 Option A.**
Keep complete authored component values in each scene instance and keep the
prefab override set sparse metadata. The current scene serializer is
value-first (`RT2App/src/SceneSerializer.cpp:564-644,1087-1207`) and external
resource resolution is separate from authored values
(`RT2App/src/SceneSerializer.h:105-109`; `RT2App/src/SceneAssetResolver.h:17-58`).
This makes a missing prefab recoverable. The price is precisely the new W4
work: an explicit propagation pass must rewrite non-overridden values.

The recommended implementation seam is a **plan-then-apply prefab
propagation service** that is shared by three entry points:

1. scene load/recovery, before the temporary document is adopted;
2. an explicit content-browser “Reimport/Propagate” action; and
3. the main-thread watcher drain after a `.rt2prefab` event, deferred while
   background work or Play mode makes mutation unsafe.

The service should read and stage the prefab, map frozen template IDs to
`PrefabMemberComponent` entities, compute complete before/after authored
component state for every non-overridden member, and only then apply one
document mutation. It should reuse the existing model/material staging and
`sourceKey` matching in `SceneAssetResolver` rather than inventing a second
import/resource-repair path (`RT2App/src/SceneAssetResolver.cpp:390-553,
688-915`). This is an extension at the resolver/resource seam, but a new
dependent-update mechanism at the scene-authoring seam.

### Mechanism options

| Option | Shape | Grounded assessment |
|---|---|---|
| A — extend `SceneAssetResolver::ResolveAll` directly | Add prefab-file loading, template/member matching, override filtering, component application, and resource repair to the existing aggregate resolver. | Reuses the existing staging/diagnostic seam, but risks making a model/resource resolver own authoring metadata and inheriting its partial/stale-`MeshRef` transaction caveats (`RT2App/src/SceneAssetResolver.cpp:241-915`). It also gives watcher, load, and explicit actions no distinct plan/command boundary. **Viable foundation, not preferred as the entire mechanism.** |
| B — `PrefabPropagationService` plan/apply, then one existing resolver pass | A CPU-only service reads the prefab and computes complete before/after component state by template ID and override metadata; it applies the final effective components to a staged scene/document, then invokes the existing model/material resolver for resource indices. | **Recommended.** It keeps prefab identity/override semantics separate from imported-resource staging while extending the existing resolver rather than duplicating mesh/material repair. The plan makes the mutation extent known before command construction and supports load, watcher, and content-browser callers. |
| C — host-only direct mutation | Have `WalnutApp` watcher/content-browser callbacks open the prefab and mutate ECS entities themselves. | **Reject.** It would create a second source parser/update path outside CPU-only tests, make rollback/diagnostics/history host-specific, and repeat the context-boundary failures the glossary calls out (`docs/glossary.md:39-55`). |

Option B is therefore the recommendation: **extend existing event delivery and
resource staging, but build one new CPU-only plan/apply authoring mechanism**.

## Q1 — Existing precedent: what actually happens for imported meshes?

### 1. No live reimport propagation exists — verified

- The complete production `ResolveAll` call sites are the scene-open worker
  (`RT2App/src/WalnutApp.cpp:4360-4383`) and recovery restore
  (`RT2App/src/SceneRecoveryService.cpp:524-546`). The slice runner calls it
  only while loading its authoring document (`RT2SliceRunner/src/Main.cpp:893-911`).
  All other tree hits are tests or the declaration/contract comment.
- `RefreshProjectAssets()` only scans the asset root, swaps the project
  database, and pushes the context to `SceneManager`
  (`RT2App/src/WalnutApp.cpp:3845-3878`). `SetAssetResolutionContext` only
  stores the context (`RT2App/src/SceneManager.h:64-68`); it does not resolve
  or mutate ECS.
- W7's current watcher policy deliberately sends model/texture/environment
  changes to database refresh, not reimport. The listener publishes classified
  events (`RT2App/src/WalnutApp.cpp:3413-3452`), the drain accumulates
  `m_DebouncedRefreshPaths` and calls one refresh (`RT2App/src/WalnutApp.cpp:3693-3771`),
  and no path is passed to a scene resolver.

Therefore W4 cannot “extend the existing source-changed → dependants path”
because that path does not exist. It should extend the **event delivery** and
the **resource staging** paths, while implementing the missing authoring
propagation layer.

### 2. Existing load-time resolver path — verified

When a `.rt2scene` is opened, the worker parses a temporary `SceneDocument`,
binds its project/asset context, runs `ResolveAll`, and rejects adoption on a
false result (`RT2App/src/WalnutApp.cpp:4198-4383`). On success the temporary
document is adopted, history is cleared, and GPU upload is scheduled
(`RT2App/src/WalnutApp.cpp:4419-4464`). Recovery follows the same temporary
document shape (`RT2App/src/SceneRecoveryService.cpp:524-546`).

Inside `ResolveAll` the current end-to-end model path is:

1. enumerate entities carrying `ImportedMeshSourceComponent`
   (`RT2App/src/SceneAssetResolver.cpp:334-381`);
2. deduplicate model references and locate each through the read-only asset
   resolver (`RT2App/src/SceneAssetResolver.cpp:295-330`);
3. load each model into a staging `ECSScene` through `SceneLoader`
   (`RT2App/src/SceneAssetResolver.cpp:390-469`);
4. build a durable source-key → staged mesh/material map
   (`RT2App/src/SceneAssetResolver.cpp:472-529`);
5. plan each target entity without initially mutating the target document,
   producing `Missing`/`Unresolved`/`Stale` diagnostics
   (`RT2App/src/SceneAssetResolver.cpp:555-769`); and
6. commit staged meshes, materials and textures, repair authored material
   texture indices, install `MeshRef`, and update world transforms
   (`RT2App/src/SceneAssetResolver.cpp:785-915`).

That is a useful foundation for W4's resource half. It is not a propagation
path: it only visits existing `ImportedMeshSourceComponent` entities and it
does not inspect prefab records, instance/member metadata, or an override set.

### 3. Explicit model “Reimport” is not an update-in-place precedent — verified

`ReimportContentBrowserAsset` validates the source/sidecar, requires a stable
sidecar ID, invokes a host callback, then checks that the callback did not
change the durable ID (`RT2App/src/ContentBrowserOperations.cpp:535-600`).
The Walnut callback dispatches by extension to `ImportObj` or `ImportGltf`
(`RT2App/src/WalnutApp.cpp:1591-1617`). Those functions call the importers
directly on the live `m_EcsScene`, assign UUIDs to newly imported entities,
record source provenance, and set only `m_EntityCacheDirty`
(`RT2App/src/SceneManager.cpp:680-734,736-789`). The host then marks dirty,
requests a pending full GPU sync, and refreshes the database
(`RT2App/src/WalnutApp.cpp:1617-1627`).

The W6 design record says reimport “replaces” decoded cache state and merges
into the live scene (`docs/game-engine-development-plan.md:9337-9351,
9451-9470`), but the current callback does not identify or replace existing
dependants. Whether that mismatch is a deliberate W6 boundary or a latent
defect is **undetermined from code**. Either way, it is not a safe W4
precedent. W4's acceptance must assert that existing prefab instance UUIDs
remain and their component values change in place.

### 4. The asset database is not a ready dependent index — verified

`AssetRecord` has `dependentEntities`, and `AssetDatabase` exposes
`AddEntityDependency` (`RT2App/src/AssetDatabase.h:59-79,119-154`). However,
the only production caller of that mutator is none: the scanner creates
records with asset identity/path only (`RT2App/src/ProjectAssetScanner.cpp:171-235`),
and `git grep` finds `AddEntityDependency` only in `AssetDatabase.cpp/.h` and
tests. The live dependent query instead walks the scene's durable references
(`RT2App/src/ContentBrowserOperations.cpp:407-437`) through
`CollectSceneAssetReferences`, which currently covers imported models, scripts,
and environment, not prefab components (`RT2App/src/SceneAssetReferenceVisitor.cpp:31-57`).

W4 should extend that visitor or add an equivalent prefab-specific traversal;
it must not assume `AssetDatabase::dependentEntities` is populated.

## Q2 — Does the watcher see `.rt2prefab` and what happens now?

**Verified:** yes, but only as a database-refresh signal.

- `.rt2prefab` is in `ClassifyExtension` beside model/HDR/EXR/sidecar
  extensions (`RT2App/src/AssetWatchPolicy.cpp:21-30`). Classification is
  case-folded (`RT2App/src/AssetWatchPolicy.cpp:46-62`).
- The watcher recursively watches the project `assetRoot`
  (`RT2App/src/WalnutApp.cpp:3792-3842`), so a prefab under that root is seen.
- The listener publishes the normalized path into the queue
  (`RT2App/src/WalnutApp.cpp:3413-3452`); the drain deduplicates it in
  `m_DebouncedRefreshPaths` (`RT2App/src/WalnutApp.cpp:3693-3718`).
- After debounce, the current drain calls `RefreshProjectAssets()` once
  (`RT2App/src/WalnutApp.cpp:3720-3777`). The path is not passed to a loader,
  and the refresh does not call `SceneAssetResolver::ResolveAll`
  (`RT2App/src/WalnutApp.cpp:3845-3878`).

Thus an external `.rt2prefab` edit currently updates the content-browser
database snapshot only. Open scene entities and the renderer remain unchanged.
W4 can preserve this safe event boundary while adding a second, explicit
main-thread action for prefab propagation; a watcher thread must not mutate
ECS or Vulkan state.

The content-browser branch currently rejects explicit prefab reimport with a
specific W0 diagnostic (`RT2App/src/ContentBrowserOperations.cpp:552-559`).
W4 should replace that rejection with the same staged propagation service used
by the watcher and load path, while retaining stable-ID verification.

## Q3 — `ResolveAll` entry points and partial-failure contract

### Production call-site assumptions — verified

| Call site | Document state and failure policy |
|---|---|
| `RT2App/src/WalnutApp.cpp:4198-4383` | Parsed into `resultDoc`; project binding and asset context are set before `ResolveAll`. False means “keep current scene” and no adoption. True adopts the temporary document; partial diagnostics are logged and the partial document is accepted. |
| `RT2App/src/SceneRecoveryService.cpp:524-546` | Recovery bytes are materialized into `temp`; logical source/project metadata is restored before resolve. False aborts recovery; true moves `temp` to `outDoc` and marks it dirty. |
| `RT2SliceRunner/src/Main.cpp:893-911` | Loads an authoring document and resolves before running the slice. False exits the process. |

The serializer intentionally does not resolve external assets
(`RT2App/src/SceneSerializer.h:105-109`); each caller owns the subsequent
resolver call.

### Total failure versus partial success — verified, with two transaction caveats

If all pending imported entities fail, the model plan returns
`Error::MissingAsset` and commits no staged model resources
(`RT2App/src/SceneAssetResolver.cpp:772-783`). A clean fixture with no
pre-existing transient references therefore observes an unchanged ECS
(`RT2Tests/src/Phase7W3CharacterizationTests.cpp:509-538`). If at least one
entity resolves, the resolver returns true and commits accepted resources
while failed entries only receive diagnostics (`RT2App/src/SceneAssetResolver.cpp:785-915`),
matching the partial-success test (`RT2Tests/src/Phase7W3CharacterizationTests.cpp:706-741`).

Two details matter for W4 and supersede an over-broad reading of the W3
grounding note:

1. **A failed entity is not actively cleared.** The plan pass does not mutate
   its existing `MeshRef`, and the commit loop only installs/repairs refs for
   successful entries (`RT2App/src/SceneAssetResolver.cpp:673-685,851-906`).
   On a file-load document, `BuildDocumentFromRecords` can already have
   installed a placeholder/stale `MeshRef` for an imported entity
   (`RT2App/src/SceneSerializer.cpp:1154-1174`). A partial in-place resolve
   can therefore leave a failed entity rendering through an old transient
   scene index rather than “without a resolved MeshRef.” The existing test
   starts the failed entity without a `MeshRef`, so it does not cover this
   case (`RT2Tests/src/Phase7W3CharacterizationTests.cpp:717-734`).
2. **`ResolveEnvironment` mutates before the model aggregate decision.** It
   clears pixels on environment failure (`RT2App/src/SceneAssetResolver.cpp:173-205`)
   and caches an effective sidecar ID on success
   (`RT2App/src/SceneAssetResolver.cpp:228-238`) before model
   resolution can later return false (`:772-783`). The “false leaves the
   document unchanged” comment is therefore only safe for the staged model
   ECS in the tested model-only cases, not for every `SceneDocument` field.

W4 must run prefab propagation on a clone/staged document or introduce an
   explicit transaction that includes prefab metadata, authored components,
   resource arrays, and environment state. It must not apply `ResolveAll`
   directly to the live authoring document and infer transactionality from
   the current comment.

## Q4 — Notifications, revision, cache dirtiness, and renderer sync

### Authoring notifications — verified

`NotifyAuthoringChanged()` marks the document dirty and increments the single
authoring revision (`RT2App/src/SceneManager.cpp:3730-3734`). The public
contract says editor mutations call it (`RT2App/src/SceneManager.h:531-551`).
`ResolveAll` itself never calls it. `m_EntityCacheDirty` is a cache for indexed
entity enumeration (`RT2App/src/SceneManager.cpp:3428-3439`); current imports
set it (`RT2App/src/SceneManager.cpp:729,785`), but resolver commits do not.

For propagation, call the notification once per successful batch, not once
per entity, after the plan has committed. Mark `m_EntityCacheDirty` if the
batch adds/removes entities or changes component membership; value-only
rewrites do not need to invalidate the entity-list cache. A failed plan must
leave dirty/revision/cache state unchanged.

### Resource-generation trap — verified

`EditorSyncRouter` downgrades a requested `Structural` result to
`MaterialSync` when `SceneManager::ResourceGeneration()` equals the last
synced generation (`RT2App/src/EditorSyncRouter.cpp:25-49`; contract
`RT2App/src/EditorSyncRouter.h:18-25`). `m_ResourceGeneration` currently
increments on document adoption/load/clear and compaction, not on ordinary
component mutations (`RT2App/src/SceneManager.cpp:372-392,4091-4097,4249-4275`).
`ResolveAll` appends meshes/materials/textures but does not increment it
(`RT2App/src/SceneAssetResolver.cpp:800-838`).

If W4 reports `Structural` after appending new resources without also bumping
the generation or setting the host's full-sync flag, the router can take the
downgrade path and preserve old GPU textures. This is a concrete Asset → Scene
→ GPU boundary hazard, not speculation. The glossary defines those contexts
and the required translation explicitly (`docs/glossary.md:39-55`).

### What each sync path actually costs — verified

The render bridge defines `Transform` as instance-buffer-only, `Material` as
material rebuild with textures preserved, and `Structural` as full texture,
material, mesh and AS rebuild (`RT2App/src/ISceneRenderBridge.h:17-50`).
Walnut maps those impacts to transform, material, or full callbacks
(`RT2App/src/WalnutApp.cpp:329-358`).

The concrete GPU paths are heavier than the names suggest:

- full sync calls `RendererGPU::SetScene`, which hands the data to
  `SceneResources::SetScene` and marks AS rebuild required
  (`RT2App/src/RendererGPU.cpp:531-560`; `RT2App/src/SceneResources.cpp:86-152`);
- material/keep-textures sync calls `SetSceneKeepTextures`, preserves existing
  textures but also sets `m_NeedsASRebuild = true`
  (`RT2App/src/RendererGPU.cpp:468-487`; `RT2App/src/SceneResources.cpp:46-84`);
- the eventual rebuild constructs BLASes for all current meshes and a TLAS
  for all instances (`RT2App/src/SceneResources.cpp:230-335`). The UI uses an
  async fence path where possible, with a blocking fallback
  (`RT2App/src/WalnutApp.cpp:2108-2155`).

Therefore W4 should aggregate one sync impact for the whole propagation. Use
`Structural` whenever resource tables, `MeshRef`, geometry, textures,
hierarchy, or component membership changes; use `Material` only for a proven
no-new-resource scalar/material-table rewrite. In both cases, bump resource
generation when resource arrays change and ensure newly staged textures take
the full upload path. Do not issue one GPU sync per instance.

## Q5 — Undo judgement

### What exists today — verified

`IEditorCommand` requires UUID-keyed commands with complete before/after state
provided at construction; `Execute` and `Undo` must not capture state while
running (`RT2App/src/EditorCommand.h:12-25`). Structural commands are the
closest existing pattern: the host captures a complete `SubtreeSnapshot` and
the command restores/removes it (`RT2App/src/EditorStructuralCommands.h:22-46`).
History records only successful effective commands and clears both stacks on a
failed undo/redo (`RT2App/src/EditorCommandHistory.cpp:3-33,64-117`).

There is **no** existing command for wide asset reimport. The current model
reimport callback mutates/imports directly and marks dirty/full-sync
(`RT2App/src/WalnutApp.cpp:1591-1627`); it does not enter
`EditorCommandHistory`. Thus reimport is not a precedent for an unknown-size
document mutation. The closest reliable precedent is “stage/capture a full
structural snapshot, then record the applied command.”

### Recommendation — make propagation undoable after a plan is known

**Recommendation (unsettled implementation detail): propagation should be an
undoable `IEditorCommand`, not an untracked mutation.** Option A actively
rewrites authored values on every non-overridden instance; treating that like
resource-only reimport would make a source edit able to destroy user-visible
scene state with no recovery. That cost is not defensible for the feature's
central operation.

Use a two-stage flow:

1. A worker/read-only phase loads the source prefab and computes a complete
   propagation plan: target scene UUID, template ID, component presence, full
   before value, full after value, override metadata before/after, and any
   resource/diagnostic requirements. The extent is unknown only before this
   scan; it is known before command construction.
2. On the main thread, validate the document generation and source fingerprint,
   construct the command with that complete plan, then execute it. Redo uses
   the stored after state; undo uses the stored before state. Never read the
   live component after mutation to manufacture the “before” state. This is
   the exact failure repaired by `697d3c9`, and the hard contract is recorded
   at `RT2App/src/EditorCommand.h:19-21` and
   `docs/game-engine-development-plan.md:12838-12847`.

Automatic watcher propagation can coalesce one source event into one history
entry. Load-time propagation happens on the temporary document before scene
adoption and therefore does not need a history entry. Explicit content-browser
propagation should use the same command path.

Undo restores scene state, not the external `.rt2prefab` file. The source edit
therefore remains on disk; a later scene reload may reapply it. W4 must make
that limitation visible and define whether an undone automatic adoption is
marked stale/deferred until the source changes again. If the team instead
chooses an out-of-band policy, it must explicitly accept this data-loss cost
and provide a separate recoverable snapshot; “reimport does not use history”
is not sufficient justification because propagation changes authored values.

The history snapshot also inherits the no-compaction invariant: while undo or
redo is live, `WalnutApp` defers compaction (`RT2App/src/WalnutApp.cpp:296-315`),
and the snapshot contract calls out transient `MeshRef` indices
(`RT2App/src/EditorStructuralCommands.h:37-46`). A propagation command that
stores scene indices or resource deltas must preserve that invariant and clear
history on document adoption as the existing host does
(`RT2App/src/WalnutApp.cpp:4425-4437`).

## Q6 — When propagation should run

**Recommendation (inferred): use all three entry points, with one common
service.**

| Trigger | Current status | W4 recommendation |
|---|---|---|
| `.rt2prefab` file watcher event | Verified: database refresh only (`RT2App/src/WalnutApp.cpp:3693-3777`) | Retain the event queue/debounce; after the database refresh, schedule a staged propagation on the main thread when safe. |
| Explicit content-browser action | Verified: prefab reimport rejected (`RT2App/src/ContentBrowserOperations.cpp:552-559`) | Replace rejection with the staged/command path; preserve sidecar ID validation (`:564-600`). |
| Scene load/recovery | Verified: model resolver runs on a temporary document (`RT2App/src/WalnutApp.cpp:4198-4383`; `RT2App/src/SceneRecoveryService.cpp:524-546`) | Apply current prefab source to non-overridden members before adoption, so source edits made while the editor was closed are observed. |

The watcher is project-scoped and is not active in standalone mode
(`RT2App/src/WalnutApp.cpp:3792-3842`). That makes load-time reconciliation
mandatory: an external edit while the editor is closed generates no queued
event, but the next scene load can still read the current source.

### Play mode — verified lifecycle and recommendation

`RuntimeSceneController::Play` clones the authoring document into a separate
runtime document, then full-syncs the clone (`RT2App/src/RuntimeSceneController.cpp:21-78`).
The editor is made non-editable while Play/Paused
(`RT2App/src/WalnutApp.cpp:3995-4017`; `RT2App/src/SceneEditorUI.cpp:90-99`).
`Stop` destroys the runtime clone and full-syncs the unchanged authoring
document (`RT2App/src/RuntimeSceneController.cpp:190-236`; host transition
`RT2App/src/WalnutApp.cpp:4031-4041`).

Today a prefab watcher event during Play only refreshes the asset database and
does not affect either document. W4 must not mutate the runtime clone (it is a
simulation snapshot) or the read-only authoring document during Play. Queue
the source path/fingerprint and apply it once the state returns to Edit, after
Stop has reactivated authoring. If a source changes repeatedly during Play,
coalesce to the newest source revision and report superseded events; do not
run propagation from the watcher thread.

## Q7 — Structural deltas and diagnostics

W1/W0 currently provide no structural prefab records: W0 refuses every
non-empty prefab record list (`RT2App/src/PrefabSerializer.cpp:72-84,161-172`),
and W1's template/member components are spec intent only. D6 explicitly defers
child add/delete merge behind a loud `AssetDiagnostic`
(`docs/game-engine-development-plan.md:12789-12803`).

**Recommendation (inferred, no merge design):** during propagation, compare
the frozen template-ID set in the source with the instance's
`PrefabMemberComponent` set. A missing source member, an unexpected instance
member, or a source component key not present in the instance is a structural
delta. Emit one deterministic diagnostic containing the prefab `refPath`,
instance UUID, template ID/source key, and reason; leave the existing instance
subtree unchanged for that member. Do not silently delete children, invent
UUIDs, or match by name. The stable-ID requirement is spec intent from A1,
not current code (`docs/game-engine-development-plan.md:13066-13108`; current
snapshot has only document UUID, `RT2App/src/SubtreeSnapshot.h:43-83`).

`AssetDiagnostic` already carries severity, kind, reference path, entity UUID,
source key, and detail (`RT2App/src/AssetResolver.h:75-103`), and the host logs
those fields (`RT2App/src/WalnutApp.cpp:2434-2451`). W4 should route the
structural guard through that existing diagnostic sink. The exact severity and
whether a structurally affected instance blocks the entire batch remain open
decisions; D6 says only that the condition must be loud and not mis-merged.

## Q8 — Material interaction and runtime acceptance hazards

### Existing material layer — verified

`MaterialOverrideComponent` is one whole-value snapshot plus an authored flag,
durable source-material key, and transient scene material index
(`RT2App/src/ECSComponents.h:241-269`). The resolver's effective order today
is source material → authored snapshot → one installed `MeshRef` slot
(`RT2App/src/SceneAssetResolver.cpp:688-769,857-906`).

On resolve, a new-form `sourceMaterialKey` is matched against staged material
keys; a miss emits `Stale` and falls back to the resolved slot
(`RT2App/src/SceneAssetResolver.cpp:695-731`). For an authored override, the
resolver copies texture indices from the staged material into the authored
snapshot, appends the snapshot, and installs its transient index
(`RT2App/src/SceneAssetResolver.cpp:857-890`). This repair is the only reason
an imported material override survives a rebuilt texture table.

### W4 hazards — verified/inferred boundary

- **One effective component only (inferred requirement):** W3's recommended
  order is source → template material component → instance prefab component
  override → one resolver append/install. Appending both template and instance
  snapshots would produce competing material slots and stale `MeshRef`
  indices (`docs/phase8-w3-overrides-grounding-findings.md:136-171`).
- **Do not apply after resource resolution (inferred):** applying a prefab
  material value after `ResolveAll` leaves `MeshRef::materialIndex` and the
  appended material slot describing the old value. Materialize the final
  effective component first, then run one source/material repair pass
  (`docs/phase8-w3-overrides-grounding-findings.md:165-171`).
- **Prefab files cannot carry scene indices (verified W0 rule):** `.rt2prefab`
  must strip `MeshRef::meshIndex`, `MaterialOverrideComponent::materialIndex`,
  and override texture indices (`RT2App/src/PrefabSerializer.h:26-35`). A
  propagation pass must repair those indices while a staged source material
  exists; copying the raw `MaterialOverrideComponent` from a prefab record is
  unsafe.
- **`authored == false` is ambiguous (verified):** the resolver ignores the
  snapshot when false (`RT2App/src/ECSComponents.h:248-251`), but the prefab
  layer has not defined whether that means “inherited,” “explicitly restored
  to source,” or “invalid metadata” (`docs/phase8-w3-overrides-grounding-findings.md:173-178`).
- **Source-key miss is loud but still slot-fallback:** a renamed/removed
  source material produces `Stale` and then follows slot position
  (`RT2App/src/SceneAssetResolver.cpp:717-730`). W4 acceptance must assert the
  diagnostic and the intended fallback, not just visual output.

The roadmap acceptance — several fixture instances, one material override,
source update, and expected propagation — crosses Asset, Authoring, Scene,
and GPU contexts. The glossary explicitly warns that each resource index must
be translated at those boundaries (`docs/glossary.md:39-55`). A test that only
compares component scalars can pass while texture/material indices are wrong.

## Q9 — What propagation cannot know from missing or stale metadata

**Recommendation (inferred from Option A): preserve, diagnose, and detach; do
not guess.** Because the scene keeps full authored values, missing metadata
must not be treated as “all components are inherited.”

| Condition | Safe W4 behaviour | Evidence/constraint |
|---|---|---|
| No prefab link/member metadata on an otherwise complete subtree | Leave current values untouched; treat it as ordinary/detached scene data and emit a `Stale`/`Malformed` diagnostic if the source was expected. | Scene values are complete before external resolution (`RT2App/src/SceneSerializer.cpp:1087-1207`); no current prefab component exists on this branch. |
| Instance link exists but `templateId`/`instanceId` is nil, duplicated, or absent from the source | Do not match by name, order, or scene UUID. Preserve the member and report a diagnostic. | Durable scene identity is document UUID (`RT2App/src/ECSComponents.h:144-152`); A1's frozen template ID is spec intent (`docs/game-engine-development-plan.md:13066-13108`). |
| Override set is missing for an entity | Treat override membership as unknown, not empty. Preserve the entity's full values and skip automatic rewrite for that entity/instance. | W3 Option A makes the sparse set the only authority for “non-overridden” (`docs/phase8-w3-overrides-grounding-findings.md:25-69`). |
| Override names a component key that the template no longer has | Do not remove the live component or silently clear metadata. Raise the D6 structural diagnostic and preserve the authored value for repair. | Component wire IDs are not currently stable; unknown keys must be diagnosed (`docs/phase8-w3-overrides-grounding-findings.md:196-210`). |
| Template gains/loses a child or component | Detect as structural delta and leave the affected member/subtree unchanged; do not design a merge in W4. | D6 defers structural merge (`docs/game-engine-development-plan.md:12789-12803`). |

Names are explicitly non-unique and therefore cannot be a recovery key
(`docs/game-engine-development-plan.md:12848-12851`; lookup code
`RT2App/src/ScriptSystem.cpp:1661-1683`). A stale/missing metadata case is a
recoverable Option A scene, but it is not a propagation success.

## Contradictions and supersession notes

1. **Briefing premise contradicted by code:** no existing imported-mesh live
   propagation exists. The event half exists; the dependent traversal/apply
   half does not. W4 is the first such mechanism (`RT2App/src/WalnutApp.cpp:4363`,
   `RT2App/src/AssetWatchPolicy.cpp:21-30`).
2. **“Reimport” name versus current mutation:** W6 prose says decoded state is
   replaced, but the current host callback calls append-style import functions
   on the live ECS (`docs/game-engine-development-plan.md:9451-9466`;
   `RT2App/src/WalnutApp.cpp:1591-1622`; `RT2App/src/SceneManager.cpp:680-789`).
   Intent is undetermined; it is not a propagation precedent.
3. **W3 partial-resolve wording needs supersession:** the current resolver does
   not clear an already-present failed entity `MeshRef`
   (`RT2App/src/SceneAssetResolver.cpp:673-685,851-906`), and environment
   resolution can mutate before a later hard failure
   (`RT2App/src/SceneAssetResolver.cpp:143-238,772-783`). W4 must not treat
   “false leaves document unchanged” as a complete live-document transaction.
4. **Resource-generation gap:** resolver resource commits do not bump
   `m_ResourceGeneration`, while Structural routing relies on that counter for
   full-versus-material sync (`RT2App/src/SceneAssetResolver.cpp:785-915`;
   `RT2App/src/EditorSyncRouter.cpp:25-49`). This must be settled before live
   propagation can safely notify the renderer.

## Verified versus inferred, and what W4 cannot settle yet

### Verified on `62d0eb4`

- W0 watches `.rt2prefab` only as a database-refresh event and rejects prefab
  reimport (`RT2App/src/AssetWatchPolicy.cpp:21-30`; `RT2App/src/ContentBrowserOperations.cpp:552-559`).
- No production live-scene `ResolveAll` call exists; current calls prepare
  temporary load/recovery documents (`RT2App/src/WalnutApp.cpp:4198-4383`;
  `RT2App/src/SceneRecoveryService.cpp:524-546`).
- Model resolution stages resources and repairs `MeshRef`/material overrides
  (`RT2App/src/SceneAssetResolver.cpp:390-915`).
- Existing notifications, sync router, GPU full/keep-texture paths, Play clone,
  Stop restoration, history contract, and no-compaction guard are as cited
  above.
- `AssetDatabase::dependentEntities` is not populated by production scanning,
  and the asset-reference visitor does not know prefabs
  (`RT2App/src/ProjectAssetScanner.cpp:171-235`;
  `RT2App/src/SceneAssetReferenceVisitor.cpp:31-57`).

### Inferred from spec/W1/W3 intent, not verified

- `PrefabInstanceComponent`, `PrefabMemberComponent`, frozen `templateId`,
  sparse override storage, prefab record codecs, and instance-to-template
  matching do not exist in this branch. Their intended shape is described in
  D1/D2/A1 (`docs/game-engine-development-plan.md:12711-12745,13066-13108`).
- The Option A “full copied values plus sparse metadata” load model remains a
  recommendation until W1 proves what it actually persists
  (`docs/phase8-w3-overrides-grounding-findings.md:25-69`).
- The correct component wire IDs, propagation command payload, source revision
  fingerprint, Play-mode queue policy, structural diagnostic severity, and
  whether undo should mark a source adoption stale are all open.

W4 cannot safely finalize those decisions until W1 and W3 land and their
serializer/metadata shapes can be grounded. It can, however, already settle
the headline scope: W4 must build the first source-to-dependent propagation
path, extending the watcher event seam and existing resolver staging while
adding a new plan/apply authoring mechanism.
