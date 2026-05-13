-- ============================================================================
-- register_dongle_v2.lua
-- Linux-first prototype: state-machine version of the dongle registration chain.
--
-- Adds a top-level se_state_machine on the `dongle_state` blackboard field.
--   • BOOT state:        listens (event_dispatch) for OP_REGISTER_ACK;
--                        on receive, transitions to OPERATIONAL.
--   • OPERATIONAL state: fork{ heartbeat, LED, event_dispatch{OP_PING -> send_pong} }.
--
-- Locked design decisions (this session):
--   • (a) OP_REGISTER_ACK as explicit BOOT -> OPERATIONAL trigger
--   • HANDSHAKE state deferred until F1/F2/F3 opcodes exist
--   • dongle_state field added as blackboard int32
-- ============================================================================

local M = require("s_expr_dsl")
local mod = start_module("register_dongle_v2")
use_32bit()
set_debug(false)

-- Opcode constants (also defined in vendor/libcomm/opcodes.h on SAMD21).
--   Note: m2s opcodes are used as event_id values inside the engine. They MUST
--   avoid SE_EVENT_TICK=4, SE_EVENT_INIT=0xfffe, SE_EVENT_TERMINATE=0xfffd
--   (s_engine_types.h:104). We allocate m2s in 0x0100+ to stay clear.
--   s2m opcodes (OP_REGISTER, OP_HEARTBEAT, OP_PONG) are NOT dispatched on —
--   they're only the `cmd` field on outgoing frames — so they can share the
--   low range (0x0001+) without conflict.
local OP_REGISTER_ACK = 0x0103   -- m2s: host acknowledges dongle's OP_REGISTER
local OP_PING         = 0x0104   -- m2s: host pings dongle

-- Dongle state values.
local DONGLE_BOOT        = 0
local DONGLE_OPERATIONAL = 1

-- ============================================================================
-- Blackboard
-- ============================================================================
RECORD("dongle_record")
    FIELD("dongle_state", "int32")
END_RECORD()

-- ============================================================================
-- BOOT-state event handlers (table-of-factories pattern, as in event_dispatch docs)
-- ============================================================================
local boot_dispatch = {}

boot_dispatch[1] = function()
    se_event_case(OP_REGISTER_ACK, function()
        se_chain_flow(function()
            se_log("BOOT: received OP_REGISTER_ACK -> OPERATIONAL")
            se_set_field("dongle_state", DONGLE_OPERATIONAL)
            se_return_pipeline_reset()
        end)
    end)
end

boot_dispatch[2] = function()
    se_event_case('default', function()
        se_chain_flow(function()
            se_return_pipeline_halt()
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
    se_event_case('default', function()
        se_chain_flow(function()
            se_return_pipeline_halt()
        end)
    end)
end

-- ============================================================================
-- State-machine cases
-- ============================================================================
local case_fn = {}

case_fn[1] = function()
    se_case(DONGLE_BOOT, function()
        -- Single composite (event_dispatch) is the case action. Returns CONTINUE
        -- until an OP_REGISTER_ACK arrives and the action sets dongle_state=1.
        se_event_dispatch(boot_dispatch)
    end)
end

case_fn[2] = function()
    se_case(DONGLE_OPERATIONAL, function()
        -- Fork keeps all three branches active indefinitely. heartbeat re-cycles
        -- via chain_flow + pipeline_reset; LED branch returns CONTINUE every tick;
        -- event_dispatch reacts to OP_PING and reset-cycles per dispatched event.
        se_fork(
            function()
                se_chain_flow(function()
                    local hb = o_call("send_heartbeat")
                    end_call(hb)
                    se_tick_delay(3)                -- 1 Hz at 250 ms base
                    se_return_pipeline_reset()
                end)
            end,
            function()
                -- Bare leaf inside fork (rule 1: fork is the complex parent).
                -- toggle_led must return SE_PIPELINE_CONTINUE to stay active.
                local led = m_call("toggle_led")
                end_call(led)
            end,
            function()
                se_event_dispatch(op_dispatch)
            end
        )
    end)
end

case_fn[3] = function()
    se_case('default', function()
        -- Should never run — but state_machine requires a default to avoid
        -- "no matching case" exception. Treat as fatal-on-arrival.
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
        -- Boot-once: send_register fires on first INIT, never re-fires
        -- (SURVIVES_RESET) even when sub-trees reset.
        local reg = io_call("send_register")
        end_call(reg)

        -- One-shot init: dongle_state = BOOT (se_i_set_field fires once at INIT)
        se_i_set_field("dongle_state", DONGLE_BOOT)

        -- The top-level driver.
        se_state_machine("dongle_state", case_fn)

        se_return_halt()
    end)
end_tree("register_dongle_v2")

return end_module()
