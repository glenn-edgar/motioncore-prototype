-- ============================================================================
-- state_machine.lua
-- S-Expression DSL State Machine Test
-- Reconstructed from conversation history
-- ============================================================================

local M = require("s_expr_dsl")
local mod = start_module("advanced_primitive_test")
use_32bit()
set_debug(true)

-- ============================================================================
-- RECORD DEFINITION
-- ============================================================================

RECORD("state_machine_blackboard")
    FIELD("state", "int32")
    FIELD("event_data_1", "float")
    FIELD("event_data_2", "float")
    FIELD("event_data_3", "float")
    FIELD("event_data_4", "float")
END_RECORD()

local USER_EVENT_TYPE = 1
local USER_EVENT_1 = 1
local USER_EVENT_2 = 2
local USER_EVENT_3 = 3
local USER_EVENT_4 = 4

-- ============================================================================
-- EMPTY TREE (placeholder)
-- ============================================================================





-- ============================================================================
-- ALTERNATIVE: Direct se_state_machine pattern (no se_case)
-- ============================================================================

event_displays_fn = {}
event_displays_fn[1] = function() 
    
        se_chain_flow(function()
            se_log("if then branch  start")
            local o0 = o_call("DISPLAY_EVENT_INFO")
            end_call(o0)
            se_log("if then branch  end")
            se_return_pipeline_reset()
        end) 
end

event_displays_fn[2] = function() 
    
        se_chain_flow(function()
            se_log("if else branch start")
            local o0 = o_call("DISPLAY_EVENT_INFO")
            end_call(o0)
            se_log("if else branch end")
            se_return_pipeline_reset()
        end)
     
end

event_actions_fn = {}
event_actions_fn[1] = function() 
    se_event_case(USER_EVENT_1, function()
        se_chain_flow(function()
            se_log("event_actions_fn[1]")
            local o0 = o_call("DISPLAY_EVENT_INFO")
            end_call(o0)
            -- se_if_then_else - pred_fn is a closure, call it
            se_if_then_else(
                se_check_event(USER_EVENT_1, USER_EVENT_3),
                event_displays_fn[1],
                event_displays_fn[2]
            )
            se_set_field("state", 1)
            se_log("event_actions_fn[1] terminated")
            se_return_pipeline_reset()
        end)
    end) 
end

event_actions_fn[2] = function() 
    se_event_case(USER_EVENT_3, function()
        se_chain_flow(function()
            se_log("event_actions_fn[2]")
            local o0 = o_call("DISPLAY_EVENT_INFO")
            end_call(o0)
            se_set_field("state", 2)
            se_log("event_actions_fn[2] terminated")
            se_return_pipeline_reset()
        end)
    end) 
end

event_actions_fn[3] = function() 
    se_event_case(USER_EVENT_2, function()
        se_chain_flow(function()
            se_log("event_actions_fn[3]")
            local o0 = o_call("DISPLAY_EVENT_INFO")
            end_call(o0)
            se_log("event_actions_fn[3] terminated")
            se_return_pipeline_reset()
        end)
    end) 
end

event_actions_fn[4] = function() 
    se_event_case(USER_EVENT_4, function()
        se_chain_flow(function()
            se_log("event_actions_fn[4]")
            -- se_if_then_else - pred_fn is a closure, call it
            se_if_then_else(
                se_check_event(USER_EVENT_1, USER_EVENT_3),
                event_displays_fn[1],
                event_displays_fn[2]
            )
            local o0 = o_call("DISPLAY_EVENT_INFO")
            end_call(o0)
            se_log("event_actions_fn[4] terminated")
            se_return_pipeline_reset()
        end)
    end) 
end
event_actions_fn[5] = function() 
    se_event_case('default', function()
        se_chain_flow(function()
        se_return_pipeline_halt()            
        end)
    end) 
end


function test_cond_instruction()
    se_cond({
        se_cond_case(
            se_check_event(USER_EVENT_1, USER_EVENT_3),
            function()
                se_chain_flow(function()
                    se_log("Matched EVENT_1 or EVENT_3")
                    local o0 = o_call("DISPLAY_EVENT_INFO")
                    end_call(o0)
                    se_return_pipeline_reset()
                end)
            end
        ),
        se_cond_case(
            se_check_event(USER_EVENT_2),
            function()
                se_chain_flow(function()
                    se_log("Matched EVENT_2")
                    local o0 = o_call("DISPLAY_EVENT_INFO")
                    end_call(o0)
                    se_return_pipeline_reset()
                end)
            end
        ),
        se_cond_case(
            se_check_event(USER_EVENT_4),
            function()
                se_chain_flow(function()
                    se_log("Matched EVENT_4")
                    local o0 = o_call("DISPLAY_EVENT_INFO")
                    end_call(o0)
                    se_return_pipeline_reset()
                end)
            end
        ),   
        se_cond_default(
            function()
                se_chain_flow(function()
                    se_return_pipeline_reset()
                end)
            end
        )
    })
end


state_case_fn = {}
state_case_fn[1] = function() 
    se_case(0, function()
        se_chain_flow(function()
            se_log("State 0")
            se_tick_delay(20)
            se_set_field("event_data_1", 1.1)
            se_set_field("event_data_2", 2.2)
            se_queue_event( USER_EVENT_TYPE, USER_EVENT_1, "event_data_1")
            se_queue_event( USER_EVENT_TYPE, USER_EVENT_2, "event_data_2")
            se_return_pipeline_reset()
        end)
    end) 
end

state_case_fn[2] = function() 
    se_case(1, function()
        se_chain_flow(function()
            se_log("State 1")
            se_tick_delay(20)
            se_set_field("event_data_3", 3.3)
            se_set_field("event_data_4", 4.4)
            se_queue_event( USER_EVENT_TYPE, USER_EVENT_3, "event_data_3")
            se_queue_event( USER_EVENT_TYPE, USER_EVENT_4, "event_data_4")
            se_return_pipeline_reset()
        end)
    end) 
end

state_case_fn[3] = function() 
    se_case(2, function()
        se_fork(function()
            se_log("State 2")
            se_tick_delay(20)
            se_log("State 2 terminated")
            se_return_terminate()
        end)
    end) 
end

start_tree("dispatch_test")
    use_record("state_machine_blackboard")

    se_function_interface(function()
        se_i_set_field("state", 0)
        se_log("State machine test started ")
         se_event_dispatch(event_actions_fn)
         test_cond_instruction()
        se_state_machine("state", state_case_fn)
    end)
    
    
end_tree("dispatch_test")

return end_module(mod)

