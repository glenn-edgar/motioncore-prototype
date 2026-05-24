-- chains/digest_user_functions.lua — ct_* user fns for the daily-digest KB.
--
-- DAILY_DIGEST: build the per-day operator message from in-memory state and
-- publish it on the shared `fleet/notify/digest/daily` topic. The
-- notification_service subscribes there and POSTs to Discord.
--
-- The digest reads ONLY blackboard state already maintained by the other
-- KBs:
--   bb._moisture_slots[device.."/"..location].ring  — newest entry +
--                                                     last-24h uplink count
--   bb._cimis[source].last_record                   — newest CIMIS day
--
-- It does NOT query persistence (that would be a Zenoh round-trip for data
-- this process already has). It does NOT format network state, just sensor
-- data — degraded heartbeats surface elsewhere.
--
-- Containment: each publish is pcall-wrapped — a zenohd outage cannot crash
-- the KB or the pump. App-heartbeat is always stamped so KB0 sees us alive.

local cjson         = require("cjson")
local clock         = require("clock")
local format_table  = require("format_table")
local app_heartbeat = require("app_heartbeat")

local M = { main = {}, one_shot = {}, boolean = {} }

local DIGEST_TOPIC   = "fleet/notify/digest/daily"
local SCHEMA_DIGEST  = "fleet.notify.digest/1"
local PERIOD_S       = 86400        -- mirrors digest.lua DIGEST_PERIOD_S
local WINDOW_S       = 86400        -- "uplinks in last 24h"

local function log(id, fmt, ...)
    io.stderr:write(string.format(
        "digest [%s]: " .. fmt .. "\n", id.namespace, ...))
end

-- Parse an RFC3339 UTC timestamp ("2026-05-23T16:48:03Z" with optional
-- subseconds) to epoch seconds. Returns nil on malformed input.
local function rfc3339_to_epoch(ts)
    if type(ts) ~= "string" then return nil end
    local y, mo, d, h, mi, s = ts:match(
        "^(%d+)%-(%d+)%-(%d+)T(%d+):(%d+):(%d+)")
    if not y then return nil end
    return clock.days_from_civil(tonumber(y), tonumber(mo), tonumber(d))
        * 86400 + tonumber(h) * 3600 + tonumber(mi) * 60 + tonumber(s)
end

-- Turn the in-memory moisture slots into the rows format_table expects.
-- Enumerated from class_spec.device_locations (the canonical sensor roster)
-- so a sensor that has stopped reporting — or that has never reported yet —
-- surfaces in the digest as "no data" rather than silently vanishing. For
-- an operator-facing report a missing sensor matters more than a present one.
-- uplinks_in_window counts ring entries with received_at within WINDOW_S of now.
local function moisture_rows_from_slots(device_locations, slots, now_epoch)
    local rows = {}
    if not device_locations then return rows end
    slots = slots or {}
    for device, location in pairs(device_locations) do
        local slot = slots[device .. "/" .. location]
        local ring = slot and slot.ring or nil
        local newest = ring and ring[#ring] or nil
        local count = 0
        if ring then
            for _, r in ipairs(ring) do
                local e = rfc3339_to_epoch(r.received_at)
                if e and (now_epoch - e) <= WINDOW_S then
                    count = count + 1
                end
            end
        end
        rows[#rows + 1] = {
            device_id         = device,
            latest_value      = newest and (newest.measurements or {}).moisture or nil,
            latest_ts         = newest and newest.received_at or nil,
            uplinks_in_window = count,
        }
    end
    table.sort(rows, function(a, b)
        return tostring(a.device_id) < tostring(b.device_id)
    end)
    return rows
end

-- Turn the per-source CIMIS state into ETo rows (most-recent first). Each
-- source contributes at most one row — the freshest finalized day held in
-- last_record.
local function eto_rows_from_cimis(cimis_state)
    local rows = {}
    if not cimis_state then return rows end
    for _source_id, st in pairs(cimis_state) do
        local r = st and st.last_record
        if r then
            rows[#rows + 1] = {
                date = r.date, value = r.value, unit = r.unit,
            }
        end
    end
    table.sort(rows, function(a, b) return tostring(a.date) > tostring(b.date) end)
    return rows
end

M.one_shot.DAILY_DIGEST = function(handle, _node)
    local bb     = handle.blackboard
    local id, ps = bb._identity, bb._pubsub
    local cs     = bb._class_spec
    local now    = os.time()
    local today  = os.date("!%Y-%m-%d", now)

    local moisture_rows = moisture_rows_from_slots(
        cs and cs.device_locations or nil, bb._moisture_slots, now)
    local eto_rows      = eto_rows_from_cimis(bb._cimis)
    local body          = format_table.format_daily_report(
        moisture_rows, eto_rows, { report_date = today })

    local payload = cjson.encode({
        schema   = SCHEMA_DIGEST,
        class    = id.class,
        instance = id.instance,
        body     = body,
    })

    local ok, err = pcall(function() ps:publish(DIGEST_TOPIC, payload) end)
    if ok then
        log(id, "published digest (%d moisture rows, %d eto rows, %d body chars)",
            #moisture_rows, #eto_rows, #body)
        app_heartbeat.stamp(handle, "digest", "ok",
            string.format("%d moisture, %d eto", #moisture_rows, #eto_rows),
            PERIOD_S)
    else
        log(id, "digest publish FAILED: %s", tostring(err))
        app_heartbeat.stamp(handle, "digest", "degraded",
            "digest publish failed", PERIOD_S)
    end
end

M.registry = { main = M.main, one_shot = M.one_shot, boolean = M.boolean }
return M
