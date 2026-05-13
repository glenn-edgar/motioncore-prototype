    # S-Expression Engine Composite Functions

## Overview

The S-Expression engine provides a set of composite functions for building behavior trees and control flow structures. Each composite has specific execution semantics, result code handling, and use cases.

## Three-Tier Result Code System

All composites operate within a three-tier result code system:

| Level | Codes | Range | Purpose |
|-------|-------|-------|---------|
| **APPLICATION** | `SE_CONTINUE`, `SE_HALT`, `SE_TERMINATE`, `SE_RESET`, `SE_DISABLE`, `SE_SKIP_CONTINUE` | 0-5 | Fatal/global control — propagates to tick engine |
| **FUNCTION** | `SE_FUNCTION_*` variants | 6-11 | Tick engine interface — blocks siblings |
| **PIPELINE** | `SE_PIPELINE_*` variants | 12-17 | Internal composite coordination |

## Composite Function Summary

| Composite | Execution Model | Blocks Siblings | Primary Use Case |
|-----------|-----------------|-----------------|------------------|
| `se_function_interface` | Parallel | N/A (top-level) | Tick engine entry point |
| `se_fork` | Parallel | No | Background parallel tasks |
| `se_fork_join` | Parallel | Yes | Blocking parallel phases |
| `se_sequence` | Sequential | No | Ordered step execution |
| `se_chain_flow` | Sequential with control | No | ChainTree walker emulation, looping |
| `se_while` | Conditional loop | No | Predicate-controlled iteration |
| `se_if_then_else` | Conditional branch | No | Binary conditional execution |
| `se_cond` | Priority dispatch | No | Lisp-style multi-way conditional |
| `se_state_machine` | Field-based dispatch | No | Finite state machines |
| `se_field_dispatch` | Field-based dispatch | No | Mode-based continuous behavior |
| `se_event_dispatch` | Event-based dispatch | No | Reactive event handling |
| `se_trigger_on_change` | Edge detection | No | Rising/falling edge actions |

---

## Execution Model Composites

### se_function_interface

**Purpose:** Top-level interface between tick engine and internal pipeline system.

**When to use:**
- Root of every behavior tree
- Entry point from external tick engine
- When you need to return `SE_FUNCTION_*` codes

**Lua DSL:**
```lua
se_function_interface(function()
    -- Children run in parallel
    se_fork_join(function() ... end)
    se_state_machine("state", cases)
    se_return_function_terminate()
end)
```

**Key behavior:** Translates PIPELINE codes to FUNCTION codes. Children returning `SE_FUNCTION_HALT` block subsequent siblings.

---

### se_fork

**Purpose:** Non-blocking parallel execution of all children.

**When to use:**
- Background tasks that run alongside other work
- Fire-and-forget parallel operations
- When completion order doesn't matter

**Lua DSL:**
```lua
se_fork(function()
    se_log("Background task")
    se_tick_delay(100)
end)
-- Siblings run in parallel with fork
```

**Key behavior:** Returns `SE_PIPELINE_CONTINUE` while running — does not block siblings.

---

### se_fork_join

**Purpose:** Blocking parallel execution — waits for all children to complete.

**When to use:**
- Initialization phases that must complete first
- Setup/teardown sequences
- When subsequent code depends on fork completion
- Synchronization barriers

**Lua DSL:**
```lua
se_fork_join(function()
    se_tick_delay(10)  -- Blocks siblings for 10 ticks
end)
-- Siblings only run after fork_join completes
```

**Key behavior:** Returns `SE_FUNCTION_HALT` while running — blocks siblings. Wraps `se_while` body implicitly.

---

### se_sequence

**Purpose:** Execute children one at a time, in order.

**When to use:**
- Ordered step execution
- When each step must complete before the next begins
- Simple sequential workflows

**Lua DSL:**
```lua
se_sequence(function()
    se_log("Step 1")      -- Fires immediately
    se_tick_delay(10)     -- Waits 10 ticks
    se_log("Step 2")      -- Fires after delay
end)
```

**Key behavior:** Advances to next child when current completes. ONESHOT/PRED children complete immediately on the same tick.

**Restriction:** Do NOT use return code functions inside sequences — they never disable and will hang.

---

### se_chain_flow

**Purpose:** ChainTree walker emulation with explicit control flow.

**When to use:**
- Cyclic/looping behavior via `SE_PIPELINE_RESET`
- When you need explicit control over chain progression
- Porting ChainTree behavior patterns
- Event generation loops

**Lua DSL:**
```lua
se_chain_flow(
    function() se_log("Loop iteration") end,
    function() se_tick_delay(10) end,
    function() se_queue_event(...) end,
    function() se_return_pipeline_reset() end  -- Loops forever
)
```

**Key behavior:**
- `SE_PIPELINE_HALT` stops chain this tick, resumes next tick
- `SE_PIPELINE_RESET` resets ALL children, enabling loops
- ONESHOT/PRED fire once per activation, then marked inactive

---

## Conditional Composites

### se_while

**Purpose:** Conditional loop with predicate evaluation.

**When to use:**
- Counter-based loops
- Condition-controlled iteration
- When body should run to completion each iteration

**Lua DSL:**
```lua
se_while(se_field_increment_and_test("counter", 5),
    function() se_log("Iteration") end,
    function() se_tick_delay(10) end
)
```

**Key behavior:**
- Evaluates predicate before each iteration
- Body wrapped in implicit `se_fork_join` (parallel children)
- Body reset recursively each iteration
- Predicate NOT reset (can maintain state across iterations)

---

### se_if_then_else

**Purpose:** Binary conditional execution.

**When to use:**
- Simple true/false branching
- When you have exactly two possible paths

**Lua DSL:**
```lua
se_if_then_else(
    se_pred("CONDITION"),
    function() se_log("True branch") end,
    function() se_log("False branch") end
)
```

---

### se_cond

**Purpose:** Lisp-style priority-ordered conditional dispatch.

**When to use:**
- Multiple conditions with priority ordering
- Complex boolean decision trees
- When first matching condition should win
- Exhaustive case coverage (mandatory default)

**Lua DSL:**
```lua
se_cond({
    se_cond_case(
        se_field_gt("temp", 100),
        function() se_chain_flow(...) end
    ),
    se_cond_case(
        se_field_lt("temp", 0),
        function() se_chain_flow(...) end
    ),
    se_cond_default(
        function() se_chain_flow(...) end
    )
})
```

**Key behavior:**
- Evaluates predicates in declaration order
- First true predicate's action executes
- Action persists across ticks while its predicate remains first-true
- Mandatory default case (like Lisp's `t`)

---

## Dispatch Composites

### se_state_machine

**Purpose:** Field-based finite state machine with explicit transitions.

**When to use:**
- Finite state machines
- When actions complete and advance to next state
- State-centric logic: "I'm in state X, do state X things"
- Actions that set the next state before completing

**Lua DSL:**
```lua
se_state_machine("state", {
    se_case(0, function()
        se_sequence(function()
            se_log("State 0")
            se_tick_delay(10)
            se_set_field("state", 1)
            se_return_pipeline_disable()
        end)
    end),
    se_case(1, function() ... end),
    se_case('default', function() ... end)
})
```

**Key behavior:**
- Actions run to completion, then wait for next state change
- Completed actions are reset and ready for re-entry
- Cyclic state machines supported (0 → 1 → 2 → 0)

---

### se_field_dispatch

**Purpose:** Continuous reactive dispatch based on field value.

**When to use:**
- Mode-based continuous behavior
- Actions that loop while their case matches
- External code changes the field value
- Value-centric logic: "While field=X, keep doing X things"

**Lua DSL:**
```lua
se_field_dispatch("mode", {
    se_case(1, function()
        se_chain_flow(function()
            se_log("Mode 1 active")
            se_tick_delay(5)
            se_return_pipeline_reset()  -- Loop while mode=1
        end)
    end),
    se_case('default', function() ... end)
})
```

**Key behavior:**
- Actions loop via `SE_PIPELINE_RESET` while case matches
- Field changes terminate old action, start new action fresh

---

### se_event_dispatch

**Purpose:** Reactive dispatch based on incoming event ID.

**When to use:**
- Handling external events (user input, timers, hardware)
- Event-driven state transitions
- Reactive event processing

**Lua DSL:**
```lua
se_event_dispatch({
    se_event_case(BUTTON_PRESSED, function()
        se_chain_flow(function()
            se_log("Button pressed!")
            se_set_field("state", 1)
            se_return_pipeline_reset()
        end)
    end),
    se_event_case('default', function()
        se_chain_flow(function()
            se_return_pipeline_halt()
        end)
    end)
})
```

**Key behavior:**
- Dispatches based on `event_id` from tick engine
- Actions reset after completion, ready for next event
- No field parameter — event_id comes from tick context

---

### se_trigger_on_change

**Purpose:** Edge detection for boolean conditions.

**When to use:**
- Rising edge detection (false → true)
- Falling edge detection (true → false)
- Reactive behavior on state transitions
- Monitoring boolean conditions

**Lua DSL:**
```lua
se_trigger_on_change(0,  -- initial_state
    function() p_call("TEST_BIT") int(0) end_call() end,  -- predicate
    function() se_chain_flow(...) end,  -- rising edge action
    function() se_chain_flow(...) end   -- falling edge action
)

-- Convenience functions:
se_on_rising_edge(predicate, action)
se_on_falling_edge(predicate, action)
```

**Key behavior:**
- Only fires on transitions, not while condition holds
- `initial_state` prevents spurious trigger on first tick
- Rising and falling actions are mutually exclusive

---

## Quick Reference: When to Use What

| Scenario | Use This |
|----------|----------|
| Tree entry point | `se_function_interface` |
| Background parallel work | `se_fork` |
| Blocking initialization phase | `se_fork_join` |
| Simple ordered steps | `se_sequence` |
| Looping with explicit control | `se_chain_flow` + `se_return_pipeline_reset` |
| Counter-based loop | `se_while` |
| Simple if/else | `se_if_then_else` |
| Priority-ordered conditions | `se_cond` |
| Finite state machine | `se_state_machine` |
| Mode-based continuous behavior | `se_field_dispatch` |
| React to external events | `se_event_dispatch` |
| Edge detection | `se_trigger_on_change` |

---

## Common Patterns

### Initialization → Main Loop

```lua
se_function_interface(function()
    -- Phase 1: Blocking init
    se_fork_join(function()
        se_log("Initializing...")
        se_tick_delay(10)
    end)
    
    -- Phase 2: Main operation
    se_state_machine("state", cases)
    se_event_dispatch(event_handlers)
    
    se_return_function_terminate()
end)
```

### Parallel Event Handling + State Machine

```lua
se_function_interface(function()
    se_event_dispatch(event_handlers)      -- React to events
    se_field_dispatch("mode", mode_actions) -- Mode-based behavior
    se_state_machine("state", state_cases)  -- State transitions
end)
```

### Cyclic Event Generator

```lua
se_chain_flow(
    function() se_log("Tick") end,
    function() se_tick_delay(20) end,
    function() se_queue_event(TYPE, ID, "data") end,
    function() se_return_pipeline_reset() end
)
```

### Conditional Loop with Parallel Body

```lua
se_while(se_field_increment_and_test("i", 10),
    function() se_log("Iteration %d", "i") end,
    function() se_tick_delay(5) end,
    function() se_set_field("progress", 1) end
)
```

---

## Child Type Handling Summary

| Composite | ONESHOT | PRED | MAIN |
|-----------|---------|------|------|
| `se_fork` | Invoke | Invoke | Invoke, track active |
| `se_fork_join` | Fire once, skip | Fire once, skip | Track for completion |
| `se_sequence` | Fire, advance | Fire, advance | Wait for completion |
| `se_chain_flow` | Fire once, terminate | Fire once, terminate | Handle result codes |
| `se_while` | N/A (body is fork_join) | Child 0 = condition | Child 1 = body |

---

## Return Code Function Placement

Return code functions (`se_return_*`) should be placed at the appropriate composite level:

| Function | Valid Placement |
|----------|-----------------|
| `se_return_function_terminate` | `se_function_interface` |
| `se_return_pipeline_disable` | `se_chain_flow`, `se_state_machine` cases |
| `se_return_pipeline_reset` | `se_chain_flow`, dispatch cases |
| `se_return_pipeline_halt` | Event dispatch default cases |

**Never use return code functions inside:** `se_fork`, `se_fork_join`, `se_sequence` — they will hang.
