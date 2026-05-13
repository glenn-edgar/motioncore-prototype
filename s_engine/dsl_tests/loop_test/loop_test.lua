-- ============================================================================
-- state_machine.lua
-- S-Expression DSL State Machine Test
-- Reconstructed from conversation history
-- ============================================================================

local M = require("s_expr_dsl")
local mod = start_module("loop_test")
use_32bit()
set_debug(true)

-- ============================================================================
-- RECORD DEFINITION
-- ============================================================================

RECORD("loop_test_blackboard")
    FIELD("outer_sequence_counter", "uint32")
    FIELD("inner_sequence_counter", "uint32")
    FIELD("field_test_counter", "uint32")
    FIELD("field_test_increment", "uint32")
    FIELD("field_test_limit", "uint32")
END_RECORD()



inner_sequence_fn = function()
    se_chain_flow(function()
        se_log("inner_sequence_fn start")
        se_log_slot_integer("inner_sequence_counter %d","inner_sequence_counter")
        se_increment_field("inner_sequence_counter", 1)
        se_tick_delay(3)
        se_log("inner_sequence_fn end")
        se_return_pipeline_disable()
    end)
end

loop_sequence_fn = function()
    se_chain_flow(function()
        se_log("loop_sequence_fn start")
        se_log_slot_integer("outer_sequence_counter %d","outer_sequence_counter")
        se_increment_field("outer_sequence_counter", 1)
        inner_sequence_fn()
        se_tick_delay(5)
        se_log("loop_sequence_fn end")
        se_return_pipeline_disable()
    end)
end

loop_test_fn_1 = function()
    se_while(se_state_increment_and_test(1,10),loop_sequence_fn)
end
loop_test_fn_2 = function()
    se_set_field("field_test_increment", 1)
    se_set_field("field_test_limit", 10)
    se_while(se_field_increment_and_test("field_test_counter","field_test_increment","field_test_limit"),loop_sequence_fn)
end
start_tree("loop_test")
    use_record("loop_test_blackboard")

    se_function_interface(function()
        se_set_field("outer_sequence_counter", 0)
        se_set_field("inner_sequence_counter", 0)
        se_fork_join(loop_test_fn_1)
        se_fork_join(loop_test_fn_2)
        se_return_terminate()
    end)
    
    
end_tree("loop_test")

return end_module(mod)

