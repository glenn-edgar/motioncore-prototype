# SE_WAIT_EVENT

## Overview

`se_wait_event` is a blocking MAIN function that waits for a specific event to occur a specified number of times. It halts the pipeline until the target event has been received the required number of times.

## Function Type

- **Type:** MAIN (pt_m_call)
- **Storage:** 64-bit pointer array slot (packs target_event and remaining count)
- **Blocking:** Yes (returns `SE_PIPELINE_HALT` while waiting)

## Parameters

| Index | Type | Description |
|-------|------|-------------|
| 0 | UINT | Target event ID to wait for |
| 1 | UINT | Number of times the event must occur |

## Lua DSL

```lua
-- Wait for event to occur 'count' times
se_wait_event(target_event, count)

-- Wait for event to occur once (convenience wrapper)
se_wait_event_once(target_event)
```

### Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `target_event` | number | (required) | Event ID to wait for |
| `count` | number | 1 | Number of occurrences required |

### Validation

- `target_event` must be a number
- `count` must be a positive integer (≥ 1)
- Both values are floored to integers

## Behavior

### State Machine

```
┌─────────────────────────────────────────────────────┐
│                      INIT                           │
│  • Store target_event and count in 64-bit state     │
│  • Return SE_PIPELINE_CONTINUE                      │
└─────────────────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────┐
│                     WAITING                         │
│  • On each tick:                                    │
│    - If event_id == target_event: decrement count   │
│    - If count == 0: return SE_PIPELINE_DISABLE      │
│    - Else: return SE_PIPELINE_HALT                  │
└─────────────────────────────────────────────────────┘
                         │
                         ▼ (count reaches 0)
┌─────────────────────────────────────────────────────┐
│                    COMPLETE                         │
│  • Return SE_PIPELINE_DISABLE                       │
└─────────────────────────────────────────────────────┘
```

### Event Handling

| Event Type | Behavior |
|------------|----------|
| `SE_EVENT_INIT` | Pack target_event and count into 64-bit state |
| `SE_EVENT_TERMINATE` | Return `SE_PIPELINE_CONTINUE` (no cleanup needed) |
| `SE_EVENT_TICK` | Check event_id, decrement counter if match |

### Return Codes

| Condition | Returns |
|-----------|---------|
| Waiting for events | `SE_PIPELINE_HALT` |
| Count reached zero | `SE_PIPELINE_DISABLE` |

## Storage Layout

The function packs two 32-bit values into a single 64-bit pointer slot:

```
┌────────────────────────────────────────────────────────────────┐
│                         64-bit state                           │
├────────────────────────────────┬───────────────────────────────┤
│     target_event (32 bits)     │     remaining (32 bits)       │
│          [63:32]               │          [31:0]               │
└────────────────────────────────┴───────────────────────────────┘
```

## Usage Examples

### Wait for Single Event

```lua
se_chain_flow(function()
    se_log("Waiting for button press...")
    se_wait_event_once(BUTTON_PRESS_EVENT)
    se_log("Button pressed!")
    se_return_pipeline_disable()
end)
```

### Wait for Multiple Occurrences

```lua
se_chain_flow(function()
    se_log("Waiting for 3 sensor readings...")
    se_wait_event(SENSOR_READING_EVENT, 3)
    se_log("Got all 3 readings!")
    se_return_pipeline_disable()
end)
```

### Event Generator and Waiter Pattern

```lua
se_fork(function()
    -- Event generator
    se_chain_flow(function()
        se_log("Starting event generator")
        se_while(se_state_increment_and_test(1, 10), function()
            se_time_delay(1.0)
            se_queue_event(1, USER_EVENT_42, nil)
        end)
        se_log("Generator complete")
        se_return_pipeline_disable()
    end)

    -- Event waiter
    se_chain_flow(function()
        se_log("Waiting for 10 events...")
        se_wait_event(USER_EVENT_42, 10)
        se_log("Received all 10 events!")
        se_return_pipeline_disable()
    end)
end)
```

### Timeout Pattern with Race

```lua
se_fork(function()
    -- Wait for response event
    se_chain_flow(function()
        se_wait_event_once(RESPONSE_EVENT)
        se_set_field("got_response", 1)
        se_return_pipeline_terminate()  -- Kill sibling
    end)

    -- Timeout
    se_chain_flow(function()
        se_time_delay(5.0)
        se_set_field("timed_out", 1)
        se_return_pipeline_terminate()  -- Kill sibling
    end)
end)
```

### Synchronization Barrier

```lua
-- Wait for all subsystems to report ready
se_chain_flow(function()
    se_log("Waiting for subsystem ready events...")
    se_wait_event(SUBSYSTEM_READY_EVENT, 4)  -- 4 subsystems
    se_log("All subsystems ready!")
    se_queue_event(1, START_OPERATION_EVENT, nil)
    se_return_pipeline_disable()
end)
```

## Comparison with Related Functions

| Function | Waits For | Blocking | Storage |
|----------|-----------|----------|---------|
| `se_wait_event` | Specific event ID, N times | Yes (`SE_PIPELINE_HALT`) | 64-bit (pt_m_call) |
| `se_wait` | Predicate to become true | Yes (`SE_PIPELINE_HALT`) | None (m_call) |
| `se_tick_delay` | N ticks | Yes (`SE_PIPELINE_HALT`) | 16-bit user_data |
| `se_time_delay` | Elapsed time | Yes (`SE_PIPELINE_HALT`) | 64-bit (pt_m_call) |
| `se_check_event` | Event match (predicate) | No (returns bool) | None |

## C Implementation Notes

### Initialization

```c
if (event_type == SE_EVENT_INIT) {
    uint32_t target_event = (uint32_t)params[0].int_val;
    uint32_t count = (uint32_t)params[1].int_val;
    
    // Pack into 64-bit state
    uint64_t state = ((uint64_t)target_event << 32) | count;
    s_expr_set_user_u64(inst, state);
    
    return SE_PIPELINE_CONTINUE;
}
```

### Event Matching

```c
if (event_id == target_event) {
    remaining--;
    state = ((uint64_t)target_event << 32) | remaining;
    s_expr_set_user_u64(inst, state);
    
    if (remaining == 0) {
        return SE_PIPELINE_DISABLE;
    }
}
```

### Why pt_m_call?

The function needs to store two 32-bit values (target_event and remaining count). The standard node state only provides:
- 8-bit state
- 16-bit user_data

By using `pt_m_call`, the function gets a 64-bit pointer slot, allowing it to pack both values efficiently.

## Error Handling

| Error | Trigger |
|-------|---------|
| `param_count < 2` | Missing parameters in INIT |

Exceptions halt execution (Erlang-style fail-fast).

## Notes

- The function processes **all** events, not just `SE_EVENT_TICK`. This allows it to respond to user-queued events dispatched via `se_queue_event`.
- If the same event is queued multiple times before the waiter is ticked, each tick will only decrement once (one event per tick).
- The count is stored as remaining (countdown), not as a running total.

