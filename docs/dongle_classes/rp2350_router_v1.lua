-- ============================================================================
-- rp2350_router_v1 — RP2350 Pico 2 W router dongle
-- ============================================================================
-- Unlike SAMD21/RA4M1 (leaf dongles), a router has a DOWNSTREAM bus where
-- additional slaves live. The router translates between:
--   * Host ↔ Dongle traffic (L2 — uses our OP_* opcode catalogue, addr=0xFE)
--   * Dongle ↔ Slave traffic (L3 — canonical libcomm comm_link_cmd_t,
--                              addr=0x01..0xFC on the downstream bus)
--
-- Per the architectural memo: L3 is the canonical libcomm slave protocol
-- (PING, JOIN_REQ, JOIN_ACK, JOIN_CONFIRM, TIME_SYNC, etc. from comm.h).
-- The router proxies these onto its downstream PIO RS-485 or can2040 CAN bus.
--
-- The router has more complex DSL because it manages two protocol layers
-- AND a slave roster. It also typically uses FreeRTOS-SMP on the RP2350
-- (two cores: core0 = router state, core1 = bus PIO/CAN service).
--
-- This DSL chain is the L2 (host-facing) side. L3 (slave-bus side) is
-- driven by parallel chain trees or by C code outside the engine — TBD
-- when the router work starts in earnest.
-- ============================================================================

local M = require("s_expr_dsl")
local mod = start_module("rp2350_router_v1")
use_32bit()
set_debug(false)

-- ============================================================================
-- Opcode constants
-- ============================================================================
local OP_REGISTER_ACK   = 0x0103
local OP_PING           = 0x0104
local OP_COMMISSION_SET = 0x0105
local OP_COMMISSION_CLEAR = 0x0106

-- Router-specific L2 opcodes (m2s, 0x0180+ to leave 0x0107-0x017F for
-- shared leaf-dongle commands the router may also implement)
local OP_BUS_RS485_SEND     = 0x0180  -- {addr:u8, cmd:u16, payload[]}: forward to slave
local OP_BUS_CAN_SEND       = 0x0181  -- {addr:u8, cmd:u16, payload[]}: forward to slave
local OP_BUS_RS485_BAUDRATE = 0x0182  -- {baud_rate:u32}
local OP_BUS_CAN_BITRATE    = 0x0183  -- {bit_rate:u32}
local OP_SLAVE_COMMISSION_BEGIN = 0x0184  -- {bus_id:u8, slave_addr:u8}
local OP_SLAVE_LIST_QUERY   = 0x0185  -- -> reply with active slave roster
local OP_BUS_STATS_QUERY    = 0x0186  -- {bus_id:u8} -> reply with frame counters

-- Internal events
local EV_HOST_REATTACH  = 0xFE00
local EV_COMMISSION_SET = 0xFE01
-- Router-specific internal events: a slave appears/disappears
-- on the downstream bus → notify host via s2m unsolicited frame
local EV_SLAVE_JOINED   = 0xFE10
local EV_SLAVE_TIMEOUT  = 0xFE11

local DONGLE_BOOT        = 0
local DONGLE_OPERATIONAL = 1

-- ============================================================================
-- Blackboard — router has more state than a leaf
-- ============================================================================
RECORD("dongle_record")
    FIELD("dongle_state",      "int32")
    FIELD("slave_count",       "int32")   -- # of joined slaves on either bus
    FIELD("rs485_tx_count",    "int32")   -- ops counters
    FIELD("rs485_rx_count",    "int32")
    FIELD("can_tx_count",      "int32")
    FIELD("can_rx_count",      "int32")
END_RECORD()

-- ============================================================================
-- BOOT dispatch — universal
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
-- OPERATIONAL dispatch (router-specific opcodes)
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

-- Bus-routing commands. C user fns forward to the L3 libcomm subsystem
-- (running on core1 with FreeRTOS, handles PIO/CAN hardware).
op_dispatch[2] = function()
    se_event_case(OP_BUS_RS485_SEND, function()
        se_chain_flow(function()
            local c = o_call("handle_bus_rs485_send"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[3] = function()
    se_event_case(OP_BUS_CAN_SEND, function()
        se_chain_flow(function()
            local c = o_call("handle_bus_can_send"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[4] = function()
    se_event_case(OP_SLAVE_COMMISSION_BEGIN, function()
        se_chain_flow(function()
            local c = o_call("handle_slave_commission_begin"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[5] = function()
    se_event_case(OP_SLAVE_LIST_QUERY, function()
        se_chain_flow(function()
            local c = o_call("handle_slave_list_query"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[6] = function()
    se_event_case(OP_BUS_STATS_QUERY, function()
        se_chain_flow(function()
            local c = o_call("handle_bus_stats_query"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
-- Router-specific internal events (slave joined/dropped on downstream bus)
op_dispatch[7] = function()
    se_event_case(EV_SLAVE_JOINED, function()
        se_chain_flow(function()
            local c = o_call("notify_slave_joined"); end_call(c)  -- emits s2m
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[8] = function()
    se_event_case(EV_SLAVE_TIMEOUT, function()
        se_chain_flow(function()
            local c = o_call("notify_slave_timeout"); end_call(c) -- emits s2m
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[9] = function()
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
        -- Router OPERATIONAL has 5 parallel branches: heartbeat, LED,
        -- dispatch, slave-roster-monitor (periodic), and bus-stats refresh.
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
                -- CLASS-SPECIFIC: poll the downstream bus subsystem for
                -- joined-slave-roster changes. C side maintains a roster;
                -- this m_call detects deltas and pushes EV_SLAVE_*
                -- events to the engine queue for op_dispatch to handle.
                local m = m_call("slave_roster_monitor"); end_call(m)
            end,
            function()
                -- CLASS-SPECIFIC: refresh blackboard's bus stats fields
                -- every tick so OP_BUS_STATS_QUERY replies are up-to-date
                -- without needing locks against the bus thread.
                local s = m_call("bus_stats_refresh"); end_call(s)
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
start_tree("rp2350_router_v1")
    use_record("dongle_record")
    se_function_interface(function()
        se_i_set_field("dongle_state",   DONGLE_BOOT)
        se_i_set_field("slave_count",    0)
        se_i_set_field("rs485_tx_count", 0)
        se_i_set_field("rs485_rx_count", 0)
        se_i_set_field("can_tx_count",   0)
        se_i_set_field("can_rx_count",   0)
        local wd = m_call("wdt_strobe");             end_call(wd)
        local ie = m_call("handle_internal_events"); end_call(ie)
        se_state_machine("dongle_state", case_fn)
        se_return_halt()
    end)
end_tree("rp2350_router_v1")

return end_module()
