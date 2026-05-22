-- class_spec.lua — farm_soil class spec.
--
-- Per decision #31 every robot class ships a class_spec.lua. The shared KB0
-- consumes it: `capabilities` go in the registration payload, `app_kbs` are
-- spawned once the namespace is up, and `on_namespace_up` adds any
-- class-specific topology.

local M = {}

M.capabilities = {
    "heartbeat",
    "soil_moisture",
}

-- Application KBs spawned after the robot reaches operating.
M.app_kbs = { "moisture" }

-- TTN v3 storage API config for the moisture skill. The bearer token is NOT
-- here — it is a secret, read from the TTN_BEARER_TOKEN env var (run.sh
-- sources secrets/ttn.env).
M.ttn = {
    url_base       = "https://nam1.cloud.thethings.network/api/v3/as/applications/",
    app_name       = "seeedec",
    url_after      = "/packages/storage/uplink_message?",
    lookback_hours = 24,
    limit          = 200,
}

-- device_id -> location (the sensing-point sub-namespace). Adding a sensor is
-- one line here. The locations below are placeholders — set the real
-- plot/zone names per deployment. An unmapped device publishes under
-- <device>/unknown rather than being dropped.
M.device_locations = {
    lacima1c  = "zone1",
    lacima1d  = "zone2",
    lacamia1b = "zone3",
}

-- Class hook, run after KB0 publishes the core namespace leaves. The
-- farm_soil namespace is data-driven: per-device <device>/<location>
-- subtrees appear as the moisture KB publishes them, so there is no static
-- class topology to declare here.
function M.on_namespace_up(session, identity, bb)
    io.stderr:write(string.format(
        "FARM_SOIL [%s]: on_namespace_up — device subtrees are published dynamically\n",
        identity.namespace))
end

return M
