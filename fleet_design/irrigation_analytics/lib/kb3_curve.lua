-- kb3_curve.lua — ETO-curve-aware live leak detector.
--
-- Sister to lib/kb3_live.lua. Where kb3_live compares filtered flow against
-- a flat per-bin `ref` GPM, kb3_curve uses KB4's per-bin ETO ceiling
-- (baselines_eto.flow_5_15_med) — the peak-pressure 5..15 min envelope.
-- Beyond ~15 min the well naturally droops, so the 5..15 ceiling IS the
-- max flow the sprinklers ever see. Anything above it is a leak.
--
-- WSL-only, monitor-only. NO actuation; NO SKIP_STATION. WARN goes to
-- DB (persistence) only; LEAK goes to Discord + DB.
--
-- Window:  5-sample sliding mean of FILTERED_HUNTER_VALVE.
-- Trigger: avg5 > ceiling + warn_delta  → WARN  (DB only)
--          avg5 > ceiling + leak_delta  → LEAK  (Discord + DB)
-- Eligibility: bin must have a baselines_eto row (long-run ETO bin).
--              Non-ETO short-run bins fall through silently.
--
-- One-shot per run: once a level fires, it stays fired (suppressed) until
-- the next STATION_START resets the session state.

local M = {}

-- ---------------------------------------------------------------------------
-- Tuneables (defaults; class_spec.kb3_curve can override)
-- ---------------------------------------------------------------------------
M.WINDOW_N            = 5     -- sliding-average window
M.LEAK_DELTA_GPM      = 5.0   -- avg5 > ceiling + 5  → Discord
M.WARN_DELTA_GPM      = 2.0   -- avg5 > ceiling + 2  → DB only
M.CEILING_OFFSET_GPM  = 0.0   -- ADDED to ceiling; set negative to force-fire (test)

-- Canonicalize a compound bin_key for lookup. Mirrors
-- thresholds.canonicalize_key / baselines.canonicalize_key.
function M.canonicalize_key(bin_key)
    if not bin_key or type(bin_key) ~= "string" then return bin_key end
    if not bin_key:find("/", 1, true) then return bin_key end
    local parts = {}
    for p in bin_key:gmatch("[^/]+") do parts[#parts+1] = p end
    table.sort(parts)
    return table.concat(parts, "/")
end

-- ---------------------------------------------------------------------------
-- Load baselines_eto from kb4.db
-- ---------------------------------------------------------------------------
-- Reads the existing KB4 schema. Returns
--   { [canonical_bin_key] = { ceiling = number, n_healthy = int } }, n_loaded, nil
-- on success, or (nil, 0, err) on failure.
function M.load_baselines(db_path)
    local ok, lsqlite3 = pcall(require, "lsqlite3")
    if not ok then return nil, 0, "lsqlite3 not available: " .. tostring(lsqlite3) end

    local db, code, errmsg = lsqlite3.open(db_path)
    if not db then
        return nil, 0, string.format("open %s failed: %s/%s",
            db_path, tostring(code), tostring(errmsg))
    end

    local out = {}
    local n = 0
    for r in db:nrows("SELECT bin, flow_5_15_med, n_healthy FROM baselines_eto") do
        local ceiling = tonumber(r.flow_5_15_med)
        if ceiling and ceiling > 0 then
            out[M.canonicalize_key(r.bin)] = {
                ceiling   = ceiling,
                n_healthy = tonumber(r.n_healthy) or 0,
            }
            n = n + 1
        end
    end
    db:close()

    return out, n, nil
end

-- Lookup the ceiling for a bin_key. Returns nil if non-ETO / not in table.
function M.lookup(baselines, bin_key)
    if not baselines or not bin_key then return nil end
    return baselines[M.canonicalize_key(bin_key)]
end

-- ---------------------------------------------------------------------------
-- Per-tick update
-- ---------------------------------------------------------------------------
-- baseline_entry: result of M.lookup (or nil → not eligible)
-- state:          arming.kb3_curve = { samples={}, fired_leak=bool, fired_warn=bool }
-- filt_sample:    tonumber(popup.FILTERED_HUNTER_VALVE)
-- step:           tonumber(popup.STEP) — used for logging only
-- cfg:            { leak_delta_gpm, warn_delta_gpm, ceiling_offset_gpm } (optional)
--
-- Returns a result table for logging (always):
--   { sample, avg5, ceiling, eff_ceiling, win_n, fired_leak, fired_warn,
--     suppressed_leak, suppressed_warn }
-- and an events list (possibly empty) of NEW fires this tick to be appended
-- to the detector's events[] array.
function M.update(baseline_entry, state, filt_sample, step, cfg)
    if not baseline_entry or not state or not filt_sample then return nil, {} end

    local win_n = cfg and cfg.window_n or M.WINDOW_N
    local leak_d = cfg and cfg.leak_delta_gpm or M.LEAK_DELTA_GPM
    local warn_d = cfg and cfg.warn_delta_gpm or M.WARN_DELTA_GPM
    local off    = cfg and cfg.ceiling_offset_gpm or M.CEILING_OFFSET_GPM

    state.samples = state.samples or {}
    state.samples[#state.samples+1] = filt_sample
    while #state.samples > win_n do
        table.remove(state.samples, 1)
    end

    local n = #state.samples
    local sum = 0
    for i = 1, n do sum = sum + state.samples[i] end
    local avg5 = (n > 0) and (sum / n) or 0

    local ceiling     = baseline_entry.ceiling
    local eff_ceiling = ceiling + off

    local result = {
        sample          = filt_sample,
        step            = step or 0,
        avg5            = avg5,
        ceiling         = ceiling,
        eff_ceiling     = eff_ceiling,
        win_n           = n,
        fired_leak      = false,
        fired_warn      = false,
        suppressed_leak = false,
        suppressed_warn = false,
    }

    -- Need a full window before evaluating — single-sample spikes don't fire.
    if n < win_n then return result, {} end

    local events = {}

    if avg5 > eff_ceiling + leak_d then
        if state.fired_leak then
            result.suppressed_leak = true
        else
            result.fired_leak = true
            state.fired_leak = true
        end
    end
    if avg5 > eff_ceiling + warn_d then
        if state.fired_warn then
            result.suppressed_warn = true
        else
            result.fired_warn = true
            state.fired_warn = true
        end
    end

    return result, events  -- caller builds the event(s) via M.event_* below
end

-- ---------------------------------------------------------------------------
-- Event builders. The detector loop wraps these and appends to events[].
-- ---------------------------------------------------------------------------
function M.event_leak(bin_key, result)
    return {
        kind     = "KB3C_LEAK_HIGH",
        level    = "RED",
        action   = nil,        -- monitor-only; no SKIP_STATION
        db_only  = false,      -- Discord + DB
        msg      = string.format(
            "KB3-CURVE LEAK: avg5=%.2f GPM > ceiling %.2f + %.1f at step %d (bin=%s, monitor-only)",
            result.avg5, result.ceiling,
            result.eff_ceiling - result.ceiling + M.LEAK_DELTA_GPM,
            result.step, bin_key),
        bin_key  = bin_key,
        avg5     = result.avg5,
        ceiling  = result.ceiling,
        step     = result.step,
    }
end

function M.event_warn(bin_key, result)
    return {
        kind     = "KB3C_LEAK_WARN",
        level    = "YELLOW",
        action   = nil,
        db_only  = true,       -- DB only, no Discord
        msg      = string.format(
            "KB3-CURVE WARN: avg5=%.2f GPM > ceiling %.2f + %.1f at step %d (bin=%s)",
            result.avg5, result.ceiling,
            result.eff_ceiling - result.ceiling + M.WARN_DELTA_GPM,
            result.step, bin_key),
        bin_key  = bin_key,
        avg5     = result.avg5,
        ceiling  = result.ceiling,
        step     = result.step,
    }
end

return M
