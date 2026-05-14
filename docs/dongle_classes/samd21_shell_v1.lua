-- ============================================================================
-- samd21_shell_v1 — SAMD21 Xiao leaf dongle, functional HIL
-- ============================================================================
-- DSL chain template for the SAMD21 "shell" leaf dongle. Functional HIL:
-- exposes GPIO, PWM, 12-bit ADC, 10-bit DAC, counters, quadrature decode.
-- No downstream bus — it IS a leaf, talks directly to peripheral hardware.
--
-- Structure (chain-level, universal across all dongle classes):
--   se_function_interface
--     io_call("send_register")               [chain-level retry loop runs in BOOT]
--     se_i_set_field("dongle_state", BOOT)
--     m_call("wdt_strobe")                   [pet WDT every tick]
--     m_call("handle_internal_events")       [reattach + commissioning]
--     se_state_machine("dongle_state", { BOOT, OPERATIONAL })
--     se_return_halt()
--
-- Class-specific behavior lives inside the OPERATIONAL case's fork:
--   * heartbeat + LED (universal)
--   * event_dispatch with SAMD21-specific opcodes (GPIO/ADC/PWM/DAC/etc.)
--
-- Compile-time:
--   class_id = fnv1a("samd21_shell_v1")  baked from kb_build's class_ids.h
-- ============================================================================

local M = require("s_expr_dsl")
local mod = start_module("samd21_shell_v1")
use_32bit()
set_debug(false)

-- ============================================================================
-- Opcode constants (all m2s opcodes use 0x0100+ range)
-- ============================================================================
-- Universal opcodes
local OP_REGISTER_ACK   = 0x0103  -- locked
local OP_PING           = 0x0104  -- locked
local OP_COMMISSION_SET = 0x0105  -- locked (m2s commissioning)
local OP_COMMISSION_CLEAR = 0x0106 -- locked

-- SAMD21 shell-specific app-shell opcodes (m2s, 0x0107+)
local OP_GPIO_CONFIG    = 0x0107  -- {pin:u8, mode:u8}
local OP_GPIO_SET       = 0x0108  -- {pin:u8, level:u8}
local OP_GPIO_READ      = 0x0109  -- {pin:u8}     -> reply with level
local OP_ADC_READ       = 0x010A  -- {channel:u8} -> reply with u16 sample
local OP_PWM_SET        = 0x010B  -- {pin:u8, duty_pct:u8, period_us:u16}
local OP_DAC_SET        = 0x010C  -- {channel:u8, value:u16}
local OP_COUNTER_READ   = 0x010D  -- {channel:u8} -> reply with u32 count
local OP_COUNTER_RESET  = 0x010E  -- {channel:u8}
local OP_QUAD_READ      = 0x010F  -- {channel:u8} -> reply with i32 position

-- Engine-internal events (range 0xFE00+, never on the wire)
-- Universal across all dongle classes
local EV_HOST_REATTACH  = 0xFE00
local EV_COMMISSION_SET = 0xFE01

local DONGLE_BOOT        = 0
local DONGLE_OPERATIONAL = 1

-- ============================================================================
-- Blackboard
-- ============================================================================
RECORD("dongle_record")
    FIELD("dongle_state", "int32")
END_RECORD()

-- ============================================================================
-- BOOT-state event handlers (universal)
-- ============================================================================
local boot_dispatch = {}

boot_dispatch[1] = function()
    se_event_case(OP_REGISTER_ACK, function()
        se_chain_flow(function()
            se_log("BOOT: OP_REGISTER_ACK -> OPERATIONAL")
            se_set_field("dongle_state", DONGLE_OPERATIONAL)
            se_return_pipeline_reset()
        end)
    end)
end
boot_dispatch[2] = function()
    se_event_case('default', function()
        se_chain_flow(function() se_return_pipeline_halt() end)
    end)
end

-- ============================================================================
-- OPERATIONAL-state event handlers (CLASS-SPECIFIC — this is what makes
-- samd21_shell_v1 different from other classes)
-- ============================================================================
local op_dispatch = {}

op_dispatch[1] = function()
    se_event_case(OP_PING, function()
        se_chain_flow(function()
            local p = o_call("send_pong"); end_call(p)
            se_return_pipeline_reset()
        end)
    end)
end

-- SAMD21-specific commands. Each o_call invokes a chip-specific C user fn
-- that reads the request payload from the engine event_data buffer pool
-- (per design memory option B) and emits an OP_SHELL_REPLY frame.
op_dispatch[2] = function()
    se_event_case(OP_GPIO_CONFIG, function()
        se_chain_flow(function()
            local c = o_call("handle_gpio_config"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[3] = function()
    se_event_case(OP_GPIO_SET, function()
        se_chain_flow(function()
            local c = o_call("handle_gpio_set"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[4] = function()
    se_event_case(OP_GPIO_READ, function()
        se_chain_flow(function()
            local c = o_call("handle_gpio_read"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[5] = function()
    se_event_case(OP_ADC_READ, function()
        se_chain_flow(function()
            local c = o_call("handle_adc_read"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[6] = function()
    se_event_case(OP_PWM_SET, function()
        se_chain_flow(function()
            local c = o_call("handle_pwm_set"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[7] = function()
    se_event_case(OP_DAC_SET, function()
        se_chain_flow(function()
            local c = o_call("handle_dac_set"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[8] = function()
    se_event_case(OP_COUNTER_READ, function()
        se_chain_flow(function()
            local c = o_call("handle_counter_read"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[9] = function()
    se_event_case(OP_COUNTER_RESET, function()
        se_chain_flow(function()
            local c = o_call("handle_counter_reset"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[10] = function()
    se_event_case(OP_QUAD_READ, function()
        se_chain_flow(function()
            local c = o_call("handle_quad_read"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[11] = function()
    se_event_case('default', function()
        se_chain_flow(function() se_return_pipeline_halt() end)
    end)
end

-- ============================================================================
-- State machine cases
-- ============================================================================
local case_fn = {}

case_fn[1] = function()
    se_case(DONGLE_BOOT, function()
        se_fork(
            function()
                se_chain_flow(function()
                    local r = o_call("send_register"); end_call(r)
                    se_tick_delay(0)
                    se_return_pipeline_reset()
                end)
            end,
            function()
                se_event_dispatch(boot_dispatch)
            end
        )
    end)
end

case_fn[2] = function()
    se_case(DONGLE_OPERATIONAL, function()
        se_fork(
            function()
                se_chain_flow(function()
                    local hb = o_call("send_heartbeat"); end_call(hb)
                    se_tick_delay(3)
                    se_return_pipeline_reset()
                end)
            end,
            function()
                local led = m_call("toggle_led"); end_call(led)
            end,
            function()
                se_event_dispatch(op_dispatch)
            end
        )
    end)
end

case_fn[3] = function()
    se_case('default', function()
        se_chain_flow(function()
            se_log("FATAL: unknown dongle_state")
            se_return_terminate()
        end)
    end)
end

-- ============================================================================
-- Tree
-- ============================================================================
start_tree("samd21_shell_v1")
    use_record("dongle_record")
    se_function_interface(function()
        -- One-shot init
        se_i_set_field("dongle_state", DONGLE_BOOT)
        -- Universal siblings (above state_machine, fire every tick)
        local wd = m_call("wdt_strobe");                end_call(wd)
        local ie = m_call("handle_internal_events");    end_call(ie)
        -- Class-specific driver
        se_state_machine("dongle_state", case_fn)
        se_return_halt()
    end)
end_tree("samd21_shell_v1")

return end_module()
