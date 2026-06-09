-- kb2_within_run.lua — per-minute coil R trace analysis post-STEP_COMPLETE.
--
-- KB2 extension: when an ETO bin's STEP_COMPLETE fires, fetch the run's
-- IRRIGATION_CURRENT data[] (per-minute), back-derive coil R per minute,
-- and detect within-run anomalies:
--
--   R(t) = 1 / (1/R_total(t) - 1/R_master)
--   where R_total(t) = V_PSU / (I_observed(t) - null_offset)
--   and the bin may contain multiple zones (then we get effective Σ 1/R_zone)
--
-- Detections:
--   R_HEATING_DURING_RUN   linear-fit slope of R over minutes > +0.20 Ω/min
--                          (~10 Ω rise over 50 min = thermal failure margin)
--   R_STEP_DURING_RUN      |R(t+1) - R(t)| > 5 Ω in one minute (intermittent
--                          dropout / coil arcing)
--   R_INSTABILITY           sample-to-sample MAD > 2 Ω (noisy coil)
--   R_END_HIGH              R_end - R_start > 8 Ω (significant aging
--                          signature, valve approaching failure)
--   OK                     no within-run anomaly
--
-- The 5-15 min window is excluded from slope-fit because the start of the
-- run has master+zone pressurizing (transient) and the window after 15 min
-- captures the steady-state thermal trend.

local cjson = require("cjson")
local M = {}

M.PSU_VOLTAGE          = 15.6
M.R_MASTER_DEFAULT     = 40.0
M.HEATING_SLOPE_OHM_PM = 0.20      -- Ω per minute (warn)
M.STEP_DELTA_OHM       = 5.0       -- |ΔR| per minute (warn)
M.INSTABILITY_MAD_OHM  = 2.0       -- MAD over run (warn)
M.END_HIGH_DELTA_OHM   = 8.0       -- R_end - R_start (warn)
M.SLOPE_START_MIN      = 5         -- skip first 5 min (priming transient)
M.SLOPE_END_MIN        = 45        -- cap at 45 min to avoid well-drawdown tail
M.MIN_SAMPLES_FOR_FIT  = 10

-- =========================================================================
-- Per-minute R back-derivation
-- =========================================================================
--
-- bin_valves: list of "satellite_X:Y" in the bin
-- kb2_R:      KB2 baselines (for computing parallel master + other zone coils)
-- For SINGLE-zone bins: R_zone(t) = 1 / (1/R_total(t) - 1/R_master)
-- For MULTI-coil bins (dual, redundant, city-mixed): we can only estimate
--   one aggregate "effective zone R" since we don't have per-coil current.
--   Track this aggregate; the bin baseline absorbs the multi-coil reality.

function M.compute_R_per_minute(I_data, null_offset, R_master, n_zone_coils)
    if not I_data or #I_data == 0 then return nil end
    R_master = R_master or M.R_MASTER_DEFAULT
    n_zone_coils = n_zone_coils or 1
    local out = {}
    for i, I_obs in ipairs(I_data) do
        local I_real = I_obs - (null_offset or 0)
        if I_real > 0.05 then
            local R_total = M.PSU_VOLTAGE / I_real
            -- 1/R_total = 1/R_master + (n_zone)/R_zone_effective
            -- → R_zone_effective = n_zone / (1/R_total - 1/R_master)
            local inv = 1.0/R_total - 1.0/R_master
            if inv > 0 then
                out[i] = n_zone_coils / inv
            else
                out[i] = nil
            end
        else
            out[i] = nil
        end
    end
    return out
end

-- Calibrated variant: takes a KB2 calibration object instead of separate
-- null_offset + hardcoded PSU. When the controller publishes v_psu +
-- sensor_offset (post-2026-06-09 rewrite), this uses them; otherwise
-- the caller passes a legacy_2null calibration built from kb2_resistance's
-- DB cache + M.PSU_VOLTAGE.
--
-- NOTE: IRRIGATION_CURRENT.data[] in TIME_HISTORY is collected during
-- normal runs (not valve_test), so it is RAW regardless of whether the
-- controller upgrade has shipped. The calibration object provides the
-- offset to subtract here in either mode:
--   - "controller" mode: offset = controller_offset (the value the
--     controller measured at sensor null this cycle)
--   - "legacy_2null" mode: offset = 2-null heuristic from valve_test
function M.compute_R_per_minute_calibrated(I_data, calibration, R_master, n_zone_coils)
    if not I_data or #I_data == 0 then return nil end
    if not calibration then return nil end
    R_master = R_master or M.R_MASTER_DEFAULT
    n_zone_coils = n_zone_coils or 1
    local v_psu = calibration.v_psu or M.PSU_VOLTAGE
    -- For TIME_HISTORY we always need to subtract a sensor offset because
    -- IRRIGATION_CURRENT is raw. In controller mode, prefer the published
    -- controller_offset; in legacy mode, the 2-null offset is in
    -- calibration.offset.
    local null_offset
    if calibration.source == "controller" then
        null_offset = calibration.controller_offset or 0
    else
        null_offset = calibration.offset or 0
    end
    local out = {}
    for i, I_obs in ipairs(I_data) do
        local I_real = I_obs - null_offset
        if I_real > 0.05 then
            local R_total = v_psu / I_real
            local inv = 1.0/R_total - 1.0/R_master
            if inv > 0 then
                out[i] = n_zone_coils / inv
            else
                out[i] = nil
            end
        else
            out[i] = nil
        end
    end
    return out
end

-- =========================================================================
-- Stats
-- =========================================================================

local function median(values)
    if not values or #values == 0 then return nil end
    local t = {}
    for _, v in ipairs(values) do t[#t+1] = v end
    table.sort(t)
    local n = #t
    if n % 2 == 1 then return t[math.floor((n+1)/2)] end
    return (t[n/2] + t[n/2+1]) / 2
end

local function mad(values, med)
    if not values or #values == 0 then return nil end
    med = med or median(values)
    local devs = {}
    for _, v in ipairs(values) do devs[#devs+1] = math.abs(v - med) end
    return median(devs)
end

local function linfit_slope(values, start_idx, end_idx)
    -- linear fit over values[start_idx..end_idx]; returns slope (per index)
    local pts = {}
    for i = start_idx, end_idx do
        if values[i] then pts[#pts+1] = { i, values[i] } end
    end
    local n = #pts
    if n < 2 then return 0, 0 end
    local sx, sy = 0, 0
    for _, p in ipairs(pts) do sx = sx + p[1]; sy = sy + p[2] end
    local xm, ym = sx/n, sy/n
    local num, den = 0, 0
    for _, p in ipairs(pts) do
        num = num + (p[1] - xm) * (p[2] - ym)
        den = den + (p[1] - xm)^2
    end
    if den == 0 then return 0, ym end
    local slope = num / den
    local icpt  = ym - slope * xm
    return slope, icpt
end

-- =========================================================================
-- Classify
-- =========================================================================
-- Returns table with all detection results + selected cls + Discord-worthy flag.
function M.analyze_run(R_series)
    local clean = {}
    for _, v in ipairs(R_series or {}) do
        if v and v > 10 and v < 200 then clean[#clean+1] = v end
    end
    if #clean < M.MIN_SAMPLES_FOR_FIT then
        return { cls = "TOO_FEW_SAMPLES", n = #clean }
    end
    local n = #clean

    -- Slope fit over min 5..45 (steady-state window)
    local start_i = math.min(M.SLOPE_START_MIN + 1, n)
    local end_i = math.min(M.SLOPE_END_MIN, n)
    local slope, icpt = linfit_slope(clean, start_i, end_i)

    -- Step detect — max |R(t+1) - R(t)| in run
    local max_step = 0
    local max_step_i = nil
    for i = 2, n do
        if clean[i] and clean[i-1] then
            local d = math.abs(clean[i] - clean[i-1])
            if d > max_step then max_step = d; max_step_i = i end
        end
    end

    -- MAD-based instability
    local med = median(clean)
    local m = mad(clean, med) or 0

    -- End-vs-start
    local n_window = math.min(5, n)
    local sum_start = 0
    for i = 1, n_window do sum_start = sum_start + clean[i] end
    local sum_end = 0
    for i = n - n_window + 1, n do sum_end = sum_end + clean[i] end
    local R_start = sum_start / n_window
    local R_end   = sum_end / n_window
    local end_delta = R_end - R_start

    -- Priority classification (most-severe first)
    local cls = "OK"
    local note = nil
    local severity = "ok"
    if max_step > M.STEP_DELTA_OHM then
        cls = "R_STEP_DURING_RUN"; severity = "alert"
        note = string.format("ΔR=%.1f Ω jump at minute %d (intermittent dropout?)",
            max_step, max_step_i or 0)
    elseif slope > M.HEATING_SLOPE_OHM_PM then
        cls = "R_HEATING_DURING_RUN"; severity = "warn"
        note = string.format("R slope=%+.2f Ω/min (thermal aging — failure margin)",
            slope)
    elseif end_delta > M.END_HIGH_DELTA_OHM then
        cls = "R_END_HIGH"; severity = "warn"
        note = string.format("R_end %.1f − R_start %.1f = %+.1f Ω (significant aging)",
            R_end, R_start, end_delta)
    elseif m > M.INSTABILITY_MAD_OHM then
        cls = "R_INSTABILITY"; severity = "warn"
        note = string.format("MAD=%.1f Ω over run (noisy coil)", m)
    end

    return {
        cls = cls, severity = severity, note = note,
        n = n,
        R_med = med, R_mad = m,
        R_start = R_start, R_end = R_end, end_delta = end_delta,
        slope_ohm_per_min = slope, intercept = icpt,
        max_step_ohm = max_step, max_step_minute = max_step_i,
    }
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
CREATE TABLE IF NOT EXISTS runs_kb2_within (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    ts_ms           INTEGER NOT NULL,
    sid             TEXT,
    bin             TEXT NOT NULL,
    step            INTEGER,
    schedule        TEXT,
    run_time_m      INTEGER,
    n_samples       INTEGER,
    null_offset     REAL,
    R_master        REAL,
    R_start         REAL,
    R_end           REAL,
    end_delta       REAL,
    R_med           REAL,
    R_mad           REAL,
    slope_ohm_pm    REAL,
    intercept_ohm   REAL,
    max_step_ohm    REAL,
    max_step_minute INTEGER,
    cls             TEXT,
    severity        TEXT,
    note            TEXT,
    UNIQUE(sid, bin)
);
CREATE INDEX IF NOT EXISTS idx_runs_kb2_within_bin ON runs_kb2_within(bin);
CREATE INDEX IF NOT EXISTS idx_runs_kb2_within_ts  ON runs_kb2_within(ts_ms);
CREATE INDEX IF NOT EXISTS idx_runs_kb2_within_cls ON runs_kb2_within(cls);
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
        INSERT OR IGNORE INTO runs_kb2_within(
            ts_ms, sid, bin, step, schedule, run_time_m,
            n_samples, null_offset, R_master,
            R_start, R_end, end_delta,
            R_med, R_mad, slope_ohm_pm, intercept_ohm,
            max_step_ohm, max_step_minute,
            cls, severity, note)
        VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    ]])
    if not stmt then return nil, db:errmsg() end
    stmt:bind_values(
        fields.ts_ms, fields.sid, fields.bin, fields.step, fields.schedule, fields.run_time_m,
        fields.n_samples, fields.null_offset, fields.R_master,
        fields.R_start, fields.R_end, fields.end_delta,
        fields.R_med, fields.R_mad, fields.slope_ohm_pm, fields.intercept_ohm,
        fields.max_step_ohm, fields.max_step_minute,
        fields.cls, fields.severity, fields.note)
    stmt:step()
    stmt:finalize()
    return true
end

return M
