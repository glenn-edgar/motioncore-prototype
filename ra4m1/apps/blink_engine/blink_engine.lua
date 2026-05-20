-- ============================================================================
-- blink_engine.lua
-- Minimal single-tree, single-node M-port DSL chain for SAMD21 Xiao bring-up.
-- Calls user main `toggle_led` every engine tick (returns SE_CONTINUE).
-- ============================================================================

local M = require("s_expr_dsl")
local mod = start_module("blink_engine")
use_32bit()

start_tree("blink_engine")
    local c = m_call("toggle_led")
    end_call(c)
end_tree("blink_engine")

return end_module(mod)
