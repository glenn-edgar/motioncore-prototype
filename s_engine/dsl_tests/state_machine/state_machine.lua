-- ============================================================================
-- state_machine.lua
-- S-Expression DSL State Machine Test
-- Reconstructed from conversation history
-- ============================================================================

local M = require("s_expr_dsl")
local mod = start_module("state_machine_test")
use_32bit()
set_debug(true)

-- ============================================================================
-- RECORD DEFINITION
-- ============================================================================

RECORD("state_machine_blackboard")
    FIELD("state", "int32")
END_RECORD()

-- ============================================================================
-- EMPTY TREE (placeholder)
-- ============================================================================



-- ============================================================================
-- STATE MACHINE TEST
-- Uses se_case/se_field_dispatch pattern
-- ============================================================================

case_fn = {}

case_fn[1] = function() 
    se_case(0, function()
        se_sequence(function()
            se_log("State 0")
            local o0 = o_call("CFL_DISABLE_CHILDREN")
            end_call(o0)
            local o1 = o_call("CFL_ENABLE_CHILD")
                int(0)
            end_call(o1)
            se_tick_delay(10)
            se_set_field("state", 1)
            se_log("State 0 terminated")
            se_return_pipeline_disable()
        end)
    end) 
end

case_fn[2] = function() 
    se_case(1, function()
        se_sequence(function()
            se_log("State 1")
            local o0 = o_call("CFL_DISABLE_CHILDREN")
            end_call(o0)
            local o1 = o_call("CFL_ENABLE_CHILD")
                int(1)
            end_call(o1)
            se_tick_delay(10)
            se_set_field("state", 2)
            se_log("State 1 terminated")
            se_return_pipeline_disable()
        end)
    end) 
end

case_fn[3] = function() 
    se_case(2, function()
        se_sequence(function()
            se_log("State 2")
            local o0 = o_call("CFL_DISABLE_CHILDREN")
            end_call(o0)
            local o1 = o_call("CFL_ENABLE_CHILD")
                int(2)
            end_call(o1)
            se_tick_delay(10)
            se_set_field("state", 0)
            se_log("State 2 terminated")
            se_return_pipeline_disable()
        end)
    end) 
end

case_fn[4] = function() 
    se_case('default', function()
        se_sequence(function()
            se_log("State 2")
            local o0 = o_call("CFL_DISABLE_CHILDREN")
            end_call(o0)
            local o1 = o_call("CFL_ENABLE_CHILD")
                int(2)
            end_call(o1)
            se_tick_delay(100)
            se_log("State 2 terminated")
            se_return_terminate()
        end)
    end) 
end

start_tree("state_machine_test")
    use_record("state_machine_blackboard")

    se_function_interface(function()
        se_log("Fork Join Test Started")
        se_fork_join(function()
            se_log("Fork Join Test Started")
            se_tick_delay(10)
            se_log("Fork Join Test Terminated")
        end)

        se_fork(function()
            se_sequence(function()
            se_log("Fork 1 Test Started")
                se_tick_delay(10)
                se_log("Fork 1 Test Terminated")
            end)
        end)
        -- Removed the extra end) here
        
        se_i_set_field("state", 0)
        se_log("State machine test started")
        se_state_machine("state", case_fn)
        se_tick_delay(350)
        se_log("State machine test finished")
        se_return_function_terminate()
    end)
    
    
end_tree("state_machine_test")



return end_module(mod)