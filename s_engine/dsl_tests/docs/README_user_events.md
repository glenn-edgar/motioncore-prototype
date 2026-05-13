# S-Engine User Event System

## Overview

The S-Engine provides a user event system that enables behavior trees to generate and respond to internal events. Events are queued during tree execution and processed by the external tick loop, allowing for reactive event-driven programming within the behavior tree framework.

## Architecture

### Event Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                        TICK LOOP                                │
│                                                                 │
│  1. Regular Tick ──────────────────────────────────────────┐   │
│     s_expr_node_tick(tree, SE_EVENT_TICK, NULL)            │   │
│                                                             │   │
│          ┌──────────────────────────────────────┐          │   │
│          │        BEHAVIOR TREE                 │          │   │
│          │                                      │          │   │
│          │  se_state_machine ───────────────┐   │          │   │
│          │       │                          │   │          │   │
│          │       └─► se_queue_event() ──────┼───┼──► EVENT │   │
│          │                                  │   │    QUEUE │   │
│          │  se_event_dispatch ◄─────────────┘   │          │   │
│          │       │                              │          │   │
│          │       └─► (handles events)           │          │   │
│          └──────────────────────────────────────┘          │   │
│                                                             │   │
│  2. Check Queue ◄───────────────────────────────────────────┘   │
│     event_count = s_expr_event_queue_count(tree)                │
│                                                                 │
│  3. Process Events (while event_count > 0)                      │
│     s_expr_event_pop(tree, &tick_type, &event_id, &event_data)  │
│     tree->tick_type = tick_type  (save/set/restore)             │
│     s_expr_node_tick(tree, event_id, event_data)                │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### Key Components

1. **Event Queue** - Circular buffer in tree instance (default 16 events)
2. **tick_type** - Distinguishes regular ticks from user events
3. **se_queue_event** - DSL function to queue events from tree
4. **se_event_dispatch** - Composite that handles user events

## Data Structures

### Tree Instance Event Fields

```c
struct s_expr_tree_instance {
    // ... other fields ...
    
    // Event queue (circular buffer)
    s_expr_queued_event_t  event_queue[S_EXPR_EVENT_QUEUE_SIZE];
    uint8_t                event_queue_head;
    uint8_t                event_queue_count;
    
    // Current tick type
    uint16_t               tick_type;  // SE_EVENT_TICK or SE_EVENT_USER
};
```

### Queued Event Structure

```c
typedef struct {
    uint16_t  tick_type;    // SE_EVENT_USER for user events
    uint16_t  event_id;     // Application-defined event identifier
    void*     event_data;   // Pointer to event data (typically blackboard field)
} s_expr_queued_event_t;
```

### Tick Types

```c
typedef enum {
    SE_EVENT_TICK = 0,       // Regular tick (default)
    SE_EVENT_INIT = 1,       // Initialization
    SE_EVENT_TERMINATE = 2,  // Termination
    SE_EVENT_USER = 3,       // User-defined event
} s_expr_event_type_t;
```

## API Reference

### Queue Management

```c
// Initialize queue (called by s_expr_tree_create)
void s_expr_event_queue_init(s_expr_tree_instance_t* inst);

// Get number of queued events
uint16_t s_expr_event_queue_count(s_expr_tree_instance_t* inst);

// Push event onto queue
void s_expr_event_push(
    s_expr_tree_instance_t* inst,
    uint16_t tick_type,
    uint16_t event_id,
    void* event_data
);

// Pop event from queue
void s_expr_event_pop(
    s_expr_tree_instance_t* inst,
    uint16_t* tick_type,
    uint16_t* event_id,
    void** event_data
);

// Clear all queued events
void s_expr_event_queue_clear(s_expr_tree_instance_t* inst);
```

### Lua DSL

```lua
-- Queue an event with data from blackboard field
se_queue_event(event_type, event_id, "field_name")
```

## Usage

### 1. Define Event Constants

```lua
local USER_EVENT_TYPE = 1  -- Application event type
local USER_EVENT_1 = 1     -- Specific event IDs
local USER_EVENT_2 = 2
local USER_EVENT_3 = 3
```

### 2. Queue Events from Tree

Events are queued using `se_queue_event`, which takes:
- `event_type` - The tick_type (typically `SE_EVENT_USER`)
- `event_id` - Application-defined event identifier
- `field_name` - Blackboard field containing event data

```lua
se_case(0, function()
    se_chain_flow(function()
        se_log("State 0 - generating events")
        se_tick_delay(20)
        
        -- Set event data in blackboard
        se_set_field("event_data_1", 1.1)
        se_set_field("event_data_2", 2.2)
        
        -- Queue events (processed after this tick)
        se_queue_event(USER_EVENT_TYPE, USER_EVENT_1, "event_data_1")
        se_queue_event(USER_EVENT_TYPE, USER_EVENT_2, "event_data_2")
        
        se_return_pipeline_reset()
    end)
end)
```

### 3. Handle Events with se_event_dispatch

```lua
event_handlers = {}

event_handlers[1] = function()
    se_event_case(USER_EVENT_1, function()
        se_chain_flow(function()
            se_log("Handling USER_EVENT_1")
            local o = o_call("DISPLAY_EVENT_INFO")
            end_call(o)
            se_set_field("state", 1)  -- Trigger state change
            se_return_pipeline_reset()
        end)
    end)
end

event_handlers[2] = function()
    se_event_case(USER_EVENT_2, function()
        se_chain_flow(function()
            se_log("Handling USER_EVENT_2")
            se_return_pipeline_reset()
        end)
    end)
end

event_handlers[3] = function()
    se_event_case('default', function()
        se_chain_flow(function()
            -- Default: do nothing on regular ticks
            se_return_pipeline_halt()
        end)
    end)
end
```

### 4. Implement Tick Loop

The external tick loop must process queued events:

```c
do {
    // Regular tick
    result = s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    tick_count++;
    
    // Process queued events
    uint16_t event_count = s_expr_event_queue_count(tree);
    
    while (event_count > 0) {
        uint16_t tick_type;
        uint16_t event_id;
        void* event_data;
        
        // Pop event from queue
        s_expr_event_pop(tree, &tick_type, &event_id, &event_data);
        
        // Save, set, execute, restore
        uint16_t saved_tick_type = tree->tick_type;
        tree->tick_type = tick_type;
        
        s_expr_result_t event_result = s_expr_node_tick(tree, event_id, event_data);
        
        tree->tick_type = saved_tick_type;
        
        if (result_is_complete(event_result)) {
            result = event_result;
            break;
        }
        
        event_count = s_expr_event_queue_count(tree);
    }
    
} while (!result_is_complete(result) && tick_count < max_ticks);
```

## How se_event_dispatch Works

The `se_event_dispatch` composite checks `inst->tick_type` to determine behavior:

| tick_type | Behavior |
|-----------|----------|
| `SE_EVENT_TICK` | Dispatch to **default** case (typically no-op) |
| `SE_EVENT_USER` | Dispatch to matching **event_id** case |

```c
// Inside se_event_dispatch
if (inst->tick_type == SE_EVENT_USER) {
    dispatch_event_id = event_id;  // Match against event cases
} else {
    dispatch_event_id = (uint16_t)-1;  // Use default case
}
```

This means:
- **Regular ticks**: Default case runs (usually `se_return_pipeline_halt()`)
- **User event ticks**: Matching event handler runs

## Event Data

Event data is passed as a pointer to blackboard data:

```lua
-- In tree: set field, then queue event referencing it
se_set_field("sensor_reading", 42.5)
se_queue_event(USER_EVENT_TYPE, SENSOR_EVENT, "sensor_reading")
```

```c
// In event handler: event_data points to blackboard field
void handle_sensor_event(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    ...
) {
    // event_data is available via inst->current_event_data
    float* reading = (float*)inst->current_event_data;
    printf("Sensor reading: %f\n", *reading);
}
```

## Complete Example

```lua
-- Constants
local USER_EVENT_TYPE = 1
local SENSOR_EVENT = 1
local TIMER_EVENT = 2
local ALARM_EVENT = 3

-- Record with event data fields
RECORD("controller_blackboard")
    FIELD("state", "int32")
    FIELD("sensor_data", "float")
    FIELD("timer_data", "float")
    FIELD("alarm_level", "int32")
END_RECORD()

-- Event handlers
event_handlers = {}

event_handlers[1] = function()
    se_event_case(SENSOR_EVENT, function()
        se_chain_flow(function()
            se_log("Sensor event received")
            -- Process sensor data, possibly change state
            se_set_field("state", 1)
            se_return_pipeline_reset()
        end)
    end)
end

event_handlers[2] = function()
    se_event_case(TIMER_EVENT, function()
        se_chain_flow(function()
            se_log("Timer event received")
            se_return_pipeline_reset()
        end)
    end)
end

event_handlers[3] = function()
    se_event_case(ALARM_EVENT, function()
        se_chain_flow(function()
            se_log("ALARM!")
            se_set_field("state", 99)  -- Emergency state
            se_return_pipeline_reset()
        end)
    end)
end

event_handlers[4] = function()
    se_event_case('default', function()
        se_chain_flow(function()
            se_return_pipeline_halt()
        end)
    end)
end

-- State machine that generates events
state_cases = {}

state_cases[1] = function()
    se_case(0, function()
        se_chain_flow(function()
            se_log("Idle - checking sensors")
            se_tick_delay(10)
            se_set_field("sensor_data", 25.5)
            se_queue_event(USER_EVENT_TYPE, SENSOR_EVENT, "sensor_data")
            se_return_pipeline_reset()
        end)
    end)
end

-- Tree structure
start_tree("event_demo")
    use_record("controller_blackboard")
    
    se_function_interface(function()
        se_i_set_field("state", 0)
        
        -- All run in parallel
        se_event_dispatch(event_handlers)  -- Handles events
        se_state_machine("state", state_cases)  -- Generates events
    end)
end_tree("event_demo")
```

## Configuration

### Queue Size

Default queue size is 16 events:

```c
#define S_EXPR_EVENT_QUEUE_SIZE 16
```

Adjust in `s_engine_types.h` if needed for your application.

### Error Handling

| Error | Behavior |
|-------|----------|
| Queue full on push | `EXCEPTION("s_expr_event_push: queue full")` |
| Queue empty on pop | `EXCEPTION("s_expr_event_pop: queue empty")` |

## Best Practices

1. **Always check queue after tick** - Events generated during a tick should be processed before the next tick

2. **Use default case** - Always provide a default handler in `se_event_dispatch` to handle regular ticks gracefully

3. **Save/set/restore tick_type** - Essential for `se_event_dispatch` to distinguish event ticks from regular ticks

4. **Store event data in blackboard** - The event_data pointer should reference stable memory (blackboard fields)

5. **Process all events** - Continue popping events until queue is empty or tree terminates

## Comparison with External Events

| Aspect | Internal Events (queue) | External Events |
|--------|------------------------|-----------------|
| Source | Tree via `se_queue_event` | Application code |
| Timing | Processed after generating tick | Injected into tick loop |
| Data | Blackboard field reference | Application-provided |
| Use case | Inter-component communication | Hardware/network events |

External events can be injected by calling `s_expr_event_push` directly from application code, or by calling `s_expr_node_tick` with custom event_id and event_data.

