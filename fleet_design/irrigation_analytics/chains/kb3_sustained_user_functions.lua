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
local KB4V2         = require("kb4_v2")    -- read-only access to baselines_kb4v2 for secondary trip
local NOTIFY        = require("notifications")
local WsCommand     = require("ws_command")
local app_heartbeat = require("app_heartbeat")

local NOTIFY_DB_PATH = os.getenv("NOTIFY_DB_PATH") or "/var/fleet/notify/notifications.db"

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
        st.notify_db = NOTIFY.open_db(NOTIFY_DB_PATH)  -- past-actions log (shared)
        if not st.notify_db then log(id, "notifications log open failed at %s", NOTIFY_DB_PATH) end
        log(id, "db ready at %s (armed=%s, threshold=%.1f GPM, warmup=%d min, consec=%d, secondary=baseline+%.1f after n>=%d)",
            db_path, tostring(KB3_ARM_KILL),
            KB3.GPM_THRESHOLD, KB3.WARMUP_MINUTES, KB3.CONSECUTIVE_REQUIRED,
            KB3.BASELINE_DELTA_GPM, KB3.BASELINE_MIN_N_CLEAN)
    end
    local db = st.db

    -- KB4 v2 baselines DB (read-only access for secondary trip)
    if not st.kb4v2_db then
        local kb4v2_path = cfg.kb4v2_db_path or "/var/fleet/kb4v2/kb4v2.db"
        local kb4_db, kerr = KB4V2.open_db(kb4v2_path)
        if not kb4_db then
            log(id, "kb4v2 db open FAILED at %s: %s — secondary trip disabled",
                kb4v2_path, tostring(kerr))
            st.kb4v2_db = false  -- false = tried-and-failed, don't retry every tick
        else
            st.kb4v2_db = kb4_db
            log(id, "kb4v2 baselines available at %s", kb4v2_path)
        end
    end

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
                local is_city = KB3.is_city_bin(io_setup)

                -- Secondary (per-bin baseline) trip is DORMANT as of
                -- 2026-06-10: the leak signal is now the SMOOTH HUNTER meter,
                -- but KB4 v2 only collects a PLC-frame base_flow_gpm (the
                -- gallons curve). Comparing the Hunter trip signal to a PLC
                -- baseline is a ~1 GPM unit mismatch, so we leave baseline_gpm
                -- nil and run primary-only (absolute Hunter > GPM_THRESHOLD).
                -- Re-enable when a Hunter-GPM per-bin baseline is available.
                local baseline_gpm = nil

                st.arming = {
                    bin           = bin_key,
                    is_city       = is_city,
                    baseline_gpm  = baseline_gpm,
                    schedule      = ent.details.schedule_name,
                    station_step  = ent.details.step,
                    started_sid   = ent.stream_id,
                    prev_elapsed  = nil,
                    consecutive   = 0,
                    fired         = false,
                }
                log(id, "STATION_START bin=%s sched=%s step=%s (ETO%s%s — armed)",
                    bin_key,
                    tostring(ent.details.schedule_name),
                    tostring(ent.details.step),
                    is_city and ", CITY" or "",
                    baseline_gpm
                        and string.format(", baseline=%.1f GPM secondary trip @ %.1f",
                            baseline_gpm, baseline_gpm + KB3.BASELINE_DELTA_GPM)
                        or ", no baseline — primary only")
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

    -- Per-minute log line (the trace Glenn asked for). On city bins also
    -- show city_delta = FHV - PLC (positive = city water flowing).
    if st.arming.is_city and result.city_delta then
        log(id, "bin=%s minute=%s PLC=%s HUNTER=%s city_delta=%+.1f %s cons=%d",
            st.arming.bin,
            tostring(elapsed),
            plc    and string.format("%.1f", plc)    or "nil",
            hunter and string.format("%.1f", hunter) or "nil",
            result.city_delta,
            result.action,
            result.consecutive or 0)
    else
        log(id, "bin=%s minute=%s PLC=%s HUNTER=%s %s cons=%d",
            st.arming.bin,
            tostring(elapsed),
            plc    and string.format("%.1f", plc)    or "nil",
            hunter and string.format("%.1f", hunter) or "nil",
            result.action,
            result.consecutive or 0)
    end

    -- Write evaluation row
    KB3.insert_eval(db, {
        ts_ms          = now_ms(),
        bin            = st.arming.bin,
        is_city        = st.arming.is_city,
        schedule       = st.arming.schedule,
        station_step   = st.arming.station_step,
        elapsed_min    = elapsed,
        plc_gpm        = plc,
        hunter_gpm     = hunter,
        city_delta_gpm = result.city_delta,
        baseline_gpm   = result.baseline_gpm,
        trip_primary   = result.trip_primary,
        trip_secondary = result.trip_secondary,
        trip_path      = result.trip_path,
        consecutive    = result.consecutive,
        in_warmup      = result.in_warmup,
        fired          = result.fired,
        action         = result.action,
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

        local city_tail = st.arming.is_city and result.city_delta
            and string.format("\ncity_delta=%+.1f GPM (FHV-PLC)", result.city_delta)
            or ""
        local trip_desc
        if result.trip_path == "primary" then
            trip_desc = string.format("3 consec min HUNTER > %.0f GPM (primary/absolute)", KB3.GPM_THRESHOLD)
        elseif result.trip_path == "secondary" then
            trip_desc = string.format("3 consec min HUNTER > %.1f GPM (secondary: baseline %.1f + %.1f)",
                (result.baseline_gpm or 0) + KB3.BASELINE_DELTA_GPM,
                result.baseline_gpm or 0, KB3.BASELINE_DELTA_GPM)
        else
            trip_desc = string.format("3 consec min HUNTER > %.0f GPM (both primary AND secondary)", KB3.GPM_THRESHOLD)
        end
        local body = string.format(
            "🚨 KB3 SUSTAINED LEAK — %s%s\nschedule=%s step=%s minute=%d\nPLC=%.1f GPM  HUNTER=%.1f GPM%s\n%s\n%s",
            st.arming.bin,
            st.arming.is_city and " [CITY]" or "",
            tostring(st.arming.schedule),
            tostring(st.arming.station_step),
            elapsed or 0,
            plc or 0, hunter or 0,
            city_tail,
            trip_desc,
            KB3_ARM_KILL and ("ACTUATED: " .. table.concat(actions_sent, ", "))
                         or "MONITOR-ONLY (KB3_ARM_KILL not set)")
        local nok, nerr = push_notify(ps, id, body)
        if not nok then
            log(id, "Discord push FAILED: %s", tostring(nerr))
        end

        -- Past-actions log (the /irrigation/actions page reads this).
        if st.notify_db then
            NOTIFY.record(st.notify_db, {
                ts_ms  = now_ms(), level = "RED", source = "KB3", kind = "LEAK",
                target = st.arming.bin,
                action = KB3_ARM_KILL and table.concat(actions_sent, "+") or "(monitor-only)",
                title  = string.format("KB3 SUSTAINED LEAK %s — HUNTER=%.1f GPM @ min %d",
                    st.arming.bin, hunter or 0, elapsed or 0),
                body   = body,
            })
        end

        KB3.insert_fire(db, {
            ts_ms          = now_ms(),
            bin            = st.arming.bin,
            is_city        = st.arming.is_city,
            schedule       = st.arming.schedule,
            station_step   = st.arming.station_step,
            elapsed_min    = elapsed,
            plc_gpm        = plc,
            hunter_gpm     = hunter,
            city_delta_gpm = result.city_delta,
            baseline_gpm   = result.baseline_gpm,
            trip_path      = result.trip_path,
            actions_sent   = table.concat(actions_sent, ","),
            armed          = KB3_ARM_KILL,
            note           = trip_desc,
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
