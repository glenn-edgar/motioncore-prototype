# Dispatch Test

## Overview

This test demonstrates the S-Engine's event-driven architecture by combining three dispatch mechanisms running in parallel:

1. **se_event_dispatch** - Handles user events from the queue
2. **se_field_dispatch** - Reacts to blackboard field changes
3. **se_state_machine** - Generates events and drives state transitions

The test shows how these components interact to create a reactive, event-driven system.

## Test Structure

### Blackboard Record

```lua
RECORD("state_machine_blackboard")
    FIELD("state", "int32")
    FIELD("event_data_1", "float")
    FIELD("event_data_2", "float")
    FIELD("event_data_3", "float")
    FIELD("event_data_4", "float")
END_RECORD()
```

### Event Constants

```lua
local USER_EVENT_TYPE = 1
local USER_EVENT_1 = 1
local USER_EVENT_2 = 2
local USER_EVENT_3 = 3
local USER_EVENT_4 = 4
```

### Tree Architecture

```lua
se_function_interface(function()
    se_i_set_field("state", 0)
    se_log("State machine test started")
    
    se_event_dispatch(event_actions_fn)      -- Handles events
    se_field_dispatch("state", field_actions_fn)  -- Reacts to state field
    se_state_machine("state", state_case_fn)  -- Generates events
end)
```

All three composites run **in parallel** within `se_function_interface`.

## Component Interactions

```
┌─────────────────────────────────────────────────────────────────────┐
│                     SE_FUNCTION_INTERFACE                           │
│                                                                     │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────────┐ │
│  │ SE_EVENT_       │  │ SE_FIELD_       │  │ SE_STATE_MACHINE    │ │
│  │ DISPATCH        │  │ DISPATCH        │  │                     │ │
│  │                 │  │                 │  │  State 0:           │ │
│  │ USER_EVENT_1 ◄──┼──┼─────────────────┼──┼─ queue_event(1)     │ │
│  │   → state=1     │  │                 │  │   queue_event(2)    │ │
│  │                 │  │ state=1:        │  │                     │ │
│  │ USER_EVENT_3 ◄──┼──┼─ field_actions  │  │  State 1:           │ │
│  │   → state=2     │  │   [1] runs      │  │   queue_event(3)    │ │
│  │                 │  │                 │  │   queue_event(4)    │ │
│  │ default:        │  │ state=2:        │  │                     │ │
│  │   halt (no-op)  │  │ field_actions   │  │  State 2:           │ │
│  │                 │  │   [2] runs      │  │   → TERMINATE       │ │
│  └─────────────────┘  └─────────────────┘  └─────────────────────┘ │
└─────────────────────────────────────────────────────────────────────┘
```

## Execution Flow

### Phase 1: State 0 (Ticks 1-21)

1. State machine starts in state 0
2. After 20 ticks, queues `USER_EVENT_1` and `USER_EVENT_2`
3. Field dispatch runs default case (state=0 has no explicit handler)

### Phase 2: Event Processing

After tick 21, the tick loop processes queued events:

1. **USER_EVENT_1** processed:
   - `display_event_info` shows event data (1.1)
   - Sets `state = 1`
   
2. **USER_EVENT_2** processed:
   - `display_event_info` shows event data (2.2)
   - No state change

### Phase 3: State 1 (Ticks 22-42)

1. State machine now in state 1
2. Field dispatch detects state=1, runs `field_actions_fn[1]`
3. After 20 ticks, queues `USER_EVENT_3` and `USER_EVENT_4`

### Phase 4: Event Processing

1. **USER_EVENT_3** processed:
   - `display_event_info` shows event data (3.3)
   - Sets `state = 2`
   
2. **USER_EVENT_4** processed:
   - `display_event_info` shows event data (4.4)

### Phase 5: State 2 - Termination

1. State machine enters state 2
2. Field dispatch runs `field_actions_fn[2]`
3. After 20 ticks, returns `SE_TERMINATE`
4. Tree terminates

## User-Defined Function: display_event_info

This external oneshot demonstrates how to access event data:

```c
void display_event_info(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count);
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    // Skip regular ticks - only process user events
    if (event_id == SE_EVENT_TICK) {
        return;
    }
    
    printf("[display_event_info] Event type: %d, Event ID: %d\n", 
           event_type, event_id);
    
    // event_data contains the blackboard field offset
    uint16_t offset = (uint16_t)(size_t)event_data;
    printf("[display_event_info] Field offset: %d\n", offset);
    
    // Access the actual float value in the blackboard
    float* value = (float*)((uint8_t*)inst->blackboard + offset);
    printf("[display_event_info] Value: %f\n", *value);
}
```

### Key Pattern: Field Offset to Value

```c
// event_data is the field offset (stored as void*)
uint16_t offset = (uint16_t)(size_t)event_data;

// Calculate pointer: blackboard base + offset
float* value = (float*)((uint8_t*)inst->blackboard + offset);

// Access the value
printf("Value: %f\n", *value);
```

## Tick Loop Implementation

The test harness implements the standard event processing pattern:

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

## Expected Output

```
╔════════════════════════════════════════╗
║    DISPATCH TEST                       ║
╚════════════════════════════════════════╝

Testing dispatch with tick loop...

  Running tick loop...
  [DEBUG] State machine test started
  [DEBUG] State 0
  [DEBUG] field_actions_fn[3]          <- default case for state=0
------------------------>    Tick   1: result=FUNCTION_HALT
...
  [DEBUG] field_actions_fn[3] terminated
  [DEBUG] field_actions_fn[3]          <- default loops
------------------------>    Tick  21: result=FUNCTION_HALT
------------------------>      Event count: 2
-------------------------------->      Event: tick_type=1, event_id=1, event_data=0x4
******************[display_event_info] Event type: 0, Event ID: 1
******************[display_event_info] Field offset: 4
******************[display_event_info] Value: 1.100000
  [DEBUG] event_actions_fn[1]
  [DEBUG] event_actions_fn[1] terminated
-------------------------------->      Event result: FUNCTION_HALT
-------------------------------->      Event: tick_type=1, event_id=2, event_data=0x8
******************[display_event_info] Event type: 0, Event ID: 2
******************[display_event_info] Value: 2.200000
  [DEBUG] event_actions_fn[3]
  [DEBUG] event_actions_fn[3] terminated
-------------------------------->      Event result: FUNCTION_HALT
  [DEBUG] State 1                       <- state changed to 1
  [DEBUG] field_actions_fn[1]          <- field dispatch detects state=1
...
  [DEBUG] State 2
  [DEBUG] State 2 terminated
------------------------>    Tick  XX: result=FUNCTION_TERMINATE
✅ PASSED - Tree terminated normally
```

## Files

| File | Description |
|------|-------------|
| `dispatch_test.lua` | Lua DSL test definition |
| `dispatch_test_main.c` | C test harness with tick loop |
| `dispatch_test_user_functions.c` | User-defined `display_event_info` |
| `dispatch_test.h` | Generated tree hashes |
| `dispatch_test_bin_32.h` | Generated binary ROM |
| `dispatch_test_records.h` | Generated record definitions |
| `dispatch_test_user_functions.h` | Generated function prototypes |
| `dispatch_test_user_registration.c` | Generated registration tables |

## Key Concepts Demonstrated

1. **Parallel Composites** - Three dispatch mechanisms running simultaneously
2. **Event Generation** - State machine generates events via `se_queue_event`
3. **Event Handling** - `se_event_dispatch` processes events with `tick_type` check
4. **Field Offset as Event Data** - How to access blackboard data from event handlers
5. **State Transitions via Events** - Events trigger state changes
6. **Save/Set/Restore Pattern** - Proper tick_type management in tick loop
7. **External User Functions** - `display_event_info` as external oneshot

## Test Pass Criteria

The test passes when:
- Tree terminates normally (`SE_FUNCTION_TERMINATE`)
- State machine progresses through states 0 → 1 → 2
- Events are properly generated and handled
- Field dispatch reacts to state changes
- Total ticks < max_ticks (100)


