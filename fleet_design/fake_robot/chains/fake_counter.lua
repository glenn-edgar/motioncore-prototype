-- chains/fake_counter.lua — build-time DSL for the fake_counter application KB.
--
-- A throwaway, class-specific application KB for fake_robot. Its job is to
-- close the multi-KB loop: KB0 (the shared connection manager) spawns it via
-- SPAWN_APP_KBS once the robot reaches operating; it runs concurrently with
-- KB0; and ERROR_CONTROLLER_LOST / ERROR_ZENOH_LOST sweep it on a connection
-- loss. It is the first thing that exercises class_spec.app_kbs end-to-end.
--
-- Structure — a single column that loops forever:
--
--   fake_counter_col
--     [1] one_shot(PUBLISH_COUNTER)  publish an incrementing int on <ns>/counter
--     [2] wait_time(1.0)             1 s cadence
--     [3] reset                      CFL_RESET re-enables the column => loop
--
-- Same column shape as KB0's verify_controller_heartbeat state
-- (first-node + wait_time + reset), which is live-validated — here the
-- leading node is a one-shot (DISABLEs to advance) instead of a verify.
--
-- This file is a pure build module — no CLI block. chains/connection.lua's
-- CLI driver requires it and builds this KB into the same IR as KB0, so one
-- connection.json carries both KBs (ct_runtime.add_test spawns by name from
-- the single loaded IR).

local FAKE_COUNTER_NAME = "fake_counter"

local function build_fake_counter(ct, kb_name)
    ct:start_test(kb_name)

    -- A KB's first element must be a column. auto_start=true so the KB root
    -- gate enables it the moment KB0's SPAWN_APP_KBS calls add_test.
    local col = ct:define_column(
        "fake_counter_col", nil, nil, nil, nil, {}, true)

        ct:asm_one_shot_handler("PUBLISH_COUNTER", {})
        ct:asm_wait_time(1.0)
        ct:asm_reset()

    ct:end_column(col)

    ct:end_test()
end

return {
    build_fake_counter = build_fake_counter,
    FAKE_COUNTER_NAME  = FAKE_COUNTER_NAME,
}
