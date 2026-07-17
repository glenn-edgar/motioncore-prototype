-- chains/openmeteo_user_functions.lua — ct_* user fns for the openmeteo_eto KB.
--
-- One app KB instance: openmeteo_eto -> M.one_shot.OPENMETEO_TICK.
-- Open-Meteo's free forecast API hands back FAO-56 reference ET0 as a daily
-- value, so this is the simplest of the ETo sources: no secret, no WAF, no
-- local Penman calc (contrast cimis_user_functions / synoptic_user_functions).
--
-- Config: bb._class_spec.openmeteo (api_base, latitude, longitude, timezone,
-- lookback_days, retry_s, timeout_s). State: bb._openmeteo
-- (last_recorded_date, last_record) — initialised in main.lua.
--
-- Daily-gate (retry_s cadence):
--   1. up to date  (last_recorded_date == yesterday) -> idle, heartbeat ok.
--   2. otherwise  -> fetch the last `lookback_days` days; publish every day in
--      (last_recorded .. yesterday] oldest-first; advance last_recorded_date.
--
-- There is NO pre-window gate (unlike the CIMIS KBs): Open-Meteo's yesterday
-- value is a completed, model-derived day, stable any time after the Pacific
-- date rolls over. We still publish strictly up to yesterday so the resolver's
-- `record.date == yesterday` contract holds (a today/forecast row would read
-- as "stale" to the resolver).
--
-- Published leaves (mirrors cimis' sample/latest pair):
--   <namespace>/openmeteo/latest  — status (last-write-wins)
--   <namespace>/openmeteo/sample  — stream (one record per finalized day)
--
-- Containment: fetch errors are return values (skip cycle, stamp degraded);
-- each Zenoh publish is pcall-wrapped. No raises.

local cjson            = require("cjson")
local clock            = require("clock")
local openmeteo_client = require("openmeteo_client")
local app_heartbeat    = require("app_heartbeat")

local M = { main = {}, one_shot = {}, boolean = {} }

local SCHEMA_READING = "openmeteo.eto/1"

local function log(id, fmt, ...)
    io.stderr:write(string.format(
        "openmeteo [%s]: " .. fmt .. "\n", id.namespace, ...))
end

-- The published-reading envelope. Small (~250 B) — under the zenoh-pico
-- multi-KB drop threshold. `value` is inches (resolver/dashboard contract).
local function reading_json(id, record)
    return cjson.encode({
        schema   = SCHEMA_READING,
        class    = id.class,
        instance = id.instance,
        source   = "openmeteo",
        date     = record.date,
        item     = "et0_fao_evapotranspiration",
        value    = record.value,          -- inches
        unit     = "in",
        eto_mm   = record.eto_mm,
    })
end

local function publish_to(id, ps, record, leaf)
    local key = id.namespace .. "/openmeteo/" .. leaf
    local ok, err = pcall(function() ps:publish(key, reading_json(id, record)) end)
    if not ok then
        log(id, "publish %s failed: %s", leaf, tostring(err))
    end
    return ok
end

local function openmeteo_tick(handle)
    local bb        = handle.blackboard
    local cs        = bb._class_spec
    local id, ps    = bb._identity, bb._pubsub
    local cfg       = cs.openmeteo
    local state     = bb._openmeteo
    local kb_label  = "openmeteo_eto"
    local yesterday = clock.california_yesterday()

    -- Gate 1: yesterday already recorded -> idle.
    if state.last_recorded_date == yesterday then
        app_heartbeat.stamp(handle, kb_label, "ok",
            "up to date — last recorded " .. yesterday, cfg.retry_s)
        return
    end

    -- Gate 2: fetch the lookback window and publish the gap.
    local client = openmeteo_client.new{
        latitude  = cfg.latitude,
        longitude = cfg.longitude,
        timezone  = cfg.timezone,
        api_base  = cfg.api_base,
        timeout_s = cfg.timeout_s,
    }
    local records, err = client:daily_eto(cfg.lookback_days)
    if not records then
        log(id, "fetch FAILED (%s)", tostring(err))
        app_heartbeat.stamp(handle, kb_label, "degraded",
            "fetch failed: " .. tostring(err):sub(1, 100), cfg.retry_s)
        return
    end

    -- Keep only newly-finalized days strictly after our last record and
    -- strictly before today (drops the today/forecast row).
    local today_iso     = clock.california_today()
    local last_recorded = state.last_recorded_date or ""    -- "" sorts before any date
    local pending = {}
    for _, r in ipairs(records) do
        if r.date > last_recorded and r.date < today_iso then
            pending[#pending + 1] = r
        end
    end
    table.sort(pending, function(a, b) return a.date < b.date end)

    if #pending == 0 then
        app_heartbeat.stamp(handle, kb_label, "ok",
            string.format("in window — no new data since %s",
                last_recorded == "" and "<never>" or last_recorded),
            cfg.retry_s)
        return
    end

    -- Publish in date order; /latest gets overwritten so the freshest wins.
    for _, r in ipairs(pending) do
        local record = { date = r.date, value = r.eto_in, eto_mm = r.eto_mm }
        state.last_recorded_date = r.date
        state.last_record        = record
        publish_to(id, ps, record, "sample")
        publish_to(id, ps, record, "latest")
        log(id, "recorded %s ET0 = %.3f in (%.2f mm)",
            r.date, r.eto_in, r.eto_mm)
    end

    local newest = state.last_record
    app_heartbeat.stamp(handle, kb_label, "ok",
        string.format("recorded %d day(s); latest %s = %.3f in",
            #pending, newest.date, newest.value),
        cfg.retry_s)
end

M.one_shot.OPENMETEO_TICK = function(handle, _node)
    return openmeteo_tick(handle)
end

-- Repost RPC handler (parity with cimis) — returns the latest recorded reading
-- JSON, or the JSON literal "null" when nothing's recorded yet.
function M.handle_repost_request(handle, _req_payload)
    local bb    = handle.blackboard
    local state = bb._openmeteo or {}
    local id    = bb._identity
    if not state.last_record then return "null" end
    return reading_json(id, state.last_record)
end

M.registry = { main = M.main, one_shot = M.one_shot, boolean = M.boolean }
return M
