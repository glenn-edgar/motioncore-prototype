-- kb3_sustained.lua — schedule-aware ETO leak detector (Glenn 2026-06-09 spec).
--
-- Design: per-minute step-based detector that fires on 3 consecutive
-- minutes with EITHER PLC well meter OR popup HUNTER reading over an
-- absolute GPM threshold. After 5-minute warmup at run start. ETO bins
-- only (non-ETO short runs are skipped entirely).
--
-- Key concept: "step" = popup.ELASPED_TIME (controller's per-minute
-- counter — note the controller's typo, preserved as-is). It increments
-- once per minute at HH:MM:15. Polling at 30s, we detect each minute
-- transition by watching this value change. Each step transition is one
-- "evaluation" — we don't double-count within a minute.
--
-- Algorithm per evaluation:
--   if elapsed < 5: in warmup → reset consecutive, skip
--   plc    = popup.PLC_FLOW_METER       (well water in GPM)
--   hunter = popup.FILTERED_HUNTER_VALVE (zone-side GPM)
--   trip = (plc > THRESHOLD) or (hunter > THRESHOLD)
--   if trip: consecutive += 1
--            if consecutive >= 3: FIRE
--   else:    consecutive = 0
--
-- On FIRE: CLOSE_MASTER_VALVE (water off first) then SKIP_STATION
-- (advance queue). One fire per station — the fired flag stays true
-- until next STATION_START.
--
-- Independent of KB2 / KB4. No baseline math, no expected values.
-- Just two raw flow readings against a fixed threshold.

local M = {}

-- =========================================================================
-- Tuneables
-- =========================================================================
M.GPM_THRESHOLD       = 15.0   -- fire if either meter > this for 3 consecutive minutes
M.WARMUP_MINUTES      = 5      -- skip first N minutes of each run
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
--   started_sid     past_actions stream_id of STATION_START
--   prev_elapsed    last seen popup.ELASPED_TIME (so we eval only on change)
--   consecutive     current count of over-threshold minutes
--   fired           bool — true after we've fired this station
--
-- Returns table { action, elapsed, plc, hunter, trip_plc, trip_hunter,
--                 consecutive, fired, in_warmup, no_change } describing
-- what just happened — caller logs and may dispatch on action="FIRE".
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

    -- Warmup: don't evaluate, keep consecutive at 0.
    if elapsed < M.WARMUP_MINUTES then
        arming.consecutive = 0
        return {
            action = "warmup", elapsed = elapsed,
            plc = plc, hunter = hunter, consecutive = 0,
            in_warmup = true,
        }
    end

    -- Already fired this station — log but don't re-fire.
    if arming.fired then
        return {
            action = "fired_already", elapsed = elapsed,
            plc = plc, hunter = hunter, consecutive = arming.consecutive,
            fired = true,
        }
    end

    local trip_plc    = (plc    or 0) > M.GPM_THRESHOLD
    local trip_hunter = (hunter or 0) > M.GPM_THRESHOLD
    local trip = trip_plc or trip_hunter

    if trip then
        arming.consecutive = (arming.consecutive or 0) + 1
    else
        arming.consecutive = 0
    end

    local should_fire = (arming.consecutive >= M.CONSECUTIVE_REQUIRED) and (not arming.fired)
    if should_fire then arming.fired = true end

    return {
        action      = should_fire and "FIRE" or "checked",
        elapsed     = elapsed,
        plc         = plc,
        hunter      = hunter,
        trip_plc    = trip_plc,
        trip_hunter = trip_hunter,
        consecutive = arming.consecutive,
        fired       = arming.fired,
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
    schedule        TEXT,
    station_step    INTEGER,
    elapsed_min     INTEGER,
    plc_gpm         REAL,
    hunter_gpm      REAL,
    trip_plc        INTEGER,
    trip_hunter     INTEGER,
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
    schedule        TEXT,
    station_step    INTEGER,
    elapsed_min     INTEGER,
    plc_gpm         REAL,
    hunter_gpm      REAL,
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
            ts_ms, bin, schedule, station_step, elapsed_min,
            plc_gpm, hunter_gpm, trip_plc, trip_hunter, consecutive,
            in_warmup, fired, action)
        VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    ]])
    if not stmt then return nil, db:errmsg() end
    stmt:bind_values(fields.ts_ms, fields.bin, fields.schedule,
        fields.station_step, fields.elapsed_min,
        fields.plc_gpm, fields.hunter_gpm,
        fields.trip_plc and 1 or 0, fields.trip_hunter and 1 or 0,
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
            ts_ms, bin, schedule, station_step, elapsed_min,
            plc_gpm, hunter_gpm, actions_sent, armed, note)
        VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    ]])
    if not stmt then return nil, db:errmsg() end
    stmt:bind_values(fields.ts_ms, fields.bin, fields.schedule,
        fields.station_step, fields.elapsed_min,
        fields.plc_gpm, fields.hunter_gpm,
        fields.actions_sent or "",
        fields.armed and 1 or 0,
        fields.note)
    stmt:step()
    stmt:finalize()
    return true
end

return M
