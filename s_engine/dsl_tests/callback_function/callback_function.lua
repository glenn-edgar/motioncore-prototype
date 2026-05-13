-- function_dictionary_test.lua
-- Test for function dictionary with quad_expr/quad_multi expressions
-- Uses realistic STM32F4 peripheral base addresses
--
-- IMPORTANT: All values that will be read after a stack_push_ref() must be
-- stored in frame locals, NOT scratch (TOS) variables. stack_push advances
-- sp which can invalidate scratch-relative offsets.

local mod = start_module("callback_function")

-- ============================================================================
-- RECORD: cpu_config_blackboard
-- ============================================================================

RECORD("callback_function_blackboard")
   
    PTR64_FIELD("fn_ptr","void")
    
   
END_RECORD()


start_tree("callback_function")
use_record("callback_function_blackboard")


-- ============================================================================
-- MAIN PROGRAM
-- ============================================================================

local fns = function()
    se_sequence_once(function()
    se_log("callback function called")
    se_log("do some stack work")
    se_log("call a dictionary function")
    end)
end




se_function_interface(function()
    
    se_log("callback test started")
    se_load_function("fn_ptr",fns)
    se_exec_function("fn_ptr")
 
    se_return_function_terminate()
end)

end_tree("callback_function")

return end_module(mod)