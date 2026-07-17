-- chains/openmeteo.lua — build-time DSL for the Open-Meteo ETo app KB.
--
-- One KB instance: openmeteo_eto, handler OPENMETEO_TICK. Same single-column
-- daily-gate shape as cimis.lua / eto_resolver.lua — a one_shot leaf does all
-- the work, then the retry wait:
--
--   openmeteo_eto_col
--     [1] one_shot(OPENMETEO_TICK)
--     [2] wait_time(retry_s)
--     [3] reset
--
-- Pure build module — no CLI block; chains/build.lua requires it.

local OPENMETEO_KB_NAME = "openmeteo_eto"

local DEFAULT_RETRY_S = 900            -- 15 minutes; matches class_spec.openmeteo.retry_s

local function build_openmeteo(ct, kb_name, retry_s)
    ct:start_test(kb_name)

    local col = ct:define_column(
        kb_name .. "_col", nil, nil, nil, nil, {}, true)

        ct:asm_one_shot_handler("OPENMETEO_TICK", {})
        ct:asm_wait_time(retry_s or DEFAULT_RETRY_S)
        ct:asm_reset()

    ct:end_column(col)
    ct:end_test()
end

return {
    build_openmeteo   = build_openmeteo,
    OPENMETEO_KB_NAME = OPENMETEO_KB_NAME,
    DEFAULT_RETRY_S   = DEFAULT_RETRY_S,
}
