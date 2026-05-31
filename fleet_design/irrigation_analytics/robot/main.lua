-- main.lua — KB1 shadow robot (bare LuaJIT poll loop).
--
-- Shadow mode: observe + alert. No SKIP_STATION. No CLOSE_MASTER_VALVE.
-- No writes to controller's IRRIGATION_PAST_ACTIONS stream. Discord events
-- ARE sent (real channel). All would-have-been actions are logged.
--
-- Layout per cycle (60 s):
--   1. controller.popup_get()             ← current state + samples
--   2. controller.past_actions_xrange()   ← STATION_START / STEP_COMPLETE / etc.
--   3. update arming from past_actions delta (io_setup, eto_restriction)
--   4. state_machine.classify(popup, manual_suspend)
--   5. detect state edges, log KB2 would-have-been-enabled markers
--   6. modes.evaluate(state, popup, arming, last)
--   7. log everything; send Discord per event
--   8. sleep until next poll
--
-- Env contract:
--   POLL_INTERVAL_S       optional, default 60
--   MANUAL_SUSPEND        optional, "1" forces SUSPENDED(MANUAL) for testing
--   DISCORD_WEBHOOK_URL   optional, omit → log-only
--   KB1_THRESHOLDS_JSON   optional path to per-bin curves
--   VAR_DIR               optional, default ./var

local ffi = require("ffi")
ffi.cdef[[
    int usleep(unsigned int usec);
]]

local script_dir = (arg and arg[0] and arg[0]:match("(.*/)")) or "./"
package.path = script_dir .. "lib/?.lua;"
            .. script_dir .. "../../server/notification_service/lib/?.lua;"
            .. script_dir .. "../../vendor/lua/?.lua;"
            .. package.path

local cjson      = require("cjson")
local Controller = require("controller")
local SM         = require("state_machine")
local Modes      = require("modes")
local Logger     = require("logger")
local Discord    = require("discord")
local T          = require("thresholds")

-- ---------------------------------------------------------------------------
-- Config
-- ---------------------------------------------------------------------------

local POLL_S          = tonumber(os.getenv("POLL_INTERVAL_S") or "30")
local MANUAL_SUSPEND  = os.getenv("MANUAL_SUSPEND") == "1"
local VAR_DIR         = os.getenv("VAR_DIR") or (script_dir .. "var")
local CURVES_PATH     = os.getenv("KB1_THRESHOLDS_JSON")
                        or (script_dir .. "../explore/kb1_thresholds.json")

os.execute("mkdir -p " .. VAR_DIR)

local poll_log, err1   = Logger.open(VAR_DIR .. "/kb1.log")
if not poll_log then io.stderr:write("FATAL: open kb1.log: " .. err1 .. "\n"); os.exit(1) end
local event_log, err2  = Logger.open(VAR_DIR .. "/kb1_events.log")
if not event_log then io.stderr:write("FATAL: open kb1_events.log: " .. err2 .. "\n"); os.exit(1) end

-- ---------------------------------------------------------------------------
-- Load per-bin curves (KB2 will own this later; we read explore's file).
-- The explore file's per_bin schema:
--   { mu_i_asym, sd_i_asym, i_low_open, spike_thresh, ... }
-- We map → { mu, sd, i_low_open }.
-- ---------------------------------------------------------------------------
local curves = {}
do
    local fh = io.open(CURVES_PATH, "r")
    if fh then
        local raw = fh:read("*a")
        fh:close()
        local ok, decoded = pcall(cjson.decode, raw)
        if ok and decoded and decoded.per_bin then
            for bin_key, e in pairs(decoded.per_bin) do
                if e.mu_i_asym and e.i_low_open then
                    curves[bin_key] = {
                        mu        = e.mu_i_asym,
                        sd        = e.sd_i_asym,
                        i_low_open = e.i_low_open,
                    }
                end
            end
            io.stderr:write(string.format(
                "KB1 SHADOW: loaded %d calibrated bins from %s\n",
                (function() local n = 0; for _ in pairs(curves) do n=n+1 end; return n end)(),
                CURVES_PATH))
        else
            io.stderr:write(string.format(
                "KB1 SHADOW: curve file load failed (decode error or missing per_bin), running uncalibrated: %s\n",
                CURVES_PATH))
        end
    else
        io.stderr:write(string.format(
            "KB1 SHADOW: no curve file at %s — all bins uncalibrated\n",
            CURVES_PATH))
    end
end

-- ---------------------------------------------------------------------------
-- Persistent state across polls
-- ---------------------------------------------------------------------------

local last_stream_id_path = VAR_DIR .. "/last_stream_id"
local function load_last_stream_id()
    local fh = io.open(last_stream_id_path, "r")
    if not fh then return nil end
    local s = fh:read("*l"); fh:close()
    if s and s ~= "" then return s end
end
local function save_last_stream_id(id)
    if not id or id == "" then return end
    local fh = io.open(last_stream_id_path, "w")
    if fh then fh:write(id); fh:close() end
end

local last_stream_id = load_last_stream_id()
local prev_state = nil
local last_sample = nil   -- { irr_current, eq_current } previous poll
local arming = nil        -- { bin_key, curve, eto_restriction_seen, samples_seen,
                          --   started_at, started_at_ts,
                          --   irr_window, last_accepted_ts, sent_kinds }
local master_idle = nil   -- { armed_ts, irr_window, last_accepted_ts, sent_kinds }
                          -- nil whenever state ≠ MASTER_IDLE_CHECK

-- Shadow validation only watches state changes going forward. Fast-forward
-- the cursor to the current tip if we have no saved cursor.
if not last_stream_id then
    local tip, terr = Controller.past_actions_tip()
    if tip and tip ~= "" then
        last_stream_id = tip
        save_last_stream_id(last_stream_id)
        io.stderr:write("KB1 SHADOW: fast-forwarded past_actions cursor to " .. tip .. "\n")
    else
        io.stderr:write("KB1 SHADOW: past_actions_tip failed: " .. (terr or "nil") .. "\n")
    end
end

io.stderr:write(string.format(
    "KB1 SHADOW: starting — poll=%ds, var=%s, curves=%d, manual_suspend=%s\n",
    POLL_S, VAR_DIR,
    (function() local n=0; for _ in pairs(curves) do n=n+1 end; return n end)(),
    tostring(MANUAL_SUSPEND)))

-- ---------------------------------------------------------------------------
-- One poll cycle
-- ---------------------------------------------------------------------------

local function poll_once()
    local cycle = { t = os.date("!%Y-%m-%dT%H:%M:%SZ") }

    -- 1) popup
    local popup, perr = Controller.popup_get()
    if not popup then
        cycle.error = perr
        poll_log:write(cycle)
        return
    end
    cycle.popup = {
        SCHEDULE_NAME           = popup.SCHEDULE_NAME,
        STEP                    = popup.STEP,
        RUN_TIME                = popup.RUN_TIME,
        ELASPED_TIME            = popup.ELASPED_TIME,
        MASTER_VALVE            = popup.MASTER_VALVE,
        SUSPEND                 = popup.SUSPEND,
        PLC_IRRIGATION_CURRENT  = popup.PLC_IRRIGATION_CURRENT,
        PLC_EQUIPMENT_CURRENT   = popup.PLC_EQUIPMENT_CURRENT,
        TIME_STAMP              = popup.TIME_STAMP,
    }

    -- 2) past_actions delta
    local delta, aerr = Controller.past_actions_xrange(last_stream_id, 200)
    if not delta then
        cycle.past_actions_error = aerr
        delta = {}
    end
    cycle.past_actions_n = #delta

    -- 3) update arming from delta
    for _, ent in ipairs(delta) do
        if ent.action == "IRRIGATION_STATION_START" and type(ent.details) == "table" then
            local io_setup = ent.details.io_setup
            local bin_key  = T.canonicalize_io_setup(io_setup)
            arming = {
                bin_key                = bin_key,
                io_setup               = io_setup,
                curve                  = T.lookup_curve(curves, bin_key),
                eto_restriction_seen   = false,
                samples_seen           = 0,
                started_at             = ent.stream_id,
                started_at_ts          = os.time(),
                schedule_name          = ent.details.schedule_name,
                step                   = ent.details.step,
                irr_window             = {},   -- median window for warn-tier
                last_accepted_ts       = nil,
                sent_kinds             = {},   -- per-session Discord cooldown
            }
        elseif ent.action == "IRRIGATION_STEP_COMPLETE" then
            cycle.kb2_would_enable = (cycle.kb2_would_enable or {})
            cycle.kb2_would_enable[#cycle.kb2_would_enable+1] = {
                chain = "kb2.update_curve",
                bin_key = arming and arming.bin_key,
            }
            arming = nil
        elseif ent.action == "SKIP_OPERATION" then
            arming = nil    -- disarm but no curve update
        elseif ent.action == "IRRIGATION_ETO_RESTRICTION" then
            if arming then arming.eto_restriction_seen = true end
        end
        if ent.stream_id then last_stream_id = ent.stream_id end
    end

    -- 4) state classification
    local state = SM.classify(popup, MANUAL_SUSPEND)
    cycle.state = state

    -- 4b) track MASTER_IDLE_CHECK session (warm-up timestamp + window + cooldown)
    if state == SM.states.MASTER_IDLE_CHECK then
        if prev_state ~= SM.states.MASTER_IDLE_CHECK then
            master_idle = {
                armed_ts         = os.time(),
                irr_window       = {},
                last_accepted_ts = nil,
                sent_kinds       = {},
            }
        end
    else
        master_idle = nil
    end

    -- 5) edges (kb2 trigger on RESISTANCE exit)
    local edges = SM.edges(prev_state, state)
    if #edges > 0 then cycle.edges = edges end
    for _, ed in ipairs(edges) do
        if ed == "KB2_RUN_RESISTANCE_ANALYSIS" then
            cycle.kb2_would_enable = (cycle.kb2_would_enable or {})
            cycle.kb2_would_enable[#cycle.kb2_would_enable+1] = {
                chain = "kb2.run_resistance_analysis",
            }
        end
    end

    -- 6) advance sample counter for sample-0 skip
    if state == SM.states.ACTIVE_RUN and arming then
        arming.samples_seen = (arming.samples_seen or 0) + 1
    end

    -- 6b) gate-by-TIME_STAMP: push current irr_current into the active
    -- session's rolling-median window only when the controller has
    -- published a fresh measurement. PLC_IRRIGATION_CURRENT updates ~1/min
    -- while poll cadence is 30s, so half the polls see the same value.
    -- See thresholds.SAMPLE_DEDUP_TS_GAP_S.
    local cur_ts = tonumber(popup.TIME_STAMP)
    local irr    = tonumber(popup.PLC_IRRIGATION_CURRENT) or 0
    local active_window
    if state == SM.states.ACTIVE_RUN and arming then
        active_window = arming
    elseif state == SM.states.MASTER_IDLE_CHECK and master_idle then
        active_window = master_idle
    end
    local sample_accepted = false
    if active_window then
        local last_ts = active_window.last_accepted_ts
        if (not last_ts) or (cur_ts and (cur_ts - last_ts) >= T.SAMPLE_DEDUP_TS_GAP_S) then
            T.push_window(active_window.irr_window, irr, T.MEDIAN_WINDOW_N)
            active_window.last_accepted_ts = cur_ts
            sample_accepted = true
        end
    end
    local irr_median   = active_window and T.rolling_median(active_window.irr_window) or nil
    local irr_window_n = active_window and #active_window.irr_window or 0
    if active_window then
        cycle.window = {
            n               = irr_window_n,
            median          = irr_median,
            sample_accepted = sample_accepted,
            last_ts         = active_window.last_accepted_ts,
        }
    end

    -- 7) evaluate modes
    local events = Modes.evaluate(state, popup, arming, last_sample, {
        now_ts = os.time(),
        master_idle_armed_ts = master_idle and master_idle.armed_ts,
        irr_median           = irr_median,
        irr_window_n         = irr_window_n,
    }) or {}
    if #events > 0 then cycle.events = events end
    -- record warmup remaining for log visibility
    local warmup_left = nil
    if state == SM.states.ACTIVE_RUN and arming and arming.started_at_ts then
        local left = T.WARMUP_S - (os.time() - arming.started_at_ts)
        if left > 0 then warmup_left = left end
    elseif state == SM.states.MASTER_IDLE_CHECK and master_idle then
        local left = T.WARMUP_S - (os.time() - master_idle.armed_ts)
        if left > 0 then warmup_left = left end
    end
    if warmup_left then cycle.warmup_s_remaining = warmup_left end

    -- 8) record sample for next cycle (sustained-N comparison)
    last_sample = {
        irr_current = tonumber(popup.PLC_IRRIGATION_CURRENT) or 0,
        eq_current  = tonumber(popup.PLC_EQUIPMENT_CURRENT)  or 0,
    }
    cycle.arming_summary = arming and {
        bin_key      = arming.bin_key,
        calibrated   = arming.curve ~= nil,
        samples_seen = arming.samples_seen,
        eto_suppressed = arming.eto_restriction_seen,
    } or nil

    poll_log:write(cycle)

    -- 9) Discord per event (with per-session cooldown for warn-tier).
    -- RED events bypass cooldown — hard trips fire every time.
    -- YELLOW events fire ONCE per (session, kind); the session is
    -- arming for ACTIVE_RUN events, master_idle for MASTER_IDLE_CHECK
    -- events, nothing for IDLE-state EQ_WARN (the latter falls through
    -- as un-suppressible for now).
    local function session_for(ev)
        if state == SM.states.ACTIVE_RUN then return arming end
        if state == SM.states.MASTER_IDLE_CHECK then return master_idle end
        return nil
    end
    for _, ev in ipairs(events) do
        local sess = session_for(ev)
        local suppressed = false
        if ev.level == "YELLOW" and sess and sess.sent_kinds then
            if sess.sent_kinds[ev.kind] then
                suppressed = true
            end
        end
        local ok, derr
        if suppressed then
            ok, derr = false, "cooldown_suppressed"
        else
            local content = Discord.format_event(ev, {
                state    = state,
                schedule = popup.SCHEDULE_NAME,
                step     = popup.STEP,
                bin_key  = arming and arming.bin_key,
            })
            ok, derr = Discord.send(content, {
                logger = function(m) io.stderr:write("[discord] " .. m .. "\n") end,
            })
            if ok and ev.level == "YELLOW" and sess and sess.sent_kinds then
                sess.sent_kinds[ev.kind] = true
            end
        end
        event_log:write({
            event                 = ev,
            sent                  = ok,
            cooldown_suppressed   = suppressed or nil,
            send_err              = (not ok and not suppressed) and derr or nil,
            state                 = state,
            bin_key               = arming and arming.bin_key,
            window_n              = irr_window_n,
            irr_median            = irr_median,
        })
    end

    prev_state = state
    save_last_stream_id(last_stream_id)
end

-- ---------------------------------------------------------------------------
-- Main loop
-- ---------------------------------------------------------------------------

while true do
    local okp, perr = pcall(poll_once)
    if not okp then
        poll_log:write({ fatal = "poll_once raised: " .. tostring(perr) })
        io.stderr:write("KB1 SHADOW: poll_once raised: " .. tostring(perr) .. "\n")
    end
    ffi.C.usleep(POLL_S * 1000000)
end
