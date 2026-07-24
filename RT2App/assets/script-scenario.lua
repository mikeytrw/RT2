-- script-scenario.lua
-- Phase 6C/W7 headless scenario script.
-- Moves the entity 1 unit/second along +X via on_fixed_update.
-- After 60 frames at kFixedDt (1/60s), position.x = 1.0.

-- Field declarations are rt2.field.<type>(default) CONSTRUCTOR CALLS.
-- A plain table such as { type = "float", default = 1.0 } parses without
-- error but is silently skipped as "not an rt2.field.* declaration"
-- (ScriptFieldRegistry.cpp), leaving the field undeclared and invisible to
-- the inspector.
rt2.fields = {
    speed = rt2.field.float(1.0)
}

local accumulated = 0.0

function on_create()
    accumulated = 0.0
end

function on_fixed_update(entity, dt, input, world)
    accumulated = accumulated + dt
    local pos = entity:get_position()
    pos[1] = accumulated * self.speed
    entity:set_position(pos)
end

function on_update(entity, dt, input, world)
    -- No-op; all motion is in on_fixed_update for determinism.
end