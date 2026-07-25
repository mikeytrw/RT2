# Lua scripting

The scripting subsystem: how a `.lua` file is bound to an entity, what it
can and cannot reach, and the guarantees that keep one bad script from
taking down the engine.

Status: **Phase 6A complete** (embedding, lifecycle, sandbox), **6B
complete** (app wiring, reflection, typed storage, deterministic
reconciliation, v3 persistence, commands, inspector), **6C complete**
(hot reload, file watcher, input/light/camera/material bindings, timers,
headless scenario runner). See the Phase 6 sections of
`game-engine-development-plan.md` for the roadmap.

Related: [game-loop.md](game-loop.md) for exactly when callbacks fire,
[scene-management.md](scene-management.md) for the document model.

---

## The pieces

| File | Role |
|---|---|
| `ScriptSystem.h/.cpp` | Owns the single `sol::state` and the per-entity environment map. Implements `IRuntimeLifecycleObserver` + `IRuntimeScriptDispatch`. |
| `ScriptSandbox.h` | **The single definition of what a script may not reach.** Used by both sandboxes. |
| `ScriptFieldRegistry.h/.cpp` | Parses `rt2.fields` declarations. Own Lua state, independent of Play. |
| `ScriptFieldValue.h` | Field type/variant, typed stored entries, descriptors and compatibility helpers. |
| `ScriptFieldReconcile.h/.cpp` | Pure, Lua-free reconciliation of authored values against declarations. |
| `ScriptFieldResolver.h/.cpp` | UUID-ordered document pass; resolves paths, parses declarations and applies reconciliation. |
| `ScriptAssetPath.h/.cpp` | Shared scene-relative script path resolution for editor and runtime. |
| `ScriptFileWatchPolicy.h` | What a changed `.lua` on disk means for the current run state. Pure. |
| `ScriptScenarioCompare.h` | Headless-runner verdict logic and the `ScenarioExit` code table. Pure. |
| `IRuntimeCommandSink.h` | The only channel through which a script mutates the world. |
| `ECSComponents.h` | `ScriptComponent` — the persisted asset reference + field values. |

The two `Policy`/`Compare` headers exist so the decisions they hold are
reachable from `RT2Tests`. Every Phase 6C defect found before W8 was found
by *reading* code, not by a failing test; these seams are the response.

`ScriptComponent` is authored data (a script path plus field values). No
Lua state lives on the document; environments are built at Play and torn
down at Stop.

---

## Writing a script

```lua
rt2.fields = {
  speed = rt2.field.float(5.0),
  tint  = rt2.field.color(1, 1, 1),
}

function on_create(entity, world) end
function on_fixed_update(entity, dt, input, world) end
function on_update(entity, dt, input, world)
    local p = entity:get_position()
    entity:set_position({ p[1] + self.speed * dt, p[2], p[3] })
end
function on_destroy(entity) end
```

All four callbacks are optional. The 4-argument signature is fixed from 6A.

### Three ways to write a script that silently does nothing

Both of these are valid Lua, raise no error, and produce no diagnostic.
They are the first thing to check when a script "isn't running".

**1. Call bindings with a colon, not a dot.** Every `entity`, `world`,
`input` and `timer` method takes an ignored `self` slot as its first
parameter. `entity:set_position(p)` passes `p` as the real argument;
`entity.set_position(p)` puts `p` in the `self` slot and leaves the real
parameter `nil`, so validation rejects it and the call returns `false`
that nobody checked.

```lua
entity:set_position({ 1, 0, 0 })   -- correct
entity.set_position({ 1, 0, 0 })   -- no-op, returns false
```

**2. Declare fields with `rt2.field.*` constructors, not plain tables.**
A plain table is not recognised as a declaration and is skipped without
comment, so the field never reaches the inspector and never gets its
default — `self.speed` is simply `nil`.

```lua
rt2.fields = { speed = rt2.field.float(1.0) }              -- correct
rt2.fields = { speed = { type = "float", default = 1.0 } } -- declares nothing
```

The second form is especially deceptive on an existing entity: if the
value was already authored and saved, `self.speed` still reads correctly
from `ScriptComponent::fieldValues`, and the broken declaration only
surfaces when someone clears the value or looks for the field in the
inspector. The shipped `assets/script-scenario.lua` had this bug.

**3. Every setter returns a bool, and ignoring it is the general case of
both traps above.** `set_position`, `set_name`, `set_visible`,
`set_light`, `set_camera` and `set_material_index` return `false` rather
than raising when the write is refused — a missing component, an
out-of-range material index, a malformed vec3 table, or a runtime that is
not currently mutable. Ignoring the return is ordinary Lua style, so a
refused write looks exactly like a successful one and the script carries
on with stale state. When a binding "doesn't work", check the return
before checking anything else:

```lua
if not entity:set_material_index(i) then
    log.warn("material index " .. i .. " rejected")
end
```

### Available globals

| Global | Contents |
|---|---|
| `entity` | `get_uuid`, `get_name`, `set_name`, `get_position`, `set_position`, `get_visible`, `set_visible`, `get_light`, `set_light`, `get_camera`, `set_camera`, `set_material_index` |
| `world` | `spawn`, `destroy`, `find_by_name`, `find_by_uuid` |
| `self` | This entity's field values, from `ScriptComponent::fieldValues` |
| `log` | `info`, `warn`, `error` |
| `input` | `is_down`, `is_pressed`, `is_released`, `get_axis`, `get_mouse_delta`, `get_scroll_delta` |
| `timer` | `after`, `every`, `cancel` |
| `rt2` | `fields`, `field.bool/int/float/string/uuid/vec3/color`, `reload`, `previous_state` |

`set_material_index` has no getter — read the index through the editor, not
the script API.

Plus the safe standard library: `base` (minus the denied names below),
`math`, `string`, `table`, `utf8`.

`world:spawn{ name = "Child", script = "child.lua" }` creates a **scripted**
entity — the spawn resolves at the next safe point and the child's
`on_create` fires that same frame.

Writes to `self` are runtime-only and are not persisted.

---

## The sandbox

Scripts are user content, and the field parser runs against arbitrary
`.lua` the moment an entity is selected in the editor. Everything below
exists because the naive version was broken in a way that was not
obvious.

**Denied**, via a stub that throws (see `ScriptSandbox.h`):

`dofile`, `loadfile`, `require`, `load`, `loadstring`, `_G`,
`getmetatable`, `rawset`, `rawget`, `rawequal`, `rawlen`,
`collectgarbage`.

**Not opened at all:** `io`, `os`, `debug`, `package`, `coroutine`.

**Allowed deliberately:** `setmetatable` (the standard class idiom, and
not an escape once the above are denied — it writes a metatable rather
than reading one, and a script has no way left to obtain a privileged
table to install); `string.dump` (produces bytecode, but with no `io`, no
`os`, no network and `load` denied there is nowhere for it to go).

Each environment also receives its **own shallow copies** of `math`,
`string`, `table` and `utf8`.

### Three escapes that had to be closed

Recorded because each one looked fine and was not:

1. **`sol::nil` does not shadow.** Assigning nil to a table key *removes*
   it, so the environment's `__index` fallback to globals kicks back in.
   `dofile`, `loadfile` and `load` — all defined by `sol::lib::base` —
   stayed fully callable from every script, and `loadfile`/`load` do not
   even raise, so the escape was silent. A throwing stub stores a real
   value, which actually shadows.
2. **`getmetatable(_ENV).__index` IS the globals table.** An environment
   built on a globals fallback exposes its own metatable, so one line
   retrieves the real functions regardless of any name-level shadow.
   `getmetatable("")` does the same for the shared string metatable.
3. **Library tables are shared mutable state.** `math`, `string` etc.
   resolve through the fallback to tables shared by every script in the
   state, so `math.floor = function() return 999 end` in one entity's
   script poisoned every other script — and every editor-time field
   parse, because the registry state outlives any single parse.

**The lesson for anyone extending this:** a globals-fallback environment
is an *allowlist* problem wearing a denylist's clothes. Any new binding,
library or capability must be checked for a path back to the globals
table, and every escape found needs a regression test
(`Phase6BFieldsTests.cpp` has them).

---

## Robustness guarantees

**Errors quarantine, they don't crash.** Every Lua entry point runs
through `sol::protected_function`. On any error the instance moves to
`Quarantined` and receives no further callbacks this Play session. The
error is logged with script path, entity UUID, callback name and a stack
trace (sol's default handler supplies the trace via `luaL_traceback`,
which needs no `debug` library).

```
NeverCreated --OnCreate--> Live --error--> Quarantined
                             |                 |
                             |                 +--(reload, 6C)--> Live
                             +--OnDestroy--> Destroyed
```

`on_destroy` is **not** called on a quarantined instance at Stop — it
never had a clean lifecycle.

**Hangs are caught too.** This is the part protected calls cannot do: a
callback containing `while true do end` never returns, so no result is
produced and neither `sol::protected_function` nor `lua_atpanic` can
intervene — and quarantine itself requires the call to return. Every
runtime Lua entry point (chunk load plus all four callbacks) therefore
runs under a `ScriptInstructionBudget`, whose `LUA_MASKCOUNT` hook
regains control and routes an exhausted budget through the ordinary error
path into quarantine. Budgets are generous backstops
(`kScriptLoadInstructionBudget`, `kScriptCallbackInstructionBudget`), not
a performance policy.

**Mutation is channelled.** Scripts never see the `entt` registry or a
mutable `SceneDocument`. Everything goes through `IRuntimeCommandSink`;
spawns and destroys are queued and applied at the frame's safe point, so
the registry is never mutated mid-iteration.

**Isolation is per-entity.** Two entities sharing one script source get
separate environments and separate `self` tables. (This is also why `_G`
is denied — it was a channel between them.)

---

## Hot reload

Saving a `.lua` file while the editor is open re-binds the running
instances without leaving Play. `efsw` watches the scene directory plus
every directory containing a bound script; changes are debounced 100 ms
and drained in `OnUIRender`.

What a change means depends on the run state — `DecideScriptFileChange`
in `ScriptFileWatchPolicy.h` is the whole decision:

| Run state | Runtime | Inspector cache |
|---|---|---|
| Playing | reload now | invalidated |
| Paused | queue; drains on the first Playing frame after Resume | invalidated |
| Edit (stopped) | nothing to reload | invalidated |

The two effects are independent, not alternatives. `ScriptSystem` and the
editor hold **separate** `ScriptFieldRegistry` instances, so a runtime
reload does not refresh what the inspector is displaying — the cache is
invalidated in every state.

`Step` deliberately does **not** drain the queue. Stepping should advance
the world as authored, not swap the code mid-freeze.

**A reload builds into a scratch environment and swaps only on success.**
Two traps this avoids:

1. All four callbacks are reset before rebinding. A reload that *removes*
   `on_update` must unbind it; otherwise the stale callable keeps firing
   against a `self` that no longer exists.
2. If the scratch build fails, the running instance is left untouched.
   The quarantine logged in that path refers to the discarded scratch.

**A syntax error does not kill the running instance.** A parse failure
keeps every instance in its current state — the running code is still
valid and the author is mid-keystroke. Only a *successful* parse can
replace anything.

Across a reload:

- Field values reconcile under the ordinary 6B rules: authored values
  survive, added fields take their declared default, removed fields drop.
- The outgoing `self` is copied to `rt2.previous_state` so a script can
  carry state across the swap.
- Timers owned by the instance are cancelled before the swap.
- A `Quarantined` instance returns to `Live`, which is the case that
  matters most — reloading is how you fix the script that just died.
- An instance that never ran `on_create` fires it after the swap.

`rt2:reload()` from inside a callback **queues**; it does not reload
synchronously. Tearing down the environment whose frame is still on the
Lua stack is exactly the re-entrancy the queue exists to prevent.

---

## Timers

```lua
local h = timer:every(0.5, function() log.info("tick") end)
timer:after(2.0, function() timer:cancel(h) end)
```

Timers are C++ state in `ScriptSystem`, not Lua coroutines. `after` fires
once; `every` repeats; `cancel` takes the handle either returns.

- **Max one fire per timer per frame.** `every(0.05)` under a 0.2 s frame
  fires once, not four times. `remaining += interval` preserves the
  long-run average rate without bunching.
- Timers created *inside* a callback do not fire until the next frame.
- Callbacks are protected and instruction-budgeted like any other entry
  point; an error quarantines the owning instance.
- Timers are cancelled on Stop, on entity destruction, and on reload, and
  do not survive into the next Play session.

Driving `Update()` with a fixed `dt` is what makes timer behaviour
reproducible — which is what the headless runner does.

---

## Headless scenario runner

`RT2SliceRunner --script-scenario <file.json>` loads a scene, plays it for
N fixed steps with no editor, GPU or file watcher, and compares the
resulting transforms against expectations. This is the scripting
regression gate; `run_script_test.ps1` wraps it.

```json
{
  "scenePath":   "script-scenario.rt2scene",
  "frames":      60,
  "uuidSeed":    42,
  "forbidSpawn": true,
  "expectedTransforms": {
    "<uuid>": { "position": [1,0,0], "rotation": [0,0,0,1], "scale": [1,1,1] }
  }
}
```

`scenePath` resolves relative to the scenario file. Rotations are
`[x,y,z,w]`. Each of `position`/`rotation`/`scale` is independently
optional.

A malformed *value* is skipped without comment, in keeping with the theme
of this document: an array shorter than 3 (or shorter than 4 for
`rotation`) is ignored rather than rejected, so the expectation silently
stops being checked. Top-level keys of the wrong type do warn on stderr
and fall back to their default.

**Exit codes** are `ScenarioExit` in `ScriptScenarioCompare.h` — that enum
is the contract, and this table is a copy:

| Code | Meaning |
|---|---|
| 0 | pass |
| 1 | scenario JSON missing, unreadable, or malformed |
| 2 | scene path unresolvable, or scene load failed |
| 3 | `Play` returned false |
| 4 | `--out` path could not be opened |
| 5 | expectation failed — transform mismatch, missing entity, or spawn violation |
| 6 | script error: an instance quarantined, or none survived the run |

Code 6 outranks 5: if the script died, the transforms it failed to write
are a symptom, not the diagnosis. A run where every instance vanished
while entities remain is also a script error — that is the case where a
script never loaded at all and every transform sits at its authored
value, which would otherwise report a clean pass.

Comparison is driven by the **expectation list**, not the entity list: an
expectation naming a UUID absent from the run is a failure on the
pseudo-field `"entity"`. Iterating entities instead would silently pass a
scenario whose subject the script destroyed.

Tolerances are separate and not interchangeable. `kPositionEpsilon` is a
Euclidean distance in world units; `kRotationEpsilon` is a deviation in
`|dot|` between unit quaternions, where 1e-4 admits roughly 1.6°.
Rotation is compared via `|dot|` to absorb quaternion double-cover.

---

## Field declarations and reflection

`ScriptFieldRegistry` parses a script's `rt2.fields` block into
`ScriptFieldDescriptor`s. It is deliberately **not** part of
`ScriptSystem`: the inspector must show declared fields with the editor
*stopped*, when the Play-session instance map is empty. So the registry
owns its own Lua state and survives Play/Stop.

- Declarations are **sorted by name**. Lua table iteration is unordered
  and would otherwise reshuffle the inspector between frames. (Cost:
  renaming a field moves it in the inspector.)
- The cache key is `(mtime, size, FNV-1a of source)`. Timestamp
  granularity alone cannot distinguish a same-length in-place edit within
  one clock tick.
- Parsing is instruction-bounded, so a loop at file scope in a
  half-written script cannot hang the editor.
- `alias` names the **old** field: `vel = rt2.field.float(1, {alias="speed"})`
  means "vel used to be called speed". One hop only.

Two contracts that matter to anything consuming declarations:

1. **A failed parse returns the last known-good descriptors** with
   `parsed = false`, and does not stamp mtime/size (so fixing the error
   recovers on the next query). Callers must **skip reconciliation** when
   `parsed` is false — reconciling against zero declarations would treat
   every field as removed. This is why `GetDeclaredFields` returns the
   whole `Result` and not a descriptor vector.
2. **A malformed declaration structure is a parse failure**, not an empty
   set. A replaced `rt2` table or `rt2.fields = "unfinished"` mid-edit
   must not read as "the author deleted every field".

Type compatibility is one rule, in `ScriptFieldTypesCompatible`: two
types are compatible iff they share a `ScriptFieldValue` variant arm.
So `vec3` → `color` preserves the value (same arm, different widget)
while `float` → `vec3` does not.

### Typed storage and reconciliation

`ScriptComponent::fieldValues` is a `ScriptFieldMap`. Each entry stores
both the declared `ScriptFieldType` and its variant payload. Keeping the
tag is intentional: `vec3` and `color` share a `glm::vec3` payload but
must retain different editor and serialization identities.

Persistable field names are non-empty valid UTF-8 strings. String defaults and
authored string values must also be valid UTF-8; reflection or authoring rejects
invalid Lua byte strings before they can reach JSON serialization.

`ReconcileScriptFields` is a pure CPU function. It sorts declarations and
removed names before producing diagnostics, so results never depend on
Lua-table or `unordered_map` iteration order. Its rules are:

- compatible same-name values survive and adopt the current type tag;
- new fields receive defaults, and removed fields are dropped;
- `alias = "old"` migrates once when the new name is absent;
- an existing new-name value wins; competing aliases default and emit one
  ambiguity diagnostic;
- incompatible types or malformed tag/payload pairs reset to defaults.

`ScriptFieldResolver::ResolveDocument` applies those rules to script-bearing
entities in UUID order. Script paths use the same scene-relative resolver as
Play. A parse failure leaves the authored map unchanged and emits
`ParseFailed`; it is never interpreted as an empty declaration set. The
resolver deliberately does not change document dirty state. The load host
classifies the resulting diagnostics: any repair marks the adopted document
dirty, while destructive loss also requires acknowledgement before Save and
suppresses recovery autosnapshots.

Schema-v3 normal open and recovery are production callers. Both deserialize
typed entries, resolve the script against the document's actual logical source
path, reconcile declarations in a worker-local registry, and only then adopt
the document. Play therefore receives the reconciled typed map. Its per-entity
runtime sandbox also installs inert `rt2.field.*` constructors, allowing the
declaration-bearing source to execute while reflection remains the authority
for parsed defaults and types.

A clean parse with no `rt2.fields` declaration is intentionally different
from a parse failure: it declares zero fields, so reconciliation removes all
previously authored values. Commenting out a valid declaration block and then
running the W3 load/save path is therefore treated as deliberate
field removal. Syntax errors and structurally malformed declarations remain
non-destructive.

---

## Testing

Scripting is CPU-only — no Vulkan, ImGui or Walnut — so it links into
`RT2Tests` and runs in CI-style batch:

- `Phase6ALifecycleTests.cpp` — lifecycle, quarantine, isolation, spawn
  and destroy ordering, pause/step, instruction budgets.
- `Phase6BFieldsTests.cpp` and `SceneSerializerTests.cpp` — the `SceneManager`
  script API, field DSL, sandbox/cache behaviour, reconciliation, resolver
  ordering, parse-failure preservation, clone/duplication, v3 typed codecs,
  deterministic output, malformed-field isolation, and Save As rebasing.
- `Phase6ALifecycleTests.cpp` also proves an authored typed value reaches
  runtime `self` and can drive an entity during Play.
- `Phase6CScriptingTests.cpp` — reload (parse-failure survival,
  un-quarantine, callback add/remove, field preserve/default, timer
  cancellation, paused-queue-drains-on-resume, path canonicalisation),
  bindings (input via a mock `IInputService`, light get/set overlay,
  `set_material_index` bounds, absent-component guards, the
  `IsRuntimeMutable` gate), timers (cancel, self-cancel, Stop teardown),
  and the pure seams in `ScriptScenarioCompare.h` /
  `ScriptFileWatchPolicy.h`.

`RT2Tests.exe` resolves some fixtures by relative path — run it from the
repo root or a handful of unrelated cases fail on missing assets.

The Release suite is **green**: any failure is a real regression. Debug is
also green (555/555 as of 2026-07-25); the 8 prior Debug-only failures in
OBJ-import fixture generation were fixed (move-only fixture structs + loud
missing-file logging) — see "Test baseline" in the development plan.

Two things tests cannot cover:

- The editor UI path, which needs the inspector.
- The file watcher's shutdown ordering. `ScriptFileWatchListener` must be
  declared *after* `efsw::FileWatcher` so it outlives the watcher thread
  that calls into it; proving that needs a live watcher thread the tests
  deliberately don't link. It is guarded by a comment at the declaration,
  not by a test.

## Known gaps

- `LuaPanic` logs and calls `std::terminate()`. A panic is fatal by
  design: it does not quarantine. Throwing instead would cross C frames
  reached via `longjmp`, which is undefined on MSVC, and *returning* is
  not an alternative — Lua 5.4 then calls `abort()`. Every Lua entry point
  is protected, so a panic is unreachable except under allocation failure
  during unprotected setup. Compiling Lua as C++ (which switches
  `LUAI_THROW` to a real `throw`) would make throwing safe, and is
  deferred.
- `set_light` and `set_camera` are **mutate-only**: they fail on an entity
  that lacks the component rather than adding one. Fabricating a second
  `CameraComponent` would leave the scene in a state the renderer does not
  define.
- `set_light` writes `LightComponent`, but the GPU light list is built
  from emissive triangles, so the write may have no visual effect. The two
  paths are not yet bridged.
- No `get_material_index`.
- The watcher only watches directories known at Play time. A script added
  to a directory outside that set is not picked up until the watch list is
  rebuilt.
