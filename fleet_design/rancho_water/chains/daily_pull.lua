-- chains/daily_pull.lua — build-time DSL for the rancho_water daily-pull KB.
--
-- Once per Pacific civil day at-or-after digest.hour_pacific, fetch
-- yesterday's hourly usage from the Rancho customer portal, format it,
-- publish on fleet/notify/digest/daily AND on <namespace>/usage/{sample,latest}
-- for persistence.
--
-- Same daily-gate column shape as farm_soil's digest KB — the column ticks
-- every retry_s; the actual once-per-day decision is inside DAILY_PULL:
--
--   daily_pull_col
--     [1] one_shot(DAILY_PULL)    gate, fetch+format+publish, stamp date
--     [2] wait_time(retry_s)      15 min between attempts
--     [3] reset                   CFL_RESET -> loop

local DAILY_PULL_KB_NAME = "daily_pull"
local DEFAULT_RETRY_S    = 900

local function build_daily_pull(ct, kb_name, retry_s)
    ct:start_test(kb_name)

    local col = ct:define_column(
        kb_name .. "_col", nil, nil, nil, nil, {}, true)

        ct:asm_one_shot_handler("DAILY_PULL", {})
        ct:asm_wait_time(retry_s or DEFAULT_RETRY_S)
        ct:asm_reset()

    ct:end_column(col)
    ct:end_test()
end

return {
    build_daily_pull   = build_daily_pull,
    DAILY_PULL_KB_NAME = DAILY_PULL_KB_NAME,
    DEFAULT_RETRY_S    = DEFAULT_RETRY_S,
}
