# State Machine Test

## Overview

This test demonstrates the S-Expression engine's composite node system, including:

- **se_function_interface** - Top-level function interface to the tick engine
- **se_fork_join** - Blocking parallel execution (waits for all children)
- **se_fork** - Non-blocking parallel execution
- **se_sequence** - Sequential execution
- **se_state_machine** - Field-based state dispatch with cyclic transitions

## Test Structure

```lua
se_function_interface(function()
    -- Phase 1: Blocking initialization (ticks 1-11)
    se_fork_join(function()
        se_tick_delay(10)
    end)
    
    -- Phase 2: Parallel execution (starts tick 12)
    se_fork(function()           -- Non-blocking, runs in background
        se_tick_delay(10)
    end)
    
    se_state_machine("state", case_fn)  -- Cyclic: 0 → 1 → 2 → 0 → ...
    se_tick_delay(350)                   -- Overall timeout
    
    se_return_function_terminate()       -- Terminates at tick 363
end)
```

## Execution Phases

### Phase 1: Fork Join (Ticks 1-11)

The `se_fork_join` **blocks** all siblings until it completes.

```
Tick 1:  Fork Join starts, returns FUNCTION_HALT (blocking)
Tick 2-10: Fork Join child running (tick_delay)
Tick 11: Fork Join child completes
         "Fork Join Test Terminated" logs
```

**Key behavior:** `se_fork_join` returns `SE_FUNCTION_HALT` which prevents `se_function_interface` from invoking subsequent children.

### Phase 2: Parallel Execution (Ticks 12+)

After fork_join completes, three things run **in parallel**:

1. `se_fork` with 10-tick delay (completes tick 22)
2. `se_state_machine` cycling through states
3. `se_tick_delay(350)` overall timeout

```
Tick 12: Fork starts, State machine starts at state 0
         "Fork 1 Test Started", "State machine test started", "State 0"
Tick 22: Fork completes
         "Fork 1 Test Terminated", "State 0 terminated"
Tick 23: State machine transitions to state 1
         "State 1"
...
```

**Key behavior:** `se_fork` returns `SE_PIPELINE_CONTINUE` which does NOT block siblings.

### State Machine Cycling

The state machine cycles through states 0 → 1 → 2 → 0 → ... with 10-tick delays:

| Ticks | State | Events |
|-------|-------|--------|
| 12-22 | 0 | State 0 runs, terminates at tick 22 |
| 23-33 | 1 | State 1 runs, terminates at tick 33 |
| 34-45 | 2 | State 2 runs, terminates at tick 45 |
| 46-57 | 0 | State 0 runs again (cyclic!) |
| ... | ... | Continues cycling |

Each state cycle is ~12 ticks (11 ticks delay + 1 tick transition).

### Termination (Tick 363)

The `se_tick_delay(350)` completes at tick 362 (12 + 350), then:

```
Tick 362: tick_delay(350) completes
Tick 363: "State machine test finished" logs
          se_return_function_terminate() executes
          Returns SE_FUNCTION_TERMINATE
```

## State Machine Cases

```lua
case_fn[1] = se_case(0, ...)   -- State 0: delay 10, set state=1
case_fn[2] = se_case(1, ...)   -- State 1: delay 10, set state=2
case_fn[3] = se_case(2, ...)   -- State 2: delay 10, set state=0 (cycle)
case_fn[4] = se_case('default', ...)  -- Default: delay 100, terminate
```

Each case:
1. Logs state entry
2. Calls `CFL_DISABLE_CHILDREN` / `CFL_ENABLE_CHILD` (chain flow control)
3. Delays 10 ticks
4. Sets next state
5. Logs state exit
6. Returns `SE_PIPELINE_DISABLE`

## Result Code Flow

```
Tick 1-11:  FUNCTION_HALT     (fork_join blocking)
Tick 12+:   FUNCTION_HALT     (tick_delay(350) running)
Tick 363:   FUNCTION_TERMINATE (test complete)
```

**Why FUNCTION_HALT after fork_join completes?**

The `se_tick_delay(350)` returns `SE_FUNCTION_HALT` while running, which propagates through `se_function_interface` to the tick engine.

## Composite Behavior Summary

| Composite | Return While Running | Blocks Siblings |
|-----------|---------------------|-----------------|
| `se_fork_join` | `SE_FUNCTION_HALT` | Yes |
| `se_fork` | `SE_PIPELINE_CONTINUE` | No |
| `se_sequence` | `SE_PIPELINE_CONTINUE` | No |
| `se_state_machine` | `SE_PIPELINE_CONTINUE/HALT` | No |
| `se_tick_delay` | `SE_FUNCTION_HALT` | Via parent |

## Test Output Analysis

```
=== STATE MACHINE TEST ===

[DEBUG] Fork Join Test Started       <- Tick 1
[DEBUG] Fork Join Test Started       <- Fork join child
Tick   1: FUNCTION_HALT              <- Fork join blocking
...
Tick  11: FUNCTION_HALT
[DEBUG] Fork Join Test Terminated    <- Fork join complete
[DEBUG] Fork 1 Test Started          <- Tick 12: parallel phase starts
[DEBUG] State machine test started
[DEBUG] State 0
cfl_disable_children
cfl_enable_child: enabling child 0
Tick  12: FUNCTION_HALT              <- tick_delay(350) now blocking
...
Tick  22: FUNCTION_HALT
[DEBUG] Fork 1 Test Terminated       <- Fork completes (10 ticks)
[DEBUG] State 0 terminated           <- State 0 completes (11 ticks)
Tick  23: FUNCTION_HALT
[DEBUG] State 1                      <- State transition 0 → 1
...
(state machine cycles 0 → 1 → 2 → 0 → ...)
...
Tick 362: FUNCTION_HALT
[DEBUG] State machine test finished  <- tick_delay(350) completes
Tick 363: FUNCTION_TERMINATE         <- Test terminates
✅ PASSED
```

## Key Patterns Demonstrated

1. **Sequential phases with fork_join** - Initialization must complete before main logic
2. **Parallel execution with fork** - Background tasks alongside main logic
3. **Cyclic state machines** - States that loop back (0 → 1 → 2 → 0)
4. **Timeout pattern** - `se_tick_delay(350)` as overall execution limit
5. **Clean termination** - `se_return_function_terminate()` at function_interface level

## Files

- `state_machine.lua` - Lua DSL test definition
- `main.c` - C test harness
- `state_machine_test.h` - Generated header with tree hash
- `state_machine_test_bin_32.h` - Generated binary ROM data
- `state_machine_test_records.h` - Generated record definitions