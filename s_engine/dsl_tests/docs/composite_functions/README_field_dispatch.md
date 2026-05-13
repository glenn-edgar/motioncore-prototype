# SE_FIELD_DISPATCH - Field-Based Dispatch Composite

## Overview

`se_field_dispatch` dispatches to different actions based on an integer field value. It monitors a blackboard field each tick and executes the matching case action. When the field value changes, it terminates the old action and starts the new one.

## Syntax

### Lua DSL

```lua
se_field_dispatch("field_name", cases)
```

Where `cases` is either a function or table of `se_case` definitions:

```lua
field_actions = {}

field_actions[1] = function()
    se_case(1, function()
        se_chain_flow(function()
            se_log("Field value is 1")
            se_tick_delay(5)
            se_return_pipeline_reset()
        end)
    end)
end

field_actions[2] = function()
    se_case(2, function()
        se_chain_flow(function()
            se_log("Field value is 2")
            se_tick_delay(5)
            se_return_pipeline_reset()
        end)
    end)
end

field_actions[3] = function()
    se_case('default', function()
        se_chain_flow(function()
            se_log("Unknown field value")
            se_return_pipeline_reset()
        end)
    end)
end

se_field_dispatch("state", field_actions)
```

## Behavior

### Execution Model

1. **Each tick**: Read the integer field value
2. **Find matching case**: Search `[int, action]` pairs for match
3. **Use default if no match**: Case value `-1` is the default
4. **Crash if no match and no default**: Erlang-style fail-fast
5. **Handle branch changes**: Terminate old action, reset new action
6. **Invoke current action**: Execute the matched action

### Result Code Handling

| Action Returns | Field Dispatch Action |
|----------------|----------------------|
| `SE_PIPELINE_RESET` (15) | Terminate and reset action, return CONTINUE |
| **All other codes** | Pass through to caller |

### Branch Change Handling

When the field value changes:

1. Terminate the old action (sends `SE_EVENT_TERMINATE`)
2. Recursively reset the old action
3. Recursively reset the new action
4. Update tracking to new action index
5. Invoke the new action

## Lifecycle Events

### INIT
- Validates at least 3 parameters (field + one case)
- Sets `user_flags` to `0xFFFF` (sentinel: no active action)
- Returns `SE_PIPELINE_CONTINUE`

### TERMINATE
- Terminates current action if any
- Clears `user_flags` to `0xFFFF`
- Returns `SE_CONTINUE`

### TICK
- Reads field value
- Finds matching case (or default)
- Handles branch transitions
- Invokes current action
- Returns action's result (or modified for RESET)

## SE_FIELD_DISPATCH vs SE_STATE_MACHINE

Both dispatch based on a field value, but they have different semantics:

### SE_STATE_MACHINE

**Purpose**: Implement finite state machines with explicit state transitions.

**Behavior**:
- Actions run to completion, then the state machine waits for the next state change
- When an action returns `SE_PIPELINE_DISABLE`, it's reset and ready for re-entry
- Designed for **state-centric** logic: "I'm in state X, do state X things"
- Actions typically set the next state before completing

```lua
se_case(0, function()
    se_sequence(function()
        se_log("State 0")
        se_tick_delay(10)
        se_set_field("state", 1)  -- Transition to state 1
        se_return_pipeline_disable()  -- State 0 work complete
    end)
end)
```

**Timeline**:
```
state=0: Action runs for 10 ticks, sets state=1, completes
state=1: New action starts, runs, sets state=2, completes
state=2: New action starts...
```

### SE_FIELD_DISPATCH

**Purpose**: Continuous reactive dispatch based on current field value.

**Behavior**:
- Actions run continuously while their case matches
- When an action returns `SE_PIPELINE_RESET`, it loops
- Designed for **value-centric** logic: "While field=X, keep doing X things"
- External code typically changes the field value

```lua
se_case(1, function()
    se_chain_flow(function()
        se_log("Field is 1")
        se_tick_delay(5)
        se_queue_event(...)  -- Generate periodic events
        se_return_pipeline_reset()  -- Loop while field=1
    end)
end)
```

**Timeline**:
```
field=1: Action loops every 5 ticks, generating events
         (external code changes field to 2)
field=2: Old action terminated, new action starts looping
         (external code changes field back to 1)
field=1: Action 1 starts fresh, loops again
```

### When to Use Which

| Use Case | Use This |
|----------|----------|
| Finite state machine with explicit transitions | `se_state_machine` |
| Continuous behavior based on mode/setting | `se_field_dispatch` |
| Actions that complete and advance | `se_state_machine` |
| Actions that loop while condition holds | `se_field_dispatch` |
| State changes driven by action logic | `se_state_machine` |
| State changes driven by external events | `se_field_dispatch` |

### Side-by-Side Example

**State Machine** - Processing pipeline with stages:

```lua
-- Each stage completes and moves to next
se_state_machine("stage", {
    se_case(0, function()  -- INIT stage
        se_sequence(function()
            se_log("Initializing...")
            se_tick_delay(10)
            se_set_field("stage", 1)
            se_return_pipeline_disable()
        end)
    end),
    se_case(1, function()  -- PROCESS stage
        se_sequence(function()
            se_log("Processing...")
            se_tick_delay(50)
            se_set_field("stage", 2)
            se_return_pipeline_disable()
        end)
    end),
    se_case(2, function()  -- DONE stage
        se_sequence(function()
            se_log("Done!")
            se_return_function_terminate()
        end)
    end)
})
```

**Field Dispatch** - Mode-based periodic behavior:

```lua
-- Each mode loops until external change
se_field_dispatch("mode", {
    se_case(1, function()  -- IDLE mode
        se_chain_flow(function()
            se_log("Idle heartbeat")
            se_tick_delay(100)
            se_return_pipeline_reset()  -- Loop
        end)
    end),
    se_case(2, function()  -- ACTIVE mode
        se_chain_flow(function()
            se_log("Active processing")
            se_tick_delay(10)
            se_queue_event(...)
            se_return_pipeline_reset()  -- Loop
        end)
    end),
    se_case(3, function()  -- ALARM mode
        se_chain_flow(function()
            se_log("ALARM!")
            se_tick_delay(1)
            se_queue_event(ALARM_EVENT, ...)
            se_return_pipeline_reset()  -- Loop rapidly
        end)
    end)
})
```

## Parameter Structure

```
params[0]: FIELD reference (the dispatch field)
params[1]: INT (case value 0)
params[2]: OPEN_CALL (action for case 0)
params[3]: INT (case value 1)
params[4]: OPEN_CALL (action for case 1)
...
params[N]: INT (-1 for default)
params[N+1]: OPEN_CALL (default action)
```

## Default Case

The default case uses value `-1`:

```lua
se_case('default', function()
    se_chain_flow(function()
        se_log("Unknown value - using default")
        se_return_pipeline_reset()
    end)
end)
```

If no match and no default exists, the dispatch raises an `EXCEPTION` (Erlang-style crash).

## Duplicate Detection

The Lua DSL tracks case values and raises an error on duplicates:

```lua
se_field_dispatch("field", {
    se_case(1, ...),
    se_case(1, ...)  -- ERROR: duplicate case value: 1
})
```

## Typical Usage Pattern

```lua
se_function_interface(function()
    se_i_set_field("mode", 1)  -- Initialize mode
    
    -- Field dispatch runs continuously
    se_field_dispatch("mode", mode_actions)
    
    -- Other parallel activities
    se_event_dispatch(event_handlers)
    se_tick_delay(1000)  -- Overall timeout
end)
```

## State Storage

- **state** (uint8_t): Not used
- **user_flags** (uint16_t): Current action's physical index (or `0xFFFF` if none)

## Error Handling

| Error | Behavior |
|-------|----------|
| Missing field | `EXCEPTION("se_field_dispatch: field not found")` |
| No matching case | `EXCEPTION("se_field_dispatch: no matching case")` |
| Less than 3 parameters | `EXCEPTION` at INIT |

## Recursive Reset

The field dispatch uses `s_expr_reset_recursive_at` (via `reset_action_at_index`) to reset actions, ensuring:

- All nested composites are reset
- All oneshots can fire again
- All state variables are cleared

This is essential for looping actions that use `se_return_pipeline_reset()`.
