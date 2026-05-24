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
-- digest publishes a 24h push-notification body on fleet/notify/digest/daily
-- (notification_service POSTs to Discord).
M.app_kbs = { "moisture", "cimis_station", "cimis_spatial", "digest" }

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

-- Daily-digest config — read by chains/digest_user_functions.lua.
-- The digest is a calendar-anchored daily-gate state machine (same pattern
-- as the CIMIS KBs): the column ticks every retry_s, but DAILY_DIGEST
-- actually publishes at most once per Pacific civil day, on the first tick
-- at-or-after hour_pacific. Default hour 9 chosen so CIMIS's morning fetch
-- (window opens at 09:00 Pacific too) has a chance to publish today's ETo
-- before the digest snapshots blackboard state.
--
-- Last-published date is in-memory only (bb._digest_state.last_published_date).
-- A robot reboot AFTER today's digest already went out will re-publish today
-- when the retry cycle next opens the gate. Acceptable for v1; if dedup
-- matters later, persist to identity-dir state.
M.digest = {
    hour_pacific = 9,           -- inclusive (Pacific civil hour)
    retry_s      = 900,         -- 15 min retry cadence (matches CIMIS)
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

-- Persistence-topology declaration. Returns the list of leaves under this
-- robot's namespace that the persistence layer should store, with their kind
-- (stream = circular per-path buffer of `length` rows; status = UPSERT-by-
-- path single value) and pre-allocation size. The robot announces this
-- once on namespace_up and the persistence service uses it to idempotently
-- construct_kb the matching ltree paths (decision #6/#9: firmware/class IS
-- the schema; the persistence layer does not know the topology a priori).
--
-- Each `path` is the leaf tail under `<namespace>/`; the persistence service
-- prepends `<class>.<instance>.` and converts `/` -> `.` to derive the
-- ltree field name (e.g. `cimis/station/sample` -> field name
-- `farm_soil.lacima01.cimis.station.sample`).
function M.persistence_topology()
    local topo = {}
    for source_id, _ in pairs(M.cimis.sources) do
        topo[#topo + 1] = {
            path   = "cimis/" .. source_id .. "/sample",
            kind   = "stream",
            length = 30,                       -- ~1 month of daily ETo per source
            desc   = "CIMIS daily ETo per-day stream (" .. source_id .. ")",
        }
        topo[#topo + 1] = {
            path = "cimis/" .. source_id .. "/latest",
            kind = "status",
            desc = "CIMIS daily ETo latest (" .. source_id .. ")",
        }
    end
    for device, location in pairs(M.device_locations) do
        topo[#topo + 1] = {
            path   = device .. "/" .. location .. "/latest",
            kind   = "stream",
            length = 256,                      -- matches the in-robot ring depth
            desc   = "soil moisture readings — " .. device .. " / " .. location,
        }
    end
    topo[#topo + 1] = {
        path = "heartbeat",
        kind = "status",
        desc = "robot rolled-up heartbeat",
    }
    return topo
end

-- Publish the persistence topology onto BOTH channels:
--   <namespace>/persistence_topology
--       per-namespace channel; ad-hoc consumers (a CLI, a debug tool)
--       that already know the robot's identity subscribe here.
--   fleet/admin/persistence_topology_announce
--       fleet-wide discovery channel; the persistence service subscribes
--       only here, demuxing class+instance from the payload. The token
--       binding has no wildcard / string-prefix subscribe, so we cannot
--       use a `**/persistence_topology` sub; a single shared token is
--       the workaround.
--
-- Called from on_namespace_up (first publish) AND periodically from
-- main.lua's pump (default cadence below) so a late-joining persistence
-- service (e.g., persistence restarted while the robot is up) catches the
-- topology within one cadence and the next data publishes land on its
-- subs. `silent` suppresses the success log for periodic calls.
function M.publish_persistence_topology(ps, identity, silent)
    local cjson = require("cjson")
    local topo = M.persistence_topology()
    local payload = cjson.encode({
        schema   = "persistence_topology/1",
        class    = identity.class,
        instance = identity.instance,
        entries  = topo,
    })
    local function pub(key)
        local ok, err = pcall(function() ps:publish(key, payload) end)
        if not ok then
            io.stderr:write(string.format(
                "FARM_SOIL [%s]: persistence_topology publish to %s failed: %s\n",
                identity.namespace, key, tostring(err)))
        end
        return ok
    end
    local ok1 = pub(identity.namespace .. "/persistence_topology")
    local ok2 = pub("fleet/admin/persistence_topology_announce")
    if ok1 and ok2 and not silent then
        io.stderr:write(string.format(
            "FARM_SOIL [%s]: persistence_topology published (%d entries, 2 channels)\n",
            identity.namespace, #topo))
    end
    return ok1 and ok2
end

-- Topology re-announce cadence (seconds). Tuned to balance:
--   * how quickly a late-joining persistence service catches up (≤ this)
--   * how much wire chatter we add per robot (small payload, infrequent)
-- 30s gives a worst-case discovery delay of 30s for a persistence restart,
-- which is comfortably under the slowest data cadence on the robot
-- (CIMIS at 15 min).
M.PERSISTENCE_TOPOLOGY_REPUBLISH_S = 30

-- Class hook, run after KB0 publishes the core namespace leaves.
function M.on_namespace_up(ps, identity, bb)
    M.publish_persistence_topology(ps, identity, false)
end

return M
