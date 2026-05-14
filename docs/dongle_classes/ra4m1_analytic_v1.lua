-- ============================================================================
-- ra4m1_analytic_v1 — RA4M1 Xiao leaf dongle, analytic HIL with CMSIS-DSP
-- ============================================================================
-- Like samd21_shell_v1 — same universal scaffolding, same BOOT/OPERATIONAL
-- state machine. Differences:
--   * Class id is fnv1a("ra4m1_analytic_v1")
--   * 14-bit ADC, 12-bit DAC (vs SAMD21's 12/10) — different reply sizes
--   * Adds signal.* CMSIS-DSP commands (Goertzel, FFT, biquad, Welford,
--     cross-correlate, sliding DFT)
--   * Goertzel can emit unsolicited s2m events on lock — bin-threshold-cross
--     subscription (per `dongle_linux_protocol_2026-05-11.md`); chain handles
--     via a parallel periodic-emit branch in OPERATIONAL fork.
--
-- C-side user functions are RA4M1-specific (Renesas FSP HAL register pokes,
-- arm_math.h Goertzel/FFT calls), but the DSL chain shape is portable.
-- ============================================================================

local M = require("s_expr_dsl")
local mod = start_module("ra4m1_analytic_v1")
use_32bit()
set_debug(false)

-- ============================================================================
-- Opcode constants
-- ============================================================================
-- Universal opcodes (locked)
local OP_REGISTER_ACK   = 0x0103
local OP_PING           = 0x0104
local OP_COMMISSION_SET = 0x0105
local OP_COMMISSION_CLEAR = 0x0106

-- RA4M1 shares the SAMD21 functional opcodes (GPIO/PWM/ADC/DAC/counter)
local OP_GPIO_CONFIG    = 0x0107
local OP_GPIO_SET       = 0x0108
local OP_GPIO_READ      = 0x0109
local OP_ADC_READ       = 0x010A
local OP_PWM_SET        = 0x010B
local OP_DAC_SET        = 0x010C
local OP_COUNTER_READ   = 0x010D
local OP_COUNTER_RESET  = 0x010E

-- RA4M1-specific analytic opcodes (m2s, 0x0140+)
local OP_SIGNAL_HPF         = 0x0140  -- {channel:u8, cutoff_hz:u16, order:u8}
local OP_SIGNAL_LPF         = 0x0141  -- {channel:u8, cutoff_hz:u16, order:u8}
local OP_SIGNAL_BIQUAD      = 0x0142  -- {channel:u8, coeffs[5]:f32}
local OP_GOERTZEL_START     = 0x0143  -- {channel:u8, target_hz:u16, block_len:u16}
local OP_GOERTZEL_STOP      = 0x0144  -- {channel:u8}
local OP_GOERTZEL_LOCK_SUB  = 0x0145  -- {channel:u8, threshold:f32} subscribe
local OP_FFT_BLOCK_START    = 0x0146  -- {channel:u8, n_log2:u8, win:u8}
local OP_WELFORD_START      = 0x0147  -- {channel:u8, window_samples:u16}
local OP_WELFORD_READ       = 0x0148  -- {channel:u8} -> reply {mean,var,n}
local OP_CROSS_CORR         = 0x0149  -- {ch_a:u8, ch_b:u8, n_log2:u8}

-- Engine-internal events
local EV_HOST_REATTACH  = 0xFE00
local EV_COMMISSION_SET = 0xFE01

local DONGLE_BOOT        = 0
local DONGLE_OPERATIONAL = 1

-- ============================================================================
-- Blackboard
-- ============================================================================
RECORD("dongle_record")
    FIELD("dongle_state",      "int32")
    FIELD("goertzel_active",   "int32")  -- bitmask of active goertzel channels;
                                          -- m_call("goertzel_tick") services it
END_RECORD()

-- ============================================================================
-- BOOT dispatch (identical to samd21_shell_v1 — universal)
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
-- OPERATIONAL dispatch (CLASS-SPECIFIC — adds signal.* on top of shell)
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

-- Shell-style commands (functional HIL — shared shape with SAMD21,
-- different C bodies for 14-bit ADC and 12-bit DAC)
op_dispatch[2] = function()
    se_event_case(OP_GPIO_SET, function()
        se_chain_flow(function()
            local c = o_call("handle_gpio_set"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[3] = function()
    se_event_case(OP_ADC_READ, function()
        se_chain_flow(function()
            local c = o_call("handle_adc_read_14bit"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[4] = function()
    se_event_case(OP_DAC_SET, function()
        se_chain_flow(function()
            local c = o_call("handle_dac_set_12bit"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
-- ... (rest of shell-style opcodes omitted for brevity — follow same pattern)

-- Analytic-specific commands
op_dispatch[5] = function()
    se_event_case(OP_GOERTZEL_START, function()
        se_chain_flow(function()
            local c = o_call("handle_goertzel_start"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[6] = function()
    se_event_case(OP_GOERTZEL_STOP, function()
        se_chain_flow(function()
            local c = o_call("handle_goertzel_stop"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[7] = function()
    se_event_case(OP_GOERTZEL_LOCK_SUB, function()
        se_chain_flow(function()
            local c = o_call("handle_goertzel_lock_subscribe"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[8] = function()
    se_event_case(OP_FFT_BLOCK_START, function()
        se_chain_flow(function()
            local c = o_call("handle_fft_block_start"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[9] = function()
    se_event_case(OP_WELFORD_START, function()
        se_chain_flow(function()
            local c = o_call("handle_welford_start"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[10] = function()
    se_event_case(OP_CROSS_CORR, function()
        se_chain_flow(function()
            local c = o_call("handle_cross_correlate"); end_call(c)
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
        -- 4-branch fork: heartbeat, LED, dispatch, AND the
        -- analytic-specific goertzel/welford service ticker.
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
            end,
            function()
                -- CLASS-SPECIFIC: tick the active DSP loops every tick.
                -- Reads goertzel_active blackboard field, services each
                -- active channel, emits unsolicited OP_*_LOCK frames when
                -- thresholds cross.
                local g = m_call("dsp_tick"); end_call(g)
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
start_tree("ra4m1_analytic_v1")
    use_record("dongle_record")
    se_function_interface(function()
        se_i_set_field("dongle_state", DONGLE_BOOT)
        local wd = m_call("wdt_strobe");             end_call(wd)
        local ie = m_call("handle_internal_events"); end_call(ie)
        se_state_machine("dongle_state", case_fn)
        se_return_halt()
    end)
end_tree("ra4m1_analytic_v1")

return end_module()
