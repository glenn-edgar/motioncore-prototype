-- chains/moisture_user_functions.lua — ct_* user fns for the moisture KB.
--
-- One leaf — MOISTURE_FETCH: fetch the TTN lookback window, decode, append
-- new uplinks to the per-sensing-point in-memory ring, and publish each
-- touched sensing point's `latest` + `recent` slot onto the namespace.
--
-- The robot is the sole writer of the slots; the rings live in the
-- blackboard (bb._moisture_slots) so the pump's republish handler can
-- re-emit them. Nothing is read back from zenohd.
--
-- Containment (per the robustness requirement): a TTN fetch error is a
-- return value, not a raise — the leaf logs and skips the cycle; the next
-- tick retries. Zenoh publishes are pcall-wrapped — a zenohd outage cannot
-- crash the KB.
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

    -- Publish each touched sensing point's slots. Every Zenoh call is
    -- pcall-wrapped — a publish failure is logged and retried next cycle.
    local updated_at = os.date("!%Y-%m-%dT%H:%M:%SZ")
    local published = 0
    for _, slot in pairs(touched) do
        local base   = id.namespace .. "/" .. slot.device .. "/" .. slot.location
        local recent = moisture.recent_json(slot, id.class, id.instance, updated_at)
        local latest = moisture.latest_json(slot, id.class, id.instance, updated_at)
        local pok, perr = pcall(function()
            ps:publish(base .. "/recent", recent)
            if latest then ps:publish(base .. "/latest", latest) end
        end)
        if pok then
            published = published + 1
        else
            log(id, "publish failed for %s (%s) — retry next cycle",
                slot.device .. "/" .. slot.location, tostring(perr))
        end
    end

    log(id, "fetch ok — %d uplinks, %d new samples, %d slots published",
        #uplinks, appended, published)
end

M.registry = { main = M.main, one_shot = M.one_shot, boolean = M.boolean }
return M
