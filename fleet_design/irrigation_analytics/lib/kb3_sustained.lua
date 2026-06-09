-- kb3_sustained.lua — schedule-aware ETO leak detector (Glenn 2026-06-09 spec).
--
-- Design: per-minute step-based detector that fires on 3 consecutive
-- minutes with PLC main_flow_meter (well sensor) over an absolute GPM
-- threshold. After 5-minute warmup at run start. ETO bins only (non-ETO
-- short runs are skipped entirely).
--
-- Sensor strategy (Glenn 2026-06-09 PM, post-filter-fix + new sensor):
--   - PLC main_flow_meter (popup.PLC_FLOW_METER) is now the high-accuracy
--     primary signal. Use it for ALL trip decisions.
--   - FILTERED_HUNTER_VALVE (popup.FILTERED_HUNTER_VALVE) is no longer in
--     the trip path. For NON-CITY bins it's a side-channel sanity check
--     only. For CITY bins (any group containing sat_1:39, the city-water
--     valve) the difference FHV - PLC reveals how much city water is
--     flowing — logged as city_delta, never trips.
--
-- Key concept: "step" = popup.ELASPED_TIME (controller's per-minute
-- counter — note the controller's typo, preserved as-is). It increments
-- once per minute at HH:MM:15. Polling at 30s, we detect each minute
-- transition by watching this value change. Each step transition is one
-- "evaluation" — we don't double-count within a minute.
--
-- Algorithm per evaluation:
--   if elapsed < 5: in warmup → reset consecutive, skip
--   plc = popup.PLC_FLOW_METER (well GPM, new high-accuracy sensor)
--   trip = plc > THRESHOLD
--   if trip: consecutive += 1
--            if consecutive >= 3: FIRE
--   else:    consecutive = 0
--   if is_city_bin: city_delta = max(0, FHV - PLC)  (telemetry only)
--
-- On FIRE: CLOSE_MASTER_VALVE (water off first) then SKIP_STATION
-- (advance queue). One fire per station — the fired flag stays true
-- until next STATION_START.
--
-- Independent of KB2 / KB4. No baseline math, no expected values.

local M = {}

-- =========================================================================
-- Tuneables
-- =========================================================================
M.GPM_THRESHOLD       = 15.0   -- primary: fire if PLC > this for N consec min (absolute)
-- Secondary trip (Glenn 2026-06-09 PM): catches elevated-for-this-bin runs
-- that don't cross the absolute threshold. Reads KB4 v2's per-bin baseline
-- (base_flow_gpm from baselines_kb4v2 table). When baseline + delta is
-- lower than GPM_THRESHOLD, secondary trips first; when baseline is
-- absent or has < BASELINE_MIN_N_CLEAN runs, only primary applies.
-- Both trips share the 3-consec gate.
M.BASELINE_DELTA_GPM    = 2.0   -- secondary: fire if PLC > baseline + this
M.BASELINE_MIN_N_CLEAN  = 3     -- skip secondary until baseline has this many clean runs
-- WARMUP_MINUTES exists because of SPRINKLER LINE RECHARGE (Glenn 2026-06-09):
-- When a station starts, the dry/depressurized sprinkler distribution lines
-- fill rapidly — water rushes in to refill empty pipe between the master
-- valve and the emitters. This produces a real, large PLC spike (observed
-- 18.2 GPM at min 1 on sat_4:9 18:00 vs steady-state 10.8 GPM). Once the
-- lines are charged and pressure equilibrium is reached, flow drops to the
-- emitters' actual demand. The 5-min warmup covers this charging phase.
-- DO NOT tighten warmup below ~3 min without first verifying line-recharge
-- duration for the most distant zone.
M.WARMUP_MINUTES      = 5
M.CONSECUTIVE_REQUIRED = 3     -- N minutes in a row over threshold

-- ETO valve membership. Mirror of farm-side eto_site_setup.json — these
-- are the 20 pins that run on the No_city_water schedule. Non-ETO bins
-- (short runs, city flushes, grass schedules) are skipped entirely
-- because their 5-min warmup + 3-minute sustained check doesn't fit
-- their run profile.
M.ETO_PINS = {
    satellite_2 = { [13]=true, [14]=true, [15]=true, [16]=true },
    satellite_3 = { [1]=true, [2]=true, [5]=true, [13]=true, [14]=true,
                    [15]=true, [18]=true },
    satellite_4 = { [1]=true, [3]=true, [4]=true, [6]=true, [7]=true,
                    [9]=true, [10]=true, [11]=true, [12]=true },
}

function M.is_eto_bin(io_setup)
    if type(io_setup) ~= "table" then return false end
    for _, group in ipairs(io_setup) do
        if type(group) == "table" then
            local sat = group.remote
            for _, bit in ipairs(group.bits or {}) do
                if M.ETO_PINS[sat] and M.ETO_PINS[sat][tonumber(bit)] then
                    return true
                end
            end
        end
    end
    return false
end

-- City-water valve identity (locked from irrigation_channel_physics memo):
-- sat_1:39 is the city-water valve with dwell. When any group in the
-- io_setup contains sat_1:39, water can come from EITHER well or city,
-- which makes FHV > PLC legitimate (the delta IS the city water).
M.CITY_VALVE = { remote = "satellite_1", bit = 39 }

function M.is_city_bin(io_setup)
    if type(io_setup) ~= "table" then return false end
    for _, group in ipairs(io_setup) do
        if type(group) == "table" and group.remote == M.CITY_VALVE.remote then
            for _, bit in ipairs(group.bits or {}) do
                if tonumber(bit) == M.CITY_VALVE.bit then return true end
            end
        end
    end
    return false
end

function M.bin_key(io_setup)
    local parts = {}
    for _, group in ipairs(io_setup or {}) do
        if type(group) == "table" then
            local sat = group.remote or "?"
            for _, bit in ipairs(group.bits or {}) do
                parts[#parts+1] = sat .. ":" .. tostring(bit)
            end
        end
    end
    table.sort(parts)
    return table.concat(parts, "/")
end

-- =========================================================================
-- Step evaluation
-- =========================================================================
--
-- arming: the per-station state table. Mutated in place. Fields:
--   bin             string
--   is_city         bool — set at STATION_START; affects city_delta logging
--   baseline_gpm    REAL | nil — KB4 v2 per-bin baseline if available with
--                                n_clean_runs >= BASELINE_MIN_N_CLEAN
--   started_sid     past_actions stream_id of STATION_START
--   prev_elapsed    last seen popup.ELASPED_TIME (so we eval only on change)
--   consecutive     current count of over-threshold minutes
--   fired           bool — true after we've fired this station
--
-- Returns table describing what happened. Caller logs and may dispatch on
-- action="FIRE". `trip_path` is "primary" | "secondary" | "both" | nil so
-- the operator knows which gate fired.
--
-- Trip rule:
--   primary   = PLC > GPM_THRESHOLD
--   secondary = baseline_gpm and PLC > (baseline_gpm + BASELINE_DELTA_GPM)
--   trip      = primary OR secondary
-- FHV no longer trips (carried for telemetry; on city bins it produces city_delta).
function M.evaluate_step(arming, elapsed, plc, hunter)
    if not arming or not elapsed then
        return { action = "skip", reason = "no_arming_or_elapsed" }
    end

    -- Detect actual minute change. If elapsed hasn't changed, it's the
    -- same minute — skip silently (caller can still log periodically).
    if arming.prev_elapsed and elapsed == arming.prev_elapsed then
        return { action = "no_change", elapsed = elapsed }
    end
    arming.prev_elapsed = elapsed

    -- city_delta: signed FHV - PLC (positive = city water contributing).
    local city_delta = nil
    if plc and hunter then city_delta = hunter - plc end

    -- Warmup: don't evaluate, keep consecutive at 0.
    if elapsed < M.WARMUP_MINUTES then
        arming.consecutive = 0
        return {
            action = "warmup", elapsed = elapsed,
            plc = plc, hunter = hunter, city_delta = city_delta,
            consecutive = 0, in_warmup = true,
            baseline_gpm = arming.baseline_gpm,
        }
    end

    -- Already fired this station — log but don't re-fire.
    if arming.fired then
        return {
            action = "fired_already", elapsed = elapsed,
            plc = plc, hunter = hunter, city_delta = city_delta,
            consecutive = arming.consecutive, fired = true,
            baseline_gpm = arming.baseline_gpm,
        }
    end

    local plc_val = plc or 0
    local trip_primary = plc_val > M.GPM_THRESHOLD
    local trip_secondary = false
    if arming.baseline_gpm
       and plc_val > (arming.baseline_gpm + M.BASELINE_DELTA_GPM) then
        trip_secondary = true
    end
    local trip = trip_primary or trip_secondary

    if trip then
        arming.consecutive = (arming.consecutive or 0) + 1
    else
        arming.consecutive = 0
    end

    local should_fire = (arming.consecutive >= M.CONSECUTIVE_REQUIRED) and (not arming.fired)
    if should_fire then arming.fired = true end

    local trip_path = nil
    if trip_primary and trip_secondary then trip_path = "both"
    elseif trip_primary then trip_path = "primary"
    elseif trip_secondary then trip_path = "secondary"
    end

    return {
        action         = should_fire and "FIRE" or "checked",
        elapsed        = elapsed,
        plc            = plc,
        hunter         = hunter,
        city_delta     = city_delta,
        trip_primary   = trip_primary,
        trip_secondary = trip_secondary,
        trip_path      = trip_path,
        baseline_gpm   = arming.baseline_gpm,
        consecutive    = arming.consecutive,
        fired          = arming.fired,
    }
end

-- =========================================================================
-- SQLite — per-minute evaluation log + fires
-- =========================================================================
local lsqlite3 = nil
local function ensure_lsqlite3()
    if lsqlite3 then return lsqlite3 end
    local ok, mod = pcall(require, "lsqlite3")
    if not ok then return nil, "lsqlite3 not available: " .. tostring(mod) end
    lsqlite3 = mod
    return mod
end

local SCHEMA = [[
CREATE TABLE IF NOT EXISTS evals_kb3 (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    ts_ms           INTEGER NOT NULL,
    bin             TEXT,
    is_city         INTEGER DEFAULT 0,
    schedule        TEXT,
    station_step    INTEGER,
    elapsed_min     INTEGER,
    plc_gpm         REAL,
    hunter_gpm      REAL,
    city_delta_gpm  REAL,
    baseline_gpm    REAL,
    trip_primary    INTEGER,
    trip_secondary  INTEGER,
    trip_path       TEXT,
    consecutive     INTEGER,
    in_warmup       INTEGER,
    fired           INTEGER,
    action          TEXT
);
CREATE INDEX IF NOT EXISTS idx_evals_kb3_bin ON evals_kb3(bin);
CREATE INDEX IF NOT EXISTS idx_evals_kb3_ts  ON evals_kb3(ts_ms);
CREATE INDEX IF NOT EXISTS idx_evals_kb3_fired ON evals_kb3(fired);

CREATE TABLE IF NOT EXISTS runs_kb3 (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    ts_ms           INTEGER NOT NULL,
    bin             TEXT,
    is_city         INTEGER DEFAULT 0,
    schedule        TEXT,
    station_step    INTEGER,
    elapsed_min     INTEGER,
    plc_gpm         REAL,
    hunter_gpm      REAL,
    city_delta_gpm  REAL,
    baseline_gpm    REAL,
    trip_path       TEXT,
    actions_sent    TEXT,
    armed           INTEGER DEFAULT 0,
    note            TEXT
);
CREATE INDEX IF NOT EXISTS idx_runs_kb3_ts ON runs_kb3(ts_ms);
]]

function M.open_db(path)
    local mod, err = ensure_lsqlite3()
    if not mod then return nil, err end
    local db, code, errmsg = mod.open(path)
    if not db then
        return nil, string.format("open %s failed: %s/%s",
            path, tostring(code), tostring(errmsg))
    end
    local rc = db:exec(SCHEMA)
    if rc ~= mod.OK then
        local msg = db:errmsg()
        db:close()
        return nil, "schema migration failed: " .. tostring(msg)
    end
    return db
end

function M.insert_eval(db, fields)
    local stmt = db:prepare([[
        INSERT INTO evals_kb3(
            ts_ms, bin, is_city, schedule, station_step, elapsed_min,
            plc_gpm, hunter_gpm, city_delta_gpm, baseline_gpm,
            trip_primary, trip_secondary, trip_path, consecutive,
            in_warmup, fired, action)
        VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    ]])
    if not stmt then return nil, db:errmsg() end
    stmt:bind_values(fields.ts_ms, fields.bin,
        fields.is_city and 1 or 0,
        fields.schedule,
        fields.station_step, fields.elapsed_min,
        fields.plc_gpm, fields.hunter_gpm,
        fields.is_city and fields.city_delta_gpm or nil,
        fields.baseline_gpm,
        fields.trip_primary and 1 or 0,
        fields.trip_secondary and 1 or 0,
        fields.trip_path,
        fields.consecutive or 0,
        fields.in_warmup and 1 or 0,
        fields.fired and 1 or 0,
        fields.action)
    stmt:step()
    stmt:finalize()
    return true
end

function M.insert_fire(db, fields)
    local stmt = db:prepare([[
        INSERT INTO runs_kb3(
            ts_ms, bin, is_city, schedule, station_step, elapsed_min,
            plc_gpm, hunter_gpm, city_delta_gpm, baseline_gpm,
            trip_path, actions_sent, armed, note)
        VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    ]])
    if not stmt then return nil, db:errmsg() end
    stmt:bind_values(fields.ts_ms, fields.bin,
        fields.is_city and 1 or 0,
        fields.schedule,
        fields.station_step, fields.elapsed_min,
        fields.plc_gpm, fields.hunter_gpm,
        fields.is_city and fields.city_delta_gpm or nil,
        fields.baseline_gpm,
        fields.trip_path,
        fields.actions_sent or "",
        fields.armed and 1 or 0,
        fields.note)
    stmt:step()
    stmt:finalize()
    return true
end

return M
