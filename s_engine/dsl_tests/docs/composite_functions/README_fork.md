# SE_FORK - Parallel Execution Composite

## Overview

`se_fork` executes all its children **in parallel** each tick. Children run concurrently and independently. The fork completes when all children have finished.

## Behavior

### Execution Model

- Invokes **all active children** on each tick
- Children run independently - one child's completion doesn't affect others
- Fork completes when all MAIN children have completed
- Does **not** block sibling composites (returns `SE_PIPELINE_CONTINUE`)

### Result Code Handling

| Child Returns | Fork Action |
|---------------|-------------|
| **APPLICATION (0-5)** | Propagate immediately to caller |
| `SE_FUNCTION_HALT` (7) | Convert to `SE_PIPELINE_HALT` |
| **FUNCTION (6,8-11)** | Propagate immediately to caller |
| `SE_PIPELINE_CONTINUE` (12) | Child running - count as active |
| `SE_PIPELINE_HALT` (13) | Child running - count as active |
| `SE_PIPELINE_DISABLE` (16) | Child complete - terminate it |
| `SE_PIPELINE_TERMINATE` (14) | Child complete - terminate it |
| `SE_PIPELINE_RESET` (15) | Child complete - terminate and reset |
| `SE_PIPELINE_SKIP_CONTINUE` (17) | Skip remaining children this tick |

### Child Type Handling

| Child Type | Behavior |
|------------|----------|
| **ONESHOT** | Invoked via `s_expr_child_invoke` |
| **PRED** | Invoked via `s_expr_child_invoke` |
| **MAIN** | Invoked if active, result determines state |

## Lifecycle Events

### INIT
- Sets state to `FORK_STATE_RUNNING`
- Resets all callable children
- Returns `SE_CONTINUE`

### TERMINATE
- Terminates all children
- Sets state to `FORK_STATE_COMPLETE`
- Returns `SE_CONTINUE`

### TICK
- Invokes all active children
- Returns `SE_PIPELINE_DISABLE` when all children complete
- Returns `SE_PIPELINE_CONTINUE` while children are running

## Usage

### Lua DSL

```lua
se_fork(function()
    se_log("Fork started")
    se_tick_delay(10)
    se_log("Fork child 1 done")
end)
```

### Non-Blocking Parallel Execution

`se_fork` allows sibling composites to run in parallel:

```lua
se_function_interface(function()
    se_fork(function()
        se_log("Fork started")
        se_tick_delay(10)
        se_log("Fork done")
    end)
    
    -- These run IN PARALLEL with the fork
    se_state_machine("state", cases)
    se_tick_delay(350)
end)
```

### Execution Timeline

```
Tick 1:  Fork starts, "Fork started" logs
         State machine starts (parallel)
         tick_delay(350) starts (parallel)
Tick 2:  Fork child running, state machine running, delay running
...
Tick 11: Fork child completes, "Fork done" logs
         State machine still running
         tick_delay(350) still running
...
```

## Comparison with SE_FORK_JOIN

| Aspect | `se_fork` | `se_fork_join` |
|--------|-----------|----------------|
| **Blocks siblings** | No | Yes |
| **Return while running** | `SE_PIPELINE_CONTINUE` | `SE_FUNCTION_HALT` |
| **Use case** | Parallel background tasks | Sequential phases |

## Important Restrictions

### Do NOT Use Return Code Functions in Fork

Return code functions (`se_return_function_terminate`, `se_return_pipeline_disable`, etc.) are MAIN functions that return a code but **never disable themselves**. This causes the fork to hang indefinitely waiting for them to complete.

**BAD - Will hang:**
```lua
se_fork(function()
    se_log("Starting")
    se_tick_delay(5)
    se_return_function_terminate()  -- HANGS! Never disables
end)
```

**GOOD - Use return code functions at the function_interface level:**
```lua
se_function_interface(function()
    se_fork(function()
        se_log("Starting")
        se_tick_delay(5)
        se_log("Done")
    end)
    se_return_function_terminate()  -- Correct: at function_interface level
end)
```

## State Storage

- **state** (uint8_t): `FORK_STATE_RUNNING` or `FORK_STATE_COMPLETE`
- **user_flags**: Not used (set to 0)

## Error Handling

- Unknown result codes increment active count and continue
- Invalid child indices are skipped
- Non-callable parameters are skipped