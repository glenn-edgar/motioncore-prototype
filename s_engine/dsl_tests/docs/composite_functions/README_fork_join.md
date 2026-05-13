# SE_FORK_JOIN - Blocking Parallel Execution Composite

## Overview

`se_fork_join` executes all its children **in parallel** each tick, but **blocks sibling composites** until all children complete. This creates a synchronization barrier - nothing after the fork_join runs until it finishes.

## Behavior

### Execution Model

- Invokes **all active MAIN children** on each tick
- ONESHOTs and PREDs fire once (optimized: skip if already initialized)
- Returns `SE_FUNCTION_HALT` while children are running (blocks siblings)
- Returns `SE_PIPELINE_DISABLE` when all MAIN children complete

### Result Code Handling

| Child Returns | Fork_Join Action |
|---------------|------------------|
| **APPLICATION (0-5)** | Propagate immediately to caller |
| **FUNCTION (6-11)** | Propagate immediately to caller |
| `SE_PIPELINE_CONTINUE` (12) | Child running |
| `SE_PIPELINE_HALT` (13) | Child running |
| `SE_PIPELINE_DISABLE` (16) | Child complete - terminate it |
| `SE_PIPELINE_TERMINATE` (14) | Child complete - terminate it |
| `SE_PIPELINE_RESET` (15) | Child complete - terminate and reset |
| `SE_PIPELINE_SKIP_CONTINUE` (17) | Skip to completion check |

### Child Type Handling

| Child Type | Behavior |
|------------|----------|
| **ONESHOT** | Fire once, skip on subsequent ticks (optimized) |
| **PRED** | Evaluate once, skip on subsequent ticks (optimized) |
| **MAIN** | Invoked if active, tracked for completion |

### Completion Tracking

- Only **MAIN** children are counted for completion
- ONESHOTs and PREDs complete immediately, don't affect completion
- Fork_join completes when `active_main_count == 0`

## Lifecycle Events

### INIT
- Returns `SE_PIPELINE_CONTINUE`
- Children are already reset by parent

### TERMINATE
- Terminates all children
- Returns `SE_PIPELINE_CONTINUE`

### TICK
- Invokes all children (ONESHOTs/PREDs once, MAINs if active)
- Returns `SE_FUNCTION_HALT` while MAIN children are running
- Returns `SE_PIPELINE_DISABLE` when all MAIN children complete

## Usage

### Lua DSL

```lua
se_fork_join(function()
    se_log("Starting")
    se_tick_delay(10)
    se_log("Done")
end)
```

### Blocking Behavior - Sequential Phases

`se_fork_join` creates sequential phases in `se_function_interface`:

```lua
se_function_interface(function()
    -- PHASE 1: This runs first, blocks everything after
    se_fork_join(function()
        se_log("Phase 1 started")
        se_tick_delay(10)
        se_log("Phase 1 done")
    end)
    
    -- PHASE 2: Only runs AFTER fork_join completes
    se_log("Phase 2 starting")
    se_state_machine("state", cases)
    se_tick_delay(350)
end)
```

### Execution Timeline

```
Tick 1:  Fork_join starts, "Phase 1 started" logs
         Returns SE_FUNCTION_HALT - siblings blocked
Tick 2:  Fork_join child running
         Returns SE_FUNCTION_HALT - siblings blocked
...
Tick 11: Fork_join child completes, "Phase 1 done" logs
         Returns SE_PIPELINE_DISABLE
Tick 12: "Phase 2 starting" logs
         State machine starts
         tick_delay(350) starts
...
```

### Parallel Children Within Fork_Join

Children inside a fork_join run in parallel with each other:

```lua
se_fork_join(function()
    -- These run in parallel
    se_sequence(function()
        se_log("Task A")
        se_tick_delay(5)
    end)
    
    se_sequence(function()
        se_log("Task B")
        se_tick_delay(10)
    end)
end)
-- Fork_join completes when BOTH tasks finish (tick 10)
```

## Comparison with SE_FORK

| Aspect | `se_fork` | `se_fork_join` |
|--------|-----------|----------------|
| **Blocks siblings** | No | Yes |
| **Return while running** | `SE_PIPELINE_CONTINUE` | `SE_FUNCTION_HALT` |
| **Use case** | Parallel background tasks | Sequential phases |

### When to Use Which

**Use `se_fork`:**
- Background tasks that should run alongside other work
- Fire-and-forget parallel operations
- When completion order doesn't matter

**Use `se_fork_join`:**
- Initialization phases that must complete first
- Setup/teardown sequences
- When subsequent code depends on fork completion

## Important Restrictions

### Do NOT Use Return Code Functions in Fork_Join

Return code functions (`se_return_function_terminate`, `se_return_pipeline_disable`, etc.) are MAIN functions that return a code but **never disable themselves**. This causes the fork_join to hang indefinitely waiting for them to complete.

**BAD - Will hang:**
```lua
se_fork_join(function()
    se_log("Starting")
    se_tick_delay(5)
    se_return_function_terminate()  -- HANGS! Never disables
end)
```

**GOOD - Use return code functions at the function_interface level:**
```lua
se_function_interface(function()
    se_fork_join(function()
        se_log("Starting")
        se_tick_delay(5)
        se_log("Done")
    end)
    se_return_function_terminate()  -- Correct: at function_interface level
end)
```

## ONESHOT Optimization

ONESHOTs inside fork_join are optimized to skip invocation after first tick:

```c
if (func_type == S_EXPR_PARAM_ONESHOT) {
    if (!s_expr_child_is_initialized(inst, params, param_count, i)) {
        s_expr_invoke_any(inst, params, phys_idx);
    }
    continue;
}
```

This avoids unnecessary function calls on ticks 2-N while waiting for MAIN children.

## State Storage

- No state variable used
- No user_flags used
- Completion determined by counting active MAIN children each tick

## Error Handling

- Unknown result codes trigger `EXCEPTION`
- Invalid child indices are skipped
- Non-callable parameters are skipped