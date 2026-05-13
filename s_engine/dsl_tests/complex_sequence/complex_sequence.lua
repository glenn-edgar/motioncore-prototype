-- ============================================================================
-- complex_sequence.lua
-- S-Expression DSL Complex Sequence Test
-- ============================================================================

local M = require("s_expr_dsl")
local mod = start_module("complex_sequence")
use_32bit()
set_debug(true)

-- ============================================================================
-- RECORD DEFINITION
-- ============================================================================

RECORD("complex_sequence_blackboard")
    FIELD("complex_sequence_condition_1", "uint32")
    FIELD("complex_sequence_condition_2", "uint32")
    FIELD("event_field", "float")
    FIELD("field_test_counter", "uint32")
    FIELD("field_test_increment", "uint32")
    FIELD("field_test_limit", "uint32")
END_RECORD()

-- ============================================================================
-- TEST FUNCTION
-- ============================================================================

wait_event_test_fn = function()
    se_fork(function()
        -- Event generator
        se_chain_flow(function()
            se_log("event generator start")
            se_set_field("field_test_counter", 0)
            se_set_field("field_test_increment", 1)
            se_set_field("field_test_limit", 10)
            se_while(se_field_increment_and_test("field_test_counter", "field_test_increment", "field_test_limit"), function()
                se_log_slot_integer("event generator iteration %d", "field_test_counter")
                se_time_delay(1.0)
                se_set_field("event_field", 10.25)
                se_queue_event(1, 43, "event_field")
            end)
            se_log("event generator end")
            se_return_pipeline_disable()
        end)

        -- Event waiter
        se_chain_flow(function()
            se_log("wait event test start")
            se_wait_event(43, 10)
            se_log("wait event test end")
            se_return_pipeline_disable()
        end)
    end)
end
error_function = function()
    se_log("verify timeout expired this is expected")
end
verify_time_test_fn = function()
    se_chain_flow(function()
        se_log("verify time test start")
        se_verify_and_check_elapsed_time(5.0,false,error_function)
        se_log("waiting for termination due to eladoed time")
        se_return_pipeline_continue()  -- keep pipeline running
    end)
end
verify_events_test_fn = function()
    se_fork(function()
        -- Event generator
        se_chain_flow(function()
            se_log("event generator start")
            se_set_field("field_test_counter", 0)
            se_set_field("field_test_increment", 1)
            se_set_field("field_test_limit", 10)
            se_while(se_field_increment_and_test("field_test_counter", "field_test_increment", "field_test_limit"), function()
                se_log_slot_integer("event generator iteration %d", "field_test_counter")
                se_time_delay(1.0)
                se_set_field("event_field", 10.25)
                se_queue_event(1, 43, "event_field")
            end)
            se_log("event generator end")
            se_return_pipeline_disable()
        end)
        se_chain_flow(function()
            se_log("verify events test start")
            se_verify_and_check_elapsed_events(43,9,false,error_function)
            se_log("waiting for termination due to elapsed events")
            se_return_pipeline_continue()  -- keep pipeline running
        end)
    end)
end

error_function_1 = function()
    se_log("verify timeout expired this is expected")
end
error_function_2 = function()
    se_log("verify timeout expired this is expected")
end

pred_fn_1 = se_field_eq("complex_sequence_condition_1", 1)
pred_fn_2 = se_field_eq("complex_sequence_condition_2", 1)

error_function_1 = function()
    se_log("error function 1 when high condition is expected and will produce a reset")
end
error_function_2 = function()
    se_log("error function 2 when low condition is expected and will produce a terminate")
end

pred_fn_1 = se_field_eq("complex_sequence_condition_1", 1)
pred_fn_2 = se_field_eq("complex_sequence_condition_2", 1)

complex_sequence_test_fn = function()

 se_fork(function()
    se_set_field("complex_sequence_condition_1", 0)
    se_set_field("complex_sequence_condition_2", 0)
    se_chain_flow(function()
        se_log("condition generator start")
        se_set_field("complex_sequence_condition_1", 0)
        se_set_field("complex_sequence_condition_2", 0)
        se_log("condition generator set 0,0")
        se_tick_delay(10)
        se_set_field("complex_sequence_condition_1", 1)
        se_log("condition generator set 1,0")
        se_tick_delay(10)
        se_set_field("complex_sequence_condition_2", 1)
        se_log("condition generator set 1,1")
        se_tick_delay(10)
        se_set_field("complex_sequence_condition_1", 0)
        se_log("condition generator set 0,1")
        se_tick_delay(10)
        se_set_field("complex_sequence_condition_1",1)
        se_log("condition generator set 1,1")
        se_tick_delay(10)
        se_set_field("complex_sequence_condition_2", 0)
        se_log("condition generator set 1,0")
        se_tick_delay(10)
        se_log("condition generator end")
        se_return_pipeline_terminate()
        
    end)
    
    se_chain_flow(function()
        se_log("complex sequence test start")
        se_wait(pred_fn_1)
        se_verify(pred_fn_1,true,error_function_1)
        se_log("complex sequence pass test 1")
        se_wait(pred_fn_2)
        se_verify(pred_fn_2,false,error_function_2)
        se_log("complex sequence pass test 2")
        se_log("waiting for error actions")
        se_return_pipeline_continue()  -- keep pipeline running
    end)
    
 end)
end
wait_timeout_test_fn = function()
    se_fork(function()
        se_chain_flow(function()
            se_log("wait timeout terminate test start")
            se_wait_timeout(se_false(), 5.0, false, error_function_2)
            se_log("waiting for timeout")
            se_return_pipeline_terminate()  -- keep pipeline running
        end)
        se_chain_flow(function()
            se_log("wait timeout resettest start")
            se_wait_timeout(se_false(), 2.0, true, error_function_1)
            se_log("waiting for timeout")
            se_return_pipeline_terminate()  -- keep pipeline running
        end)
        se_chain_flow(function()
            se_log("wait normal timeout test start")
            se_wait_timeout(se_true(), 5.0, false, error_function_1)
            se_time_delay(10.0)
            se_log("ending timeout test")
            se_return_terminate()  -- keep pipeline running
        end)
    end)

end
-- ============================================================================
-- TREE DEFINITION
-- ============================================================================

start_tree("complex_sequence_test")
    use_record("complex_sequence_blackboard")

    se_function_interface(function()
        se_fork_join(wait_event_test_fn)
        se_fork_join(verify_time_test_fn)
        se_fork_join(verify_events_test_fn)
        se_fork_join(complex_sequence_test_fn)
        se_fork_join(wait_timeout_test_fn)
        se_return_terminate()
    end)
end_tree("complex_sequence_test")

return end_module(mod)