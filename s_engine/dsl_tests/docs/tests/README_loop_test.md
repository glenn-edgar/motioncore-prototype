# Loop Test - Nested Loop Execution Example

## Overview

This test demonstrates nested `se_while` loops with both state-based and field-based counter predicates. It validates the loop control flow, predicate initialization, and proper termination behavior of the S-Expression engine.

## Lua DSL Source

```lua
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

-- ============================================================================
-- INNER SEQUENCE
-- ============================================================================

inner_sequence_fn = function()
    se_chain_flow(function()
        se_log("inner_sequence_fn start")
        se_log_slot_integer("inner_sequence_counter %d", "inner_sequence_counter")
        se_increment_field("inner_sequence_counter", 1)
        se_tick_delay(3)
        se_log("inner_sequence_fn end")
        se_return_pipeline_disable()
    end)
end

-- ============================================================================
-- OUTER LOOP BODY
-- ============================================================================

loop_sequence_fn = function()
    se_chain_flow(function()
        se_log("loop_sequence_fn start")
        se_log_slot_integer("outer_sequence_counter %d", "outer_sequence_counter")
        se_increment_field("outer_sequence_counter", 1)
        inner_sequence_fn()
        se_tick_delay(5)
        se_log("loop_sequence_fn end")
        se_return_pipeline_disable()
    end)
end

-- ============================================================================
-- LOOP TEST FUNCTIONS
-- ============================================================================

-- Test 1: State-based counter (uses node user_flags)
loop_test_fn_1 = function()
    se_while(se_state_increment_and_test(1, 10), loop_sequence_fn)
end

-- Test 2: Field-based counter (uses blackboard fields for counter, increment, limit)
loop_test_fn_2 = function()
    se_set_field("field_test_increment", 1)
    se_set_field("field_test_limit", 10)
    se_while(se_field_increment_and_test("field_test_counter", "field_test_increment", "field_test_limit"), loop_sequence_fn)
end

-- ============================================================================
-- TREE DEFINITION
-- ============================================================================

start_tree("loop_test")
    use_record("loop_test_blackboard")

    se_function_interface(function()
        se_set_field("outer_sequence_counter", 0)
        se_set_field("inner_sequence_counter", 0)
        se_fork_join(loop_test_fn_1)   -- Blocking: runs first 10 iterations
        se_fork_join(loop_test_fn_2)   -- Blocking: runs second 10 iterations
        se_return_terminate()
    end)
end_tree("loop_test")

return end_module(mod)
```

## Structure

```
se_function_interface
├── se_set_field("outer_sequence_counter", 0)     [ONESHOT]
├── se_set_field("inner_sequence_counter", 0)     [ONESHOT]
├── se_fork_join                                   [MAIN - blocking]
│   └── se_while(se_state_increment_and_test(1, 10))
│       └── loop_sequence_fn (se_chain_flow)
│           ├── se_log("loop_sequence_fn start")  [ONESHOT]
│           ├── se_log_slot_integer(...)          [ONESHOT]
│           ├── se_increment_field(...)           [ONESHOT]
│           ├── inner_sequence_fn (se_chain_flow)
│           │   ├── se_log("inner_sequence_fn start")
│           │   ├── se_log_slot_integer(...)
│           │   ├── se_increment_field(...)
│           │   ├── se_tick_delay(3)              [MAIN - 3 ticks]
│           │   ├── se_log("inner_sequence_fn end")
│           │   └── se_return_pipeline_disable()
│           ├── se_tick_delay(5)                  [MAIN - 5 ticks]
│           ├── se_log("loop_sequence_fn end")
│           └── se_return_pipeline_disable()
├── se_fork_join                                   [MAIN - blocking]
│   └── loop_test_fn_2
│       ├── se_set_field("field_test_increment", 1)  [ONESHOT]
│       ├── se_set_field("field_test_limit", 10)     [ONESHOT]
│       └── se_while(se_field_increment_and_test("field_test_counter", 
│                                                  "field_test_increment", 
│                                                  "field_test_limit"))
│           └── loop_sequence_fn (same as above)
└── se_return_terminate()                          [MAIN]
```

## Execution Profile Analysis

### Phase 1: First se_fork_join (Ticks 1-110)

The first `se_fork_join` blocks until all 10 iterations of `loop_test_fn_1` complete.

**Per-iteration breakdown:**

| Step | Ticks | Description |
|------|-------|-------------|
| Iteration start | 1 | ONESHOTs fire: "loop_sequence_fn start", log counter, increment |
| Inner sequence | 1 | ONESHOTs fire: "inner_sequence_fn start", log, increment |
| Inner delay | 3 | `se_tick_delay(3)` - chain halts |
| Inner complete | 1 | "inner_sequence_fn end", DISABLE propagates |
| Outer delay | 5 | `se_tick_delay(5)` - chain halts |
| Iteration end | 1 | "loop_sequence_fn end", DISABLE propagates, while loops back |

**Ticks per iteration:** ~11 ticks (1 + 4 + 5 + 1)

**Total for 10 iterations:** ~110 ticks

### Phase 2: Second se_fork_join (Ticks 111-220)

The second `se_fork_join` runs `loop_test_fn_2`, which:
1. Sets `field_test_increment` to 1 (ONESHOT)
2. Sets `field_test_limit` to 10 (ONESHOT)
3. Runs `se_while` with field-based counter

The loop uses `se_field_increment_and_test("field_test_counter", "field_test_increment", "field_test_limit")` — all three parameters are field references, allowing runtime configuration of the loop.

**Ticks per iteration:** ~11 ticks (same pattern as first loop)

**Total for 10 iterations:** ~110 ticks

### Phase 3: Termination (Tick 221)

After both `se_fork_join` blocks complete, `se_return_terminate()` executes and returns `SE_TERMINATE`.

### Summary

| Phase | Ticks | Description |
|-------|-------|-------------|
| First loop (state counter) | 1-110 | 10 iterations × ~11 ticks |
| Second loop (field counter) | 111-220 | 10 iterations × ~11 ticks |
| Termination | 221 | `se_return_terminate()` |
| **Total** | **221** | |

## Key Observations from Execution Log

### Counter Values

The log shows counters incrementing from 0 to 19:

```
outer_sequence_counter 0    (iteration 1, first loop)
inner_sequence_counter 0
...
outer_sequence_counter 9    (iteration 10, first loop)
inner_sequence_counter 9
outer_sequence_counter 10   (iteration 1, second loop)
inner_sequence_counter 10
...
outer_sequence_counter 19   (iteration 10, second loop)
inner_sequence_counter 19
```

The `outer_sequence_counter` and `inner_sequence_counter` fields are **not** reset between the two loops because they're blackboard fields shared across both test functions.

### Field-Based Loop Configuration

The second loop demonstrates runtime-configurable loop parameters:

```lua
loop_test_fn_2 = function()
    se_set_field("field_test_increment", 1)   -- Set increment at runtime
    se_set_field("field_test_limit", 10)      -- Set limit at runtime
    se_while(se_field_increment_and_test("field_test_counter", 
                                          "field_test_increment", 
                                          "field_test_limit"), 
             loop_sequence_fn)
end
```

This pattern allows:
- Dynamic loop counts based on runtime conditions
- Modifying increment during loop execution
- Adjusting limit based on external factors

### Blocking Behavior

The `se_fork_join` composites ensure sequential execution:

1. First loop runs to completion (all 10 iterations)
2. Only then does the second loop start
3. Finally, `se_return_terminate()` executes

Without `se_fork_join`, both loops would run in parallel.

### Tick Timing

Each iteration shows consistent timing:
- **Tick N**: Iteration starts, inner sequence starts
- **Ticks N+1 to N+3**: Inner delay (3 ticks)
- **Tick N+4**: Inner sequence ends
- **Ticks N+5 to N+9**: Outer delay (5 ticks)
- **Tick N+10**: Outer sequence ends, next iteration begins

The `+1` at the end accounts for the tick where the while loop evaluates the predicate and starts the next iteration.

## Predicate Comparison

### se_state_increment_and_test(1, 10)

- **Storage:** Node's `user_flags` (16-bit)
- **Parameters:** 2 values (increment, limit) — fixed at compile time
- **Auto-initialized:** Yes (zeros counter on first invocation)
- **Visibility:** Private to predicate
- **Iterations:** 10 (counter goes 1→10, fails at 11)

### se_field_increment_and_test("field_test_counter", "field_test_increment", "field_test_limit")

- **Storage:** Three blackboard fields (all `ct_int_t`)
- **Parameters:** 3 field names (counter, increment, limit) — runtime-configurable
- **Auto-initialized:** Yes (zeros counter field on first invocation)
- **Visibility:** All fields visible and modifiable by any function
- **Iterations:** 10 (counter goes 1→10, fails at 11)

### Key Difference

| Aspect | State-Based | Field-Based |
|--------|-------------|-------------|
| Increment | Fixed value: `1` | Field reference: `"field_test_increment"` |
| Limit | Fixed value: `10` | Field reference: `"field_test_limit"` |
| Counter storage | 16-bit node state | Blackboard field |
| Runtime modifiable | No | Yes |
| Use case | Simple fixed loops | Dynamic/adaptive loops |

## Test Validation

The test validates:

1. ✅ **Nested loops** — Inner chain_flow inside outer chain_flow
2. ✅ **Blocking fork_join** — Second loop waits for first to complete
3. ✅ **State-based counter** — `se_state_increment_and_test` counts correctly
4. ✅ **Field-based counter** — `se_field_increment_and_test` with 3 field references counts correctly
5. ✅ **Runtime configuration** — Increment and limit set via `se_set_field` before loop
6. ✅ **se_tick_delay** — Proper halting and resumption
7. ✅ **ONESHOT execution** — Logs fire once per chain activation
8. ✅ **Termination** — Clean exit with `SE_TERMINATE`

## Files

| File | Description |
|------|-------------|
| `loop_test.lua` | Lua DSL source |
| `loop_test.h` | Generated C header (hashes, tree names) |
| `loop_test_bin_32.h` | Generated binary data (32-bit) |
| `loop_test_32.bin` | Binary file for file-based loading |