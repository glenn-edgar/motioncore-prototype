-- kb3_live.lua — per-bin live-flow detector.
--
-- Polls the controller's FILTERED_HUNTER_VALVE (the controller's own 5-tap
-- MA — see explore/in_cycle_long_run.py for the recipe we mirror in the
-- post-event analyzer). On each TIME_STAMP-gated sample:
--   - Push into the active bin's rolling sample buffer.
--   - Compute err = sample - baseline.ref
--   - If err > +baseline.kb3_threshold (HIGH only), increment consec;
--     else reset to 0.  KB3 IS HIGH-WATER ONLY by design — low water
--     (blocked emitters, starvation, dropout) does NOT warrant a
--     hard-kill, only HIGH water (pipe break / over-spray) does.
--     Low-water signals are detected by KB4 post-event cumulative.
--   - When consec >= KB3_CONSEC and bin is kb3_eligible and not yet
--     fired-this-run → emit ONE event (hard-kill semantics, run-scoped).
--   - Bin marked fired=true; suppresses further KB3 events for this run.
--
-- Reset: arming.kb3 = { samples = {}, consec = 0, fired = false } on
--        each new STATION_START (handled in main.lua).
--
-- Eligibility:
--   - Bin must be in baselines.json with mode == "long"
--   - Bin must have kb3_eligible == true (excludes physically noisy bins
--     whose 5×stdev > 3.0 GPM — KB4 post-event handles those)
--
-- Note: FILTERED_HUNTER_VALVE is the WHOLE-meter flow when only one
-- valve is active. For concurrent runs (e.g. sat_1:39 + sat_2:16), the
-- baseline.ref already captures the COMBINED flow because baselines.json
-- was generated from the combined-bin TIME_HISTORY. So no decomposition
-- needed — bin_key uniquely identifies the expected baseline flow.

local M = {}

local KB3_CONSEC      = 5    -- 5 consecutive over-threshold samples to fire
local KB3_MAX_BUFFER  = 64   -- keep recent samples for diagnostics

-- Push one filtered-flow sample into the bin's KB3 state. Returns:
--   { fired = bool, suppressed = bool, err = float|nil,
--     consec = int, sample = float, ref = float, threshold = float }
-- nil-fields if not enough info.
--
-- baseline: the per-bin entry from baselines.json
-- kb3_state: arming.kb3 (mutated in place)
-- filtered_sample: tonumber(popup.FILTERED_HUNTER_VALVE)
function M.update(baseline, kb3_state, filtered_sample)
    if not baseline or baseline.mode ~= "long" then return nil end
    if not baseline.kb3_eligible then return nil end
    if not baseline.kb3_threshold or not baseline.ref then return nil end
    if not filtered_sample then return nil end
    if not kb3_state then return nil end

    -- Push sample
    kb3_state.samples = kb3_state.samples or {}
    kb3_state.samples[#kb3_state.samples + 1] = filtered_sample
    if #kb3_state.samples > KB3_MAX_BUFFER then
        table.remove(kb3_state.samples, 1)
    end

    -- Compute error — POSITIVE direction only (HIGH water = pipe break /
    -- over-spray). Negative err (low water) is ignored by KB3 by design;
    -- KB4 cumulative handles blocked-emitter / starvation detection.
    local err = filtered_sample - baseline.ref
    local thresh  = baseline.kb3_threshold

    if err > thresh then
        kb3_state.consec = (kb3_state.consec or 0) + 1
    else
        kb3_state.consec = 0
    end

    local result = {
        sample    = filtered_sample,
        ref       = baseline.ref,
        threshold = thresh,
        err       = err,
        consec    = kb3_state.consec,
        fired     = false,
        suppressed= false,
    }

    if kb3_state.consec >= KB3_CONSEC then
        if kb3_state.fired then
            result.suppressed = true   -- already fired this run, hard-kill is one-shot
        else
            result.fired = true
            kb3_state.fired = true
        end
    end
    return result
end

-- Construct event payload for main.lua to dispatch. Called only when
-- update() returned fired=true.
function M.event(bin_key, baseline, kb3_state, result)
    return {
        kind   = "KB3_PIPE_BREAK_HIGH",
        level  = "RED",
        action = "SKIP_STATION",
        msg    = string.format(
            "KB3 LIVE: filtered flow %.2f GPM HIGH (err +%.2f, thresh %.2f) "
                .. "vs ref %.2f over %d consec samples on bin=%s  "
                .. ">> SKIP_STATION (pipe break / over-spray)",
            result.sample, result.err, result.threshold, result.ref,
            result.consec, bin_key),
        bin_key   = bin_key,
        sample    = result.sample,
        ref       = result.ref,
        threshold = result.threshold,
        err       = result.err,
        consec    = result.consec,
    }
end

return M
