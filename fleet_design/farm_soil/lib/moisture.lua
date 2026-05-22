-- lib/moisture.lua — moisture skill core: uplink records, the per-sensing-
-- point ring buffer, and the latest/recent slot JSON.
--
-- Engine-agnostic — pure data logic, no chain_tree imports. The chain_tree
-- wiring lives in chains/moisture_user_functions.lua.
--
-- A "slot" is one sensing point (<device>/<location>): a ring of up to
-- RING_CAPACITY uplink records, oldest-first. The robot is the sole writer,
-- so the ring in memory is the source of truth — nothing is read back.

local cjson = require("cjson")

local M = {}

M.SCHEMA_RECENT  = "moisture.recent/1"
M.SCHEMA_LATEST  = "moisture.latest/1"
M.RING_CAPACITY  = 256

-- SenseCAP S2105 measurement ids -> field names. The decoder is generic
-- (id + value); naming is the moisture skill's job.
M.MEASUREMENT_NAME = {
    [4108] = "moisture",     -- volumetric water content (m3/m3)
    [4102] = "soil_temp",    -- soil temperature (C)
    [4103] = "ec",           -- soil EC (mS/cm)
}

local UNITS = {
    moisture = "m3/m3", soil_temp = "C", ec = "mS/cm",
    rssi = "dBm", snr = "dB", battery = "%",
}

-- Build a slot entry (one uplink record) from a decoder uplink: map
-- measurement ids to named fields, fold gateway/battery into `link`.
function M.record_from_uplink(up)
    local measurements = {}
    for _, m in ipairs(up.measurements or {}) do
        local name = M.MEASUREMENT_NAME[m.measurement_id]
        if name then measurements[name] = m.value end
    end
    local g = up.gateway or {}
    return {
        received_at  = up.received_at,
        f_cnt        = up.f_cnt,
        measurements = measurements,
        link = {
            gateway_id       = g.gateway_id,
            gateway_count    = g.gateway_count,
            rssi             = g.rssi,
            channel_rssi     = g.channel_rssi,
            snr              = g.snr,
            frequency        = g.frequency,
            spreading_factor = g.spreading_factor,
            bandwidth        = g.bandwidth,
            coding_rate      = g.coding_rate,
            airtime_s        = g.airtime_s,
            battery          = (up.battery_present and up.battery_value) or nil,
        },
    }
end

-- A fresh, empty slot for a sensing point.
function M.new_slot(device, location)
    return { device = device, location = location, ring = {} }
end

-- Append `record` to the slot's ring IFF it is strictly newer than the
-- ring's current newest entry — this is the timestamp reconcile: a re-fetched
-- uplink (received_at <= newest) is dropped. Evicts the oldest past
-- RING_CAPACITY. Records must be fed oldest-first. Returns true if appended.
--
-- received_at is TTN's RFC3339 UTC string; consistent formatting makes a
-- lexicographic compare equivalent to a chronological one.
function M.ring_append(slot, record)
    local ring = slot.ring
    local newest = ring[#ring]
    if newest and tostring(record.received_at) <= tostring(newest.received_at) then
        return false
    end
    ring[#ring + 1] = record
    while #ring > M.RING_CAPACITY do
        table.remove(ring, 1)
    end
    return true
end

-- The `recent` slot JSON — the whole ring. `updated_at` is the robot's
-- wall-clock at publish (RFC3339 UTC string).
function M.recent_json(slot, class, instance, updated_at)
    return cjson.encode({
        schema     = M.SCHEMA_RECENT,
        class      = class,
        instance   = instance,
        device     = slot.device,
        location   = slot.location,
        capacity   = M.RING_CAPACITY,
        updated_at = updated_at,
        units      = UNITS,
        entries    = slot.ring,          -- oldest -> newest
    })
end

-- The `latest` slot JSON — the newest entry only (the persistence app's
-- cheap integrate-trigger). Returns nil for an empty ring.
function M.latest_json(slot, class, instance, updated_at)
    local newest = slot.ring[#slot.ring]
    if not newest then return nil end
    return cjson.encode({
        schema     = M.SCHEMA_LATEST,
        class      = class,
        instance   = instance,
        device     = slot.device,
        location   = slot.location,
        updated_at = updated_at,
        units      = UNITS,
        entry      = newest,
    })
end

return M
