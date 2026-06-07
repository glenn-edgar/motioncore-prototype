-- kb1_overcurrent.lua — live overcurrent detector.
--
-- Reads KB2's per-valve baseline coil R (kb2.db baselines_kb2.R_med),
-- computes expected total current for the bin currently running as
-- expected_I = V/R_master + sum(V/R_zone) + null_offset.
-- Compares to popup.PLC_IRRIGATION_CURRENT each tick. Two classes:
--
--   KB1_KILL        I_measured > 1.8 A absolute (catastrophic — multi-coil
--                   short, harness fault, runaway parallel load)
--                   → Discord; on Pi production also dispatches SKIP_STATION
--                     (gated; WSL test phase has no actuation)
--
--   KB1_WARN        I_measured > expected_I + 0.3 A (partial short, parallel
--                   leak path, coil insulation degradation)
--                   → DB only (no Discord push)
--
-- Calibration (locked 2026-06-06):
--   V_PSU = 15.6 V                                 (verified by meter)
--   R_master (sat_1:43) = ~40 Ω                    (KB2 baseline)
--   null_offset = mean(I[sat_3:1], I[sat_4:6])     (latest valve_test cycle)
--   wire-drop empirical multiplier: 0.93           (applied to expected)
--
-- The math was cross-validated against today's IRRIGATION_CURRENT traces
-- and matches V/R within 0.5-8% across 10 bins.

local cjson = require("cjson")

local M = {}

M.PSU_VOLTAGE       = 15.6
M.KILL_ABSOLUTE_A   = 1.8
M.WARN_DELTA_A      = 0.3
M.WIRE_DROP_FACTOR  = 0.93     -- empirical multiplier on V/R sum (Glenn's data)
M.MISCAL_MIN_R      = 18.0     -- below this, treat as miscal, skip valve
M.MISCAL_MAX_R      = 80.0
M.NULL_VALVES       = { "satellite_3:1", "satellite_4:6" }

-- =========================================================================
-- Compute expected current for a bin
-- =========================================================================
-- bin_valves: { "satellite_X:Y", ... }     (sorted unique)
-- kb2_R:      { ["satellite_X:Y"] = R_ohm, ... }   (last_R from baselines_kb2)
-- R_master:   R of sat_1:43 (or fallback default)
-- null_offset_A: most recent 2-null offset
-- Returns expected current (A), or nil if any required input missing.
function M.expected_I_for_bin(bin_valves, kb2_R, R_master, null_offset_A)
    if not bin_valves or #bin_valves == 0 then return nil, "empty bin" end
    if not R_master or R_master < M.MISCAL_MIN_R then
        return nil, "R_master missing or out of range"
    end

    local sum_inv_R = 1.0 / R_master      -- master always energized
    local n_known = 0
    local n_unknown = 0
    for _, v in ipairs(bin_valves) do
        -- Skip null channels (electrically inactive)
        local is_null = false
        for _, nv in ipairs(M.NULL_VALVES) do
            if v == nv then is_null = true break end
        end
        if not is_null then
            local R = kb2_R and kb2_R[v]
            if R and R >= M.MISCAL_MIN_R and R <= M.MISCAL_MAX_R then
                sum_inv_R = sum_inv_R + (1.0 / R)
                n_known = n_known + 1
            else
                n_unknown = n_unknown + 1
            end
        end
    end

    if n_known == 0 and n_unknown > 0 then
        return nil, "no known coil R in bin"
    end

    local expected = M.PSU_VOLTAGE * sum_inv_R * M.WIRE_DROP_FACTOR
    if null_offset_A then expected = expected + null_offset_A end
    return expected, nil, { n_known = n_known, n_unknown = n_unknown }
end

-- =========================================================================
-- Classify
-- =========================================================================
-- Returns (cls, severity, delta, note)
-- cls: KB1_KILL | KB1_WARN | OK
function M.classify(measured_A, expected_A)
    if not measured_A then return "no_reading", "info", nil, "popup current missing" end

    if measured_A > M.KILL_ABSOLUTE_A then
        return "KB1_KILL", "critical", measured_A - M.KILL_ABSOLUTE_A,
            string.format("I=%.2f A > %.1f A absolute kill threshold", measured_A, M.KILL_ABSOLUTE_A)
    end

    if not expected_A then return "no_expected", "info", nil, "expected_I unavailable" end

    local d = measured_A - expected_A
    if d > M.WARN_DELTA_A then
        return "KB1_WARN", "warn", d,
            string.format("I=%.2f A > expected %.2f A + %.1f A",
                measured_A, expected_A, M.WARN_DELTA_A)
    end

    return "OK", "ok", d, nil
end

-- =========================================================================
-- SQLite
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
CREATE TABLE IF NOT EXISTS runs_kb1 (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    ts_ms           INTEGER NOT NULL,
    sid             TEXT,
    bin             TEXT NOT NULL,
    step            INTEGER,
    schedule        TEXT,
    I_measured      REAL,
    I_expected      REAL,
    delta           REAL,
    n_known         INTEGER,
    n_unknown       INTEGER,
    null_offset     REAL,
    cls             TEXT,
    severity        TEXT,
    note            TEXT
);
CREATE INDEX IF NOT EXISTS idx_runs_kb1_bin ON runs_kb1(bin);
CREATE INDEX IF NOT EXISTS idx_runs_kb1_ts  ON runs_kb1(ts_ms);
CREATE INDEX IF NOT EXISTS idx_runs_kb1_cls ON runs_kb1(cls);
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

function M.insert_run(db, fields)
    local stmt = db:prepare([[
        INSERT INTO runs_kb1(
            ts_ms, sid, bin, step, schedule,
            I_measured, I_expected, delta,
            n_known, n_unknown, null_offset,
            cls, severity, note)
        VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    ]])
    if not stmt then return nil, db:errmsg() end
    stmt:bind_values(fields.ts_ms, fields.sid, fields.bin, fields.step,
        fields.schedule, fields.I_measured, fields.I_expected, fields.delta,
        fields.n_known or 0, fields.n_unknown or 0, fields.null_offset,
        fields.cls, fields.severity, fields.note)
    stmt:step()
    stmt:finalize()
    return true
end

-- =========================================================================
-- KB2 baseline reader (read-only from kb2.db)
-- =========================================================================
-- Returns { ["satellite_X:Y"] = R_med, ... } for valves with n_healthy >= min_n
function M.load_kb2_R(kb2_db_path, min_n)
    min_n = min_n or 1
    local mod, err = ensure_lsqlite3()
    if not mod then return nil, err end
    local db, code, errmsg = mod.open(kb2_db_path)
    if not db then
        return nil, string.format("open %s failed: %s/%s",
            kb2_db_path, tostring(code), tostring(errmsg))
    end
    local out = {}
    local R_master = nil
    local n = 0
    for r in db:nrows(
        "SELECT valve, R_med, n_healthy, last_R FROM baselines_kb2") do
        if r.R_med and r.R_med >= M.MISCAL_MIN_R and r.R_med <= M.MISCAL_MAX_R
           and (r.n_healthy or 0) >= min_n then
            out[r.valve] = r.R_med
            n = n + 1
            if r.valve == "satellite_1:43" then R_master = r.R_med end
        end
    end
    db:close()
    return out, n, R_master
end

-- Read the most recent 2-null offset from kb2.db.
-- The KB2 chain stores the most recent offset_used in runs_kb2; we pick
-- the latest non-null value for any valve at the latest cycle_id.
function M.load_kb2_offset(kb2_db_path)
    local mod, err = ensure_lsqlite3()
    if not mod then return nil, err end
    local db, code, errmsg = mod.open(kb2_db_path)
    if not db then
        return nil, "open kb2 db: " .. tostring(errmsg)
    end
    local off = nil
    for r in db:nrows([[
        SELECT offset_used FROM runs_kb2
        WHERE cycle_id = (SELECT MAX(cycle_id) FROM runs_kb2)
        AND offset_used IS NOT NULL LIMIT 1
    ]]) do
        off = r.offset_used
    end
    db:close()
    return off
end

return M
