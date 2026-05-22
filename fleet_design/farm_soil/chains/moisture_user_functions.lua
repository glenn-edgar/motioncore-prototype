-- chains/moisture_user_functions.lua — ct_* user fns for the moisture KB.
--
-- MOISTURE_FETCH (a one-shot leaf): fetch the TTN lookback window, decode,
-- append new uplinks to the per-sensing-point in-memory ring, and publish
-- each touched sensing point's `latest` + `recent` slot onto the namespace.
--
-- Also exports republish_all(handle) — re-emit *every* slot from the ring.
-- main.lua's pump calls it to service the `republish` RPC: the catch-up
-- door for a late subscriber (a fresh persistence app, a dashboard) that
-- wants current state without waiting for the next hourly publish.
--
-- The robot is the sole writer of the slots; the rings live in the
-- blackboard (bb._moisture_slots). Nothing is read back from zenohd.
--
-- Containment (the robustness requirement): a TTN fetch error is a return
-- value, not a raise — the leaf logs and skips the cycle. Zenoh publishes
-- are pcall-wrapped — a zenohd outage cannot crash the KB or the pump.
--
-- Config — from class_spec, attached to the blackboard by main.lua:
--   bb._class_spec.ttn = { url_base, app_name, url_after, lookback_hours, limit }
--   bb._class_spec.device_locations = { [device_id] = location, ... }
-- Secret: TTN_BEARER_TOKEN from the environment (run.sh sources secrets/).

local ttn_client = require("ttn_client")
local decoder    = require("decoder")
local moisture   = require("moisture")

local M = { main = {}, one_shot = {}, boolean = {} }

local function log(id, fmt, ...)
    io.stderr:write(string.format(
        "moisture [%s]: " .. fmt .. "\n", id.namespace, ...))
end

-- RFC3339 UTC timestamp `lookback_hours` before now — the TTN `after` arg.
local function after_iso(lookback_hours)
    return os.date("!%Y-%m-%dT%H:%M:%SZ", os.time() - lookback_hours * 3600)
end

-- Publish one sensing point's `recent` + `latest` slots onto the namespace.
-- Every Zenoh call is pcall-wrapped — a publish failure is contained and
-- logged; the ring keeps the data for the next cycle. Returns true on
-- success. Shared by MOISTURE_FETCH and republish_all.
local function publish_slot(id, ps, slot)
    local base       = id.namespace .. "/" .. slot.device .. "/" .. slot.location
    local updated_at = os.date("!%Y-%m-%dT%H:%M:%SZ")
    local recent     = moisture.recent_json(slot, id.class, id.instance, updated_at)
    local latest     = moisture.latest_json(slot, id.class, id.instance, updated_at)
    local ok, err = pcall(function()
        ps:publish(base .. "/recent", recent)
        if latest then ps:publish(base .. "/latest", latest) end
    end)
    if not ok then
        log(id, "publish failed for %s/%s (%s) — retry next cycle",
            slot.device, slot.location, tostring(err))
    end
    return ok
end

M.one_shot.MOISTURE_FETCH = function(handle, node)
    local bb = handle.blackboard
    local id, cs, ps = bb._identity, bb._class_spec, bb._pubsub
    local ttn_cfg = cs.ttn or {}

    local token = os.getenv("TTN_BEARER_TOKEN")
    if not token or token == "" then
        log(id, "TTN_BEARER_TOKEN not set — skipping fetch")
        return
    end

    bb._moisture_slots = bb._moisture_slots or {}
    local slots = bb._moisture_slots

    -- Fetch the lookback window. ttn_client returns errors, never raises.
    local client = ttn_client.new{
        url_base     = ttn_cfg.url_base,
        app_name     = ttn_cfg.app_name,
        url_after    = ttn_cfg.url_after,
        bearer_token = token,
        limit        = ttn_cfg.limit or 200,
    }
    local body, ok, err = client:fetch(after_iso(ttn_cfg.lookback_hours or 24))
    if not ok then
        log(id, "TTN fetch failed (%s) — skipping cycle", tostring(err))
        return
    end

    -- Decode + timestamp-reconcile into the per-sensing-point rings.
    local uplinks = decoder.parse_uplinks(body)
    table.sort(uplinks, function(a, b)          -- oldest-first for ring_append
        return tostring(a.received_at) < tostring(b.received_at)
    end)

    local locations = cs.device_locations or {}
    local appended, touched = 0, {}
    for _, up in ipairs(uplinks) do
        local device   = up.device_id
        local location = locations[device] or "unknown"
        local sp = device .. "/" .. location
        local slot = slots[sp]
        if not slot then
            slot = moisture.new_slot(device, location)
            slots[sp] = slot
        end
        if moisture.ring_append(slot, moisture.record_from_uplink(up)) then
            appended = appended + 1
            touched[sp] = slot
        end
    end

    local published = 0
    for _, slot in pairs(touched) do
        if publish_slot(id, ps, slot) then published = published + 1 end
    end

    log(id, "fetch ok — %d uplinks, %d new samples, %d slots published",
        #uplinks, appended, published)
end

-- Re-emit every slot the robot currently holds — the `republish` RPC
-- handler (in main.lua's pump) calls this. Returns the count published.
function M.republish_all(handle)
    local bb = handle.blackboard
    local id, ps = bb._identity, bb._pubsub
    local n = 0
    for _, slot in pairs(bb._moisture_slots or {}) do
        if publish_slot(id, ps, slot) then n = n + 1 end
    end
    return n
end

M.registry = { main = M.main, one_shot = M.one_shot, boolean = M.boolean }
return M
