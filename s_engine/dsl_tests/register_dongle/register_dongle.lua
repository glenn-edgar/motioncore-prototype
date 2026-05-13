-- ============================================================================
-- register_dongle.lua
-- Linux-first prototype of the dongle registration chain.
-- Per s_engine_dsl_composition_rules.md (locked 2026-05-12):
--   • io_call("send_register") fires once on boot (SURVIVES_RESET)
--   • se_fork branch A: chain_flow { heartbeat ; tick_delay ; pipeline_reset } -> 1 Hz
--   • se_fork branch B: bare m_call("toggle_led") -> every tick (returns SE_PIPELINE_CONTINUE)
-- User-supplied C functions on Linux are mocks that printf; on SAMD21 they
-- will build/send libcomm frames + flip GPIO.
-- ============================================================================

local M = require("s_expr_dsl")
local mod = start_module("register_dongle")
use_32bit()
set_debug(false)

start_tree("register_dongle")
    se_function_interface(function()
        -- Boot-once: send OP_REGISTER. io_call carries SURVIVES_RESET so it
        -- fires only on the very first INIT and never re-fires on any reset.
        local reg = io_call("send_register")
        end_call(reg)

        se_fork(
            function()
                -- Heartbeat branch: fire-once-per-cycle. o_call (oneshot)
                -- auto-terminates after one fire, so chain_flow advances to
                -- tick_delay rather than re-firing heartbeat every tick.
                se_chain_flow(function()
                    local hb = o_call("send_heartbeat")
                    end_call(hb)
                    se_tick_delay(3)                   -- 3+1 ticks = 1 sec @ 250 ms base
                    se_return_pipeline_reset()         -- cycle chain_flow
                end)
            end,
            function()
                -- LED branch: single leaf. Fork is the immediate complex parent
                -- (rule 1 satisfied). toggle_led's C function returns
                -- SE_PIPELINE_CONTINUE so fork keeps the branch active across ticks.
                local led = m_call("toggle_led")
                end_call(led)
            end
        )
        se_return_halt()
    end)
end_tree("register_dongle")

return end_module()
