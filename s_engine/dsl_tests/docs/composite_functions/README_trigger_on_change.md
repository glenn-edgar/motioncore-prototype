# se_trigger_on_change - Edge Detection Composite

## Overview

`se_trigger_on_change` is a composite function that monitors a predicate and fires actions when the predicate transitions between true and false states. It provides edge detection for boolean conditions, enabling reactive behavior in behavior trees.

## Lua DSL

### Primary Function

```lua
function se_trigger_on_change(initial_state, pred_fn, then_fn, else_fn)
    local c = m_call("SE_TRIGGER_ON_CHANGE")
        int(initial_state)
        pred_fn()
        then_fn()
        else_fn()
    end_call(c)
end
```

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `initial_state` | int (0 or 1) | Starting state of the predicate (prevents spurious trigger on first tick) |
| `pred_fn` | function | Predicate that returns true/false |
| `then_fn` | function | Action executed on rising edge (false → true) |
| `else_fn` | function | Action executed on falling edge (true → false) |

### Convenience Functions

```lua
-- Rising edge only - fires when predicate goes from false to true
function se_on_rising_edge(pred_fn, action_fn)
    se_trigger_on_change(0, pred_fn, action_fn, function()
        se_nop()
    end)
end

-- Falling edge only - fires when predicate goes from true to false
function se_on_falling_edge(pred_fn, action_fn)
    se_trigger_on_change(1, pred_fn, function()
        se_nop()
    end, action_fn)
end
```

## Behavior

### State Machine

```
                    ┌──────────────────────────────────────────┐
                    │         se_trigger_on_change             │
                    │                                          │
                    │   ┌─────────────────────────────────┐   │
                    │   │         State Machine           │   │
                    │   │                                 │   │
   predicate        │   │    ┌───────┐     ┌───────┐     │   │
   becomes ────────►│   │    │ FALSE │────►│ TRUE  │     │   │
   true             │   │    │ state │     │ state │     │   │
                    │   │    └───────┘◄────└───────┘     │   │
   predicate        │   │         ▲           │         │   │
   becomes ◄────────│   │         │           │         │   │
   false            │   │         │           ▼         │   │
                    │   │    ┌─────────────────────┐    │   │
                    │   │    │   Execute Action    │    │   │
                    │   │    │  (rising/falling)   │    │   │
                    │   │    └─────────────────────┘    │   │
                    │   │                                 │   │
                    │   └─────────────────────────────────┘   │
                    │                                          │
                    └──────────────────────────────────────────┘
```

### Execution Flow

Each tick:

1. **Evaluate predicate** - Call the predicate function to get current boolean state
2. **Compare with stored state** - Check if predicate result differs from last known state
3. **On state change:**
   - Terminate and reset the previously running action (if any)
   - Reset the new action to be invoked
   - Invoke the appropriate action (rising or falling)
   - Update stored state
4. **No state change:**
   - Continue running the current action (if any)

### Rising Edge (false → true)

```
Tick N:   predicate = false, state = 0
Tick N+1: predicate = true,  state = 0 → RISING EDGE DETECTED
          → terminate falling action (if running)
          → reset falling action
          → reset rising action
          → invoke rising action
          → state = 1
```

### Falling Edge (true → false)

```
Tick N:   predicate = true,  state = 1
Tick N+1: predicate = false, state = 1 → FALLING EDGE DETECTED
          → terminate rising action (if running)
          → reset rising action
          → reset falling action
          → invoke falling action
          → state = 0
```

## Initial State

The `initial_state` parameter is critical for preventing spurious triggers on the first tick:

| initial_state | Predicate on first tick | Result |
|---------------|------------------------|--------|
| 0 | false | No action (state matches) |
| 0 | true | Rising action fires |
| 1 | true | No action (state matches) |
| 1 | false | Falling action fires |

**Rule of thumb:**
- Set `initial_state = 0` if predicate is expected to start false
- Set `initial_state = 1` if predicate is expected to start true

### NOT Predicate Example

For a NOT predicate (inverted logic), the initial state is inverted:

```lua
-- NOT bit5: When bit5=0, predicate is TRUE
-- So initial_state should be 1 (predicate starts true when bit5=0)
se_trigger_on_change(1,
    function()
        local pred = p_call("SE_PRED_NOT")
            local p1 = p_call("TEST_BIT") int(5) end_call(p1)
        end_call(pred)
    end,
    function()
        -- Rising: NOT bit5 went true (bit5 was cleared)
        se_chain_flow(function()
            se_log("Bit 5 cleared!")
            se_return_continue()
        end)
    end,
    function()
        -- Falling: NOT bit5 went false (bit5 was set)
        se_chain_flow(function()
            se_log("Bit 5 set!")
            se_return_continue()
        end)
    end
)
```

## Result Code Handling

`se_trigger_on_change` uses the three-tier result code system:

### Non-PIPELINE Codes (0-11)

Propagated immediately to caller without modification:

```c
if (r < SE_PIPELINE_CONTINUE) return r;
```

### PIPELINE Codes (12-17)

Handled internally:

| Result Code | Behavior |
|-------------|----------|
| SE_PIPELINE_CONTINUE | Action running, return CONTINUE |
| SE_PIPELINE_HALT | Action paused, return CONTINUE |
| SE_PIPELINE_DISABLE | Terminate and reset action, return CONTINUE |
| SE_PIPELINE_TERMINATE | Terminate and reset action, return CONTINUE |
| SE_PIPELINE_RESET | Terminate and reset action, return CONTINUE |
| SE_PIPELINE_SKIP_CONTINUE | Return CONTINUE |

The trigger always returns `SE_PIPELINE_CONTINUE` to keep the parent composite running.

## C Implementation

### Child Structure

```c
// Child indices
const uint16_t INIT_STATE_CHILD = 0;  // Initial state (INT 0 or 1)
const uint16_t PRED_CHILD = 1;        // Predicate function
const uint16_t RISING_CHILD = 2;      // Rising edge action
const uint16_t FALLING_CHILD = 3;     // Falling edge action (optional)
```

### Event Handling

```c
// TERMINATE EVENT
if (event_type == SE_EVENT_TERMINATE) {
    s_expr_children_terminate_all(inst, params, param_count);
    return SE_PIPELINE_CONTINUE;
}

// INIT EVENT
if (event_type == SE_EVENT_INIT) {
    uint16_t init_phys_idx = s_expr_child_index(params, param_count, INIT_STATE_CHILD);
    int32_t initial_state = (int32_t)params[init_phys_idx].int_val;
    s_expr_set_state(inst, initial_state ? 1 : 0);
    return SE_PIPELINE_CONTINUE;
}
```

### Tick Handling

```c
// TICK EVENT
// 1. Evaluate predicate
uint16_t pred_phys_idx = s_expr_child_index(params, param_count, PRED_CHILD);
bool pred_result = s_expr_invoke_pred(inst, params, pred_phys_idx);

// 2. Get current state
uint8_t current_state = s_expr_get_state(inst);
bool rising = (pred_result && current_state == 0);
bool falling = (!pred_result && current_state == 1);

// 3. Handle rising edge
if (rising) {
    // Terminate and reset falling action (if exists)
    if (has_falling) {
        s_expr_child_terminate(inst, params, param_count, FALLING_CHILD);
        s_expr_child_reset(inst, params, param_count, FALLING_CHILD);
    }
    // Reset rising action
    s_expr_child_terminate(inst, params, param_count, RISING_CHILD);
    s_expr_child_reset(inst, params, param_count, RISING_CHILD);
    
    // Invoke rising action
    uint16_t phys_idx = s_expr_child_index(params, param_count, RISING_CHILD);
    s_expr_result_t r = s_expr_invoke_any(inst, params, phys_idx);
    
    // Update state
    s_expr_set_state(inst, 1);
    
    // Handle result...
}

// 4. Handle falling edge (similar)
```

## Usage Examples

### Simple Bit Monitor

```lua
se_trigger_on_change(0,
    function()
        local pred = p_call("TEST_BIT")
            int(0)  -- bit index
        end_call(pred)
    end,
    function()
        se_chain_flow(function()
            se_log("Bit 0 went HIGH")
            se_return_continue()
        end)
    end,
    function()
        se_chain_flow(function()
            se_log("Bit 0 went LOW")
            se_return_continue()
        end)
    end
)
```

### Compound Condition (AND)

```lua
se_trigger_on_change(0,
    function()
        local pred = p_call("SE_PRED_AND")
            local p1 = p_call("IS_ENABLED") end_call(p1)
            local p2 = p_call("HAS_DATA") end_call(p2)
        end_call(pred)
    end,
    function()
        -- Both conditions now true
        se_chain_flow(function()
            se_log("System ready: enabled AND has data")
            se_return_continue()
        end)
    end,
    function()
        -- At least one condition now false
        se_chain_flow(function()
            se_log("System not ready")
            se_return_continue()
        end)
    end
)
```

### Rising Edge Only

```lua
se_on_rising_edge(
    function()
        local pred = p_call("BUTTON_PRESSED") end_call(pred)
    end,
    function()
        se_chain_flow(function()
            se_log("Button pressed!")
            se_set_field("button_count", 1)  -- Increment would need custom function
            se_return_continue()
        end)
    end
)
```

### Falling Edge Only

```lua
se_on_falling_edge(
    function()
        local pred = p_call("MOTOR_RUNNING") end_call(pred)
    end,
    function()
        se_chain_flow(function()
            se_log("Motor stopped")
            se_queue_event(SE_EVENT_USER, MOTOR_STOPPED_EVENT, "motor_id")
            se_return_continue()
        end)
    end
)
```

### Looping Actions

Actions can loop by returning `SE_PIPELINE_RESET`:

```lua
se_trigger_on_change(0,
    function()
        local pred = p_call("ALARM_ACTIVE") end_call(pred)
    end,
    function()
        -- Continuously flash while alarm is active
        se_chain_flow(function()
            se_set_field("led", 1)
            se_tick_delay(10)
            se_set_field("led", 0)
            se_tick_delay(10)
            se_return_pipeline_reset()  -- Loop until alarm clears
        end)
    end,
    function()
        -- Turn off LED when alarm clears
        se_chain_flow(function()
            se_set_field("led", 0)
            se_return_continue()
        end)
    end
)
```

## Multiple Triggers in Parallel

Use `se_function_interface` to run multiple triggers simultaneously:

```lua
se_function_interface(function()
    
    -- Monitor bit 0
    se_trigger_on_change(0,
        function() local p = p_call("TEST_BIT") int(0) end_call(p) end,
        function() se_chain_flow(function() se_log("Bit 0 HIGH") se_return_continue() end) end,
        function() se_chain_flow(function() se_log("Bit 0 LOW") se_return_continue() end) end
    )
    
    -- Monitor bit 1
    se_trigger_on_change(0,
        function() local p = p_call("TEST_BIT") int(1) end_call(p) end,
        function() se_chain_flow(function() se_log("Bit 1 HIGH") se_return_continue() end) end,
        function() se_chain_flow(function() se_log("Bit 1 LOW") se_return_continue() end) end
    )
    
    -- Monitor bits 2 AND 3
    se_trigger_on_change(0,
        function()
            local pred = p_call("SE_PRED_AND")
                local p1 = p_call("TEST_BIT") int(2) end_call(p1)
                local p2 = p_call("TEST_BIT") int(3) end_call(p2)
            end_call(pred)
        end,
        function() se_chain_flow(function() se_log("Bits 2&3 HIGH") se_return_continue() end) end,
        function() se_chain_flow(function() se_log("Bits 2&3 LOW") se_return_continue() end) end
    )
    
    se_return_continue()
end)
```

## Comparison with Other Dispatch Mechanisms

| Feature | se_trigger_on_change | se_field_dispatch | se_state_machine |
|---------|---------------------|-------------------|------------------|
| Trigger | Predicate edge | Field value change | Action completion |
| Actions | 2 (rise/fall) | N (one per value) | N (one per state) |
| Looping | Via PIPELINE_RESET | Via PIPELINE_RESET | Via next_state |
| Use Case | Boolean conditions | Multi-value selection | State-driven flow |

## Key Points

1. **Edge Detection** - Only fires on transitions, not while condition holds
2. **Initial State** - Prevents spurious trigger on first tick
3. **Action Reset** - Actions automatically reset after completion, enabling re-trigger
4. **Mutual Exclusion** - Rising and falling actions are mutually exclusive (one terminates the other)
5. **Predicate Flexibility** - Any predicate (simple or composite) can be used
6. **PIPELINE_CONTINUE** - Always returns CONTINUE to keep parent running
7. **Parallel Operation** - Multiple triggers can run simultaneously in `se_function_interface`

## Files

| File | Description |
|------|-------------|
| `s_expr_primitives.c` | C implementation of se_trigger_on_change |
| `s_expr_dsl_primitives.lua` | Lua DSL helper functions |
| `s_engine_eval.c` | Predicate invocation (s_expr_invoke_pred) |

