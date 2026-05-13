--============================================================================
-- s_engine_helpers.lua
-- Core S-Expression Engine Helper Functions - Version 5.2
-- 
-- Modular loader - includes all sub-modules in dependency order.
--============================================================================

dofile("se_helpers_dir/s_engine_equation.lua")

-- Shared field validation (used by dictionary and function_dict modules)
dofile("se_helpers_dir/se_field_validation.lua")

-- Result code emitters (application, function, pipeline)
dofile("se_helpers_dir/se_result_codes.lua")

-- Predicates (builder, composites, leaves, emit_typed_value)
-- NOTE: Must load before se_oneshot.lua which uses emit_typed_value
dofile("se_helpers_dir/se_predicates.lua")

-- Oneshot operations (log, set field, inc/dec, push stack)
dofile("se_helpers_dir/se_oneshot.lua")

-- Control flow (sequence, if/then, fork, while, cond)
dofile("se_helpers_dir/se_control_flow.lua")

-- Timing, delays, waits, verify, event queueing
dofile("se_helpers_dir/se_timing_events.lua")

-- State machine and event dispatch
dofile("se_helpers_dir/se_state_machine.lua")

-- Dictionary/JSON loading and extraction
dofile("se_helpers_dir/se_dictionary.lua")

-- Quad operations (arithmetic, comparison, logical, math, trig)
dofile("se_helpers_dir/se_quad_ops.lua")

-- Predicate quad operations (boolean comparisons, accumulate, range)
dofile("se_helpers_dir/se_p_quad_ops.lua")

-- Stack frame management (instance, call, frame_allocate)
dofile("se_helpers_dir/se_stack_frame.lua")

-- Function dictionary (load, exec, spawn, internal dispatch)
dofile("se_helpers_dir/se_function_dict.lua")

--- chain tree functions
dofile("se_helpers_dir/se_chain_tree.lua")

print("S-Expression Engine helpers loaded (v5.2)")