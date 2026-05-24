-- chains/digest.lua — build-time DSL for the daily-digest application KB.
--
-- A push-notification skill: every 24h, read the in-memory moisture rings
-- and the latest CIMIS records, format a fixed-width report, and publish it
-- on `fleet/notify/digest/daily`. The notification_service (layer 60)
-- subscribes there and POSTs to Discord.
--
-- The column fires once immediately at app-KB spawn so we get one digest
-- at boot (testable, also harmless on real deployment), then loops on the
-- 24h cadence:
--
--   digest_col
--     [1] one_shot(DAILY_DIGEST)
--     [2] wait_time(86400)        24h between subsequent digests
--     [3] reset                    CFL_RESET -> loop
--
-- Robot owns content, service owns transport — this KB does not know
-- Discord exists. See discord-integration-architecture-2026-05-23.
--
-- Pure build module — no CLI block; chains/build.lua requires it.

local DIGEST_KB_NAME = "digest"
local DIGEST_PERIOD_S = 86400          -- 24 hours

local function build_digest(ct, kb_name)
    ct:start_test(kb_name)

    local col = ct:define_column(
        kb_name .. "_col", nil, nil, nil, nil, {}, true)

        ct:asm_one_shot_handler("DAILY_DIGEST", {})
        ct:asm_wait_time(DIGEST_PERIOD_S)
        ct:asm_reset()

    ct:end_column(col)
    ct:end_test()
end

return {
    build_digest    = build_digest,
    DIGEST_KB_NAME  = DIGEST_KB_NAME,
    DIGEST_PERIOD_S = DIGEST_PERIOD_S,
}
