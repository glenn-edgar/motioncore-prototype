-- kb2_resistance.lua — per-valve coil resistance trend detector.
--
-- Reads the IRRIGATION_VALVE_TEST hash from the controller, applies the
-- 2-null offset method (mean of sat_3:1 + sat_4:6), computes calc R via
-- R = 15.6 V / (I_raw - offset), classifies vs rolling-median baseline.
--
-- WSL test phase: monitor-only. No actuation. DB on WARN, Discord on
-- R_DRIFT_ALERT (sustained 3-cycle) and MASTER_RELAY_CREEP. The 5-Ω
-- step-change detector (R_STEP_NOTED) flags maintenance events for
-- the run-log; not an alert.
--
-- Calibration anchors (verified 2026-06-06 PM by meter):
--   PSU = 15.6 V                         (NOT terminal block 14.5-15 V)
--   offset = mean(I[sat_3:1], I[sat_4:6])
--   sat_1:43 = 40.0 Ω measured ~ 40.06 Ω calc → no sensor-scale correction needed
--   sat_1:44 = 23.0 Ω measured ~ 23.17 Ω calc (parallel of two ~46 Ω coils)
--   sat_1:29/30/31 all calc within 1.7 Ω of meter
--
-- Step-change observed in past valve_test history: sat_1:43 dropped from
-- ~46 Ω → ~40 Ω between cycles 15 and 16 (maintenance event in cabinet).
-- R_STEP_NOTED flag exists specifically for this signature.

local cjson = require("cjson")

local M = {}

-- =========================================================================
-- Tuneables
-- =========================================================================
M.PSU_VOLTAGE              = 15.6
M.WARN_DELTA_OHM           = 4.0    -- |Δ| > 4 → DB warn
M.ALERT_DELTA_OHM          = 8.0    -- |Δ| > 8 over 3 cycles → Discord
M.STEP_DELTA_OHM           = 5.0    -- |ΔR_prev| > 5 in one cycle → maintenance flag
M.MASTER_RELAY_CREEP_DELTA = 3.0    -- sat_1:43 upward only: R > baseline + 3 → DB warn
M.MISCAL_LOW               = 18.0
M.MISCAL_HIGH              = 80.0
M.WINDOW_N                 = 15     -- rolling-median window cap
M.ALERT_STREAK_REQUIRED    = 3      -- consecutive cycles in same direction → Discord
M.NEAR_NULL_REJECT_COUNT   = 8      -- >N near-null readings → reject cycle validity

M.NULL_VALVES = { "satellite_3:1", "satellite_4:6" }

-- Per-valve topology metadata. Anything not listed uses defaults.
M.TOPOLOGY = {
    ["satellite_1:43"] = {
        is_master = true,
        note = "master path, single coil ~40 Ω, drift upward = relay aging",
    },
    ["satellite_1:44"] = {
        parallel_valves = 2,
        note = "two ~46 Ω coils in parallel = 23 Ω; half-coil failure → R doubles to ~46",
    },
}

-- 43 driven valves (excludes the 2 nulls used for offset).
M.VALVE_LIST = {
    "satellite_1:17", "satellite_1:29", "satellite_1:30", "satellite_1:31",
    "satellite_1:32", "satellite_1:39", "satellite_1:43", "satellite_1:44",
    "satellite_2:1",  "satellite_2:2",  "satellite_2:3",  "satellite_2:4",
    "satellite_2:5",  "satellite_2:6",  "satellite_2:7",  "satellite_2:11",
    "satellite_2:13", "satellite_2:14", "satellite_2:15", "satellite_2:16",
    "satellite_2:17", "satellite_3:2",  "satellite_3:4",  "satellite_3:5",
    "satellite_3:7",  "satellite_3:11", "satellite_3:12", "satellite_3:13",
    "satellite_3:14", "satellite_3:15", "satellite_3:18", "satellite_3:19",
    "satellite_4:1",  "satellite_4:2",  "satellite_4:3",  "satellite_4:4",
    "satellite_4:7",  "satellite_4:8",  "satellite_4:9",  "satellite_4:10",
    "satellite_4:11", "satellite_4:12", "satellite_4:13",
}

-- =========================================================================
-- Math
-- =========================================================================

function M.compute_offset_2null(currents)
    local n31 = currents[M.NULL_VALVES[1]]
    local n46 = currents[M.NULL_VALVES[2]]
    if not n31 or not n46 then return nil end
    return (n31 + n46) / 2.0
end

function M.compute_R(I_raw, offset)
    if not I_raw or not offset then return nil end
    local Inet = I_raw - offset
    if Inet <= 0 then return nil end
    return M.PSU_VOLTAGE / Inet
end

function M.median(values)
    if not values or #values == 0 then return nil end
    local sorted = {}
    for i, v in ipairs(values) do sorted[i] = v end
    table.sort(sorted)
    local n = #sorted
    if n % 2 == 1 then return sorted[math.floor((n+1)/2)] end
    return (sorted[n/2] + sorted[n/2+1]) / 2.0
end

function M.mad(values, med)
    if not values or #values == 0 then return nil end
    med = med or M.median(values)
    local devs = {}
    for i, v in ipairs(values) do devs[i] = math.abs(v - med) end
    return M.median(devs)
end

function M.push_ring(ring, value, cap)
    ring[#ring+1] = value
    while #ring > cap do table.remove(ring, 1) end
    return ring
end

-- =========================================================================
-- Classification
-- =========================================================================
--
-- Returns (cls, severity, delta_baseline, delta_step, note).
-- cls strings: OK | R_DRIFT_WARN | R_DRIFT_ALERT_CANDIDATE | MASTER_RELAY_CREEP
--              | R_STEP_NOTED | POSSIBLE_CHIP_MISCAL | no_reading
--
-- R_DRIFT_ALERT_CANDIDATE is a single-cycle observation. The KB2_TICK
-- caller bumps a streak counter; once 3 consecutive cycles in the same
-- direction land, it promotes to R_DRIFT_ALERT and pushes Discord.

function M.classify(R_calc, baseline_med, prev_R, bin_key)
    if not R_calc then
        return "no_reading", "info", nil, nil, "I_net <= 0"
    end
    if R_calc < M.MISCAL_LOW or R_calc > M.MISCAL_HIGH then
        return "POSSIBLE_CHIP_MISCAL", "info", nil, nil,
            string.format("R=%.1f outside [%.0f..%.0f]", R_calc, M.MISCAL_LOW, M.MISCAL_HIGH)
    end

    local delta_step = prev_R and (R_calc - prev_R) or nil

    -- Step-change vs the immediate prior cycle = maintenance signature.
    -- Fire even if we have no baseline yet. This is INFO, not WARN.
    if delta_step and math.abs(delta_step) > M.STEP_DELTA_OHM then
        return "R_STEP_NOTED", "info",
            baseline_med and (R_calc - baseline_med) or nil,
            delta_step,
            string.format("ΔR_prev=%+.1f Ω in one cycle (likely maintenance)", delta_step)
    end

    if not baseline_med then
        return "OK", "ok", nil, delta_step, "seeding baseline"
    end

    local d = R_calc - baseline_med

    -- sat_1:43 master path: only upward drift is alarming (relay aging).
    local topo = M.TOPOLOGY[bin_key]
    if topo and topo.is_master and d > M.MASTER_RELAY_CREEP_DELTA then
        return "MASTER_RELAY_CREEP", "warn", d, delta_step,
            string.format("master R %+.1f Ω vs baseline %.1f", d, baseline_med)
    end

    if math.abs(d) > M.ALERT_DELTA_OHM then
        return "R_DRIFT_ALERT_CANDIDATE", "alert", d, delta_step,
            string.format("|Δ|=%.1f Ω > %.1f (alert pending 3-cycle confirm)",
                math.abs(d), M.ALERT_DELTA_OHM)
    end
    if math.abs(d) > M.WARN_DELTA_OHM then
        return "R_DRIFT_WARN", "warn", d, delta_step,
            string.format("|Δ|=%.1f Ω > %.1f", math.abs(d), M.WARN_DELTA_OHM)
    end

    return "OK", "ok", d, delta_step, nil
end

-- =========================================================================
-- SQLite open + schema
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
CREATE TABLE IF NOT EXISTS runs_kb2 (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    ts_ms           INTEGER NOT NULL,
    cycle_id        INTEGER,
    valve           TEXT NOT NULL,
    I_raw           REAL,
    offset_used     REAL,
    R_calc          REAL,
    baseline_used   REAL,
    prev_R          REAL,
    delta_baseline  REAL,
    delta_step      REAL,
    cls             TEXT,
    severity        TEXT,
    note            TEXT,
    UNIQUE(cycle_id, valve)
);
CREATE INDEX IF NOT EXISTS idx_runs_kb2_valve ON runs_kb2(valve);
CREATE INDEX IF NOT EXISTS idx_runs_kb2_ts    ON runs_kb2(ts_ms);
CREATE INDEX IF NOT EXISTS idx_runs_kb2_cls   ON runs_kb2(cls);

CREATE TABLE IF NOT EXISTS baselines_kb2 (
    valve            TEXT PRIMARY KEY,
    R_med            REAL,
    R_mad            REAL,
    n_healthy        INTEGER DEFAULT 0,
    last_updated_ms  INTEGER,
    last_R           REAL,
    window_json      TEXT,
    topology         TEXT,
    note             TEXT
);

CREATE TABLE IF NOT EXISTS alert_state_kb2 (
    valve                  TEXT PRIMARY KEY,
    drift_streak           INTEGER DEFAULT 0,
    last_streak_direction  TEXT,
    last_alert_ts_ms       INTEGER
);

CREATE TABLE IF NOT EXISTS cycle_state_kb2 (
    k     TEXT PRIMARY KEY,
    v     TEXT
);
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

-- =========================================================================
-- Baseline IO
-- =========================================================================

function M.load_baseline(db, valve)
    for r in db:nrows(string.format(
            "SELECT R_med, R_mad, n_healthy, last_R, window_json FROM baselines_kb2 WHERE valve=%q",
            valve)) do
        local ring = {}
        if r.window_json and r.window_json ~= "" then
            local ok, decoded = pcall(cjson.decode, r.window_json)
            if ok and type(decoded) == "table" then ring = decoded end
        end
        return {
            R_med     = r.R_med,
            R_mad     = r.R_mad,
            n_healthy = r.n_healthy or 0,
            last_R    = r.last_R,
            ring      = ring,
        }
    end
    return nil
end

function M.upsert_baseline(db, valve, R_med, R_mad, n_healthy, last_R, ring, now_ms)
    local topo = M.TOPOLOGY[valve]
    local topo_str = topo and cjson.encode(topo) or ""
    local note = topo and topo.note or ""
    local ring_json = cjson.encode(ring or {})

    local stmt = db:prepare([[
        INSERT INTO baselines_kb2(valve, R_med, R_mad, n_healthy, last_updated_ms, last_R, window_json, topology, note)
        VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(valve) DO UPDATE SET
            R_med=excluded.R_med, R_mad=excluded.R_mad,
            n_healthy=excluded.n_healthy, last_updated_ms=excluded.last_updated_ms,
            last_R=excluded.last_R, window_json=excluded.window_json,
            topology=excluded.topology, note=excluded.note
    ]])
    if not stmt then return nil, db:errmsg() end
    stmt:bind_values(valve, R_med or 0, R_mad or 0, n_healthy or 0,
                     now_ms, last_R or 0, ring_json, topo_str, note)
    stmt:step()
    stmt:finalize()
    return true
end

function M.insert_run(db, valve, fields)
    local stmt = db:prepare([[
        INSERT OR IGNORE INTO runs_kb2(
            ts_ms, cycle_id, valve, I_raw, offset_used,
            R_calc, baseline_used, prev_R, delta_baseline, delta_step,
            cls, severity, note)
        VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    ]])
    if not stmt then return nil, db:errmsg() end
    stmt:bind_values(fields.ts_ms, fields.cycle_id, valve,
        fields.I_raw, fields.offset_used,
        fields.R_calc, fields.baseline_used, fields.prev_R,
        fields.delta_baseline, fields.delta_step,
        fields.cls, fields.severity, fields.note)
    stmt:step()
    stmt:finalize()
    return true
end

-- Alert-streak helpers: track consecutive cycles in same direction.
function M.load_alert_state(db, valve)
    for r in db:nrows(string.format(
            "SELECT drift_streak, last_streak_direction FROM alert_state_kb2 WHERE valve=%q",
            valve)) do
        return { streak = r.drift_streak or 0, dir = r.last_streak_direction }
    end
    return { streak = 0, dir = nil }
end

function M.upsert_alert_state(db, valve, streak, dir, last_alert_ts_ms)
    local stmt = db:prepare([[
        INSERT INTO alert_state_kb2(valve, drift_streak, last_streak_direction, last_alert_ts_ms)
        VALUES(?, ?, ?, ?)
        ON CONFLICT(valve) DO UPDATE SET
            drift_streak=excluded.drift_streak,
            last_streak_direction=excluded.last_streak_direction,
            last_alert_ts_ms=COALESCE(excluded.last_alert_ts_ms, alert_state_kb2.last_alert_ts_ms)
    ]])
    if not stmt then return nil, db:errmsg() end
    stmt:bind_values(valve, streak or 0, dir, last_alert_ts_ms)
    stmt:step()
    stmt:finalize()
    return true
end

-- Cycle-state KV (last-seen probe value for new-cycle detection).
function M.cycle_state_get(db, key)
    for r in db:nrows(string.format("SELECT v FROM cycle_state_kb2 WHERE k=%q", key)) do
        return r.v
    end
    return nil
end

function M.cycle_state_set(db, key, value)
    local stmt = db:prepare([[
        INSERT INTO cycle_state_kb2(k, v) VALUES(?, ?)
        ON CONFLICT(k) DO UPDATE SET v=excluded.v
    ]])
    if not stmt then return nil, db:errmsg() end
    stmt:bind_values(key, tostring(value))
    stmt:step()
    stmt:finalize()
    return true
end

return M
