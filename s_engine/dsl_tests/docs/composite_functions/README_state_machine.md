# SE_STATE_MACHINE - Field-Based State Dispatch Composite

## Overview

`se_state_machine` dispatches to different actions based on an integer field value. It monitors a blackboard field and executes the matching case action. When the field value changes, it automatically terminates the old action and starts the new one.

## Behavior

### Execution Model

- Reads an integer field from the blackboard each tick
- Finds the matching `[case_value, action]` pair
- On state change: terminates old action, recursively resets both old and new actions
- Invokes the current action
- When action completes, resets it for next activation (state machines cycle)

### Parameter Structure

```
params[0]: FIELD reference (the state field)
params[1]: INT (case value 0)
params[2]: OPEN_CALL (action for case 0)
params[3]: INT (case value 1)
params[4]: OPEN_CALL (action for case 1)
...
params[N]: INT (-1 for default)
params[N+1]: OPEN_CALL (default action)
```

### Result Code Handling

| Child Returns | State Machine Action |
|---------------|----------------------|
| **APPLICATION (0-5)** | Propagate immediately to caller |
| `SE_FUNCTION_HALT` (7) | Convert to `SE_PIPELINE_HALT` |
| **FUNCTION (6,8-11)** | Propagate immediately to caller |
| `SE_PIPELINE_CONTINUE` (12) | Action running |
| `SE_PIPELINE_HALT` (13) | Action running |
| `SE_PIPELINE_DISABLE` (16) | Action complete - terminate and reset |
| `SE_PIPELINE_TERMINATE` (14) | Action complete - terminate and reset |
| `SE_PIPELINE_RESET` (15) | Action complete - terminate and reset |
| `SE_PIPELINE_SKIP_CONTINUE` (17) | Return `SE_PIPELINE_CONTINUE` |

### State Change Handling

When the field value changes from state A to state B:

1. Terminate action A (sends `SE_EVENT_TERMINATE`)
2. Recursively reset action A (ready for re-entry)
3. Recursively reset action B (fresh start)
4. Update `user_flags` to track current action index
5. Invoke action B

### Action Completion Handling

When the current action returns `SE_PIPELINE_DISABLE/TERMINATE/RESET`:

1. Terminate the action
2. Recursively reset the action (ready for next time this state is entered)
3. Return `SE_PIPELINE_CONTINUE` (state machine keeps running)

This allows cyclic state machines (0 → 1 → 2 → 0 → ...).

## Lifecycle Events

### INIT
- Validates at least 3 parameters (field + one case)
- Sets `user_flags` to `0xFFFF` (sentinel: no active action)
- Returns `SE_PIPELINE_CONTINUE`

### TERMINATE
- Terminates current action if any
- Sets `user_flags` to `0xFFFF`
- Returns `SE_PIPELINE_CONTINUE`

### TICK
- Reads field value
- Finds matching case (or default)
- Handles state transitions
- Invokes current action
- Returns appropriate result code

## Usage

### Lua DSL

```lua
-- Define cases
case_fn = {}

case_fn[1] = function()
    se_case(0, function()
        se_sequence(function()
            se_log("State 0")
            se_tick_delay(10)
            se_set_field("state", 1)
            se_return_pipeline_disable()
        end)
    end)
end

case_fn[2] = function()
    se_case(1, function()
        se_sequence(function()
            se_log("State 1")
            se_tick_delay(10)
            se_set_field("state", 2)
            se_return_pipeline_disable()
        end)
    end)
end

case_fn[3] = function()
    se_case(2, function()
        se_sequence(function()
            se_log("State 2")
            se_tick_delay(10)
            se_set_field("state", 0)  -- Cycle back
            se_return_pipeline_disable()
        end)
    end)
end

case_fn[4] = function()
    se_case('default', function()
        se_sequence(function()
            se_log("Unknown state - terminating")
            se_return_terminate()
        end)
    end)
end

-- Use in tree
se_function_interface(function()
    se_i_set_field("state", 0)  -- Initialize state
    se_state_machine("state", case_fn)
    se_return_function_terminate()
end)
```

### Execution Timeline (Cyclic State Machine)

```
Tick 1:  state=0, "State 0" logs, delay starts
...
Tick 11: delay completes, state set to 1
         Action 0 completes (DISABLE), gets reset
Tick 12: state=1, state change detected
         Action 0 terminated and reset
         Action 1 reset and invoked
         "State 1" logs
...
Tick 22: state=2, state change detected
         "State 2" logs
...
Tick 32: state=0, state change detected (cycle!)
         "State 0" logs (action was reset, runs fresh)
...
```

## Default Case

The default case (value `-1`) is used when no exact match is found:

```lua
se_case('default', function()
    -- Handle unknown states
end)
```

If no match and no default exists, the state machine raises an `EXCEPTION` (Erlang-style crash).

## Important Notes

### Actions Should Use se_sequence

Case actions typically contain multiple steps (log, delay, set field, etc.). Wrap them in `se_sequence`:

```lua
se_case(0, function()
    se_sequence(function()
        se_log("State 0")
        se_tick_delay(10)
        se_set_field("state", 1)
        se_return_pipeline_disable()
    end)
end)
```

### Return Codes in State Machine Cases

Unlike `se_fork` and `se_fork_join`, return code functions like `se_return_pipeline_disable()` **work correctly** inside state machine cases because:

1. The sequence propagates `SE_PIPELINE_DISABLE` to the state machine
2. The state machine terminates and resets the action
3. The state machine returns `SE_PIPELINE_CONTINUE` (keeps running)

This is the intended pattern for signaling "this state's work is done".

### Terminating the Entire State Machine

To terminate the entire state machine (not just a state), use `se_return_terminate()` which propagates `SE_TERMINATE` (APPLICATION level) through to the caller:

```lua
se_case('default', function()
    se_sequence(function()
        se_log("Fatal error - terminating")
        se_return_terminate()  -- Stops everything
    end)
end)
```

## Recursive Reset

The state machine uses `s_expr_reset_recursive_at` to reset actions, ensuring:

- All nested composites are reset
- All oneshots can fire again
- All state variables are cleared

This is essential for cyclic state machines where states are re-entered.

## State Storage

- **state** (uint8_t): Not used
- **user_flags** (uint16_t): Current action's physical index (or `0xFFFF` if none)

## Error Handling

- Missing field: `EXCEPTION`, returns `SE_PIPELINE_CONTINUE`
- No matching case and no default: `EXCEPTION`, returns `SE_PIPELINE_CONTINUE`
- Unknown result codes: `EXCEPTION`, returns `SE_PIPELINE_CONTINUE`
- Less than 3 parameters: `EXCEPTION` at INIT