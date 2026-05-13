# SE_WAIT_TIMEOUT

## Overview

`se_wait_timeout` is a blocking wait function with timeout protection. It waits for a predicate to become true, but if the timeout is exceeded before the predicate passes, it invokes an error handler and either resets or terminates. This combines the functionality of `se_wait` with the timeout protection of `se_verify_and_check_elapsed_time`.

## Function Type

- **Type:** MAIN (pt_m_call)
- **Storage:** 64-bit pointer array slot (stores start time as f64)
- **Blocking:** Yes (returns `SE_PIPELINE_HALT` while waiting)

## Parameters

| Index | Type | Description |
|-------|------|-------------|
| 0 | PRED | Predicate function to evaluate |
| 1 | FLOAT | Timeout in seconds |
| 2 | INT | Reset flag (0 = terminate, 1 = reset) |
| 3 | ONESHOT | Error function to invoke on timeout |

## Lua DSL

```lua
se_wait_timeout(pred_function, timeout, reset_flag, error_function)
```

### Parameters

| Parameter | Type | Description |
|-----------|------|-------------|
| `pred_function` | function | Predicate to evaluate each tick |
| `timeout` | number | Maximum wait time in seconds |
| `reset_flag` | boolean | `true` = `SE_PIPELINE_RESET`, `false` = `SE_PIPELINE_TERMINATE` |
| `error_function` | function | ONESHOT function to invoke when timeout exceeded |

### Validation

- `pred_function` must be a function
- `timeout` must be a number
- `reset_flag` must be a boolean
- `error_function` must be a function

## Behavior

### State Machine

```
┌─────────────────────────────────────────────────────┐
│                      INIT                           │
│  • Record start_time = get_time()                   │
│  • Store in 64-bit pointer slot                     │
│  • Return SE_PIPELINE_CONTINUE                      │
└─────────────────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────┐
│                    WAITING                          │
│  • On SE_EVENT_TICK only:                           │
│    1. Evaluate predicate                            │
│       - If true: return SE_PIPELINE_DISABLE         │
│    2. Check elapsed time                            │
│       - If elapsed > timeout: TIMEOUT               │
│       - Else: return SE_PIPELINE_HALT               │
│  • On other events: return SE_PIPELINE_HALT         │
└─────────────────────────────────────────────────────┘
           │                           │
           ▼ (predicate true)          ▼ (timeout)
┌─────────────────────┐    ┌─────────────────────────┐
│      COMPLETE       │    │        TIMEOUT          │
│  SE_PIPELINE_DISABLE│    │  • Reset error function │
└─────────────────────┘    │  • Invoke error function│
                           │  • Return RESET or TERM │
                           └─────────────────────────┘
```

### Event Handling

| Event Type | Behavior |
|------------|----------|
| `SE_EVENT_INIT` | Record start time in f64 storage |
| `SE_EVENT_TERMINATE` | Return `SE_PIPELINE_CONTINUE` (no cleanup) |
| `SE_EVENT_TICK` | Evaluate predicate, check timeout |
| Other events | Return `SE_PIPELINE_HALT` (still waiting) |

### Return Codes

| Condition | Returns |
|-----------|---------|
| Predicate becomes true | `SE_PIPELINE_DISABLE` (complete) |
| Waiting (within timeout) | `SE_PIPELINE_HALT` |
| Timeout + reset_flag=true | `SE_PIPELINE_RESET` |
| Timeout + reset_flag=false | `SE_PIPELINE_TERMINATE` |

### Reset Flag Behavior

| reset_flag | On Timeout | Use Case |
|------------|------------|----------|
| `true` | `SE_PIPELINE_RESET` | Retry operation from beginning |
| `false` | `SE_PIPELINE_TERMINATE` | Fatal error, abort |

## Usage Examples

### Basic Wait with Timeout (Fatal)

```lua
se_chain_flow(function()
    se_log("Waiting for sensor ready (max 5 seconds)...")
    
    se_wait_timeout(se_pred("SENSOR_READY"), 5.0, false, function()
        se_log("FATAL: Sensor did not respond in time!")
        se_set_field("error_code", ERR_SENSOR_TIMEOUT)
    end)
    
    se_log("Sensor ready!")
    se_do_sensor_work()
    se_return_pipeline_disable()
end)
```

### Wait with Retry on Timeout

```lua
se_chain_flow(function()
    se_log("Attempting connection (will retry on timeout)...")
    
    se_wait_timeout(se_pred("CONNECTED"), 2.0, true, function()
        se_log("Connection timeout - retrying...")
        se_increment_field("retry_count", 1)
    end)
    
    se_log("Connected!")
    se_return_pipeline_disable()
end)
```

### Wait for Field Condition

```lua
se_chain_flow(function()
    se_log("Waiting for counter to reach 100...")
    
    se_wait_timeout(se_field_ge("counter", 100), 30.0, false, function()
        se_log("Timeout: Counter never reached 100")
        se_log_int("Final value: %d", "counter")
    end)
    
    se_log("Counter reached 100!")
    se_return_pipeline_disable()
end)
```

### Immediate Completion (Predicate Already True)

```lua
se_chain_flow(function()
    se_set_field("ready", 1)
    
    -- Completes immediately because predicate is already true
    se_wait_timeout(se_field_eq("ready", 1), 5.0, false, function()
        se_log("This will never fire")
    end)
    
    se_log("Continued immediately!")
    se_return_pipeline_disable()
end)
```

### Complex Predicate

```lua
se_chain_flow(function()
    -- Build complex predicate: (mode == READY) AND (level > 0)
    pred_begin()
        local and1 = se_pred_and()
            se_field_eq("mode", MODE_READY)
            se_field_gt("level", 0)
        pred_close(and1)
    local ready_condition = pred_end()
    
    se_wait_timeout(ready_condition, 10.0, false, function()
        se_log("System failed to reach ready state")
    end)
    
    se_log("System ready!")
    se_return_pipeline_disable()
end)
```

### Repeated Retry with se_false()

```lua
-- This will timeout and reset forever (for testing)
se_chain_flow(function()
    se_log("Starting retry loop")
    
    se_wait_timeout(se_false(), 2.0, true, function()
        se_log("Timeout - resetting...")
        se_increment_field("timeout_count", 1)
    end)
    
    -- Never reached because se_false() never becomes true
    se_log("This will never print")
end)
```

## Comparison with Related Functions

| Function | Waits For | Timeout | On Success | On Timeout | Blocking |
|----------|-----------|---------|------------|------------|----------|
| `se_wait_timeout` | Predicate | Yes | DISABLE | RESET/TERMINATE | Yes |
| `se_wait` | Predicate | No | DISABLE | N/A | Yes |
| `se_wait_event` | Event N times | No | DISABLE | N/A | Yes |
| `se_verify` | Predicate fail | N/A | CONTINUE | RESET/TERMINATE | No |
| `se_verify_and_check_elapsed_time` | Timeout | Yes | N/A | RESET/TERMINATE | No |

## Key Differences

### vs se_wait

| Aspect | `se_wait` | `se_wait_timeout` |
|--------|-----------|-------------------|
| Timeout | No | Yes |
| Error handler | No | Yes |
| Storage | None (m_call) | 64-bit (pt_m_call) |
| Use case | Simple wait | Wait with protection |

### vs se_verify_and_check_elapsed_time

| Aspect | `se_verify_and_check_elapsed_time` | `se_wait_timeout` |
|--------|-----------------------------------|-------------------|
| Blocking | No | Yes |
| On success | Continues monitoring | Completes (DISABLE) |
| Purpose | Watchdog | Protected wait |

## Error Function Behavior

The error function is reset before invocation to allow repeated firing on `reset_flag=true`:

```c
// Reset and invoke error function at logical child 3
s_expr_child_reset(inst, params, param_count, 3);
s_expr_child_invoke_oneshot(inst, params, param_count, 3);
```

This ensures the error function fires on **every** timeout, not just the first.

## C Implementation Notes

### Logical Child Indexing

Because the predicate is an OPEN_CALL that spans multiple physical indices, scalar parameters must be found using `s_expr_child_index`:

```c
// Logical child mapping:
// 0: OPEN_CALL(PRED) — predicate function
// 1: FLOAT — timeout
// 2: INT — reset_flag
// 3: OPEN_CALL(ONESHOT) — error function

uint16_t timeout_idx = s_expr_child_index(params, param_count, 1);
ct_float_t timeout = params[timeout_idx].float_val;

uint16_t reset_flag_idx = s_expr_child_index(params, param_count, 2);
bool reset_flag = (params[reset_flag_idx].int_val != 0);
```

### Event Filtering

Returns `SE_PIPELINE_HALT` on non-tick events to maintain blocking behavior:

```c
if (event_id != SE_EVENT_TICK) {
    return SE_PIPELINE_HALT;  // Still waiting
}
```

### Time Storage

Uses 64-bit pointer slot to store start time as `double`:

```c
// INIT
s_expr_set_user_f64(inst, start_time);

// TICK
double start_time = s_expr_get_user_f64(inst);
```

## Error Handling

| Error | Trigger |
|-------|---------|
| `param_count < 4` | Missing parameters in INIT |
| `timeout not found` | Logical child 1 missing |
| `reset_flag not found` | Logical child 2 missing |

Exceptions halt execution (Erlang-style fail-fast).

## Notes

- Predicate is evaluated **first** each tick, so immediate completion is possible if predicate is already true
- The timeout is checked on every `SE_EVENT_TICK`, not on user events
- Non-tick events return `SE_PIPELINE_HALT` (maintains blocking) rather than `SE_PIPELINE_CONTINUE` (would indicate non-blocking)
- If `get_time` is not provided, times will be 0.0 and timeout may trigger immediately
- The error function reset ensures repeated firing when `reset_flag=true`
- When the parent composite resets this function, the start time is re-recorded (timer restarts)


