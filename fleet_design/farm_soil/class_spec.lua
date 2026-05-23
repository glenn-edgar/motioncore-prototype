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
    "et_reference",       -- daily ASCE ETo via the CIMIS Web API
}

-- Application KBs spawned after the robot reaches operating.
-- cimis_station/cimis_spatial are two instances of the same skill module
-- (chains/cimis.lua) — one per CIMIS provider, each with its own retry loop.
M.app_kbs = { "moisture", "cimis_station", "cimis_spatial" }

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

-- CIMIS Web API config for the cimis_station / cimis_spatial KBs. The appKey
-- is NOT here — it is a secret, read from the CIMIS_APP_KEY env var (run.sh
-- sources secrets/ttn.env). Spatial targets are zip codes ONLY (coordinate
-- forms are blocked by et.water.ca.gov's WAF, verified live 2026-05-22).
--
-- Daily-gate semantics (per KB): each retry_s seconds, run the gate.
--   * pre-window  (Pacific hour < window_start_h) -> idle, ok.
--   * in-window   (>= window_start_h, gap exists) -> fetch the last
--                 lookback_days through yesterday, publish every newly-
--                 finalized day in order, advance last_recorded_date.
--   * up-to-date  (last_recorded_date == yesterday) -> idle, ok.
--
-- window_start_h is 09:00 Pacific because CIMIS posts today's row earlier
-- in the day as a provisional/partial value the filter cannot reliably
-- reject. After 09:00 the Qc=="A" station flag and the spatial-today-
-- 0.0-with-blank-Qc trap are both stable. There is NO post-window cutoff:
-- the robot keeps retrying past 15:00 / overnight until the gap closes.
-- Publishes go to two leaves per source:
--   * <namespace>/cimis/<source>/latest  — status (last-write-wins)
--   * <namespace>/cimis/<source>/sample  — stream (one per finalized day)
M.cimis = {
    api_base       = "https://et.water.ca.gov/api/data",
    data_items     = "day-asce-eto",
    window_start_h = 9,            -- inclusive (Pacific civil)
    lookback_days  = 7,            -- multi-day fetch window for gap-self-heal
    retry_s        = 900,          -- 15 minutes between attempts
    sources = {
        -- station 237 = Temecula East II (closest to the Murrieta site).
        station = { target_kind = "station", target = "237"   },
        -- 92562 = Murrieta CA (21005 Paseo Montañez).
        spatial = { target_kind = "spatial", target = "92562" },
    },
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
