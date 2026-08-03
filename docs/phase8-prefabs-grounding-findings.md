# Phase 8 (Prefabs) — read-only grounding findings

Grounded against `master` at commit `697d3c9` (2026-08-03). This is an
investigation record for the Phase 8 spec, not a spec and not a design
proposal. No source files were modified. Where I could not determine
something, I say what I checked and what the spec author must decide.

Method per question is stated inline so each claim is re-verifiable; every
claim carries a `file:line`. The two authoritative state documents for the
tree at this commit are the "Test baseline" in the development plan and
`docs/glossary.md` (four resource-index boundaries: Asset / Authoring /
Scene / GPU).

---

## Q1 — Entity identity: what exists beyond the `entt::entity` handle

**Claim: there is exactly one identity: the document-global UUID carried by
`EntityIdComponent`. There is no subtree-stable identity of any kind.**

- `EntityIdComponent` (`RT2App/src/ECSComponents.h:144-152`): a single
  `rt2::core::UUID id`, populated by the serializer and by duplication from a
  host-reserved known UUID (see Q3). It is a scalar POD.
- `SceneDocument::uuidIndex` is the UUID → `entt::entity` map
  (`SceneDocument.h:35-36`); maintained on create/destroy, consulted via
  `FindByUuid`/`Count`/`NextByOrder` (`SceneDocument.h:91-116`).
- Commands and editor state are UUID-keyed and must never hold `entt::entity`
  (`EditorCommand.h:14-16`).
- Serialized identity (`EntityRecord`, `SceneSerializer.cpp:517-555`): `uuid`,
  `name`, `parentUuid` (nil = root), TRS, `visible`, plus per-component
  flags/payloads. `meshIndex` is transient — "NOT serialized in v2"
  (`:528`); `materialIndex` is serialized.
- Load: `BuildDocumentFromRecords` pass 1 creates entities with
  `EntityIdComponent` and builds `uuidToEntity` (`SceneSerializer.cpp:1087-1123`);
  pass 2 wires `parentUuid` → `Hierarchy` and rebuilds children
  (`:1217-1241`).
- `SubtreeSnapshot`/`SubtreeEntityRecord` (`SubtreeSnapshot.h:43-83`) — the
  only "subtree" structure in the tree — stores **document UUIDs**, not
  subtree-local IDs.
- **Grep for `localId|LocalId|local_id|instanceId|InstanceId` across the
  whole tree: zero matches.** The roadmap's "stable local IDs"
  (`docs/game-engine-development-plan.md:595`) do not exist in code.
- Soft identities that are not identity: names (duplication appends
  `" Copy"`, `SceneManager.cpp:2716-2721`, so names are not unique), and
  `RuntimeCommandSink::FindByName` which picks "the first in UUID order"
  when names collide (`ScriptSystem.cpp:1661-1683`).

**Determined:** UUID + handle is all there is; prefabs need a mechanism for
"instance index within this subtree" (or a decision that document UUIDs
suffice) built from nothing that exists.

---

## Q2 — Every site that stores an entity reference

Search method: (a) `graphify query` orientation (BFS depth 2 around
`SceneDocument`/`EditorSceneState`/`SubtreeSnapshot`/`EntityRecord`); (b) grep
`entt::entity` across `RT2App/src` excluding vendored dirs; (c) grep
UUID-typed fields in every component in `ECSComponents.h`; (d) read the
serializer, snapshot, command, editor-state and runtime-controller structs in
full; (e) grep `follow|Target|activeCamera|cameraUuid|FocusOn`.

### Persisted references (survive save/load)
1. **Hierarchy parent/children** (`ECSComponents.h:62-66`): raw
   `entt::entity` parent + `std::vector<entt::entity> children` — the only
   entity-typed fields in any component. Never serialized directly: converted
   to `parentUuid` in `BuildEntityRecord` (`SceneSerializer.cpp:579-588`),
   rebuilt from `parentUuid` on load (`:1217-1241`), and deliberately excluded
   from `PersistedComponents` because "duplication remaps identity and
   relationships" (`PersistedComponents.h:10-14`).
2. **`ScriptComponent.fieldValues`** — `ScriptFieldValue` has a `Uuid` arm
   (`ScriptFieldValue.h:119-126`; `ScriptFieldType::Uuid` at `:51`). Stored
   per entity via `ScriptComponent` (`ECSComponents.h:158-165`). Serialized
   as strings (`SceneSerializer.cpp:771-792`). **Nothing in the engine ever
   resolves these to entities** — they are opaque strings (exposed to Lua via
   `ToString()` at `ScriptSystem.cpp:894-895`).

### Editor state (not serialized)
3. **Selection** — `EditorSelection::m_Selected` is a `std::vector<UUID>`
   (`EditorSelection.h:35`); not serialized (`EditorSceneState.h:16-17`).
4. **Locks** — `EditorSceneState::m_LockedEntities`: `std::unordered_set<UUID>`
   (`EditorSceneState.h:56`).
5. **Clipboard** — a full `SceneDocument` clone plus `clipboardRoots` UUIDs
   (`EditorSceneState.h:59-62`); guarded by `ClipboardStale` error
   (`SceneManager.cpp:2769-2786`).
6. **Camera bookmarks** — `EditorCameraPose` (position/orientation), no entity
   ref (`EditorSceneState.h:63`).
7. **UI-follow state** — gizmo/icon/camera resolve live handles from UUIDs
   per frame and keep no handles themselves (`EditorTransformGizmo.cpp:122,
   234, 337`; `EditorViewportIcons.cpp:76`; `EditorCameraWorkflow.cpp:152-230`).

### Command/undo state (not serialized)
8. **`ReparentEdit` anchors** — prev/next sibling UUIDs
   (`SubtreeSnapshot.h:85-90, 123-130`).
9. **`SubtreeSnapshot` records** — `parentUuid` per record
   (`SubtreeSnapshot.h:43-83`).
10. **`RuntimeSceneController`** — runtime structural ops queued as UUID
    + payload (`RuntimeSceneController.h:85-98`).
11. **History entries** — UUID-keyed before/after states (see Q6).

### Non-entity references that behave like entity refs
12. **Resource-index references** — `MeshRef::meshIndex` /
    `MaterialOverrideComponent::materialIndex` / `SceneMaterial` texture
    indices are *indices into document-local arrays*, i.e. references that
    are remapped when the target array changes. This is the class of bug
    behind the four glossary boundaries
    (`docs/glossary.md:39-56`) and behind the material-index undo defect
    fixed in `697d3c9`.
13. **Asset references** — `ImportedMeshSourceComponent.model`,
    `ScriptComponent.asset`, `EnvironmentSettings.ref` — these reference
    *assets*, not entities, but `ImportedMeshSourceComponent.model` is the
    closest thing to a "prefab link" in the tree today (see Q4).

**Determined:** the complete list of entity-reference sites is the nine above;
only two serialize (Hierarchy as `parentUuid`, script Uuid fields as opaque
strings), and only Hierarchy is ever remapped on duplication (Q3). **Camera
targets do not exist**: `CameraComponent` (`ECSComponents.h:124-130`) has no
entity field, and grep for `follow|Target|activeCamera|cameraUuid` found
nothing entity-typed; the editor camera merely frames the selection
(`EditorCameraWorkflow.cpp:197-230`).

---

## Q3 — What duplication already does, and what it does not remap

**Claim: hierarchy is remapped; every component payload is copied verbatim,
including UUID-typed script fields. A duplicated subtree whose script
references a sibling points at the ORIGINAL sibling.**

- `CopyAuthoredComponents` (`SceneManager.cpp:76-93`) copies all 11
  `PersistedComponents` (`PersistedComponents.h:20-36`) **verbatim** via
  `emplace<Component>(dst, *component)` — ScriptComponent (with `fieldValues`)
  and MaterialOverrideComponent included. `PersistedComponents.h:10-14`
  documents why `EntityIdComponent` and `Hierarchy` are excluded.
- `DuplicateSubtreesWithUuids` (`SceneManager.cpp:2624-2734`) and
  `PasteSubtreesWithUuids` (`:2736-2879`):
  - Fresh UUIDs assigned positionally from host-reserved known UUIDs
    (`AssignKnownUuid` `:2677, :2819`; reserved at
    `SceneEditorUI.cpp:401-402, 445-446`).
  - Hierarchy remapped after the copy: internal parents → copies
    (`:2696-2713`, `:2839-2858`); external parent → original parent on
    duplicate (`:2705-2707`) or `destinationParent` on paste
    (`:2844-2850`).
  - Rollback of already-created entities on partial failure
    (`:2679-2690`, `:2821-2831`); `" Copy"` suffix on roots
    (`:2716-2721`).
  - **No remap of `fieldValues`, no remap of anything else.** The wiring
    loop touches only Hierarchy.
- `CloneInMemory` (`SceneSerializer.cpp:1768-1790`) clones the whole
  document **preserving UUIDs** (used by Play-mode
  `RuntimeSceneController.cpp:38`, clipboard `EditorSceneState.cpp:52`,
  migration `SceneAssetMigration.cpp:293`). Because identity is preserved,
  all UUID references stay valid — this is why the clipboard/play paths
  never needed remapping, and why no one has noticed the verbatim-copy
  behavior.
- Existing test coverage:
  - `Phase2CHierarchyTests.cpp:176-206` — duplicate: fresh UUIDs, hierarchy
    remap, `meshIndex` copied verbatim (`:202`).
  - `Phase3B1CommandTests.cpp:300-345` — undo/redo restore the same UUIDs.
  - `Phase6BFieldsTests.cpp:1105-1135` — fieldValues survive duplicate
    verbatim (a Color field).
  - `Phase2CHierarchyTests.cpp:225-245` — clipboard stale error.
- **No test pins the behavior of a UUID-typed script field pointing at a
  sibling across duplicate or paste.** I could not determine whether
  verbatim-copying is *intended*; the code simply has no remap, and no test
  contradicts it. A prefab instantiate path that remaps Uuid fields to
  subtree-local references would be the first remap of its kind.

---

## Q4 — What survives a structural change under an existing override

The material-override system is the "prefab in miniature" the roadmap points
at (`docs/game-engine-development-plan.md:592-596`), so its resolution path
matters for the "match survives a structural change" question.

### How the override is actually resolved (re-import path)
- Plan pass: each entity's `ImportedMeshSourceComponent.model.sourceKey` is
  matched against the **rebuilt staged model**'s `keyMap`
  (`SceneAssetResolver.cpp:495-517`; staged from a fresh import at
  `:425-457`). So matching exists and is real — but it is **mesh** matching,
  not material matching.
- Missing mesh key → `Unresolved` diagnostic (`:656-669`); all-unresolved →
  hard failure, document unchanged (`:674-685`); partial → the resolved part
  is committed (`:687-798`).
- Commit step (`:764-783`): the override is applied **unconditionally** when
  the entity resolves — the override's `material` is appended as a new slot
  in `SceneMaterialTable` and `MeshRef::materialIndex` is re-pointed at it.
  Texture indices inside the override are repaired from the **current staged
  material at the resolved slot** (`:768-777`).

### What `sourceMaterialKey` actually is
- The component comment claims it "matches ... a rebuilt source material"
  (`ECSComponents.h:253-256`), but **the resolver never reads it** (grep for
  `sourceMaterialKey` across `RT2App`: only written at `SceneManager.cpp:3677`,
  equality-compared at `:1846` and `EditorPropertyCommands.cpp:91`).
- The written value is `src->model.sourceKey + ":material"`
  (`SceneManager.cpp:3670-3672`) — the *mesh* sourceKey plus a literal
  suffix; the `"gltf:material=<index>"` form the component comment describes
  never occurs. The key is best-effort metadata; the real match is
  sourceKey → mesh → current material slot.

### Structural change answers
- **Material removed or reordered in the source file:** the entity still
  resolves (mesh identity unchanged). The override is re-applied against
  whatever material index the rebuilt file now assigns to that primitive,
  and texture indices are repaired from it (`:768-777`). Result: the
  override **silently follows the slot** — it survives, but it binds to a
  different material with no diagnostic. If the primitive's current index is
  -1, texture repair does nothing and the override's *serialized raw*
  texture indices stand (`SceneSerializer.cpp:111-114, 708`) — stale against
  the rebuilt array.
- **Mesh/primitive removed:** `Unresolved` diagnostic; the entity keeps no
  `MeshRef` on the save path because `meshIndex` is transient
  (`SceneSerializer.cpp:1161-1182`) — it goes invisible until the source
  returns, while the override component survives on the entity (`:1197-1198`).
- **Any material value change:** handled by design — the override is a full
  `SceneMaterial` snapshot, applied on top of the (unrepaired, other-field)
  staged material.

**Determined:** the override system handles material *value* changes
robustly and material *structure* changes silently-wrong. There is no
per-property override granularity anywhere (the override is a whole-value
`SceneMaterial`, `ECSComponents.h:241-261`), and the only property the
resolver reconciles with the live source is texture indices.

---

## Q5 — Asset kinds: what adding a kind touches

- Enum: `AssetKind` Unknown/Model/Texture/Environment/Script
  (`AssetReference.h:29-36`).
- Name codec: `AssetKindName`/`AssetKindFromName` (`SceneSerializer.cpp:196-215`),
  serialized as the `"kind"` tag (`:217-233`).
- **Reader-visible change:** a scene referencing a kind the build does not
  know fails to parse — unknown kind with non-empty path is a hard Parse
  error (`SceneSerializer.cpp:304-309`). Older builds cannot load scenes that
  use a new kind.
- Asset record: `AssetRecord` (`AssetDatabase.h:61-80`) — `assetId`,
  `sourcePath`, fixed-type `ImportSettings` (not applicable to prefabs),
  `identityAuthority`, `observedKinds` (a vector — already generic),
  `dependentEntities`, `dependencies`.
- Identity: sidecar `"<asset>.rt2meta"` (`AssetIdentity.h:16-33`;
  `kSidecarSuffix = ".rt2meta"` `ProjectAssetScanner.cpp:16`);
  `ResolveOrAssign` mints and writes the sidecar on first import
  (`AssetIdentity.cpp:157-174`), called from every import site
  (`SceneManager.cpp:300, 593, 636, 909`; `SceneLoader.cpp:80`;
  `ScriptSystem.cpp:3933`; `TextureAssetPipeline.cpp:498`;
  `SceneAssetMigration.cpp:333`).
- Scanner: an asset is a file with a sidecar (`ProjectAssetScanner.cpp:144-223`).
- Content browser: lists all database records, search filters by name only
  (`WalnutApp.cpp:1484-1506`; `SearchContentBrowserAssets`
  `ContentBrowserOperations.cpp:392-405`); **no kind filter, no per-kind
  icons**; rename/move/delete/reimport dispatch at `:1589+`; reimport is
  hardcoded to `.glb/.gltf/.obj` (`ContentBrowserOperations.cpp:552`).
- Watch policy extension list: `.lua/.glb/.gltf/.obj/.hdr/.exr/.rt2meta`
  (`AssetWatchPolicy.cpp:23-27`).
- Scene schema: `.rt2scene` v4 (`SceneSerializer.h:124`; root `"version"`
  written in `SaveInternal` `SceneSerializer.cpp:1391-1392`).
- **No subtree serializer exists:** `SaveInternal` emits whole documents;
  the only subtree-shaped serialization is the in-memory `SubtreeSnapshot`
  (`SubtreeSnapshot.h`), captured by `CaptureSubtreeSnapshot`
  (`SceneManager.h:246`, `SceneManager.cpp:2364`). A prefab asset file format
  is net-new.

**Determined:** a new kind threads through: enum arm, name codec both ways,
reader acceptance (parse-fail risk), watcher extension list, sidecar
minting on first save, content-browser reimport policy, and — because
`ImportSettings` is a fixed type — either a new settings arm or acceptance
that prefabs carry no import settings. The database/`observedKinds` side is
already generic.

---

## Q6 — Command layer: what a structural command must guarantee

- Contract: `IEditorCommand` (`EditorCommand.h:29-44`) — Execute/Undo/
  Description; UUID-keyed, never `entt::entity` (`:14-16`); graceful Failure
  on missing entity; "Construction is separated from application: a command
  is built with the COMPLETE before/after state up front (never captured
  inside Execute)" (`:19-21`).
- Structural shapes (`EditorStructuralCommands.h/.cpp`):
  - Creation/duplication/paste: construction captures the **post-mutation**
    `SubtreeSnapshot` + created root UUIDs; Execute/Redo =
    `RestoreSubtrees(snapshot)`; Undo = `RemoveSubtreesExact(snapshot)`
    (exact, not compaction — `RemoveSubtreesNoCompact` is the deletion
    counterpart).
  - Deletion: construction captures the **pre-mutation** snapshot.
  - Reparent: stores before/after `ReparentEdit` lists; Undo re-applies the
    before list with `PreserveLocal`.
- Host flows (`SceneEditorUI.cpp:380-458`): deletion captures the snapshot
  **before** Execute; duplication/paste reserve known UUIDs, apply, then
  capture the post-mutation snapshot from the authoritative document
  (`CaptureSubtreeSnapshot`), then construct the command and
  `RecordApplied` — the construction-time capture is what makes
  before/after complete (creation commands legitimately have no "before"
  value: undo re-derives it by exact removal).
- History (`EditorCommandHistory.h:15-44`): Execute/RecordApplied/Undo/Redo;
  document-generation guard clears both stacks; a failed Undo or Redo clears
  both stacks; bounded (64 entries, evicting oldest).
- **The `697d3c9` precedent (material-index undo defect):** the failure mode
  was before/after captured at the call site *after* the mutation, so the two
  snapshots were identical and undo became a no-op. The fix moved capture
  into `SetMaterialIndexState` itself via out-params
  (`SceneManager.cpp:3784-3826`). The structural patterns are already immune
  to that class of bug because "before" is either captured pre-mutation or
  derived by exact removal. A prefab instantiate/override command must
  follow the same rule: complete state at construction, or the
  RecordApplied + authoritative post-mutation snapshot pattern — never
  read-after-mutate.
- Invariant the snapshot mechanism depends on: no compaction while commands
  are in history (`EditorStructuralCommands.h:43-46`; `historyLive` guard
  `WalnutApp.cpp:304-307`; compaction only after history `Clear`) — this is
  what keeps `MeshRef::meshIndex` in snapshots stable
  (`SubtreeSnapshot.h:55-56`). Prefab commands holding snapshots inherit
  the same dependency.

---

## Also report

### Contradictions to the framing
1. **`sourceMaterialKey` does not do what its own component comment claims.**
   The resolver never reads it; it is written once as
   `sourceKey + ":material"` and used only for equality. The "match" that
   actually governs re-import is sourceKey → mesh → current material slot
   (Q4). A prefab spec must not assume the material key is a real key.
2. **Material override is a whole-value snapshot, not per-property.** The
   roadmap's "per-property overrides" have no analogue in
   `MaterialOverrideComponent`; the only reconciliation with the live source
   is texture-index repair.
3. **The mirror invariant is already documented as Phase 8's breaking
   point** (`docs/game-engine-development-plan.md:11739-11748`): an override
   mirrors a *live material slot*; prefab overrides target an *asset*.
   The Phase 8 pre-work verification report (same section) already worked
   through the compaction/history interplay
   (`:11848-11858`; `InstallMaterialOverride` `SceneManager.cpp:4001-4012`;
   compaction sweep `SceneManager.cpp:4179-4190`).

### Anticipation code (prefab-shaped things already in the tree)
- `ImportedMeshSourceComponent` + durable `MaterialOverrideComponent` — the
  source-link + override pattern prefabs would generalize
  (`ECSComponents.h:221-261`).
- `SubtreeSnapshot` — a pre-order snapshot with `parentUuid` and full
  payloads: the shape a prefab asset file would need; in-memory only.
- Script sourceKey `"lua:asset=<path>"` — comment says "ready for the
  future asset-UUID form" (`ECSComponents.h:286-287`).
- `PersistedComponents` canonical list + `static_assert` coverage
  (`PersistedComponents.h:10-14`, `SceneSerializer.cpp:38`) — "add a
  component here when it becomes authored; serializer and duplication
  coverage tests then fail together".
- `ClipboardStale` resource validation on paste (`SceneManager.cpp:2771-2786`)
  — precedent for validating instance-time resource references.
- The Phase 8 pre-work spec and verification report already committed in the
  plan (`docs/game-engine-development-plan.md:11667-11998`), including the
  "Carried into Phase 8" list (`:11667-11686`).

### Fragility intersections
- **Transient resource indices must never be serialized by prefab files:**
  `MeshRef::meshIndex` and `MaterialOverrideComponent::materialIndex` are
  transient by design (`SceneSerializer.cpp:528`;
  `ECSComponents.h:258-260`); override *texture* indices are serialized raw
  (`SceneSerializer.cpp:111-114`) and repaired only by the resolver from a
  currently-staged material (`SceneAssetResolver.cpp:768-777`) — a prefab
  instance whose variant is not currently materialized has nothing to repair
  from.
- **Names are not unique** (duplication appends `" Copy"`); `FindByName`
  silently picks the first in UUID order (`ScriptSystem.cpp:1661-1683`).
  Prefab instance naming will collide with this lookup.
- **Hierarchy dual representation** (parent pointer + children vector) must
  stay consistent; instantiation should reuse the same reconciliation as
  load (`SceneHierarchy.cpp` `RebuildChildren`) rather than hand-wiring.
- **History eviction** destroys held snapshots at 64 entries
  (`EditorCommandHistory.h:41-44`); prefab command snapshots share the
  no-compaction invariant with all other structural snapshots.
- **Resolve-order coupling:** entity creation and override application are
  two phases in the resolver (`SceneAssetResolver.cpp:674-798`); a prefab
  instance that mixes pre-existing and new entities crosses the same
  partial-failure boundary.

### Could not determine (needs a spec decision)
- Whether UUID-typed script fields are *intended* as entity references: the
  field/DSL layer treats them as opaque strings; nothing resolves them. I
  could not determine intent from the code — the spec must decide what an
  instance-time remap means for them.
- Whether verbatim field copy across duplication is intended behavior (Q3):
  no code contradicts it, no test pins it.
- The behavior of a duplicated subtree whose script field references an
  entity *outside* the subtree: same as inside — no remap, no test.
