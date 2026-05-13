-- ============================================================================
-- door_window_controller.lua
-- S-Expression Engine DSL Test - Car Door Window Controller
--
-- Follows the dispatch_test pattern: flat DSL calls inside
-- se_chain_flow(function() ... end), table-indexed state/event cases.
--
-- Compile:
--   luajit s_compile.lua door_window_controller.lua \
--       --helpers=s_engine_helpers.lua --all-bin --outdir=generated/
-- ============================================================================

local M = require("s_expr_dsl")
local mod = start_module("door_window_controller")
use_32bit()
set_debug(true)

-- ============================================================================
-- EVENTS (must be before any code that references EVENT_ID)
-- ============================================================================

EVENTS({
    EVT_OPEN_REQUEST        = 0x0001,
    EVT_CLOSE_REQUEST       = 0x0002,
    EVT_STOP_REQUEST        = 0x0003,
    EVT_STATUS_REQUEST      = 0x0004,
    EVT_EMERGENCY_STOP      = 0x0005,

    EVT_OVER_CURRENT        = 0x0010,
    EVT_OBSTRUCTION         = 0x0011,
    EVT_FULLY_OPEN          = 0x0012,
    EVT_FULLY_CLOSED        = 0x0013,
    EVT_AUTO_REVERSE        = 0x0014,
    EVT_MANUAL_RESET        = 0x0015,
    EVT_THERMAL_SHUTDOWN    = 0x0016,
    EVT_VOLTAGE_ERROR       = 0x0017,
    EVT_TLE_FAULT           = 0x0018,

    EVT_PERIODIC_1S         = 0x0020,
    EVT_NEW_COMMAND         = 0x0021,
})

-- ============================================================================
-- ALL CONSTANTS (must be defined before any case tables or helpers)
-- ============================================================================

-- Bridge modes
local BRIDGE_FREEWHEEL = 0
local BRIDGE_FORWARD   = 1
local BRIDGE_REVERSE   = 2
local BRIDGE_BRAKE     = 3

-- Motor states
local STATE_IDLE           = 0
local STATE_OPENING        = 1
local STATE_CLOSING        = 2
local STATE_AUTO_REVERSE   = 3
local STATE_EMERGENCY      = 4

-- Serial command codes
local CMD_OPEN      = 1
local CMD_CLOSE     = 2
local CMD_STOP      = 3
local CMD_STATUS    = 4
local CMD_EMERGENCY = 5

-- ============================================================================
-- RECORD
-- ============================================================================

RECORD("door_controller_bb")
    FIELD("motor_state",        "int32")
    FIELD("bridge_mode",        "int32")
    FIELD("motor_pwm",          "int32")
    FIELD("motor_enabled",      "int32")

    FIELD("motor_current",      "float")
    FIELD("max_current",        "float")
    FIELD("position",           "int32")
    FIELD("temperature",        "float")
    FIELD("voltage",            "float")

    FIELD("open_speed",         "int32")
    FIELD("close_speed",        "int32")
    FIELD("thermal_threshold",  "float")
    FIELD("min_voltage",        "float")
    FIELD("max_voltage",        "float")

    FIELD("fully_open",         "int32")
    FIELD("fully_closed",       "int32")
    FIELD("emergency",          "int32")
    FIELD("obstruction",        "int32")
    FIELD("over_current",       "int32")
    FIELD("system_shutdown",    "int32")
    FIELD("manual_reset",       "int32")
    FIELD("fault_pin_active",   "int32")

    FIELD("open_request",       "int32")
    FIELD("close_request",      "int32")
    FIELD("stop_request",       "int32")
    FIELD("status_request",     "int32")
    FIELD("auto_reverse_req",   "int32")

    FIELD("serial_data_avail",  "int32")
    FIELD("serial_msg_type",    "int32")

    PTR64_FIELD("config_ptr",   "void")
    PTR64_FIELD("diag_ptr",     "void")
END_RECORD()

-- ============================================================================
-- DEFAULTS
-- ============================================================================

CONST("door_defaults", "door_controller_bb")
    VALUE("motor_state",        STATE_IDLE)
    VALUE("bridge_mode",        BRIDGE_FREEWHEEL)
    VALUE("motor_pwm",          0)
    VALUE("motor_enabled",      0)
    VALUE("motor_current",      0.0)
    VALUE("max_current",        5.0)
    VALUE("position",           0)
    VALUE("temperature",        25.0)
    VALUE("voltage",            13.8)
    VALUE("open_speed",         200)
    VALUE("close_speed",        180)
    VALUE("thermal_threshold",  85.0)
    VALUE("min_voltage",        9.0)
    VALUE("max_voltage",        16.0)
    VALUE("fully_open",         0)
    VALUE("fully_closed",       1)
    VALUE("emergency",          0)
    VALUE("obstruction",        0)
    VALUE("over_current",       0)
    VALUE("system_shutdown",    0)
    VALUE("manual_reset",       0)
    VALUE("fault_pin_active",   0)
    VALUE("open_request",       0)
    VALUE("close_request",      0)
    VALUE("stop_request",       0)
    VALUE("status_request",     0)
    VALUE("auto_reverse_req",   0)
    VALUE("serial_data_avail",  0)
    VALUE("serial_msg_type",    0)
END_CONST()

-- ============================================================================
-- MOTOR PRIMITIVES (plain functions - emit DSL nodes when called)
-- ============================================================================

local function disable_motor()
    local c = o_call("DISABLE_MOTOR_TLE7269G")
    end_call(c)
end

local function enable_motor()
    local c = o_call("ENABLE_MOTOR_TLE7269G")
    end_call(c)
end

local function apply_bridge_mode()
    local c = o_call("SET_BRIDGE_MODE")
        field_ref("bridge_mode")
    end_call(c)
end

local function set_pwm_field(speed_field)
    local c = o_call("SET_MOTOR_PWM")
        field_ref(speed_field)
    end_call(c)
end

local function set_pwm_value(val)
    local c = o_call("SET_MOTOR_PWM")
        int(val)
    end_call(c)
end

local function send_status(msg)
    local c = o_call("SEND_STATUS_MESSAGE")
        str(msg)
    end_call(c)
end

local function motor_start(direction, speed_field)
    se_set_field("bridge_mode", direction)
    apply_bridge_mode()
    set_pwm_field(speed_field)
    enable_motor()
    se_set_field("motor_enabled", 1)
end

local function motor_shutdown(status_msg)
    disable_motor()
    se_set_field("motor_enabled", 0)
    se_set_field("bridge_mode", BRIDGE_BRAKE)
    apply_bridge_mode()
    se_tick_delay(10)
    se_set_field("bridge_mode", BRIDGE_FREEWHEEL)
    apply_bridge_mode()
    send_status(status_msg)
end

local function motor_kill()
    disable_motor()
    se_set_field("motor_enabled", 0)
    se_set_field("bridge_mode", BRIDGE_FREEWHEEL)
    apply_bridge_mode()
end

local function clear_all_faults()
    se_set_field("emergency", 0)
    se_set_field("over_current", 0)
    se_set_field("obstruction", 0)
    se_set_field("manual_reset", 0)
    se_set_field("auto_reverse_req", 0)
end

-- ============================================================================
-- PREDICATE HELPERS
-- ============================================================================

local function pred_any_stop(position_flag)
    return function()
        pred_begin()
            local or_id = se_pred_or()
                se_field_eq(position_flag, 1)
                se_field_eq("stop_request", 1)
                se_field_eq("over_current", 1)
                se_field_eq("obstruction", 1)
                se_field_eq("emergency", 1)
            pred_close(or_id)
        local p = pred_end()
        p()
    end
end

-- Helper: wrap a single predicate leaf in pred_begin/pred_end and call it
local function pred_field_eq(field_name, value)
    return function()
        pred_begin()
            se_field_eq(field_name, value)
        local p = pred_end()
        p()
    end
end

local function pred_field_gt(field_name, value)
    return function()
        pred_begin()
            se_field_gt(field_name, value)
        local p = pred_end()
        p()
    end
end

local function pred_field_lt(field_name, value)
    return function()
        pred_begin()
            se_field_lt(field_name, value)
        local p = pred_end()
        p()
    end
end

-- ============================================================================
-- MONITORING LOOPS (return closures for se_fork children)
-- ============================================================================

local function monitor_current(guard_flag)
    return function()
        se_while(
            pred_field_eq(guard_flag, 0),
            function()
                se_if_then(
                    pred_field_gt("motor_current", 5.0),
                    function() se_chain_flow(function()
                        se_set_field("over_current", 1)
                        se_queue_event(0, EVENT_ID("EVT_OVER_CURRENT"), "over_current")
                        se_log("FAULT: Over-current detected")
                    end) end
                )
            end
        )
    end
end

local function monitor_limit_switch(guard_flag, pred_name, done_flag, done_event)
    return function()
        se_while(
            pred_field_eq(guard_flag, 0),
            function()
                se_if_then(
                    function()
                        local c = p_call(pred_name)
                        end_call(c)
                    end,
                    function() se_chain_flow(function()
                        se_set_field(done_flag, 1)
                        se_queue_event(0, EVENT_ID(done_event), done_flag)
                    end) end
                )
            end
        )
    end
end

local function monitor_obstruction(guard_flag, with_auto_reverse)
    return function()
        se_while(
            pred_field_eq(guard_flag, 0),
            function()
                se_if_then(
                    function()
                        local c = p_call("CHECK_MOTOR_STALL")
                        end_call(c)
                    end,
                    function() se_sequence(function()
                        se_set_field("obstruction", 1)
                        se_queue_event(0, EVENT_ID("EVT_OBSTRUCTION"), "obstruction")
                        if with_auto_reverse then
                            se_sequence(function()
                                se_set_field("auto_reverse_req", 1)
                                se_queue_event(0, EVENT_ID("EVT_AUTO_REVERSE"), "auto_reverse_req")
                                se_log("FAULT: Obstruction - auto-reversing")
                            end)
                        else
                            se_log("FAULT: Obstruction detected")
                        end
                    end) end
                )
            end
        )
    end
end

-- ============================================================================
-- COMPOSITE HELPERS (emit DSL nodes inline)
-- ============================================================================

local function concurrent_monitors(guard_flag, limit_pred, done_flag, done_event, auto_reverse)
    se_fork(
        monitor_current(guard_flag),
        monitor_limit_switch(guard_flag, limit_pred, done_flag, done_event),
        monitor_obstruction(guard_flag, auto_reverse)
    )
end

local function wait_for_completion(position_flag, timeout_msg)
    se_chain_flow(function()
        se_verify_and_check_elapsed_time(30.0, false, function()
            se_log(timeout_msg)
        end)
        se_wait(pred_any_stop(position_flag))
    end)
end

-- ============================================================================
-- TRANSITION HELPERS
-- ============================================================================

local function transition_after_open()
    se_cond({
        se_cond_case(
            function() se_field_eq("emergency", 1) end,
            function() se_set_field("motor_state", STATE_EMERGENCY) end
        ),
        se_cond_case(
            function() se_field_eq("obstruction", 1) end,
            function() se_set_field("motor_state", STATE_EMERGENCY) end
        ),
        se_cond_case(
            function() se_field_eq("over_current", 1) end,
            function() se_set_field("motor_state", STATE_EMERGENCY) end
        ),
        se_cond_default(function() se_sequence(function()
            se_set_field("stop_request", 0)
            se_set_field("motor_state", STATE_IDLE)
        end) end),
    })
end

local function transition_after_close()
    se_cond({
        se_cond_case(
            function() se_field_eq("auto_reverse_req", 1) end,
            function() se_sequence(function()
                se_set_field("auto_reverse_req", 0)
                se_set_field("obstruction", 0)
                se_set_field("motor_state", STATE_AUTO_REVERSE)
            end) end
        ),
        se_cond_case(
            function() se_field_eq("emergency", 1) end,
            function() se_set_field("motor_state", STATE_EMERGENCY) end
        ),
        se_cond_case(
            function() se_field_eq("over_current", 1) end,
            function() se_set_field("motor_state", STATE_EMERGENCY) end
        ),
        se_cond_default(function() se_sequence(function()
            se_set_field("stop_request", 0)
            se_set_field("motor_state", STATE_IDLE)
        end) end),
    })
end

-- ============================================================================
-- BRANCH 1: SERIAL COMMAND DISPATCH (table-indexed cases)
-- ============================================================================

local cmd_cases = {}

cmd_cases[1] = function()
    se_case(CMD_OPEN, function() se_chain_flow(function()
        se_set_field("open_request", 1)
        se_queue_event(0, EVENT_ID("EVT_OPEN_REQUEST"), "open_request")
        se_log("CMD: Open window")
        se_return_pipeline_reset()
    end) end)
end

cmd_cases[2] = function()
    se_case(CMD_CLOSE, function() se_chain_flow(function()
        se_set_field("close_request", 1)
        se_queue_event(0, EVENT_ID("EVT_CLOSE_REQUEST"), "close_request")
        se_log("CMD: Close window")
        se_return_pipeline_reset()
    end) end)
end

cmd_cases[3] = function()
    se_case(CMD_STOP, function() se_chain_flow(function()
        se_set_field("stop_request", 1)
        se_queue_event(0, EVENT_ID("EVT_STOP_REQUEST"), "stop_request")
        se_log("CMD: Stop")
        se_return_pipeline_reset()
    end) end)
end

cmd_cases[4] = function()
    se_case(CMD_STATUS, function() se_chain_flow(function()
        se_set_field("status_request", 1)
        se_queue_event(0, EVENT_ID("EVT_STATUS_REQUEST"), "status_request")
        se_log("CMD: Status request")
        se_return_pipeline_reset()
    end) end)
end

cmd_cases[5] = function()
    se_case(CMD_EMERGENCY, function() se_chain_flow(function()
        se_set_field("emergency", 1)
        se_queue_event(0, EVENT_ID("EVT_EMERGENCY_STOP"), "emergency")
        se_log("CMD: Emergency stop!")
        se_return_pipeline_reset()
    end) end)
end

cmd_cases[6] = function()
    se_case("default", function() se_chain_flow(function()
        se_log("CMD: Unknown serial command")
        se_return_pipeline_halt()
    end) end)
end

local function branch_serial_handler()
    se_chain_flow(
        function()
            se_wait(pred_field_eq("serial_data_avail", 1))
        end,
        function() se_sequence(function()
            local c = o_call("READ_SERIAL_MESSAGE")
            end_call(c)
            local c2 = o_call("PARSE_MESSAGE_TYPE")
            end_call(c2)
        end) end,
        function()
            se_field_dispatch("serial_msg_type", cmd_cases)
        end
    )
end

-- ============================================================================
-- BRANCH 2: MOTOR STATE MACHINE (table-indexed cases)
-- ============================================================================

local motor_cases = {}

-- STATE 0: IDLE
motor_cases[1] = function()
    se_case(STATE_IDLE, function() se_chain_flow(function()
        motor_kill()
        se_log("MOTOR: Idle")
        se_cond({
            se_cond_case(
                function() se_field_eq("open_request", 1) end,
                function() se_sequence(function()
                    se_set_field("open_request", 0)
                    se_set_field("motor_state", STATE_OPENING)
                end) end
            ),
            se_cond_case(
                function() se_field_eq("close_request", 1) end,
                function() se_sequence(function()
                    se_set_field("close_request", 0)
                    se_set_field("motor_state", STATE_CLOSING)
                end) end
            ),
            se_cond_case(
                function() se_field_eq("emergency", 1) end,
                function() se_set_field("motor_state", STATE_EMERGENCY) end
            ),
            se_cond_default(function()
                se_return_pipeline_halt()
            end),
        })
    end) end)
end

-- STATE 1: OPENING
motor_cases[2] = function()
    se_case(STATE_OPENING, function() se_chain_flow(function()
        se_log("MOTOR: Opening window")
        motor_start(BRIDGE_FORWARD, "open_speed")
        concurrent_monitors("fully_open", "CHECK_LIMIT_SWITCH_OPEN",
                            "fully_open", "EVT_FULLY_OPEN", false)
        wait_for_completion("fully_open", "TIMEOUT: Opening exceeded 30s")
        motor_shutdown("DOOR_OPEN")
        transition_after_open()
    end) end)
end

-- STATE 2: CLOSING
motor_cases[3] = function()
    se_case(STATE_CLOSING, function() se_chain_flow(function()
        se_log("MOTOR: Closing window")
        motor_start(BRIDGE_REVERSE, "close_speed")
        concurrent_monitors("fully_closed", "CHECK_LIMIT_SWITCH_CLOSED",
                            "fully_closed", "EVT_FULLY_CLOSED", true)
        wait_for_completion("fully_closed", "TIMEOUT: Closing exceeded 30s")
        motor_shutdown("DOOR_CLOSED")
        transition_after_close()
    end) end)
end

-- STATE 3: AUTO-REVERSE
motor_cases[4] = function()
    se_case(STATE_AUTO_REVERSE, function() se_chain_flow(function()
        se_log("MOTOR: Auto-reverse after obstruction")
        disable_motor()
        se_set_field("motor_enabled", 0)
        se_tick_delay(5)
        -- Reverse at half speed
        se_set_field("bridge_mode", BRIDGE_FORWARD)
        apply_bridge_mode()
        set_pwm_value(100)
        enable_motor()
        se_set_field("motor_enabled", 1)
        se_tick_delay(50)
        -- Shutdown to freewheel
        disable_motor()
        se_set_field("motor_enabled", 0)
        se_set_field("bridge_mode", BRIDGE_FREEWHEEL)
        apply_bridge_mode()
        send_status("OBSTRUCTION_DETECTED")
        se_log("MOTOR: Auto-reverse complete")
        se_set_field("motor_state", STATE_IDLE)
        se_return_pipeline_reset()
    end) end)
end

-- STATE 4: EMERGENCY
motor_cases[5] = function()
    se_case(STATE_EMERGENCY, function() se_chain_flow(function()
        se_log("MOTOR: EMERGENCY STOP")
        motor_kill()
        send_status("EMERGENCY_STOPPED")
        se_wait(pred_field_eq("manual_reset", 1))
        se_log("MOTOR: Manual reset received")
        clear_all_faults()
        se_set_field("motor_state", STATE_IDLE)
        se_return_pipeline_reset()
    end) end)
end

-- DEFAULT
motor_cases[6] = function()
    se_case("default", function() se_chain_flow(function()
        se_log("MOTOR: Invalid state - entering emergency")
        se_set_field("motor_state", STATE_EMERGENCY)
        se_return_pipeline_reset()
    end) end)
end

-- ============================================================================
-- BRANCH 3: STATUS REPORTER
-- ============================================================================

local function branch_status_reporter()
    se_chain_flow(
        function()
            se_wait(function()
                pred_begin()
                    local or_id = se_pred_or()
                        se_field_eq("status_request", 1)
                        se_check_event(EVENT_ID("EVT_PERIODIC_1S"))
                    pred_close(or_id)
                local p = pred_end()
                p()
            end)
        end,
        function() se_sequence(function()
            local c1 = o_call("READ_DIAGNOSTICS_TLE7269G")
            end_call(c1)
            local c2 = o_call("READ_CURRENT_POSITION")
            end_call(c2)
            local c3 = o_call("READ_MOTOR_CURRENT")
            end_call(c3)
            local c4 = o_call("READ_TEMPERATURE")
            end_call(c4)
            local c5 = o_call("ASSEMBLE_STATUS_MESSAGE")
            end_call(c5)
            local c6 = o_call("SEND_SERIAL_STATUS")
            end_call(c6)
        end) end,
        function() se_sequence(function()
            se_set_field("status_request", 0)
            se_log("STATUS: Report sent")
            se_return_pipeline_reset()
        end) end
    )
end

-- ============================================================================
-- BRANCH 4: DIAGNOSTICS MONITORS
-- ============================================================================

local function monitor_thermal()
    se_while(
        pred_field_eq("system_shutdown", 0),
        function()
            se_if_then(
                pred_field_gt("temperature", 85.0),
                function() se_sequence(function()
                    se_log("DIAG: Thermal shutdown!")
                    se_queue_event(0, EVENT_ID("EVT_THERMAL_SHUTDOWN"), "temperature")
                    se_set_field("emergency", 1)
                    se_queue_event(0, EVENT_ID("EVT_EMERGENCY_STOP"), "emergency")
                end) end
            )
        end
    )
end

local function monitor_voltage()
    se_while(
        pred_field_eq("system_shutdown", 0),
        function()
            se_if_then(
                function()
                    pred_begin()
                        local or_id = se_pred_or()
                            se_field_lt("voltage", 9.0)
                            se_field_gt("voltage", 16.0)
                        pred_close(or_id)
                    local p = pred_end()
                    p()
                end,
                function() se_sequence(function()
                    se_log("DIAG: Voltage out of range!")
                    se_queue_event(0, EVENT_ID("EVT_VOLTAGE_ERROR"), "voltage")
                    se_set_field("emergency", 1)
                    se_queue_event(0, EVENT_ID("EVT_EMERGENCY_STOP"), "emergency")
                end) end
            )
        end
    )
end

local function monitor_tle_fault()
    se_while(
        pred_field_eq("system_shutdown", 0),
        function()
            se_if_then(
                pred_field_eq("fault_pin_active", 1),
                function() se_sequence(function()
                    local c = o_call("READ_FAULT_STATUS_TLE7269G")
                    end_call(c)
                    se_log("DIAG: TLE7269G fault detected")
                    se_set_field("emergency", 1)
                    se_queue_event(0, EVENT_ID("EVT_EMERGENCY_STOP"), "emergency")
                end) end
            )
        end
    )
end

local function branch_diagnostics()
    se_fork(
        monitor_thermal,
        monitor_voltage,
        monitor_tle_fault
    )
end

-- ============================================================================
-- TREE
-- ============================================================================

start_tree("door_controller")
    use_record("door_controller_bb")
    use_defaults("door_defaults")

    se_function_interface(function()
        se_i_set_field("motor_state", STATE_IDLE)
        se_log("Door window controller started")
        se_fork(
            branch_serial_handler,                                    -- direct ref, fine
            function() se_state_machine("motor_state", motor_cases) end,  -- NEEDS wrapper
            branch_status_reporter,                                   -- direct ref, fine
            branch_diagnostics                                        -- direct ref, fine
        )
    end)

end_tree("door_controller")

return end_module(mod)