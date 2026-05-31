-- ============================================================================
-- register_dongle_v2.lua
-- Three-state chain implementing the four-layer-sync ladder per
-- common/spec/four_layer_sync.md (2026-05-19).
--
-- State machine on dongle_record.dongle_state:
--   BOOT (0):
--     * fork(retry_loop, event_dispatch)
--     * retry_loop: o_call(send_register) every ~1 s
--     * dispatch: OP_REGISTER_ACK -> set state=L1_DONE
--                 default        -> o_call(send_nak)
--
--   L1_DONE (1):
--     * event_dispatch only (no fork, no heartbeats yet)
--     * OP_GET_MANIFEST      -> o_call(send_manifest_reply)        [no state change]
--     * OP_OPERATIONAL_BEGIN -> log + set state=OPERATIONAL
--     * default              -> o_call(send_nak)
--
--   OPERATIONAL (2):
--     * fork(heartbeat_loop, LED, event_dispatch)
--     * heartbeat_loop: o_call(send_heartbeat) at 1 Hz (4 ticks)
--     * LED: m_call(toggle_led) every tick
--     * dispatch: OP_PING            -> o_call(send_pong)
--                 OP_GET_MANIFEST    -> o_call(send_manifest_reply)
--                 default            -> o_call(send_nak)
--
-- Opcode allocation rule: m2s opcodes are 0x0100+ (used as engine event_id;
-- must avoid SE_EVENT_TICK=4 / SE_EVENT_INIT=0xfffe / SE_EVENT_TERMINATE=0xfffd).
-- ============================================================================

local M = require("s_expr_dsl")
local mod = start_module("register_dongle_v2")
use_32bit()
set_debug(false)

-- m2s opcodes (mirror samd21/.../vendor/libcomm/opcodes.h).
local OP_REGISTER_ACK      = 0x0103
local OP_PING              = 0x0104
local OP_COMMISSION_SET    = 0x0105
local OP_COMMISSION_CLEAR  = 0x0106
local OP_GET_MANIFEST      = 0x0107
local OP_OPERATIONAL_BEGIN = 0x0108
local OP_SHELL_EXEC        = 0x0109

-- Dongle state values. BOOT stays 0 so handle_internal_events's reset path
-- (writes 0 on EV_HOST_REATTACH) lands us back in BOOT without changes.
local DONGLE_BOOT        = 0
local DONGLE_L1_DONE     = 1
local DONGLE_OPERATIONAL = 2

-- ============================================================================
-- Blackboard
-- ============================================================================
RECORD("dongle_record")
    FIELD("dongle_state", "int32")
END_RECORD()

-- ============================================================================
-- BOOT-state event handlers
-- ============================================================================
local boot_dispatch = {}

boot_dispatch[1] = function()
    se_event_case(OP_REGISTER_ACK, function()
        se_chain_flow(function()
            -- handle_register_ack gates the BOOT → L1_DONE transition on
            -- the C-side commissioning state. If UNCOMMISSIONED, it emits
            -- NAK err_state and leaves dongle_state alone; if COMMISSIONED,
            -- it writes DONGLE_L1_DONE directly to the blackboard.
            local r = o_call("handle_register_ack")
            end_call(r)
            se_return_pipeline_reset()
        end)
    end)
end

boot_dispatch[2] = function()
    se_event_case(OP_COMMISSION_SET, function()
        se_chain_flow(function()
            local c = o_call("handle_commission_set")
            end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end

boot_dispatch[3] = function()
    se_event_case(OP_COMMISSION_CLEAR, function()
        se_chain_flow(function()
            local c = o_call("handle_commission_clear")
            end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end

boot_dispatch[4] = function()
    se_event_case('default', function()
        se_chain_flow(function()
            local n = o_call("send_nak")
            end_call(n)
            se_return_pipeline_reset()
        end)
    end)
end

-- ============================================================================
-- L1_DONE-state event handlers
-- ============================================================================
local l1_done_dispatch = {}

l1_done_dispatch[1] = function()
    se_event_case(OP_GET_MANIFEST, function()
        se_chain_flow(function()
            local m = o_call("send_manifest_reply")
            end_call(m)
            se_return_pipeline_reset()
        end)
    end)
end

l1_done_dispatch[2] = function()
    se_event_case(OP_OPERATIONAL_BEGIN, function()
        se_chain_flow(function()
            se_log("L1_DONE: received OP_OPERATIONAL_BEGIN -> OPERATIONAL")
            se_set_field("dongle_state", DONGLE_OPERATIONAL)
            se_return_pipeline_reset()
        end)
    end)
end

l1_done_dispatch[3] = function()
    se_event_case(OP_COMMISSION_CLEAR, function()
        se_chain_flow(function()
            local c = o_call("handle_commission_clear")
            end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end

l1_done_dispatch[4] = function()
    se_event_case('default', function()
        se_chain_flow(function()
            local n = o_call("send_nak")
            end_call(n)
            se_return_pipeline_reset()
        end)
    end)
end

-- ============================================================================
-- OPERATIONAL-state event handlers
-- ============================================================================
local op_dispatch = {}

op_dispatch[1] = function()
    se_event_case(OP_PING, function()
        se_chain_flow(function()
            local p = o_call("send_pong")
            end_call(p)
            se_return_pipeline_reset()
        end)
    end)
end

op_dispatch[2] = function()
    se_event_case(OP_GET_MANIFEST, function()
        se_chain_flow(function()
            local m = o_call("send_manifest_reply")
            end_call(m)
            se_return_pipeline_reset()
        end)
    end)
end

op_dispatch[3] = function()
    se_event_case(OP_COMMISSION_CLEAR, function()
        se_chain_flow(function()
            local c = o_call("handle_commission_clear")
            end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end

op_dispatch[4] = function()
    se_event_case(OP_SHELL_EXEC, function()
        se_chain_flow(function()
            local s = o_call("handle_shell_exec")
            end_call(s)
            se_return_pipeline_reset()
        end)
    end)
end

op_dispatch[5] = function()
    se_event_case('default', function()
        se_chain_flow(function()
            local n = o_call("send_nak")
            end_call(n)
            se_return_pipeline_reset()
        end)
    end)
end

-- ============================================================================
-- State-machine cases
-- ============================================================================
local case_fn = {}

case_fn[1] = function()
    se_case(DONGLE_BOOT, function()
        -- Phase 2f retry: re-emit OP_REGISTER until ACK arrives. Chain cycles
        -- once per tick; send_register wall-clock-gates the emit to ~1/s.
        se_fork(
            function()
                se_chain_flow(function()
                    local r = o_call("send_register")
                    end_call(r)
                    se_tick_delay(0)              -- 1 tick HALT, then advance
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
    se_case(DONGLE_L1_DONE, function()
        -- No heartbeats, no LED — host is in control of the L1→L2 pull and
        -- the L1_DONE→OPERATIONAL advance. Dongle is passive.
        se_event_dispatch(l1_done_dispatch)
    end)
end

case_fn[3] = function()
    se_case(DONGLE_OPERATIONAL, function()
        se_fork(
            function()
                se_chain_flow(function()
                    local hb = o_call("send_heartbeat")
                    end_call(hb)
                    se_tick_delay(3)                -- handler wall-clock-gates to 1 Hz (tick-rate-independent)
                    se_return_pipeline_reset()
                end)
            end,
            function()
                local led = m_call("toggle_led")
                end_call(led)
            end,
            function()
                se_event_dispatch(op_dispatch)
            end
        )
    end)
end

case_fn[4] = function()
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
start_tree("register_dongle_v2")
    use_record("dongle_record")

    se_function_interface(function()
        -- One-shot init: dongle_state = BOOT (se_i_set_field fires once at INIT)
        se_i_set_field("dongle_state", DONGLE_BOOT)

        -- Engine-internal events (EV_HOST_REATTACH today). Sits BEFORE the
        -- state_machine sibling so its field-writes are visible in the same tick.
        local ie = m_call("handle_internal_events")
        end_call(ie)

        -- The top-level driver.
        se_state_machine("dongle_state", case_fn)

        se_return_halt()
    end)
end_tree("register_dongle_v2")

return end_module()
