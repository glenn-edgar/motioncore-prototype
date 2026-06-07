-- chains/kb2_resistance_user_functions.lua — KB2_TICK handler.
--
-- Per tick:
--   1. Probe: fetch the latest entry for sat_3:1 (a null channel). Compare
--      to cached probe value. If unchanged → no new cycle → bail.
--   2. New cycle detected → fetch all 43 driven valves + 2 nulls in ONE
--      SSH+Python call (45 hget + msgpack decode, returns JSON dict).
--   3. Compute offset_2null = mean(I[sat_3:1], I[sat_4:6]).
--   4. Per-valve:
--        a. R = 15.6 / (I_raw - offset). nil if I_net <= 0.
--        b. Load prior baseline + alert_state from SQLite.
--        c. Classify (see KB2.classify).
--        d. INSERT row into runs_kb2.
--        e. If OK / R_STEP_NOTED → push R into rolling window, recompute
--           median + MAD, upsert baseline.
--        f. Streak bookkeeping: ALERT_CANDIDATE same-direction streak → if
--           reaches 3 cycles, promote to R_DRIFT_ALERT + Discord.
--        g. MASTER_RELAY_CREEP fires immediately to DB (warn) but also
--           pushes Discord (master is high-stakes).
--   5. Update cycle_state probe cache; stamp app heartbeat.
--
-- All state in blackboard under bb._kb2 for namespacing.

local cjson         = require("cjson")
local KB2           = require("kb2_resistance")
local app_heartbeat = require("app_heartbeat")

local M = { main = {}, one_shot = {}, boolean = {} }

local DIGEST_TOPIC   = "fleet/notify/digest/daily"
local SCHEMA_NOTIFY  = "fleet.notify.digest/1"
local SCHEMA_KB2     = "irrigation_analytics.kb2/1"
local DEFAULT_POLL_S = 60

-- ----------------------------------------------------------------------
-- Helpers
-- ----------------------------------------------------------------------
local function log(id, fmt, ...)
    io.write(string.format("kb2_resistance [%s]: " .. fmt .. "\n", id.namespace, ...))
    io.flush()
end

local function now_ms()
    return os.time() * 1000
end

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

-- Fetch ALL valve_test latest entries in one SSH call. Returns a table
-- { ["satellite_X:Y"] = current_amps, ... } and `cycle_probe` (the
-- latest sat_3:1 value) used for new-cycle detection.
local function fetch_valve_test_latest(opts)
    local VT_DB  = 4
    local VT_KEY = "[SYSTEM:main_operations][SITE:LaCima][APPLICATION_SUPPORT:APPLICATION_SUPPORT][IRRIGIGATION_SCHEDULING_CONTROL:IRRIGIGATION_SCHEDULING_CONTROL][PACKAGE:IRRIGIGATION_SCHEDULING_CONTROL_DATA][HASH:IRRIGATION_VALVE_TEST]"

    -- Build valve list inline (Lua list -> Python JSON list literal).
    local all = {}
    for _, v in ipairs(KB2.VALVE_LIST) do all[#all+1] = v end
    for _, v in ipairs(KB2.NULL_VALVES) do all[#all+1] = v end
    local valves_json = cjson.encode(all)

    local py = string.format([[
import redis, msgpack, json, sys
r = redis.Redis(db=%d)
KEY = %q
VALVES = %s
out = {}
for k in VALVES:
    v = r.hget(KEY, k)
    if v is None:
        continue
    try:
        d = msgpack.unpackb(v, raw=False)
        if isinstance(d, list) and d:
            out[k] = float(d[-1])
    except Exception as e:
        pass
sys.stdout.write(json.dumps(out))
]], VT_DB, VT_KEY, valves_json)

    local tmp = os.tmpname()
    local f = io.open(tmp, "w")
    if not f then return nil, "kb2: cannot open tmp" end
    f:write(py); f:close()
    local cmd = string.format(
        "ssh -o ConnectTimeout=%d -o BatchMode=yes %s 'python3 -' < %s 2>/dev/null",
        opts.timeout_s or 8, opts.ssh_host or "pi@irrigation", tmp)
    local pipe = io.popen(cmd, "r")
    if not pipe then os.remove(tmp); return nil, "kb2: popen failed" end
    local raw = pipe:read("*a") or ""
    pipe:close()
    os.remove(tmp)
    if raw == "" then return nil, "kb2: ssh returned empty" end
    local ok, decoded = pcall(cjson.decode, raw)
    if not ok or type(decoded) ~= "table" then
        return nil, "kb2: decode failed: " .. raw:sub(1, 200)
    end
    return decoded
end

local function direction_of(d)
    if d > 0 then return "up"
    elseif d < 0 then return "down"
    else return nil end
end

-- ----------------------------------------------------------------------
-- KB2_TICK
-- ----------------------------------------------------------------------
M.one_shot.KB2_TICK = function(handle, _node)
    local bb       = handle.blackboard
    local id, ps   = bb._identity, bb._pubsub
    local cs       = bb._class_spec
    local cfg      = (cs and cs.kb2_resistance) or {}
    local poll_s   = cfg.poll_s or DEFAULT_POLL_S
    local ssh_host = cfg.ssh_host or "pi@irrigation"
    local db_path  = cfg.db_path or "/var/fleet/kb2/kb2.db"

    -- Init bb._kb2 state.
    if not bb._kb2 then
        bb._kb2 = { db = nil, initialized = false }
    end
    local st = bb._kb2

    if not st.db then
        local db, err = KB2.open_db(db_path)
        if not db then
            log(id, "open_db FAILED at %s: %s", db_path, tostring(err))
            app_heartbeat.stamp(handle, "kb2_resistance", "degraded",
                "open_db failed: " .. tostring(err):sub(1,100), poll_s)
            return
        end
        st.db = db
        log(id, "db ready at %s", db_path)
    end
    local db = st.db

    -- Detect new cycle by polling sat_3:1's latest.
    local latest, ferr = fetch_valve_test_latest({
        ssh_host  = ssh_host,
        timeout_s = cfg.timeout_s or 8,
    })
    if not latest then
        log(id, "fetch_valve_test_latest FAILED: %s", tostring(ferr))
        app_heartbeat.stamp(handle, "kb2_resistance", "degraded",
            "valve_test fetch failed", poll_s)
        return
    end

    local probe_val = latest[KB2.NULL_VALVES[1]]
    if not probe_val then
        log(id, "probe %s missing in fetch — controller schema mismatch?",
            KB2.NULL_VALVES[1])
        app_heartbeat.stamp(handle, "kb2_resistance", "degraded",
            "probe missing", poll_s)
        return
    end

    local cached = KB2.cycle_state_get(db, "probe_" .. KB2.NULL_VALVES[1])
    local probe_str = string.format("%.6f", probe_val)
    if cached == probe_str then
        -- No new cycle since last poll.
        app_heartbeat.stamp(handle, "kb2_resistance", "ok",
            string.format("idle: probe=%.4f unchanged", probe_val), poll_s)
        return
    end

    -- New cycle detected — process all valves.
    local offset = KB2.compute_offset_2null(latest)
    if not offset then
        log(id, "no offset (null channels missing)")
        app_heartbeat.stamp(handle, "kb2_resistance", "degraded",
            "offset missing", poll_s)
        return
    end

    -- Cycle validity: count "near-null" valves (I_net very small → noise).
    local near_null_n = 0
    for _, v in ipairs(KB2.VALVE_LIST) do
        local I = latest[v]
        if I and (I - offset) < 0.05 then near_null_n = near_null_n + 1 end
    end
    if near_null_n > KB2.NEAR_NULL_REJECT_COUNT then
        log(id, "cycle REJECTED: near_null_count=%d > %d (controller not driving valves)",
            near_null_n, KB2.NEAR_NULL_REJECT_COUNT)
        -- Update probe cache anyway so we don't loop on this bad cycle.
        KB2.cycle_state_set(db, "probe_" .. KB2.NULL_VALVES[1], probe_str)
        app_heartbeat.stamp(handle, "kb2_resistance", "skipped",
            string.format("rejected: %d near-null", near_null_n), poll_s)
        return
    end

    local cycle_ms = now_ms()
    local cycle_id = math.floor(cycle_ms / 1000)
    local processed = 0
    local flagged_warn = 0
    local flagged_alert = 0

    local notify_lines = {}

    for _, valve in ipairs(KB2.VALVE_LIST) do
        local I_raw = latest[valve]
        if I_raw then
            local R = KB2.compute_R(I_raw, offset)
            local baseline = KB2.load_baseline(db, valve)
            local baseline_med = baseline and baseline.R_med or nil
            local prev_R       = baseline and baseline.last_R or nil
            -- First-cycle seeding: no baseline yet, so accept this as first sample.
            if not baseline_med and R then
                baseline_med = R
            end

            local cls, sev, d_base, d_step, note =
                KB2.classify(R, baseline_med, prev_R, valve)

            KB2.insert_run(db, valve, {
                ts_ms          = cycle_ms,
                cycle_id       = cycle_id,
                I_raw          = I_raw,
                offset_used    = offset,
                R_calc         = R,
                baseline_used  = baseline_med,
                prev_R         = prev_R,
                delta_baseline = d_base,
                delta_step     = d_step,
                cls            = cls,
                severity       = sev,
                note           = note,
            })
            processed = processed + 1

            -- Update baseline rolling median on OK / R_STEP_NOTED. Both contribute.
            if R and (cls == "OK" or cls == "R_STEP_NOTED") then
                local ring = baseline and baseline.ring or {}
                KB2.push_ring(ring, R, KB2.WINDOW_N)
                local new_med = KB2.median(ring)
                local new_mad = KB2.mad(ring, new_med)
                KB2.upsert_baseline(db, valve, new_med, new_mad,
                    (baseline and baseline.n_healthy or 0) + 1,
                    R, ring, cycle_ms)
            elseif R then
                -- WARN/ALERT/CREEP: keep baseline, but update last_R for next-cycle step detection.
                local ring = baseline and baseline.ring or {}
                KB2.upsert_baseline(db, valve, baseline_med,
                    baseline and baseline.R_mad or 0,
                    baseline and baseline.n_healthy or 0,
                    R, ring, cycle_ms)
            end

            -- Streak bookkeeping for R_DRIFT_ALERT promotion.
            local astate = KB2.load_alert_state(db, valve)
            if cls == "R_DRIFT_ALERT_CANDIDATE" and d_base then
                local dir = direction_of(d_base)
                if astate.dir == dir then
                    astate.streak = (astate.streak or 0) + 1
                else
                    astate.streak = 1
                end
                astate.dir = dir
                if astate.streak >= KB2.ALERT_STREAK_REQUIRED then
                    cls = "R_DRIFT_ALERT"
                    flagged_alert = flagged_alert + 1
                    notify_lines[#notify_lines+1] = string.format(
                        "ALERT %s — R=%.1f Ω (baseline %.1f, Δ%+.1f) %d consecutive %s",
                        valve, R or 0, baseline_med or 0, d_base, astate.streak, dir or "?")
                    KB2.upsert_alert_state(db, valve, 0, nil, cycle_ms)
                else
                    flagged_warn = flagged_warn + 1
                    KB2.upsert_alert_state(db, valve, astate.streak, astate.dir, nil)
                end
            elseif cls == "R_DRIFT_WARN" then
                flagged_warn = flagged_warn + 1
                -- Reset streak (drift below alert threshold)
                KB2.upsert_alert_state(db, valve, 0, nil, nil)
            elseif cls == "MASTER_RELAY_CREEP" then
                flagged_warn = flagged_warn + 1
                notify_lines[#notify_lines+1] = string.format(
                    "MASTER_RELAY_CREEP %s — R=%.1f Ω (baseline %.1f, Δ%+.1f Ω upward)",
                    valve, R or 0, baseline_med or 0, d_base or 0)
                KB2.upsert_alert_state(db, valve, 0, nil, cycle_ms)
            elseif cls == "R_STEP_NOTED" then
                -- Maintenance flag — DB only, log only, no Discord.
                log(id, "STEP %s ΔR_prev=%+.1f Ω (likely maintenance)",
                    valve, d_step or 0)
                KB2.upsert_alert_state(db, valve, 0, nil, nil)
            else
                -- OK or POSSIBLE_CHIP_MISCAL: reset streak.
                KB2.upsert_alert_state(db, valve, 0, nil, nil)
            end
        end
    end

    -- Discord: one message bundling all alerts for this cycle.
    if #notify_lines > 0 then
        local body = string.format(
            "KB2 resistance — cycle %d:\n%s\n(offset=%.4f A, 2-null method)",
            cycle_id, table.concat(notify_lines, "\n"), offset)
        local ok, err = push_notify(ps, id, body)
        if not ok then
            log(id, "Discord notify FAILED: %s", tostring(err))
        end
    end

    -- Commit probe cache so we don't re-process this cycle.
    KB2.cycle_state_set(db, "probe_" .. KB2.NULL_VALVES[1], probe_str)

    log(id, "cycle %d processed: %d valves, %d warn, %d alert, offset=%.4f A",
        cycle_id, processed, flagged_warn, flagged_alert, offset)
    app_heartbeat.stamp(handle, "kb2_resistance", "ok",
        string.format("cycle=%d processed=%d warn=%d alert=%d offset=%.3f",
            cycle_id, processed, flagged_warn, flagged_alert, offset),
        poll_s)
end

M.registry = { main = M.main, one_shot = M.one_shot, boolean = M.boolean }
return M
