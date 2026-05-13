local M = require("s_expr_dsl")
local mod = start_module("basic_primitive_test")
use_32bit()
set_debug(true)

-- ============================================================================
-- Predicate Definitions
-- ============================================================================

-- Trigger 1: Simple single bit test
local pred_bit0 = se_pred_with("TEST_BIT", function() int(0) end)

-- Trigger 2: AND of two bits
pred_begin()
    local and1 = se_pred_and()
        se_pred_with("TEST_BIT", function() int(1) end)
        se_pred_with("TEST_BIT", function() int(2) end)
    pred_close(and1)
local pred_bits_12 = pred_end()

-- Trigger 3: OR of two bits
pred_begin()
    local or1 = se_pred_or()
        se_pred_with("TEST_BIT", function() int(3) end)
        se_pred_with("TEST_BIT", function() int(4) end)
    pred_close(or1)
local pred_bits_34 = pred_end()

-- Trigger 4: NOT of a bit
pred_begin()
    local not1 = se_pred_not()
    
        se_pred_with("TEST_BIT", function() int(5) end)
    pred_close(not1)
    
local pred_not_bit5 = pred_end()

-- ============================================================================
-- Tree Definition
-- ============================================================================

start_tree("basic_primitive_test")
    
    se_function_interface(function()
        
        se_trigger_on_change(0, pred_bit0,
            function()
                se_chain_flow(function()
                    local rise = o_call("ON_BIT0_RISE")
                    end_call(rise)
                    se_log("ON_BIT0_RISE")
                    se_return_continue()
                end)
            end,
            function()
                se_chain_flow(function()
                    local fall = o_call("ON_BIT0_FALL")
                    end_call(fall)
                    se_log("ON_BIT0_FALL")
                    se_return_continue()
                end)
            end
        )
        
        se_trigger_on_change(0, pred_bits_12,
            function()
                se_chain_flow(function()
                    local rise = o_call("ON_BITS_12_RISE")
                    end_call(rise)
                    se_log("ON_BITS_12_RISE")
                    se_return_continue()
                end)
            end,
            function()
                se_chain_flow(function()
                    local fall = o_call("ON_BITS_12_FALL")
                    end_call(fall)
                    se_log("ON_BITS_12_FALL")
                    se_return_continue()
                end)
            end
        )
        
        se_trigger_on_change(0, pred_bits_34,
            function()
                se_chain_flow(function()
                    local rise = o_call("ON_BITS_34_RISE")
                    end_call(rise)
                    se_log("ON_BITS_34_RISE")
                    se_return_continue()
                end)
            end,
            function()
                se_chain_flow(function()
                    local fall = o_call("ON_BITS_34_FALL")
                    end_call(fall)
                    se_log("ON_BITS_34_FALL")
                    se_return_continue()
                end)
            end
        )
        
        se_trigger_on_change(1, pred_not_bit5,
            function()
                se_chain_flow(function()
                    local clear = o_call("ON_BIT5_CLEAR")
                    end_call(clear)
                    se_log("ON_BIT5_CLEAR")
                    se_return_continue()
                end)
            end,
            function()
                se_chain_flow(function()
                    local set = o_call("ON_BIT5_SET")
                    end_call(set)
                    se_log("ON_BIT5_SET")
                    se_return_continue()
                end)
            end
        )
        
        se_return_continue()
    end)
end_tree()

return end_module(mod)