-- chains/manual_reset_user_functions.lua — manual-log baseline-reset watcher.
--
-- MONITOR-ONLY (Glenn 2026-06-14). When field maintenance changes the physical
-- system, the operator logs a per-valve `action` via the dashboard
-- (/irrigation/check -> clog_observations.action). This watcher reads those
-- rows and, for each, says what detector baselines it WOULD reset — it applies
-- NOTHING yet. Once the action->reset map is trusted we flip it to actually
-- reset (and set clog_observations.action_applied=1).
--
-- Idempotency: its own `manual_reset_log` table records every obs row it has
-- seen, so it logs each action once (not every poll).

local app_heartbeat = require("app_heartbeat")
local sqlite3       = require("lsqlite3")

local M = { main = {}, one_shot = {}, boolean = {} }

local DEFAULT_KB4_DB = os.getenv("KB4_DB_PATH") or "/var/fleet/kb4/kb4.db"
local DEFAULT_POLL_S = 120

local function log(id, fmt, ...)
    io.stderr:write(string.format("manual_reset [%s]: " .. fmt .. "\n", id.namespace, ...))
end

-- action -> human-readable reset plan. nil = no reset (record only).
-- (The ARMED version will turn each plan into actual baseline mutations.)
local PLAN = {
    cap_heads        = "rebaseline flow (baselines_eto/baselines[bin]) to new normal; phantom=1 if sub-floor",
    clean_heads      = "clear flow baseline + watch_list[bin] -> re-learn UP",
    replace_valve    = "reset coil_onset + kb2 resistance + flow baselines[bin]",
    replace_solenoid = "reset coil_onset + kb2 resistance baselines[bin]",
    repair_leak      = "clear leak watch_list[bin] -> re-learn",
    inspect_ok       = nil,
    other            = nil,
}

M.one_shot.MANUAL_RESET_TICK = function(handle, _node)
    local bb  = handle.blackboard
    local id  = bb._identity
    local cs  = bb._class_spec
    local cfg = (cs and cs.manual_reset) or {}
    local db_path = cfg.kb4_db_path or DEFAULT_KB4_DB
    local retry_s = cfg.poll_s or DEFAULT_POLL_S

    local db = sqlite3.open(db_path)
    if not db then
        app_heartbeat.stamp(handle, "manual_reset", "degraded",
            "kb4 db open failed: " .. tostring(db_path), retry_s)
        return
    end

    -- own state (idempotency). Safe if clog_observations/action absent (just
    -- finds nothing).
    db:exec([[
        CREATE TABLE IF NOT EXISTS manual_reset_log (
            obs_id   INTEGER PRIMARY KEY,
            ts_ms    INTEGER,
            bin      TEXT,
            action   TEXT,
            plan     TEXT,
            applied  INTEGER DEFAULT 0
        );
    ]])

    -- new action rows we haven't logged yet
    local pending = {}
    local ok_q = pcall(function()
        for r in db:nrows([[
            SELECT co.id AS oid, co.bin AS bin, co.action AS action
            FROM clog_observations co
            WHERE co.action IS NOT NULL AND co.action != ''
              AND co.id NOT IN (SELECT obs_id FROM manual_reset_log)
            ORDER BY co.id ]]) do
            pending[#pending + 1] = r
        end
    end)
    if not ok_q then
        -- clog_observations or its action column may not exist yet on an old DB
        db:close()
        app_heartbeat.stamp(handle, "manual_reset", "ok",
            "no field-action log yet (monitor)", retry_s)
        return
    end

    local n = 0
    for _, r in ipairs(pending) do
        local plan_txt = PLAN[r.action] or "no reset (record only)"
        log(id, "MANUAL-LOG [monitor] obs=%d bin=%s action=%s -> WOULD %s",
            r.oid, tostring(r.bin), tostring(r.action), plan_txt)
        local stmt = db:prepare(
            "INSERT OR IGNORE INTO manual_reset_log(obs_id,ts_ms,bin,action,plan,applied) " ..
            "VALUES(?,?,?,?,?,0)")
        if stmt then
            stmt:bind_values(r.oid, os.time() * 1000, r.bin, r.action, plan_txt)
            stmt:step(); stmt:finalize()
        end
        n = n + 1
    end
    db:close()

    app_heartbeat.stamp(handle, "manual_reset", "ok",
        string.format("monitor: %d new field-action row(s) logged", n), retry_s)
end

M.registry = { main = M.main, one_shot = M.one_shot, boolean = M.boolean }
return M
