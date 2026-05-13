---
---



local M = require("s_expr_dsl")
local mod = start_module("demo_test")
use_32bit()
set_debug(true)
----

RECORD("state_machine_blackboard")
    FIELD("state", "int32")
END_RECORD()


start_tree("event_dispatch_test")


end_tree("event_dispatch_test")




-- ============================================================================
-- TEST 4: State Machine
-- ============================================================================

case_fn = {}

case_fn[1] = function() se_case(0, function()
    se_sequence(function()
        se_log("State 0")
        local o0=o_call("CFL_DISABLE_CHILDREN")
        end_call(o0)
        local o1=o_call("CFL_ENABLE_CHILD")
        int(0)
        end_call(o1)
        se_tick_delay(100)
        se_set_field("state", 1)
        se_return_halt()
    end)
end) end


case_fn[2] = function() se_case(1, function()
    se_sequence(function()
        se_log("State 1")
        local o0=o_call("CFL_DISABLE_CHILDREN")
        end_call(o0)
        local o1=o_call("CFL_ENABLE_CHILD")
        int(1)
        end_call(o1)
        se_tick_delay(100)
        se_set_field("state", 2)
        se_return_halt()
    end)
end) end



case_fn[3] = function() se_case('default', function()
    se_sequence(function()
        se_log("State 2")
        local o0=o_call("CFL_DISABLE_CHILDREN")
        end_call(o0)
        local o1=o_call("CFL_ENABLE_CHILD")
        int(2)
        end_call(o1)
        se_tick_delay(100)
        se_log("State 2 terminated")
        se_return_terminate()
    end)
end) end

start_tree("state_machine_test")
    use_record("state_machine_blackboard")

    se_sequence(function()
         se_i_set_field("state", 0)
         se_log("State machine test started")
         se_field_dispatch("state", case_fn)
    end)
    
    
end_tree("state_machine_test")

--[[
event_dispatch_fn = {}

event_dispatch_fn[1] = function() se_case(0, function()
    se_sequence(function()
        se_log("State 0")
        local o0=o_call("CFL_DISABLE_CHILDREN")
        end_call(o0)
        local o1=o_call("CFL_ENABLE_CHILD")
        int(0)
        end_call(o1)
        se_tick_delay(100)
        se_set_field("state", 1)
        se_return_halt()
    end)
end) end


event_dispatch_fn[2] = function() se_case(1, function()
    se_sequence(function()
        se_log("State 1")
        local o0=o_call("CFL_DISABLE_CHILDREN")
        end_call(o0)
        local o1=o_call("CFL_ENABLE_CHILD")
        int(1)
        end_call(o1)
        se_tick_delay(100)
        se_set_field("state", 2)
        se_return_halt()
    end)
end) end



event_dispatch_fn[3] = function() se_case('default', function()
    se_sequence(function()
        se_log("State 2")
        local o0=o_call("CFL_DISABLE_CHILDREN")
        end_call(o0)
        local o1=o_call("CFL_ENABLE_CHILD")
        int(2)
        end_call(o1)
        se_tick_delay(100)
        se_log("State 2 terminated")
        se_return_terminate()
    end)
end) end





start_tree("event_dispatch_test")
    use_record("state_machine_blackboard")

    se_sequence(function()
         se_i_set_field("state", 0)
         se_log("State machine test started")
         se_field_dispatch("state", event_dispatch_fn)
    end)
    
    
end_tree("event_dispatch_test")
--]]

local result = end_module(mod)
print("Module compiled successfully: " .. result.name)

--print(M.write_debug_header(result))
return result

