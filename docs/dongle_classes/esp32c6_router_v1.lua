-- ============================================================================
-- esp32c6_router_v1 — ESP32-C6 router dongle, wireless + wired buses
-- ============================================================================
-- Builds on rp2350_router_v1's wired-bus shape but adds:
--   * Hardware TWAI CAN (not PIO — easier than RP2350's can2040)
--   * WiFi 6 station + soft-AP
--   * BLE 5 advertising + central
--   * IEEE 802.15.4 / Thread (Matter-compatible)
--
-- ESP32-C6 is single-core (RISC-V), so no FreeRTOS-SMP. ESP-IDF provides
-- its own FreeRTOS but it's single-core. The DSL chain runs on the main
-- task; wireless stacks live in their own tasks driven by ESP-IDF.
--
-- Since this router is the most-capable chip, the DSL chain has the most
-- parallel branches in OPERATIONAL fork. Demonstrates the engine's
-- scalability — same chain shape, more concurrent activities.
-- ============================================================================

local M = require("s_expr_dsl")
local mod = start_module("esp32c6_router_v1")
use_32bit()
set_debug(false)

-- ============================================================================
-- Opcode constants
-- ============================================================================
local OP_REGISTER_ACK   = 0x0103
local OP_PING           = 0x0104
local OP_COMMISSION_SET = 0x0105
local OP_COMMISSION_CLEAR = 0x0106

-- Wired-bus opcodes (shared with rp2350_router_v1, same hash on the wire)
local OP_BUS_RS485_SEND     = 0x0180
local OP_BUS_CAN_SEND       = 0x0181
local OP_SLAVE_COMMISSION_BEGIN = 0x0184

-- ESP32-C6-specific wireless opcodes (m2s, 0x01C0+)
local OP_WIFI_SCAN          = 0x01C0  -- {channel_mask:u16, scan_ms:u16}
local OP_WIFI_CONNECT       = 0x01C1  -- {ssid_hash:u32, psk_hash:u32, security:u8}
local OP_WIFI_DISCONNECT    = 0x01C2
local OP_WIFI_AP_START      = 0x01C3  -- {ssid_hash:u32, psk_hash:u32, channel:u8}
local OP_BLE_ADVERTISE      = 0x01C4  -- {name_hash:u32, payload_hash:u32, interval_ms:u16}
local OP_BLE_SCAN           = 0x01C5  -- {duration_ms:u16, active:u8}
local OP_BLE_CONNECT        = 0x01C6  -- {peer_addr[6]:u8}
local OP_THREAD_JOIN        = 0x01C7  -- {network_key_hash:u32, channel:u8}
local OP_THREAD_LEAVE       = 0x01C8
local OP_THREAD_UDP_SEND    = 0x01C9  -- {dest_addr[16]:u8, port:u16, payload[]}

-- Internal events
local EV_HOST_REATTACH      = 0xFE00
local EV_COMMISSION_SET     = 0xFE01
local EV_SLAVE_JOINED       = 0xFE10
local EV_SLAVE_TIMEOUT      = 0xFE11
-- Wireless events (unsolicited s2m emit on state change)
local EV_WIFI_CONNECTED     = 0xFE20
local EV_WIFI_DISCONNECTED  = 0xFE21
local EV_BLE_PEER_FOUND     = 0xFE22
local EV_THREAD_JOINED      = 0xFE23

local DONGLE_BOOT        = 0
local DONGLE_OPERATIONAL = 1

-- ============================================================================
-- Blackboard — wireless adds many state fields
-- ============================================================================
RECORD("dongle_record")
    FIELD("dongle_state",   "int32")
    FIELD("slave_count",    "int32")
    FIELD("wifi_state",     "int32")  -- 0=off, 1=scanning, 2=connecting, 3=connected
    FIELD("ble_state",      "int32")
    FIELD("thread_state",   "int32")
    FIELD("rssi_dbm",       "int32")  -- last-known signal strength, signed
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
-- OPERATIONAL dispatch — wired + wireless
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

-- Wired-bus opcodes (shared with rp2350_router_v1 — same names, ESP-IDF
-- specific C bodies for TWAI vs PIO)
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
            local c = o_call("handle_bus_can_send_twai"); end_call(c)  -- HW TWAI
            se_return_pipeline_reset()
        end)
    end)
end

-- Wireless-specific opcodes
op_dispatch[4] = function()
    se_event_case(OP_WIFI_SCAN, function()
        se_chain_flow(function()
            local c = o_call("handle_wifi_scan"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[5] = function()
    se_event_case(OP_WIFI_CONNECT, function()
        se_chain_flow(function()
            local c = o_call("handle_wifi_connect"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[6] = function()
    se_event_case(OP_BLE_ADVERTISE, function()
        se_chain_flow(function()
            local c = o_call("handle_ble_advertise"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[7] = function()
    se_event_case(OP_BLE_SCAN, function()
        se_chain_flow(function()
            local c = o_call("handle_ble_scan"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[8] = function()
    se_event_case(OP_THREAD_JOIN, function()
        se_chain_flow(function()
            local c = o_call("handle_thread_join"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[9] = function()
    se_event_case(OP_THREAD_UDP_SEND, function()
        se_chain_flow(function()
            local c = o_call("handle_thread_udp_send"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end

-- Wireless internal-event handlers (emit unsolicited s2m on state changes)
op_dispatch[10] = function()
    se_event_case(EV_WIFI_CONNECTED, function()
        se_chain_flow(function()
            local c = o_call("notify_wifi_connected"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[11] = function()
    se_event_case(EV_WIFI_DISCONNECTED, function()
        se_chain_flow(function()
            local c = o_call("notify_wifi_disconnected"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[12] = function()
    se_event_case(EV_BLE_PEER_FOUND, function()
        se_chain_flow(function()
            local c = o_call("notify_ble_peer_found"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[13] = function()
    se_event_case(EV_THREAD_JOINED, function()
        se_chain_flow(function()
            local c = o_call("notify_thread_joined"); end_call(c)
            se_return_pipeline_reset()
        end)
    end)
end
op_dispatch[14] = function()
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
        -- Maximum-capability fork: 6 parallel branches.
        -- Demonstrates engine scales up cleanly with chip capability.
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
                -- Wired-bus slave roster (shared shape with RP2350 router)
                local m = m_call("slave_roster_monitor"); end_call(m)
            end,
            function()
                -- CLASS-SPECIFIC: poll WiFi/BLE/Thread stack state, push
                -- EV_WIFI_*/EV_BLE_*/EV_THREAD_* events when transitions
                -- detected. ESP-IDF event-loop -> engine event_queue bridge.
                local w = m_call("wireless_event_pump"); end_call(w)
            end,
            function()
                -- CLASS-SPECIFIC: refresh blackboard's RSSI / connection
                -- state fields every tick.
                local r = m_call("wireless_stats_refresh"); end_call(r)
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
start_tree("esp32c6_router_v1")
    use_record("dongle_record")
    se_function_interface(function()
        se_i_set_field("dongle_state",  DONGLE_BOOT)
        se_i_set_field("slave_count",   0)
        se_i_set_field("wifi_state",    0)
        se_i_set_field("ble_state",     0)
        se_i_set_field("thread_state",  0)
        se_i_set_field("rssi_dbm",      -127)
        local wd = m_call("wdt_strobe");             end_call(wd)
        local ie = m_call("handle_internal_events"); end_call(ie)
        se_state_machine("dongle_state", case_fn)
        se_return_halt()
    end)
end_tree("esp32c6_router_v1")

return end_module()
