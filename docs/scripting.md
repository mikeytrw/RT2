# Lua scripting

The scripting subsystem: how a `.lua` file is bound to an entity, what it
can and cannot reach, and the guarantees that keep one bad script from
taking down the engine.

Status: **Phase 6A complete** (embedding, lifecycle, sandbox), **6B
W0+W1 complete** (app wiring, field reflection). Field persistence,
inspector UI and hot reload are not implemented — see the Phase 6
sections of `game-engine-development-plan.md` for the roadmap.

Related: [game-loop.md](game-loop.md) for exactly when callbacks fire,
[scene-management.md](scene-management.md) for the document model.

---

## The pieces

| File | Role |
|---|---|
| `ScriptSystem.h/.cpp` | Owns the single `sol::state` and the per-entity environment map. Implements `IRuntimeLifecycleObserver` + `IRuntimeScriptDispatch`. |
| `ScriptSandbox.h` | **The single definition of what a script may not reach.** Used by both sandboxes. |
| `ScriptFieldRegistry.h/.cpp` | Parses `rt2.fields` declarations. Own Lua state, independent of Play. |
| `ScriptFieldValue.h` | `ScriptFieldValue` variant, `ScriptFieldType`, `ScriptFieldDescriptor`, compatibility helpers. |
| `IRuntimeCommandSink.h` | The only channel through which a script mutates the world. |
| `ECSComponents.h` | `ScriptComponent` — the persisted asset reference + field values. |

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

All four callbacks are optional. The 4-argument signature is fixed from
6A so scripts written today keep working when `input` gains methods.

### Available globals

| Global | Contents |
|---|---|
| `entity` | `get_uuid`, `get_name`, `set_name`, `get_position`, `set_position`, `get_visible`, `set_visible` |
| `world` | `spawn`, `destroy`, `find_by_name`, `find_by_uuid` |
| `self` | This entity's field values, from `ScriptComponent::fieldValues` |
| `log` | `info`, `warn`, `error` |
| `input` | Present but **methodless** until 6C (so scripts referencing it don't error) |
| `rt2` | `fields`, `field.bool/int/float/string/uuid/vec3/color` |

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

---

## Testing

Scripting is CPU-only — no Vulkan, ImGui or Walnut — so it links into
`RT2Tests` and runs in CI-style batch:

- `Phase6ALifecycleTests.cpp` — lifecycle, quarantine, isolation, spawn
  and destroy ordering, pause/step, instruction budgets.
- `Phase6BFieldsTests.cpp` — the `SceneManager` script API, the field DSL,
  sandbox denials, cache behaviour.

The one thing tests cannot cover is the editor UI path, which needs the
inspector (Phase 6B W5).

## Known gaps

- `LuaPanic` throws a C++ exception from a handler invoked by C-compiled
  Lua. Every entry point is protected so it should be unreachable, but if
  it fired the throw would cross C frames reached via `longjmp`, which is
  undefined on MSVC. The sound fix is compiling Lua as C++. Note that
  *returning* from the handler is not an alternative — Lua 5.4 then calls
  `abort()`.
- `input` has no methods until 6C.
- No hot reload: `ReloadScript(path)` is a declared stub.
