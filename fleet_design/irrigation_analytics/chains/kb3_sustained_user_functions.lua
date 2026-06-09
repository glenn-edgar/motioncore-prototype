-- chains/kb3_sustained_user_functions.lua — KB3_TICK handler.
--
-- Per tick:
--   1. Poll past_actions on private cursor for STATION_START / STEP_COMPLETE /
--      SKIP_OPERATION events. Maintain `arming` state per station.
--      - STATION_START on ETO bin → reset arming (consecutive=0, fired=false,
--        prev_elapsed=nil, bin_key=...)
--      - STATION_START on non-ETO bin → arming = nil (skip during this run)
--      - STEP_COMPLETE / SKIP_OPERATION → arming = nil
--   2. If arming is active (= ETO bin running), fetch popup.
--   3. Call KB3.evaluate_step(arming, popup.ELASPED_TIME, plc, hunter).
--      Returns one of:
--        action = "no_change"     → same minute, skip
--        action = "warmup"        → elapsed < 5, log + continue
--        action = "checked"       → evaluated, didn't fire
--        action = "FIRE"          → 3 consecutive crossed, time to actuate
--        action = "fired_already" → log only
--   4. Write evals_kb3 row for every minute-transition tick.
--   5. On FIRE: dispatch CLOSE_MASTER_VALVE + SKIP_STATION via ws_command,
--      push Discord, write runs_kb3 row.
--
-- Per-minute log lines:
--   kb3 [bin] minute=N PLC=X.X HUNTER=X.X (warmup|checked|FIRE) cons=N

local cjson         = require("cjson")
local controller    = require("controller_client")
local KB3           = require("kb3_sustained")
local WsCommand     = require("ws_command")
local app_heartbeat = require("app_heartbeat")

local KB3_ARM_KILL = (os.getenv("KB3_ARM_KILL") == "1")

local M = { main = {}, one_shot = {}, boolean = {} }

local DIGEST_TOPIC   = "fleet/notify/digest/daily"
local SCHEMA_NOTIFY  = "fleet.notify.digest/1"
local DEFAULT_POLL_S = 30

local function log(id, fmt, ...)
    io.write(string.format("kb3_sustained [%s]: " .. fmt .. "\n", id.namespace, ...))
    io.flush()
end

local function now_ms() return os.time() * 1000 end

local function push_notify(ps, id, body)
    local payload = cjson.encode({
        schema   = SCHEMA_NOTIFY,
        class    = id.class,
        instance = id.instance,
        body     = body,
    })
    local ok, err = pcall(function() ps:publish(DIGEST_TOPIC, payload) end)
    return ok, err
end

M.one_shot.KB3_TICK = function(handle, _node)
    local bb       = handle.blackboard
    local id, ps   = bb._identity, bb._pubsub
    local cs       = bb._class_spec
    local cfg      = (cs and cs.kb3_sustained) or {}
    local poll_s   = cfg.poll_s or DEFAULT_POLL_S
    local ssh_host = cfg.ssh_host or "pi@irrigation"
    local db_path  = cfg.db_path  or "/var/fleet/kb3/kb3.db"

    -- Threshold overrides from config (optional)
    if cfg.gpm_threshold      then KB3.GPM_THRESHOLD       = cfg.gpm_threshold end
    if cfg.warmup_minutes     then KB3.WARMUP_MINUTES      = cfg.warmup_minutes end
    if cfg.consecutive_required then KB3.CONSECUTIVE_REQUIRED = cfg.consecutive_required end

    -- Init blackboard state
    if not bb._kb3 then
        bb._kb3 = {
            db = nil,
            last_stream_id = nil,
            initialized = false,
            arming = nil,
        }
    end
    local st = bb._kb3

    if not st.db then
        local db, err = KB3.open_db(db_path)
        if not db then
            log(id, "open_db FAILED at %s: %s", db_path, tostring(err))
            app_heartbeat.stamp(handle, "kb3_sustained", "degraded",
                "open_db failed", poll_s)
            return
        end
        st.db = db
        log(id, "db ready at %s (armed=%s, threshold=%.1f GPM, warmup=%d min, consec=%d)",
            db_path, tostring(KB3_ARM_KILL),
            KB3.GPM_THRESHOLD, KB3.WARMUP_MINUTES, KB3.CONSECUTIVE_REQUIRED)
    end
    local db = st.db

    -- Fast-forward past_actions cursor on first tick
    if not st.initialized then
        local tip, _ = controller.past_actions_tip({
            ssh_host = ssh_host, timeout_s = cfg.timeout_s or 8,
        })
        st.last_stream_id = tip
        st.initialized = true
        log(id, "past_actions cursor fast-forwarded to %s", tostring(tip))
    end

    -- Poll past_actions delta — process STATION_START / STEP_COMPLETE / SKIP
    local delta, _ = controller.past_actions_xrange(
        st.last_stream_id, 50,
        { ssh_host = ssh_host, timeout_s = cfg.timeout_s or 8 })
    delta = delta or {}

    for _, ent in ipairs(delta) do
        if ent.action == "IRRIGATION_STATION_START"
           and type(ent.details) == "table" then
            local io_setup = ent.details.io_setup
            local bin_key  = KB3.bin_key(io_setup)
            if KB3.is_eto_bin(io_setup) then
                st.arming = {
                    bin           = bin_key,
                    schedule      = ent.details.schedule_name,
                    station_step  = ent.details.step,
                    started_sid   = ent.stream_id,
                    prev_elapsed  = nil,
                    consecutive   = 0,
                    fired         = false,
                }
                log(id, "STATION_START bin=%s sched=%s step=%s (ETO — armed)",
                    bin_key,
                    tostring(ent.details.schedule_name),
                    tostring(ent.details.step))
            else
                st.arming = nil
                log(id, "STATION_START bin=%s — non-ETO, skipping",
                    bin_key)
            end
        elseif ent.action == "IRRIGATION_STEP_COMPLETE"
            or ent.action == "SKIP_OPERATION" then
            if st.arming then
                log(id, "STEP_COMPLETE/SKIP bin=%s — disarming",
                    st.arming.bin)
            end
            st.arming = nil
        end
        if ent.stream_id then st.last_stream_id = ent.stream_id end
    end

    -- If nothing armed, just heartbeat
    if not st.arming then
        app_heartbeat.stamp(handle, "kb3_sustained", "ok",
            "idle (no armed ETO bin)", poll_s)
        return
    end

    -- Fetch popup
    local popup, perr = controller.popup_get({
        ssh_host = ssh_host, timeout_s = cfg.timeout_s or 8,
    })
    if not popup then
        log(id, "popup fetch failed: %s", tostring(perr))
        app_heartbeat.stamp(handle, "kb3_sustained", "degraded",
            "popup fetch failed", poll_s)
        return
    end

    local elapsed = tonumber(popup.ELASPED_TIME)
    local plc     = tonumber(popup.PLC_FLOW_METER)
    local hunter  = tonumber(popup.FILTERED_HUNTER_VALVE)

    local result = KB3.evaluate_step(st.arming, elapsed, plc, hunter)

    -- Skip silent no-change ticks
    if result.action == "no_change" then
        app_heartbeat.stamp(handle, "kb3_sustained", "ok",
            string.format("bin=%s minute=%s (no change)",
                st.arming.bin, tostring(elapsed)),
            poll_s)
        return
    end

    -- Per-minute log line (the trace Glenn asked for)
    log(id, "bin=%s minute=%s PLC=%s HUNTER=%s %s cons=%d",
        st.arming.bin,
        tostring(elapsed),
        plc    and string.format("%.1f", plc)    or "nil",
        hunter and string.format("%.1f", hunter) or "nil",
        result.action,
        result.consecutive or 0)

    -- Write evaluation row
    KB3.insert_eval(db, {
        ts_ms        = now_ms(),
        bin          = st.arming.bin,
        schedule     = st.arming.schedule,
        station_step = st.arming.station_step,
        elapsed_min  = elapsed,
        plc_gpm      = plc,
        hunter_gpm   = hunter,
        trip_plc     = result.trip_plc,
        trip_hunter  = result.trip_hunter,
        consecutive  = result.consecutive,
        in_warmup    = result.in_warmup,
        fired        = result.fired,
        action       = result.action,
    })

    -- FIRE path
    if result.action == "FIRE" then
        local actions_sent = {}
        if KB3_ARM_KILL then
            -- CLOSE_MASTER_VALVE first (water off) then SKIP_STATION
            for _, action in ipairs({ "CLOSE_MASTER_VALVE", "SKIP_STATION" }) do
                local ok, code, err = WsCommand.post(action, {
                    schedule_name = st.arming.schedule or "",
                    step          = tostring(st.arming.station_step or ""),
                    run_time      = "",
                    logger        = function(m) log(id, "[ws] %s", m) end,
                })
                log(id, "ws_command %s → ok=%s code=%s err=%s",
                    action, tostring(ok), tostring(code), tostring(err))
                actions_sent[#actions_sent+1] = string.format("%s(%s)",
                    action, tostring(ok))
            end
        else
            log(id, "MONITOR-ONLY: KB3_ARM_KILL not set, actions suppressed")
        end

        local body = string.format(
            "🚨 KB3 SUSTAINED LEAK — %s\nschedule=%s step=%s minute=%d\nPLC=%.1f GPM  HUNTER=%.1f GPM\n3 consecutive minutes > %.0f GPM\n%s",
            st.arming.bin,
            tostring(st.arming.schedule),
            tostring(st.arming.station_step),
            elapsed or 0,
            plc or 0, hunter or 0,
            KB3.GPM_THRESHOLD,
            KB3_ARM_KILL and ("ACTUATED: " .. table.concat(actions_sent, ", "))
                         or "MONITOR-ONLY (KB3_ARM_KILL not set)")
        local nok, nerr = push_notify(ps, id, body)
        if not nok then
            log(id, "Discord push FAILED: %s", tostring(nerr))
        end

        KB3.insert_fire(db, {
            ts_ms        = now_ms(),
            bin          = st.arming.bin,
            schedule     = st.arming.schedule,
            station_step = st.arming.station_step,
            elapsed_min  = elapsed,
            plc_gpm      = plc,
            hunter_gpm   = hunter,
            actions_sent = table.concat(actions_sent, ","),
            armed        = KB3_ARM_KILL,
            note         = string.format("3 consec > %.0f GPM (PLC trip=%s HUNTER trip=%s)",
                KB3.GPM_THRESHOLD,
                tostring(result.trip_plc),
                tostring(result.trip_hunter)),
        })

        log(id, "FIRED bin=%s minute=%d PLC=%.1f HUNTER=%.1f armed=%s",
            st.arming.bin, elapsed or 0, plc or 0, hunter or 0,
            tostring(KB3_ARM_KILL))
    end

    app_heartbeat.stamp(handle, "kb3_sustained", "ok",
        string.format("bin=%s minute=%s plc=%.1f hunter=%.1f cons=%d %s",
            st.arming.bin, tostring(elapsed),
            plc or 0, hunter or 0, result.consecutive or 0,
            result.action),
        poll_s)
end

M.registry = { main = M.main, one_shot = M.one_shot, boolean = M.boolean }
return M
