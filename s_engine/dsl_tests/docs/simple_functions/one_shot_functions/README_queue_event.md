# SE_QUEUE_EVENT - Event Queue Oneshot

## Overview

`se_queue_event` is a ONESHOT function that pushes an event onto the tree's internal event queue. The event is processed by the external tick loop after the current tick completes, enabling asynchronous event-driven communication within behavior trees.

## Purpose

Enables behavior tree nodes to generate events that:
- Trigger `se_event_dispatch` handlers
- Communicate between different parts of the tree
- Implement publish/subscribe patterns
- Decouple event generation from event handling

## Parameters

| Parameter | Type | Description |
|-----------|------|-------------|
| `event_type` | uint16 | Event type identifier (typically `SE_EVENT_USER`) |
| `event_id` | uint16 | Application-defined event identifier |
| `field_name` | field_ref | Blackboard field containing event data |

### Parameter Constraints

- `event_type` must be ≤ 0xFFFE
- `event_id` must be ≤ 0xFFFE
- `field_name` must be a valid blackboard field

## Behavior

As a ONESHOT function:
- Fires **once** when invoked
- Completes **immediately** (same tick)
- Pushes event onto queue (not processed until after current tick)

### What Gets Queued

```c
s_expr_queued_event_t {
    .tick_type = event_type,    // From parameter 0
    .event_id = event_id,       // From parameter 1
    .event_data = field_offset  // Offset of blackboard field
}
```

**Note**: The `event_data` stores the **field offset**, not a pointer. The tick loop uses this offset to access the actual data in the blackboard.

## Lua DSL

```lua
se_queue_event(event_type, event_id, "field_name")
```

### Examples

```lua
-- Queue a sensor reading event
se_set_field("sensor_value", 42.5)
se_queue_event(USER_EVENT_TYPE, SENSOR_EVENT, "sensor_value")

-- Queue a timer event
se_set_field("timer_data", 100)
se_queue_event(USER_EVENT_TYPE, TIMER_EVENT, "timer_data")

-- Queue an alarm with level
se_set_field("alarm_level", 3)
se_queue_event(USER_EVENT_TYPE, ALARM_EVENT, "alarm_level")
```

## Usage Pattern

### 1. Set Event Data First

Always set the blackboard field **before** queuing the event:

```lua
se_set_field("event_data_1", 1.1)  -- Set data first
se_queue_event(USER_EVENT_TYPE, USER_EVENT_1, "event_data_1")  -- Then queue
```

### 2. Typical State Machine Pattern

State machines generate events that other components handle:

```lua
state_case_fn[1] = function()
    se_case(0, function()
        se_chain_flow(function()
            se_log("State 0")
            se_tick_delay(20)
            
            -- Set event data in blackboard
            se_set_field("event_data_1", 1.1)
            se_set_field("event_data_2", 2.2)
            
            -- Queue multiple events
            se_queue_event(USER_EVENT_TYPE, USER_EVENT_1, "event_data_1")
            se_queue_event(USER_EVENT_TYPE, USER_EVENT_2, "event_data_2")
            
            se_return_pipeline_reset()  -- Loop
        end)
    end)
end
```

### 3. Event Dispatch Handles Events

```lua
event_handlers[1] = function()
    se_event_case(USER_EVENT_1, function()
        se_chain_flow(function()
            se_log("Received USER_EVENT_1")
            -- event_data contains field offset to access data
            se_return_pipeline_reset()
        end)
    end)
end
```

## Event Processing Flow

```
Tick N:
┌─────────────────────────────────────────────────────┐
│  se_state_machine                                   │
│      │                                              │
│      └─► se_set_field("data", 42.5)                │
│      └─► se_queue_event(TYPE, ID, "data")          │
│              │                                      │
│              └─► event_queue: [{TYPE, ID, offset}] │
│                                                     │
│  se_event_dispatch                                  │
│      └─► (default case - tick_type != USER)        │
└─────────────────────────────────────────────────────┘

After Tick N (tick loop processes queue):
┌─────────────────────────────────────────────────────┐
│  tree->tick_type = SE_EVENT_USER                   │
│  s_expr_node_tick(tree, ID, offset)                │
│                                                     │
│  se_event_dispatch                                  │
│      └─► case ID matches!                          │
│          └─► handler executes                      │
└─────────────────────────────────────────────────────┘
```

## Event Data Access

The event_data is stored as a field offset. In event handlers, access the actual data:

### From C (External Handler)

```c
void handle_event(s_expr_tree_instance_t* inst, ...) {
    // event_data is field offset
    uintptr_t offset = (uintptr_t)inst->current_event_data;
    
    // Access actual data in blackboard
    float* value = (float*)((uint8_t*)inst->blackboard + offset);
    printf("Event data: %f\n", *value);
}
```

### From Tree (via field_ref)

The same field can be accessed by name in the event handler:

```lua
se_event_case(SENSOR_EVENT, function()
    se_chain_flow(function()
        -- Access the same field by name
        se_log_field("sensor_value")  -- If such function exists
        se_return_pipeline_reset()
    end)
end)
```

## Multiple Events Per Tick

Multiple events can be queued in a single tick:

```lua
se_chain_flow(function()
    se_set_field("data_a", 1.0)
    se_set_field("data_b", 2.0)
    se_set_field("data_c", 3.0)
    
    se_queue_event(TYPE, EVENT_A, "data_a")
    se_queue_event(TYPE, EVENT_B, "data_b")
    se_queue_event(TYPE, EVENT_C, "data_c")
    
    se_return_pipeline_reset()
end)
```

Events are processed in FIFO order by the tick loop.

## Queue Limits

Default queue size is 16 events:

```c
#define S_EXPR_EVENT_QUEUE_SIZE 16
```

If the queue is full, `s_expr_event_push` raises an `EXCEPTION`.

## Common Patterns

### Periodic Event Generator

```lua
se_chain_flow(function()
    se_log("Generating periodic event")
    se_tick_delay(100)  -- Every 100 ticks
    se_set_field("heartbeat", tick_count)
    se_queue_event(USER_EVENT_TYPE, HEARTBEAT_EVENT, "heartbeat")
    se_return_pipeline_reset()
end)
```

### Conditional Event

```lua
se_chain_flow(function()
    -- Check condition (via predicate or field)
    se_if(condition_met, function()
        se_set_field("alert_data", alert_value)
        se_queue_event(USER_EVENT_TYPE, ALERT_EVENT, "alert_data")
    end)
    se_return_pipeline_reset()
end)
```

### Event Chain

One event handler triggers another event:

```lua
se_event_case(STEP_1_COMPLETE, function()
    se_chain_flow(function()
        se_log("Step 1 complete, starting step 2")
        se_set_field("step_data", 2)
        se_queue_event(USER_EVENT_TYPE, START_STEP_2, "step_data")
        se_return_pipeline_reset()
    end)
end)
```

## Error Handling

| Error | Behavior |
|-------|----------|
| `event_type > 0xFFFE` | DSL error at compile time |
| `event_id > 0xFFFE` | DSL error at compile time |
| Queue full | `EXCEPTION("s_expr_event_push: queue full")` at runtime |
| Invalid field | Event queued with `event_data = NULL` |

## Implementation Notes

```c
static void se_queue_event(...) {
    uint16_t ev_type = (uint16_t)params[0].int_val;
    uint16_t ev_id = (uint16_t)params[1].int_val;
    
    // Get field offset as event_data
    void* ev_data = NULL;
    if (param_count > 2 && S_EXPR_PARAM_IS_FIELD(params[2].type)) {
        ev_data = (void*)(uintptr_t)params[2].field_offset;
    }
    
    s_expr_event_push(inst, ev_type, ev_id, ev_data);
}
```

The field offset is cast to `void*` for storage in the generic event structure. The tick loop interprets this as an offset into the blackboard.

