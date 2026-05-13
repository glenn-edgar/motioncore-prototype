# SE_TICK_DELAY and SE_TIME_DELAY - Delay Functions

## Overview

These two MAIN functions provide delay/wait functionality in the S-Expression engine:

- **se_tick_delay** - Waits for a specified number of ticks
- **se_time_delay** - Waits for a specified duration in seconds (wall-clock time)

Both functions return `SE_FUNCTION_HALT` while waiting, which blocks sibling execution in `se_function_interface`.

## SE_TICK_DELAY

### Purpose

Waits for a specified number of tick cycles before completing.

### Parameters

| Parameter | Type | Description |
|-----------|------|-------------|
| `tick_count` | integer | Number of ticks to wait |

### Behavior

```
INIT:      Store (tick_count + 1) in node state
TICK:      Decrement counter, return FUNCTION_HALT if > 0
           Return PIPELINE_DISABLE when counter reaches 0
TERMINATE: Return PIPELINE_CONTINUE
```

**Note:** The counter is initialized to `tick_count + 1` because the first tick decrements it immediately. This ensures `se_tick_delay(10)` actually waits 10 ticks.

### Result Codes

| Condition | Returns |
|-----------|---------|
| Counter > 0 | `SE_FUNCTION_HALT` (blocking) |
| Counter = 0 | `SE_PIPELINE_DISABLE` (complete) |

### Lua DSL

```lua
se_tick_delay(10)  -- Wait 10 ticks
```

### Example Usage

```lua
se_function_interface(function()
    se_log("Starting")
    se_tick_delay(10)     -- Blocks for 10 ticks
    se_log("After delay") -- Runs on tick 11
end)
```

### State Storage

- **u64**: Remaining tick count

---

## SE_TIME_DELAY

### Purpose

Waits until a specified duration in seconds has elapsed (wall-clock time).

### Parameters

| Parameter | Type | Description |
|-----------|------|-------------|
| `seconds` | float | Duration to wait in seconds |

### Behavior

```
INIT:      Get current time, store (now + seconds) as target time
TICK:      Compare current time to target time
           Return FUNCTION_HALT if now < target
           Return PIPELINE_DISABLE if now >= target
TERMINATE: Return PIPELINE_CONTINUE
```

### Result Codes

| Condition | Returns |
|-----------|---------|
| Current time < target | `SE_FUNCTION_HALT` (blocking) |
| Current time >= target | `SE_PIPELINE_DISABLE` (complete) |

### Lua DSL

```lua
se_time_delay(1.5)  -- Wait 1.5 seconds
```

### Example Usage

```lua
se_function_interface(function()
    se_log("Starting")
    se_time_delay(2.0)    -- Blocks for 2 seconds
    se_log("After delay") -- Runs after 2 seconds elapsed
end)
```

### State Storage

- **f64**: Target completion time (absolute timestamp)

### Time Source

Uses the allocator's `get_time` callback:

```c
double now = mod->alloc.get_time(mod->alloc.ctx);
```

The time source must return a monotonically increasing value (typically seconds since epoch).

---

## Comparison

| Aspect | `se_tick_delay` | `se_time_delay` |
|--------|-----------------|-----------------|
| **Unit** | Ticks | Seconds |
| **Precision** | Tick rate dependent | Wall-clock time |
| **Use case** | Fixed tick counts | Real-time delays |
| **Time source** | Tick counter | `get_time` callback |
| **Deterministic** | Yes | Depends on tick rate |

### When to Use Which

**Use `se_tick_delay` when:**
- You need deterministic behavior (same results every run)
- Delay is relative to other tick-based operations
- Testing/simulation where tick = logical time

**Use `se_time_delay` when:**
- You need real wall-clock time delays
- Interfacing with external systems with time constraints
- Delays independent of tick rate

---

## Blocking Behavior

Both functions return `SE_FUNCTION_HALT` while waiting. This **blocks sibling execution** in `se_function_interface`:

```lua
se_function_interface(function()
    se_log("A")           -- Tick 1
    se_tick_delay(10)     -- Ticks 1-10: returns FUNCTION_HALT
    se_log("B")           -- Tick 11: delay complete
    se_state_machine(...) -- Tick 11: starts
end)
```

### Non-Blocking Pattern

To run delays in parallel with other work, use `se_fork`:

```lua
se_function_interface(function()
    se_fork(function()
        se_tick_delay(10)     -- Runs in background
        se_log("Delay done")
    end)
    
    se_state_machine(...)     -- Runs immediately, parallel with delay
end)
```

---

## Important Notes

### Return Code Level

Both functions return `SE_FUNCTION_HALT` (not `SE_PIPELINE_HALT`) while waiting. This is intentional:

- `SE_FUNCTION_HALT` (7) blocks siblings in `se_function_interface`
- `SE_PIPELINE_HALT` (13) would be absorbed internally and not block

### Completion Code

Both return `SE_PIPELINE_DISABLE` on completion, which parent composites handle:

- `se_function_interface`: Terminates child, continues to next
- `se_fork`: Terminates child, checks if all complete
- `se_sequence`: Terminates child, advances to next

### Zero/Negative Values

- `se_tick_delay(0)`: Completes on first tick (1 tick total due to +1)
- `se_time_delay(0.0)` or negative: Completes immediately on INIT

### Pointer Call Support

Both use `pt_m_call` in Lua, indicating they support pointer-based instance indexing for multiple concurrent delays with different parameters.
