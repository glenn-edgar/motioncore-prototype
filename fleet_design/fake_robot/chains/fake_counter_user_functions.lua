-- chains/fake_counter_user_functions.lua — ct_* user fns for the fake_counter KB.
--
-- ct_* (dict runtime) signatures — see connection_user_functions.lua header.
-- main.lua merges this registry with connection_user_functions.registry when
-- it calls fn_registry.register_functions (that fn is variadic).
--
-- Context on the blackboard (attached by main.lua, shared across all KBs):
--   bb._identity   robot identity (namespace, ...)
--   bb._pubsub     zenoh pub/sub session

local cjson = require("cjson")
local clock = require("clock")

local M = { main = {}, one_shot = {}, boolean = {} }

-- ---------------------------------------------------------------------------
-- One-shots
-- ---------------------------------------------------------------------------

-- Publish an incrementing counter value on <namespace>/counter.
-- The counter lives on the blackboard under a KB-namespaced key — the
-- blackboard is engine-global (shared with KB0), so the field is prefixed to
-- avoid collisions. It persists across a sweep + respawn, which is fine:
-- spawn/kill is observed via KB0's "spawn app KB" / "killed N app KB(s)" logs,
-- not via the counter resetting.
M.one_shot.PUBLISH_COUNTER = function(handle, node)
    local bb = handle.blackboard
    local id, ps = bb._identity, bb._pubsub

    local n = (bb._fake_counter_value or 0) + 1
    bb._fake_counter_value = n

    local topic = id.namespace .. "/counter"
    local ok, err = pcall(function()
        ps:publish(topic, cjson.encode({
            value = n,
            ts    = clock.wall_now().epoch_s,
        }))
    end)
    if ok then
        io.stderr:write(string.format(
            "fake_counter [%s]: published %s = %d\n", id.namespace, topic, n))
    else
        io.stderr:write(string.format(
            "fake_counter [%s]: publish failed: %s\n", id.namespace, tostring(err)))
    end
end

-- ---------------------------------------------------------------------------

M.registry = {
    main     = M.main,
    one_shot = M.one_shot,
    boolean  = M.boolean,
}

return M
