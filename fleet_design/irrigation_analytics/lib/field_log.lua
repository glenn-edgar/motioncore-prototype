-- lib/field_log.lua — manual-log baseline-reset watcher (MONITOR-ONLY).
--
-- Runs inside kb4_clog's tick (it already owns kb4.db + clog_observations +
-- the baselines), so this is just a function — NO separate KB. When field
-- maintenance changes the physical system, the operator logs a per-valve
-- `action` via the dashboard (/irrigation/check -> clog_observations.action).
-- This scans for new action rows and logs what detector baselines that change
-- WOULD reset. It applies NOTHING yet; once the action->reset map is trusted we
-- flip it to actually reset (and set clog_observations.action_applied=1).
--
-- Idempotency: `manual_reset_log` records every obs row seen, so each action is
-- logged once (not every tick). See [[irrigation-manual-log-reset-robot]].

local M = {}

-- action -> human-readable reset plan. nil = no reset (record only).
-- (The ARMED version turns each plan into actual baseline mutations.)
M.PLAN = {
    cap_heads        = "rebaseline flow (baselines_eto/baselines[bin]) to new normal; phantom=1 if sub-floor",
    clean_heads      = "clear flow baseline + watch_list[bin] -> re-learn UP",
    replace_valve    = "reset coil_onset + kb2 resistance + flow baselines[bin]",
    replace_solenoid = "reset coil_onset + kb2 resistance baselines[bin]",
    repair_leak      = "clear leak watch_list[bin] -> re-learn",
    inspect_ok       = nil,
    other            = nil,
}

function M.ensure_schema(db)
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
end

-- Scan clog_observations for new action rows; log "WOULD reset ..." for each and
-- record it (monitor-only). db is an already-open kb4.db. log is log(fmt, ...).
-- Returns the number of new action rows handled.
function M.scan(db, now_ms, log)
    if not db then return 0 end
    local pending = {}
    local ok = pcall(function()
        for r in db:nrows([[
            SELECT co.id AS oid, co.bin AS bin, co.action AS action
            FROM clog_observations co
            WHERE co.action IS NOT NULL AND co.action != ''
              AND co.id NOT IN (SELECT obs_id FROM manual_reset_log)
            ORDER BY co.id ]]) do
            pending[#pending + 1] = r
        end
    end)
    if not ok then return 0 end   -- clog_observations / action column not present yet

    local n = 0
    for _, r in ipairs(pending) do
        local plan_txt = M.PLAN[r.action] or "no reset (record only)"
        if log then
            log("MANUAL-LOG [monitor] obs=%d bin=%s action=%s -> WOULD %s",
                r.oid, tostring(r.bin), tostring(r.action), plan_txt)
        end
        local stmt = db:prepare(
            "INSERT OR IGNORE INTO manual_reset_log(obs_id,ts_ms,bin,action,plan,applied) " ..
            "VALUES(?,?,?,?,?,0)")
        if stmt then
            stmt:bind_values(r.oid, now_ms, r.bin, r.action, plan_txt)
            stmt:step(); stmt:finalize()
        end
        n = n + 1
    end
    return n
end

return M
