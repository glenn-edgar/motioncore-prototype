-- lib/well_drawdown.lua — well-running-dry detector (MONITOR-ONLY).
--
-- Runs in PARALLEL with KB3's sustained-leak check (Glenn 2026-06-12), reading
-- the same live per-minute well-source flow KB3 already fetches
-- (popup.PLC_FLOW_METER == main_flow_meter == the WELL SOURCE, raw/low-latency).
--
-- WHY raw PLC, not HUNTER: FILTERED_HUNTER_VALVE has processing lag, so it
-- trails a real collapse by minutes — useless for a protective abort. The well
-- source (main_flow_meter) leads (today 4:11 it hit 0 within 2 min while HUNTER
-- still read 6-7). Trigger off the leader.
--
-- WHY a drawdown signature, not "wait for zero": by the time the source reads 0
-- the pump's already run dry and the next station is starved. Catch the
-- DRAWDOWN — the source falling below its own in-run plateau and the well's
-- recent cycle capacity — and act before the collapse.
--
-- SELF-REFERENCING, so it needs NO clean-run baselines (the field data is
-- broken-pipe contaminated, and leaks push flow UP, never down — so the low-flow
-- region a dying well lives in is exactly the part contamination can't corrupt):
--   * plateau  = the source's own settled level early THIS run.
--   * cycle_capacity = the well's recently demonstrated plateau across stations
--                      this cycle (passed in by the caller). Handles an
--                      already-dry START (no healthy plateau of its own).
--
-- MONITOR-ONLY: never pushes, never skips. It logs what it WOULD do (rpush the
-- 15-min `wait` recovery step + SKIP_STATION). The caller pcall-isolates it so a
-- bad tick can never disturb the armed leak detector sharing KB3's tick. The
-- rpush/skip actuation is wired separately, later, only after this is proven.

local M = {}

-- ---- tunables (GPM, minutes) — NOT load-bearing in monitor mode; we set the
-- real values from the logged data before arming. -------------------------
M.WARMUP_MIN        = 5      -- ignore the ramp
M.PLATEAU_FROM      = 5      -- establish the plateau over post-warmup minutes
M.PLATEAU_TO        = 12     -- 5..12 inclusive
M.PLATEAU_MIN_N     = 3
M.DRAW_FRAC         = 0.70   -- source below 70% of its plateau = drawdown
M.FLOOR_FRAC        = 0.50   -- source below 50% of cycle capacity = floor
M.WINDOW            = 4      -- sustained = HITS of the last WINDOW minutes...
M.WINDOW_HITS       = 3      -- ...are below (windowed, not strict-consecutive —
                             -- the dying-well sawtooth bounces, so consecutive
                             -- would miss it)
M.GUARD_REMAIN_MIN  = 1      -- don't act if the step is <= this from completing

-- The exact recovery step we WOULD rpush (byte-matched to a live
-- IRRIGATION_PENDING job; satellite_1:39 = city water, no well draw, 15 min).
M.WAIT_JOB = '{"type":"IRRIGATION_STEP","schedule_name":"wait","step":0,' ..
    '"io_setup":[{"remote":"satellite_1","bits":[39]}],"run_time":15,' ..
    '"elasped_time":0,"eto_enable":false,"eto_list":null,"eto_flag":false}'

local function median(t)
    local n = #t
    if n == 0 then return nil end
    local c = {}
    for i = 1, n do c[i] = t[i] end
    table.sort(c)
    if n % 2 == 1 then return c[(n + 1) / 2] end
    return (c[n / 2] + c[n / 2 + 1]) / 2
end

-- Fresh per-station state. Call on STATION_START.
function M.new_station()
    return {
        plateau_samples = {},
        plateau         = nil,
        recent          = {},   -- ring of last WINDOW below-flags
        triggered       = false,
    }
end

-- One per-minute observation. Returns a result table for the caller to log:
--   { phase, plateau, plc, frac, below, hits, would_trigger, guard_ok, reason }
-- phase ∈ no_plc | warmup | building | monitor
function M.observe(state, plc, elapsed, opts)
    opts = opts or {}
    local out = { phase = "monitor", plc = plc }
    plc = tonumber(plc)
    elapsed = tonumber(elapsed)
    if not plc or not elapsed then out.phase = "no_plc"; return out end
    if elapsed < M.WARMUP_MIN then out.phase = "warmup"; return out end

    -- establish / refine the plateau over the early window
    if elapsed >= M.PLATEAU_FROM and elapsed <= M.PLATEAU_TO then
        state.plateau_samples[#state.plateau_samples + 1] = plc
        if #state.plateau_samples >= M.PLATEAU_MIN_N then
            state.plateau = median(state.plateau_samples)
        end
    end
    if not state.plateau then out.phase = "building"; return out end
    out.plateau = state.plateau

    -- below either reference?
    local draw  = plc < M.DRAW_FRAC * state.plateau
    local cap   = tonumber(opts.cycle_capacity)
    local floor = cap and plc < M.FLOOR_FRAC * cap or false
    local below = draw or floor
    out.frac   = plc / state.plateau
    out.below  = below
    out.draw   = draw
    out.floor  = floor

    -- windowed sustain
    local r = state.recent
    r[#r + 1] = below and 1 or 0
    while #r > M.WINDOW do table.remove(r, 1) end
    local hits = 0
    for _, v in ipairs(r) do hits = hits + v end
    out.hits = hits

    -- guard: don't act if the step is about to finish anyway
    local run_time = tonumber(opts.run_time)
    out.guard_ok = (not run_time) or (run_time - elapsed > M.GUARD_REMAIN_MIN)

    if hits >= M.WINDOW_HITS and not state.triggered and out.guard_ok then
        state.triggered = true
        out.would_trigger = true
        out.reason = floor and "floor(cycle-cap)" or "plateau-drop"
    end
    return out
end

return M
