# SE_CHAIN_FLOW - ChainTree Walker Emulation Composite

## Overview

`se_chain_flow` emulates the default sequencing behavior of ChainTree's tree walker. It processes children in a **chain** where each child's result determines whether to continue, halt, reset, or terminate the entire chain.

This is the fundamental control flow primitive that ChainTree is built around.

## Lua DSL

```lua
function se_chain_flow(...)
    local children = {...}
    local f = m_call("SE_CHAIN_FLOW")
    for _, child in ipairs(children) do
        if type(child) == "function" then
            child()
        end
    end
    end_call(f)
end
```

## Child Type Handling

`se_chain_flow` handles the three function types differently:

### ONESHOT Functions

ONESHOT children are **fire-and-forget** actions. They execute exactly once per chain activation:

1. Check if child is active
2. If active: invoke the function
3. Immediately terminate (mark inactive)
4. Do not add to active count
5. Continue to next child

This means oneshots:
- Execute on their first tick only
- Are skipped on all subsequent ticks
- Do not block chain progression
- Do not contribute to completion detection

```lua
se_chain_flow(
    function() se_log("Fires once on tick 1") end,      -- ONESHOT
    function() se_tick_delay(10) end,                    -- MAIN: halts chain
    function() se_log("Fires once on tick 11") end,     -- ONESHOT
    function() se_log("Also fires once on tick 11") end -- ONESHOT
)
```

### PRED Functions

Predicate children follow the same pattern as oneshots:

1. Check if child is active
2. If active: invoke the predicate
3. Immediately terminate (mark inactive)
4. Do not add to active count
5. Continue to next child

The predicate's return value is ignored in chain_flow context. Use `se_if` or `se_while` for conditional behavior based on predicates.

### MAIN Functions

MAIN children are **stateful actions** that control chain flow through result codes:

1. Check if child is active
2. If active: invoke the function
3. Handle the result code (see table below)
4. Result determines whether to continue, halt, reset, or terminate

Only MAIN children:
- Can halt chain progression
- Contribute to active count
- Control chain lifecycle via result codes

## Result Code Handling (MAIN children only)

| Child Returns | Chain Flow Action |
|---------------|-------------------|
| `SE_FUNCTION_HALT` (7) | Return `SE_PIPELINE_HALT` to caller |
| **APPLICATION (0-5)** | Propagate immediately to caller |
| **FUNCTION (6, 8-11)** | Propagate immediately to caller |
| `SE_PIPELINE_CONTINUE` (12) | Increment active count, continue to next child |
| `SE_PIPELINE_HALT` (13) | **Stop chain this tick**, return CONTINUE |
| `SE_PIPELINE_DISABLE` (14) | Terminate child, continue to next |
| `SE_PIPELINE_TERMINATE` (15) | **Terminate ALL children**, return TERMINATE |
| `SE_PIPELINE_RESET` (16) | **Reset ALL children**, return CONTINUE |
| `SE_PIPELINE_SKIP_CONTINUE` (17) | Increment active count, skip remaining children this tick |

## Lifecycle Events

### INIT
- Returns `SE_PIPELINE_CONTINUE`
- All children start active

### TERMINATE
- Terminates all children (ONESHOT, PRED, and MAIN)
- Returns `SE_PIPELINE_CONTINUE`

### TICK
- Iterates through children in order
- Skips inactive children
- ONESHOT: fire, terminate, continue
- PRED: fire, terminate, continue
- MAIN: invoke, handle result code
- Returns `SE_PIPELINE_DISABLE` when `active_count == 0`

## Key Behaviors

### ONESHOT Execution Timing

ONESHOTs execute when the chain reaches them, not necessarily on the first tick:

```lua
se_chain_flow(
    function() se_tick_delay(10) end,    -- MAIN: halts on tick 1
    function() se_log("Delayed") end     -- ONESHOT: fires on tick 11
)
```

### ONESHOT After Reset

When `SE_PIPELINE_RESET` resets the chain, all children (including oneshots) are reset to active and will fire again:

```lua
se_chain_flow(
    function() se_log("Fires every loop") end,  -- ONESHOT: reset reactivates
    function() se_tick_delay(10) end,
    function() se_return_pipeline_reset() end   -- Resets all children
)
-- Output: "Fires every loop" on tick 1, 12, 23, 34, ...
```

### PIPELINE_HALT Stops the Chain

When a MAIN child returns `SE_PIPELINE_HALT`, the chain stops processing for this tick but returns `SE_PIPELINE_CONTINUE` to its parent. ONESHOTs after the halt point wait until the chain resumes.

```lua
se_chain_flow(
    function() se_log("A") end,           -- Tick 1: fires
    function() se_tick_delay(10) end,     -- Tick 1: HALT, chain stops
    function() se_log("B") end            -- Tick 11: fires
)
```

### PIPELINE_RESET Resets Everything

When a child returns `SE_PIPELINE_RESET`, ALL children are terminated and reset. This reactivates oneshots for the next iteration.

```lua
se_chain_flow(
    function() se_log("Loop start") end,   -- Fires each iteration
    function() se_tick_delay(10) end,
    function() se_queue_event(...) end,    -- Fires each iteration
    function() se_return_pipeline_reset() end
)
```

### Automatic Completion

When all MAIN children have completed (returned DISABLE or been terminated), `active_count` reaches zero and the chain returns `SE_PIPELINE_DISABLE`. ONESHOT and PRED children do not affect completion — a chain with only oneshots completes immediately after firing them all.

```lua
se_chain_flow(
    function() se_log("A") end,  -- ONESHOT: fires, doesn't count
    function() se_log("B") end,  -- ONESHOT: fires, doesn't count
    function() se_log("C") end   -- ONESHOT: fires, doesn't count
)
-- Returns SE_PIPELINE_DISABLE on tick 1 (active_count = 0)
```

## Usage Examples

### Sequential Actions with Delays

```lua
se_chain_flow(
    function() se_log("Step 1") end,
    function() se_tick_delay(10) end,
    function() se_log("Step 2") end,
    function() se_tick_delay(10) end,
    function() se_log("Step 3") end,
    function() se_return_pipeline_disable() end
)
```

### Cyclic Event Generator

```lua
se_case(1, function()
    se_chain_flow(
        function() se_log("State 1") end,
        function() se_tick_delay(20) end,
        function() se_set_field("event_data", 3.3) end,
        function() se_queue_event(USER_EVENT_TYPE, USER_EVENT_ID, "event_data") end,
        function() se_return_pipeline_reset() end
    )
end)
```

### Initialization Pattern

```lua
se_chain_flow(
    -- Initialization oneshots (fire once at start)
    function() se_set_field("counter", 0) end,
    function() se_log("Initialized") end,
    
    -- Main loop
    function() se_while(some_condition, 
        function() se_tick_delay(10) end,
        function() se_increment_field("counter") end
    ) end,
    
    -- Cleanup oneshot (fires when while completes)
    function() se_log("Complete") end
)
```

## Comparison with Other Composites

| Aspect | `se_chain_flow` | `se_fork_join` |
|--------|-----------------|----------------|
| **Execution** | Sequential with control | Parallel |
| **ONESHOT** | Fire once, terminate, continue | Fire once, skip thereafter |
| **PRED** | Fire once, terminate, continue | Fire once, skip thereafter |
| **PIPELINE_HALT** | Stops chain, returns CONTINUE | Child still running |
| **PIPELINE_RESET** | Resets ALL children | Resets that child |
| **Looping** | Yes (via RESET) | No |
| **Completion** | When active_count == 0 | When all MAIN complete |

## State Storage

- No state variable used
- No user_flags used
- Active status tracked per-child via node flags