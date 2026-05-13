# SE_EVENT_DISPATCH - Event-Based Dispatch Composite

## Overview

`se_event_dispatch` dispatches to different actions based on the current event ID. Each tick, it checks if the incoming `event_id` matches any registered case and invokes the corresponding action. This enables reactive event-driven programming within behavior trees.

## Purpose

While `se_field_dispatch` and `se_state_machine` react to **blackboard field values**, `se_event_dispatch` reacts to **incoming events**. This is the primary mechanism for handling external events (user input, timers, network messages, hardware interrupts, etc.) in the S-Expression engine.

## Syntax

### Lua DSL

```lua
se_event_dispatch(cases)
```

Where `cases` is either a function or table of `se_event_case` definitions:

```lua
event_actions = {}

event_actions[1] = function()
    se_event_case(USER_EVENT_1, function()
        se_chain_flow(function()
            se_log("Received USER_EVENT_1")
            se_set_field("state", 1)
            se_return_pipeline_reset()
        end)
    end)
end

event_actions[2] = function()
    se_event_case(USER_EVENT_2, function()
        se_chain_flow(function()
            se_log("Received USER_EVENT_2")
            se_return_pipeline_reset()
        end)
    end)
end

event_actions[3] = function()
    se_event_case('default', function()
        se_chain_flow(function()
            -- No matching event, just continue
            se_return_pipeline_halt()
        end)
    end)
end

se_event_dispatch(event_actions)
```

## Behavior

### Execution Model

1. **Each tick**: Check the incoming `event_id`
2. **Find matching case**: Search `[int, action]` pairs for exact match
3. **Use default if no match**: Case value `-1` is the default
4. **Crash if no match and no default**: Erlang-style fail-fast
5. **Invoke matched action**: Execute and handle result
6. **Reset action after completion**: Actions are reset for next event

### Result Code Handling

The `invoke_and_handle_result` helper processes all results:

| Action Returns | Event Dispatch Action |
|----------------|----------------------|
| **Non-PIPELINE (0-11)** | Propagate to caller |
| `SE_PIPELINE_CONTINUE` (12) | Action still running, return CONTINUE |
| `SE_PIPELINE_HALT` (13) | Action still running, return HALT |
| `SE_PIPELINE_DISABLE` (16) | Terminate and reset action, return CONTINUE |
| `SE_PIPELINE_TERMINATE` (14) | Terminate and reset action, return CONTINUE |
| `SE_PIPELINE_RESET` (15) | Terminate and reset action, return CONTINUE |
| `SE_PIPELINE_SKIP_CONTINUE` (17) | Return CONTINUE |

**Key behavior**: After an action completes (DISABLE/TERMINATE/RESET), it is **terminated and recursively reset**, making it ready for the next time that event fires.

## Lifecycle Events

### INIT
- Returns `SE_PIPELINE_CONTINUE`
- No special initialization needed

### TERMINATE
- Returns `SE_PIPELINE_CONTINUE`
- No cleanup needed (actions are stateless per-event)

### TICK
- Receives `event_id` from tick engine
- Dispatches to matching case
- Returns action's result (modified for completion cases)

## Event ID Flow

Events flow from the tick engine through the tree:

```
Tick Engine
    │
    │ event_id = USER_EVENT_1
    ▼
se_function_interface
    │
    ├── se_event_dispatch
    │       │
    │       ├── case USER_EVENT_1: ✓ MATCH - invoke action
    │       ├── case USER_EVENT_2: skip
    │       └── case default: skip
    │
    ├── se_field_dispatch (also receives event_id)
    └── se_state_machine (also receives event_id)
```

## Parameter Structure

```
params[0]: INT (event case value 1)
params[1]: OPEN_CALL (action for event 1)
params[2]: INT (event case value 2)
params[3]: OPEN_CALL (action for event 2)
...
params[N]: INT (-1 for default)
params[N+1]: OPEN_CALL (default action)
```

Note: Unlike `se_field_dispatch` and `se_state_machine`, there is **no field reference** parameter - the event_id comes from the tick event itself.

## Default Case

The default case handles any event_id that doesn't have an explicit handler:

```lua
se_event_case('default', function()
    se_chain_flow(function()
        se_return_pipeline_halt()  -- No-op for unknown events
    end)
end)
```

**Common default patterns:**

1. **Silent ignore**: `se_return_pipeline_halt()` - do nothing
2. **Log and continue**: Log the unknown event, then reset
3. **Error**: `se_return_terminate()` - fail on unexpected events

If no match and no default exists, the dispatch raises an `EXCEPTION`.

## Usage Patterns

### Basic Event Handling

```lua
local BUTTON_PRESSED = 1
local BUTTON_RELEASED = 2
local TIMER_EXPIRED = 3

event_handlers = {}

event_handlers[1] = function()
    se_event_case(BUTTON_PRESSED, function()
        se_chain_flow(function()
            se_log("Button pressed!")
            se_set_field("button_state", 1)
            se_return_pipeline_reset()
        end)
    end)
end

event_handlers[2] = function()
    se_event_case(BUTTON_RELEASED, function()
        se_chain_flow(function()
            se_log("Button released!")
            se_set_field("button_state", 0)
            se_return_pipeline_reset()
        end)
    end)
end

event_handlers[3] = function()
    se_event_case(TIMER_EXPIRED, function()
        se_chain_flow(function()
            se_log("Timer expired!")
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
```

### Event-Driven State Transitions

Combine with `se_state_machine` for event-driven state changes:

```lua
se_function_interface(function()
    se_i_set_field("state", 0)
    
    -- Events trigger state changes
    se_event_dispatch({
        se_event_case(START_EVENT, function()
            se_chain_flow(function()
                se_set_field("state", 1)  -- Transition to RUNNING
                se_return_pipeline_reset()
            end)
        end),
        se_event_case(STOP_EVENT, function()
            se_chain_flow(function()
                se_set_field("state", 0)  -- Transition to IDLE
                se_return_pipeline_reset()
            end)
        end),
        se_event_case('default', function()
            se_chain_flow(function()
                se_return_pipeline_halt()
            end)
        end)
    })
    
    -- State machine reacts to state field
    se_state_machine("state", state_cases)
end)
```

### Event Generation and Handling

Events can be generated internally using `se_queue_event`:

```lua
state_case_fn[1] = function()
    se_case(0, function()
        se_chain_flow(function()
            se_log("State 0 - generating events")
            se_tick_delay(20)
            se_set_field("event_data_1", 1.1)
            se_queue_event(USER_EVENT_TYPE, USER_EVENT_1, "event_data_1")
            se_return_pipeline_reset()
        end)
    end)
end

event_actions_fn[1] = function()
    se_event_case(USER_EVENT_1, function()
        se_chain_flow(function()
            se_log("Handling USER_EVENT_1")
            local o = o_call("DISPLAY_EVENT_INFO")
            end_call(o)
            se_set_field("state", 1)  -- Change state in response
            se_return_pipeline_reset()
        end)
    end)
end
```

## Comparison with Other Dispatch Functions

| Aspect | `se_event_dispatch` | `se_field_dispatch` | `se_state_machine` |
|--------|---------------------|---------------------|-------------------|
| **Dispatch key** | event_id (from tick) | Field value (from blackboard) | Field value (from blackboard) |
| **Parameter 0** | First case | Field reference | Field reference |
| **Typical action** | One-shot response | Continuous loop | Complete and transition |
| **Use case** | React to external events | Mode-based behavior | State machine logic |

## Complete Example

```lua
-- Event constants
local USER_EVENT_TYPE = 1
local USER_EVENT_1 = 1
local USER_EVENT_2 = 2
local USER_EVENT_3 = 3
local USER_EVENT_4 = 4

-- Event handlers
event_actions_fn = {}

event_actions_fn[1] = function()
    se_event_case(USER_EVENT_1, function()
        se_chain_flow(function()
            se_log("event_actions_fn[1]")
            local o = o_call("DISPLAY_EVENT_INFO")
            end_call(o)
            se_set_field("state", 1)
            se_return_pipeline_reset()
        end)
    end)
end

event_actions_fn[2] = function()
    se_event_case(USER_EVENT_3, function()
        se_chain_flow(function()
            se_log("event_actions_fn[2]")
            se_set_field("state", 2)
            se_return_pipeline_reset()
        end)
    end)
end

event_actions_fn[3] = function()
    se_event_case('default', function()
        se_chain_flow(function()
            se_return_pipeline_halt()
        end)
    end)
end

-- Tree combining event dispatch with state machine
start_tree("dispatch_test")
    use_record("state_machine_blackboard")
    
    se_function_interface(function()
        se_i_set_field("state", 0)
        se_log("Test started")
        
        -- All three run in parallel
        se_event_dispatch(event_actions_fn)      -- Handles incoming events
        se_field_dispatch("state", field_actions_fn)  -- Reacts to state changes
        se_state_machine("state", state_case_fn)  -- Generates events
    end)
end_tree("dispatch_test")
```

## State Storage

- No state variable used
- No user_flags used
- Actions are stateless - reset after each invocation

## Error Handling

| Error | Behavior |
|-------|----------|
| No matching case and no default | `EXCEPTION("se_event_dispatch: no matching event handler")` |
| Unknown result code | `EXCEPTION("se_event_dispatch: unknown result code")` |

## Integration with Event Queue

Events are typically queued using `se_queue_event` and processed on subsequent ticks:

```lua
-- Generate event with associated data
se_set_field("event_data", 42.0)
se_queue_event(EVENT_TYPE, EVENT_ID, "event_data")
```

The event dispatch receives the `event_id` on the next tick and can access the event data through the blackboard field.
