-- chains/cimis_user_functions.lua — ct_* user fns for the CIMIS KBs.
--
-- Two app KB instances share this module:
--   cimis_station  -> M.one_shot.CIMIS_TICK_STATION
--   cimis_spatial  -> M.one_shot.CIMIS_TICK_SPATIAL
-- Each is a 2-line wrapper around cimis_tick_impl(handle, source_id); the
-- only thing that differs is which source_id ("station"/"spatial") they
-- dispatch with. cimis_tick_impl reads its immutable config from
-- bb._class_spec.cimis (api_base, data_items, window_start_h, window_end_h,
-- retry_s, sources[source_id].{target_kind,target}) and its mutable state
-- from bb._cimis[source_id] (last_recorded_date, last_record).
--
-- Daily-gate semantics (per KB):
--   1. Already-recorded(yesterday)         -> idle, heartbeat ok.
--   2. Pacific hour <  window_start_h      -> idle, heartbeat ok (pre-window).
--   3. Pacific hour >= window_end_h        -> idle, heartbeat degraded
--                                             (missed window for yesterday).
--   4. In-window, CIMIS_APP_KEY missing    -> heartbeat degraded.
--   5. In-window, fetch fails              -> heartbeat degraded.
--   6. In-window, no finalized ETo yet     -> heartbeat ok (retrying).
--   7. In-window, finalized ETo for yest.  -> publish + mark done, heartbeat ok.
--
-- Each KB owns one in-memory record per source — not a ring (CIMIS ETo is
-- one number per day per source). The per-source repost queryable returns
-- that record JSON, or the JSON literal `null` when nothing's recorded yet.
-- main.lua wires the queryables and calls M.handle_repost_request.
--
-- Containment: TTN-style — fetch errors are return values (skip cycle,
-- stamp degraded); each Zenoh publish is pcall-wrapped. No raises.
--
-- Secret: CIMIS_APP_KEY from the environment (run.sh sources secrets/).

local cjson         = require("cjson")
local clock         = require("clock")
local cimis_client  = require("cimis_client")
local cimis_decoder = require("cimis_decoder")
local app_heartbeat = require("app_heartbeat")

local M = { main = {}, one_shot = {}, boolean = {} }

local SCHEMA_READING = "cimis.eto/1"
local MEASUREMENT    = "DayAsceEto"     -- the single item these KBs care about

local function log(id, source_id, fmt, ...)
    io.stderr:write(string.format(
        "cimis-%s [%s]: " .. fmt .. "\n", source_id, id.namespace, ...))
end

-- The published-reading envelope. Small (~300 B) — well under the
-- zenoh-pico multi-KB drop threshold.
local function reading_json(class, instance, source_id, src, record)
    return cjson.encode({
        schema      = SCHEMA_READING,
        class       = class,
        instance    = instance,
        source      = source_id,
        target_kind = src.target_kind,
        target      = src.target,
        date        = record.date,
        item        = record.item,
        value       = record.value,
        unit        = record.unit,
        qc          = record.qc,
    })
end

local function publish_latest(id, ps, source_id, src, record)
    local key = id.namespace .. "/cimis/" .. source_id .. "/latest"
    local msg = reading_json(id.class, id.instance, source_id, src, record)
    local ok, err = pcall(function() ps:publish(key, msg) end)
    if not ok then
        log(id, source_id, "publish failed: %s", tostring(err))
    end
    return ok
end

-- The daily-gate state machine, source-agnostic.
local function cimis_tick_impl(handle, source_id)
    local bb         = handle.blackboard
    local cs         = bb._class_spec
    local id, ps     = bb._identity, bb._pubsub
    local cfg        = cs.cimis
    local src        = cfg.sources[source_id]
    local state      = bb._cimis[source_id]
    local kb_label   = "cimis_" .. source_id
    local yesterday  = clock.california_yesterday()

    -- Gate 1: yesterday already recorded -> idle.
    if state.last_recorded_date == yesterday then
        app_heartbeat.stamp(handle, kb_label, "ok",
            "yesterday recorded: " .. yesterday, cfg.retry_s)
        return
    end

    -- Gate 2: outside the Pacific 09:00–15:00 window -> idle.
    local p = clock.pacific_now()
    local tz = p.is_dst and "PDT" or "PST"
    if p.hour < cfg.window_start_h then
        app_heartbeat.stamp(handle, kb_label, "ok",
            string.format("pre-window (now %02d:%02d %s, opens %02d:00)",
                p.hour, p.minute, tz, cfg.window_start_h),
            cfg.retry_s)
        return
    end
    if p.hour >= cfg.window_end_h then
        -- Past 15:00 today and yesterday's ETo never finalized — gave up for
        -- the day. Resets at midnight Pacific when `yesterday` rolls forward.
        app_heartbeat.stamp(handle, kb_label, "degraded",
            string.format("missed window for %s (now %02d:%02d %s)",
                yesterday, p.hour, p.minute, tz),
            cfg.retry_s)
        return
    end

    -- In window, not yet recorded -> fetch.
    local app_key = os.getenv("CIMIS_APP_KEY")
    if not app_key or app_key == "" then
        log(id, source_id, "CIMIS_APP_KEY not set — skipping fetch")
        app_heartbeat.stamp(handle, kb_label, "degraded",
            "CIMIS_APP_KEY not set", cfg.retry_s)
        return
    end

    -- Narrow window: just yesterday. The per-KB store is in-memory and one-
    -- value-deep, so a multi-day backfill window would not help us — the
    -- persistence app, when it lands, can call CIMIS itself with a wider
    -- lookback if it wants gap-filling beyond what we publish today.
    local client = cimis_client.new{
        app_key  = app_key,
        api_base = cfg.api_base,
    }
    local body, ok, err = client:fetch(
        src.target, cfg.data_items, yesterday, yesterday)
    if not ok then
        log(id, source_id, "fetch FAILED (%s)", tostring(err))
        app_heartbeat.stamp(handle, kb_label, "degraded",
            "fetch failed: " .. tostring(err):sub(1, 100), cfg.retry_s)
        return
    end

    -- Decode + filter: own target_kind, the ASCE ETo item, a finalized
    -- (non-A) qc on a non-null value, dated exactly yesterday. The
    -- date < today_iso check is belt-and-suspenders against the spatial
    -- provider's 0.0-with-blank-Qc trap (would never match yesterday anyway,
    -- but kept explicit for parity with the Python skill's filter).
    local records   = cimis_decoder.parse_response(body)
    local today_iso = clock.california_today()
    local finalized
    for _, r in ipairs(records) do
        if  r.target_kind == src.target_kind
        and r.item        == MEASUREMENT
        and r.date        == yesterday
        and r.date        <  today_iso
        and r.value       ~= nil
        and r.qc          ~= "A"
        then
            finalized = r
            break
        end
    end

    if not finalized then
        log(id, source_id, "no finalized %s for %s yet (target=%s)",
            MEASUREMENT, yesterday, src.target)
        app_heartbeat.stamp(handle, kb_label, "ok",
            "retrying — yesterday not finalized yet", cfg.retry_s)
        return
    end

    state.last_recorded_date = yesterday
    state.last_record        = finalized
    publish_latest(id, ps, source_id, src, finalized)
    log(id, source_id, "recorded %s %s = %s %s (qc=%s)",
        yesterday, MEASUREMENT,
        tostring(finalized.value), tostring(finalized.unit),
        tostring(finalized.qc))
    app_heartbeat.stamp(handle, kb_label, "ok",
        string.format("recorded %s = %s",
            yesterday, tostring(finalized.value)),
        cfg.retry_s)
end

M.one_shot.CIMIS_TICK_STATION = function(handle, _node)
    return cimis_tick_impl(handle, "station")
end

M.one_shot.CIMIS_TICK_SPATIAL = function(handle, _node)
    return cimis_tick_impl(handle, "spatial")
end

-- Per-source repost RPC handler — main.lua's pump calls this. The request
-- payload is ignored (latest-only by spec); reply is the latest recorded
-- reading JSON, or the JSON literal "null" when nothing's recorded yet.
function M.handle_repost_request(handle, source_id, _req_payload)
    local bb = handle.blackboard
    local cs = bb._class_spec
    local src = cs.cimis.sources[source_id]
    local state = (bb._cimis or {})[source_id]
    local id = bb._identity
    if not state or not state.last_record or not src then
        return "null"
    end
    return reading_json(id.class, id.instance, source_id, src, state.last_record)
end

M.registry = { main = M.main, one_shot = M.one_shot, boolean = M.boolean }
return M
