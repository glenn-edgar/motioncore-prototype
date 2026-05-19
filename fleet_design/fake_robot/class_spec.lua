-- class_spec.lua — generic fake_robot class spec
--
-- Per decision #31, every robot class ships a class_spec.lua returning
--   { capabilities = [...],
--     on_namespace_up = function(session, identity, bb) ... end }
-- The shared connection KB consumes this after declaring the core
-- publishers/subscribers, to add class-specific topology.

local M = {}

M.capabilities = {
    "heartbeat",
    "report_state",
}

function M.on_namespace_up(session, identity, bb)
    io.stderr:write(string.format(
        "FAKE_ROBOT [%s]: on_namespace_up stub (no class-specific topics yet)\n",
        identity.namespace))
end

return M
