-- class_spec.lua — generic fake_robot class spec
--
-- Per decision #31, every robot class ships a class_spec.lua returning
--   { capabilities    = [...],
--     app_kbs         = [...],
--     on_namespace_up = function(session, identity, bb) ... end }
-- The shared connection KB (KB0) consumes this after declaring the core
-- publishers/subscribers, to add class-specific topology.

local M = {}

M.capabilities = {
    "heartbeat",
    "report_state",
}

-- Application KBs to spawn once the robot reaches operating. KB0's
-- SPAWN_APP_KBS one-shot iterates this list and calls ct_runtime.add_test for
-- each name; each name must be a KB present in the loaded IR (built into
-- connection.json by chains/connection.lua's CLI driver). On a connection
-- loss KB0 sweeps every KB except itself, so these are torn down and
-- re-spawned automatically.
M.app_kbs = {
    "fake_counter",
}

function M.on_namespace_up(session, identity, bb)
    io.stderr:write(string.format(
        "FAKE_ROBOT [%s]: on_namespace_up stub (no class-specific topics yet)\n",
        identity.namespace))
end

return M
