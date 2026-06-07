-- chains/kb1_overcurrent_user_functions.lua — KB1_TICK handler.
--
-- Live overcurrent monitor. Per tick:
--   1. Fetch popup (gives PLC_IRRIGATION_CURRENT, SCHEDULE_NAME, etc.)
--   2. Poll past_actions on private cursor to track STATION_START/STEP_COMPLETE
--   3. On STATION_START: set arming.bin_key and compute expected_I from KB2
--   4. On each ACTIVE_RUN tick while armed:
--        a. Read popup.PLC_IRRIGATION_CURRENT
--        b. Classify vs expected (KB1_KILL @ 1.8 A absolute, KB1_WARN @ +0.3 A)
--        c. Edge-trigger: fire once per run per class
--        d. INSERT row in runs_kb1
--        e. Discord push on KB1_KILL only (WARN is DB)
--
-- KB2 baselines are loaded at boot AND refreshed on each STATION_START
-- (cheap query). null_offset is refreshed the same way.
--
-- WSL test phase: NO actuation. Discord only. Future Pi production can
-- add SKIP_STATION dispatch on KB1_KILL via the ws_command path.

local cjson         = require("cjson")
local controller    = require("controller_client")
local KB1           = require("kb1_overcurrent")
local WsCommand     = require("ws_command")
local app_heartbeat = require("app_heartbeat")

-- KB1 arming: only when KB1_ARM_KILL=1 will KB1_KILL events dispatch
-- SKIP_STATION to the controller. Default off — image is identical
-- between WSL (disarmed) and Pi production (armed via env).
local KB1_ARM_KILL = (os.getenv("KB1_ARM_KILL") == "1")

local M = { main = {}, one_shot = {}, boolean = {} }

local DIGEST_TOPIC   = "fleet/notify/digest/daily"
local SCHEMA_NOTIFY  = "fleet.notify.digest/1"
local SCHEMA_KB1     = "irrigation_analytics.kb1/1"
local DEFAULT_POLL_S = 30

local function log(id, fmt, ...)
    io.write(string.format("kb1_overcurrent [%s]: " .. fmt .. "\n", id.namespace, ...))
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

local function bin_valves_from_io(io_setup)
    -- io_setup = { { remote=..., bits={...} }, ... }
    local out = {}
    for _, g in ipairs(io_setup or {}) do
        local sat = g.remote or "?"
        for _, b in ipairs(g.bits or {}) do
            out[#out+1] = sat .. ":" .. tostring(b)
        end
    end
    table.sort(out)
    return out
end

local function bin_key_from_valves(valves)
    return table.concat(valves, "/")
end

local function refresh_kb2(st, kb2_path, id)
    local R_table, n, R_master = KB1.load_kb2_R(kb2_path, 1)
    if R_table then
        st.kb2_R = R_table
        st.kb2_R_master = R_master or st.kb2_R_master or 40.0
        st.kb2_R_n = n
    else
        log(id, "kb2 R load failed at %s", kb2_path)
    end
    local off = KB1.load_kb2_offset(kb2_path)
    if off then st.kb2_offset = off end
end

M.one_shot.KB1_TICK = function(handle, _node)
    local bb       = handle.blackboard
    local id, ps   = bb._identity, bb._pubsub
    local cs       = bb._class_spec
    local cfg      = (cs and cs.kb1_overcurrent) or {}
    local poll_s   = cfg.poll_s or DEFAULT_POLL_S
    local ssh_host = cfg.ssh_host or "pi@irrigation"
    local db_path  = cfg.db_path  or "/var/fleet/kb1/kb1.db"
    local kb2_path = cfg.kb2_db_path or "/var/fleet/kb2/kb2.db"

    if not bb._kb1 then
        bb._kb1 = {
            db = nil, initialized = false,
            last_stream_id = nil,
            arming = nil,
            kb2_R = nil, kb2_R_master = 40.0, kb2_offset = nil, kb2_R_n = 0,
        }
    end
    local st = bb._kb1

    if not st.db then
        local db, err = KB1.open_db(db_path)
        if not db then
            log(id, "open_db FAILED at %s: %s", db_path, tostring(err))
            app_heartbeat.stamp(handle, "kb1_overcurrent", "degraded",
                "open_db failed: " .. tostring(err):sub(1,100), poll_s)
            return
        end
        st.db = db
        log(id, "db ready at %s", db_path)
        refresh_kb2(st, kb2_path, id)
        log(id, "loaded %d KB2 baselines (R_master=%.1f Ω, offset=%s)",
            st.kb2_R_n, st.kb2_R_master,
            st.kb2_offset and string.format("%.4f A", st.kb2_offset) or "nil")
    end
    local db = st.db

    -- 1) popup
    local popup, perr = controller.popup_get({
        ssh_host = ssh_host, timeout_s = cfg.timeout_s or 8,
    })
    if not popup then
        log(id, "popup fetch failed: %s", tostring(perr))
        app_heartbeat.stamp(handle, "kb1_overcurrent", "degraded",
            "popup fetch failed", poll_s)
        return
    end

    -- 2) cursor init / past_actions delta
    if not st.initialized then
        local tip, _ = controller.past_actions_tip({
            ssh_host = ssh_host, timeout_s = cfg.timeout_s or 8,
        })
        st.last_stream_id = tip
        st.initialized = true
        log(id, "past_actions cursor fast-forwarded to %s", tostring(tip))
    end
    local delta, _ = controller.past_actions_xrange(
        st.last_stream_id, 200,
        { ssh_host = ssh_host, timeout_s = cfg.timeout_s or 8 })
    delta = delta or {}

    -- 3) walk delta — arm on STATION_START, disarm on STEP_COMPLETE
    for _, ent in ipairs(delta) do
        if ent.action == "IRRIGATION_STATION_START" and type(ent.details) == "table" then
            local io_setup = ent.details.io_setup
            local valves = bin_valves_from_io(io_setup)
            local bin_key = bin_key_from_valves(valves)
            -- Refresh KB2 R + offset at each STATION_START (cheap)
            refresh_kb2(st, kb2_path, id)
            local expected_I, exp_err, exp_meta = KB1.expected_I_for_bin(
                valves, st.kb2_R, st.kb2_R_master, st.kb2_offset)
            st.arming = {
                bin_key       = bin_key,
                valves        = valves,
                schedule_name = ent.details.schedule_name,
                step          = ent.details.step,
                started_sid   = ent.stream_id,
                expected_I    = expected_I,
                exp_err       = exp_err,
                exp_meta      = exp_meta,
                fired_kill    = false,
                fired_warn    = false,
            }
            log(id, "STATION_START bin=%s schedule=%s step=%s expected_I=%s",
                bin_key, tostring(ent.details.schedule_name),
                tostring(ent.details.step),
                expected_I and string.format("%.3f A (%d coils known)",
                    expected_I, (exp_meta and exp_meta.n_known) or 0) or "nil")
        elseif ent.action == "IRRIGATION_STEP_COMPLETE" then
            st.arming = nil
        elseif ent.action == "SKIP_OPERATION" then
            st.arming = nil
        end
        if ent.stream_id then st.last_stream_id = ent.stream_id end
    end

    -- 4) only evaluate during ACTIVE_RUN with armed bin
    local schedule_name = popup.SCHEDULE_NAME or "OFFLINE"
    local measured_I = tonumber(popup.PLC_IRRIGATION_CURRENT)

    if not st.arming then
        app_heartbeat.stamp(handle, "kb1_overcurrent", "ok",
            string.format("idle schedule=%s", schedule_name), poll_s)
        return
    end

    local arming = st.arming
    local cls, sev, delta_v, note = KB1.classify(measured_I, arming.expected_I)

    -- Always log the per-tick math at INFO level so we can see it working
    log(id, "kb1 [%s]: I=%.3f exp=%s Δ=%s cls=%s",
        arming.bin_key, measured_I or 0,
        arming.expected_I and string.format("%.3f", arming.expected_I) or "nil",
        delta_v and string.format("%+.3f", delta_v) or "nil",
        cls)

    local fire_kill = (cls == "KB1_KILL" and not arming.fired_kill)
    local fire_warn = (cls == "KB1_WARN" and not arming.fired_warn)

    if fire_kill or fire_warn then
        KB1.insert_run(db, {
            ts_ms       = now_ms(),
            sid         = arming.started_sid,
            bin         = arming.bin_key,
            step        = arming.step,
            schedule    = arming.schedule_name,
            I_measured  = measured_I,
            I_expected  = arming.expected_I,
            delta       = delta_v,
            n_known     = arming.exp_meta and arming.exp_meta.n_known,
            n_unknown   = arming.exp_meta and arming.exp_meta.n_unknown,
            null_offset = st.kb2_offset,
            cls         = cls,
            severity    = sev,
            note        = note,
        })
        if fire_kill then
            arming.fired_kill = true
            -- Discord first (always)
            local body = string.format(
                "🚨 KB1 OVERCURRENT KILL — %s\nschedule=%s step=%s\nI=%.3f A > %.1f A absolute threshold\n%s\n%s",
                arming.bin_key, tostring(arming.schedule_name),
                tostring(arming.step), measured_I, KB1.KILL_ABSOLUTE_A,
                note or "",
                KB1_ARM_KILL and "ACTUATED: SKIP_STATION dispatched" or
                                 "MONITOR-ONLY: SKIP_STATION suppressed (KB1_ARM_KILL not set)")
            local ok, err = push_notify(ps, id, body)
            if not ok then log(id, "Discord push FAILED: %s", tostring(err)) end
            log(id, "KILL fired bin=%s I=%.3f armed=%s",
                arming.bin_key, measured_I, tostring(KB1_ARM_KILL))
            -- Actuation: dispatch SKIP_STATION to the controller only when armed.
            -- ws_command itself has a SKIP_LIVE env safety gate; both must be on
            -- for an actual POST. On WSL both are off → log-only end-to-end.
            if KB1_ARM_KILL then
                local ws_ok, ws_code, ws_err = WsCommand.post("SKIP_STATION", {
                    schedule_name = arming.schedule_name or "",
                    step          = tostring(arming.step or ""),
                    run_time      = "",
                    logger        = function(m) log(id, "[ws] %s", m) end,
                })
                log(id, "ws_command SKIP_STATION → ok=%s code=%s err=%s",
                    tostring(ws_ok), tostring(ws_code), tostring(ws_err))
            end
        elseif fire_warn then
            arming.fired_warn = true
            log(id, "WARN fired bin=%s I=%.3f exp=%.3f Δ=%+.3f (DB only)",
                arming.bin_key, measured_I, arming.expected_I, delta_v)
        end
    end

    app_heartbeat.stamp(handle, "kb1_overcurrent", "ok",
        string.format("bin=%s I=%.3f exp=%s cls=%s",
            arming.bin_key, measured_I or 0,
            arming.expected_I and string.format("%.3f", arming.expected_I) or "?",
            cls),
        poll_s)
end

M.registry = { main = M.main, one_shot = M.one_shot, boolean = M.boolean }
return M
