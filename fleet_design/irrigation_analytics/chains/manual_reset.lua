-- chains/manual_reset.lua — DSL for the manual-log baseline-reset watcher KB.
--
-- Polls kb4.db `clog_observations` for new field-maintenance rows carrying an
-- `action` (cap_heads / clean_heads / replace_valve / replace_solenoid /
-- repair_leak / ...). For each, it determines which detector baselines that
-- physical change invalidates and — MONITOR-ONLY — logs what it WOULD reset.
-- Closes the human->detector loop so a repair doesn't leave a stale baseline.
-- See [[irrigation-manual-log-reset-robot]].

local MANUAL_RESET_KB_NAME  = "manual_reset"
local DEFAULT_POLL_S        = 120
local DEFAULT_BOOT_SETTLE_S = 12

local function build_manual_reset(ct, kb_name, poll_s, boot_settle_s)
    ct:start_test(kb_name)
    local col = ct:define_column(
        kb_name .. "_col", nil, nil, nil, nil, {}, true)

        ct:asm_wait_time(boot_settle_s or DEFAULT_BOOT_SETTLE_S)
        ct:asm_one_shot_handler("MANUAL_RESET_TICK", {})
        ct:asm_wait_time(poll_s or DEFAULT_POLL_S)
        ct:asm_reset()

    ct:end_column(col)
    ct:end_test()
end

return {
    build_manual_reset    = build_manual_reset,
    MANUAL_RESET_KB_NAME  = MANUAL_RESET_KB_NAME,
    DEFAULT_POLL_S        = DEFAULT_POLL_S,
    DEFAULT_BOOT_SETTLE_S = DEFAULT_BOOT_SETTLE_S,
}
