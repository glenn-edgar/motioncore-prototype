local M = require("s_expr_dsl")
local mod = start_module("return_tests")
use_32bit()
set_debug(true)

-- ============================================================================
-- APPLICATION RESULT CODE TESTS (0-5)
-- ============================================================================

start_tree("return_continue_test")
    se_return_continue()
end_tree("return_continue_test")

start_tree("return_halt_test")
    se_return_halt()
end_tree("return_halt_test")

start_tree("return_terminate_test")
    se_return_terminate()
end_tree("return_terminate_test")

start_tree("return_reset_test")
    se_return_reset()
end_tree("return_reset_test")

start_tree("return_disable_test")
    se_return_disable()
end_tree("return_disable_test")

start_tree("return_skip_continue_test")
    se_return_skip_continue()
end_tree("return_skip_continue_test")

-- ============================================================================
-- FUNCTION RESULT CODE TESTS (6-11)
-- ============================================================================

start_tree("return_function_continue_test")
    se_return_function_continue()
end_tree("return_function_continue_test")

start_tree("return_function_halt_test")
    se_return_function_halt()
end_tree("return_function_halt_test")

start_tree("return_function_terminate_test")
    se_return_function_terminate()
end_tree("return_function_terminate_test")

start_tree("return_function_reset_test")
    se_return_function_reset()
end_tree("return_function_reset_test")

start_tree("return_function_disable_test")
    se_return_function_disable()
end_tree("return_function_disable_test")

start_tree("return_function_skip_continue_test")
    se_return_function_skip_continue()
end_tree("return_function_skip_continue_test")

-- ============================================================================
-- PIPELINE RESULT CODE TESTS (12-17)
-- ============================================================================

start_tree("return_pipeline_continue_test")
    se_return_pipeline_continue()
end_tree("return_pipeline_continue_test")

start_tree("return_pipeline_halt_test")
    se_return_pipeline_halt()
end_tree("return_pipeline_halt_test")

start_tree("return_pipeline_terminate_test")
    se_return_pipeline_terminate()
end_tree("return_pipeline_terminate_test")

start_tree("return_pipeline_reset_test")
    se_return_pipeline_reset()
end_tree("return_pipeline_reset_test")

start_tree("return_pipeline_disable_test")
    se_return_pipeline_disable()
end_tree("return_pipeline_disable_test")

start_tree("return_pipeline_skip_continue_test")
    se_return_pipeline_skip_continue()
end_tree("return_pipeline_skip_continue_test")

-- ============================================================================

local result = end_module(mod)
print("Module compiled successfully: " .. result.name)

return result