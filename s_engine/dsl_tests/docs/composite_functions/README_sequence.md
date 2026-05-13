# SE_SEQUENCE - Sequential Execution Composite

## Overview

`se_sequence` executes its children **one at a time, in order**. When a child completes, the sequence advances to the next child. The sequence completes when all children have finished.

## Behavior

### Execution Model

- Maintains internal `state` tracking the current child index (0, 1, 2, ...)
- On each tick, invokes only the **current** child
- Advances to next child when current child completes
- Processes multiple oneshots/predicates in a single tick (they complete immediately)
- MAIN children may span multiple ticks

### Result Code Handling

| Child Returns | Sequence Action |
|---------------|-----------------|
| **APPLICATION (0-5)** | Propagate immediately to caller |
| `SE_FUNCTION_HALT` (7) | Convert to `SE_PIPELINE_HALT` - child running, wait |
| **FUNCTION (6,8-11)** | Propagate immediately to caller |
| `SE_PIPELINE_CONTINUE` (12) | Child running - pause, resume next tick |
| `SE_PIPELINE_HALT` (13) | Child running - pause, resume next tick |
| `SE_PIPELINE_DISABLE` (16) | Child complete - terminate and advance |
| `SE_PIPELINE_TERMINATE` (14) | Child complete - terminate and advance |
| `SE_PIPELINE_RESET` (15) | Child complete - terminate and advance |
| `SE_PIPELINE_SKIP_CONTINUE` (17) | Pause sequence this tick |

### Child Type Handling

| Child Type | Behavior |
|------------|----------|
| **ONESHOT** | Invoke and advance immediately (same tick) |
| **PRED** | Invoke and advance immediately (same tick) |
| **MAIN** | Invoke, check result, may span multiple ticks |

## Lifecycle Events

### INIT
- Sets `state` to 0 (start at first child)
- Returns `SE_PIPELINE_CONTINUE`

### TERMINATE
- Terminates current child if initialized
- Resets `state` to 0
- Returns `SE_PIPELINE_CONTINUE`

### TICK
- Executes current child based on `state`
- Advances through oneshots/predicates immediately
- Waits for MAIN children to complete before advancing
- Returns `SE_PIPELINE_DISABLE` when all children complete

## Usage

### Lua DSL

```lua
se_sequence(function()
    se_log("Step 1")           -- ONESHOT: fires immediately, advances
    se_tick_delay(10)          -- MAIN: waits 10 ticks
    se_log("Step 2")           -- ONESHOT: fires after delay completes
    se_set_field("state", 1)   -- ONESHOT: fires immediately
    se_log("Step 3")           -- ONESHOT: fires immediately
end)
```

### Execution Timeline

```
Tick 1:  "Step 1" logs, se_tick_delay starts (returns HALT)
Tick 2:  se_tick_delay running...
...
Tick 11: se_tick_delay completes (returns DISABLE)
         "Step 2" logs, state set, "Step 3" logs
         Sequence returns SE_PIPELINE_DISABLE
```

## Important Restrictions

### Do NOT Use Return Code Functions in Sequences

Return code functions (`se_return_function_terminate`, `se_return_pipeline_disable`, etc.) are MAIN functions that return a code but **never disable themselves**. This causes the sequence to hang indefinitely waiting for them to complete.

**BAD - Will hang:**
```lua
se_sequence(function()
    se_log("Starting")
    se_tick_delay(5)
    se_return_function_terminate()  -- HANGS! Never disables
end)
```

**GOOD - Use return code functions at the composite level:**
```lua
se_function_interface(function()
    se_sequence(function()
        se_log("Starting")
        se_tick_delay(5)
        se_log("Done")
    end)
    se_return_function_terminate()  -- Correct: at function_interface level
end)
```

## Blocking Behavior

`se_sequence` does **not** block sibling composites. It returns `SE_PIPELINE_*` codes, which are handled internally by parent composites like `se_function_interface`.

To block siblings until a sequence completes, wrap it in `se_fork_join`:

```lua
se_function_interface(function()
    se_fork_join(function()
        se_sequence(function()
            -- This sequence blocks siblings
            se_tick_delay(10)
        end)
    end)
    
    -- These run AFTER fork_join completes
    se_log("After sequence")
    se_state_machine("state", cases)
end)
```

## Comparison with Other Composites

| Composite | Execution Model | Blocks Siblings |
|-----------|-----------------|-----------------|
| `se_sequence` | One child at a time, in order | No |
| `se_fork` | All children in parallel | No |
| `se_fork_join` | All children in parallel, wait for all | Yes (via `SE_FUNCTION_HALT`) |
| `se_chain_flow` | All children in parallel | No |

## State Storage

- **state** (uint8_t): Current child index (0 to child_count-1)
- No user_flags used

## Error Handling

- Unknown result codes trigger `EXCEPTION` and return `SE_PIPELINE_CONTINUE`
- Invalid child indices are skipped
- Non-callable parameters are skipped