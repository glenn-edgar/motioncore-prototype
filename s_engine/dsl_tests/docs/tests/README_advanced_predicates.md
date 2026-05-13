# Advanced Primitive Test

## Overview

This test is a derivative of the [Dispatch Test](README_dispatch.md) that extends the original event-driven state machine with additional S-Engine primitives. It retains the core `se_event_dispatch` + `se_state_machine` structure from the dispatch test and adds exercising of `se_if_then_else` and `se_cond` — the two predicate-driven branching constructs.

## Relationship to Dispatch Test

The dispatch test combines three parallel mechanisms: `se_event_dispatch`, `se_field_dispatch`, and `se_state_machine`. This derivative makes two structural changes:

1. **Removed** `se_field_dispatch` — the field-reactive dispatch is dropped to focus the test on predicate-based branching.
2. **Added** `se_if_then_else` and `se_cond` — event handlers now use predicate closures (`se_check_event`) to demonstrate conditional branching within event processing and as a standalone instruction.

The state machine and event dispatch remain identical in structure.

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
    test_cond_instruction()                  -- se_cond predicate dispatch (NEW)
    se_state_machine("state", state_case_fn) -- Generates events
end)
```

Compared to the dispatch test, `se_field_dispatch` is replaced by `test_cond_instruction()` which exercises `se_cond`.

## New Primitives Tested

### se_if_then_else

Used inside event handlers to demonstrate predicate-driven branching within an event action:

```lua
se_if_then_else(
    se_check_event(USER_EVENT_1, USER_EVENT_3),  -- predicate closure
    event_displays_fn[1],                         -- then branch
    event_displays_fn[2]                          -- else branch
)
```

Two event handlers (`event_actions_fn[1]` and `event_actions_fn[4]`) embed `se_if_then_else` to test:

- Predicate closure passing — `se_check_event` returns a closure, not a boolean
- Multi-event predicates — `se_check_event(USER_EVENT_1, USER_EVENT_3)` matches either event
- Branch selection — then/else branches execute different log messages and `DISPLAY_EVENT_INFO` calls

### se_cond

The `test_cond_instruction()` function exercises Lisp-style conditional dispatch as a standalone instruction running in parallel with the event dispatch and state machine:

```lua
se_cond({
    se_cond_case(
        se_check_event(USER_EVENT_1, USER_EVENT_3),
        function()
            se_chain_flow(function()
                se_log("Matched EVENT_1 or EVENT_3")
                local o0 = o_call("DISPLAY_EVENT_INFO")
                end_call(o0)
                se_return_pipeline_reset()
            end)
        end
    ),
    se_cond_case(
        se_check_event(USER_EVENT_2),
        function() ... end
    ),
    se_cond_case(
        se_check_event(USER_EVENT_4),
        function() ... end
    ),
    se_cond_default(
        function()
            se_chain_flow(function()
                se_return_pipeline_reset()
            end)
        end
    )
})
```

This tests:

- First-match semantics — predicates evaluated in order, first true wins
- Predicate closures in se_cond_case — `se_check_event` returns a closure consumed by `se_cond_case`
- Default case — `se_cond_default` uses `SE_TRUE` predicate as the fallback
- Parallel operation — `se_cond` runs alongside `se_event_dispatch` and `se_state_machine` within `se_function_interface`

### Event Display Functions

Shared between `se_if_then_else` branches, these closures demonstrate reusable action definitions:

```lua
event_displays_fn[1] = function()
    se_chain_flow(function()
        se_log("if then branch start")
        local o0 = o_call("DISPLAY_EVENT_INFO")
        end_call(o0)
        se_log("if then branch end")
        se_return_pipeline_reset()
    end)
end

event_displays_fn[2] = function()
    se_chain_flow(function()
        se_log("if else branch start")
        local o0 = o_call("DISPLAY_EVENT_INFO")
        end_call(o0)
        se_log("if else branch end")
        se_return_pipeline_reset()
    end)
end
```

## Execution Flow

### Phase 1: State 0 (Ticks 1–21)

1. State machine starts in state 0
2. `se_cond` evaluates predicates each tick — no events queued yet, so default case runs
3. After 20 ticks, queues `USER_EVENT_1` and `USER_EVENT_2`

### Phase 2: Event Processing After State 0

Queued events are processed by the tick loop:

1. **USER_EVENT_1** → `event_actions_fn[1]`:
   - Calls `DISPLAY_EVENT_INFO` (shows event data 1.1)
   - `se_if_then_else` evaluates `se_check_event(1,3)` — matches EVENT_1, takes **then** branch
   - Sets `state = 1`
2. **USER_EVENT_2** → `event_actions_fn[3]`:
   - Calls `DISPLAY_EVENT_INFO` (shows event data 2.2)
3. `se_cond` also processes these events — `se_cond_case` for EVENT_1/EVENT_3 matches, logs and displays

### Phase 3: State 1 (Ticks 22–42)

1. State machine in state 1
2. After 20 ticks, queues `USER_EVENT_3` and `USER_EVENT_4`

### Phase 4: Event Processing After State 1

1. **USER_EVENT_3** → `event_actions_fn[2]`:
   - Calls `DISPLAY_EVENT_INFO` (shows event data 3.3)
   - Sets `state = 2`
2. **USER_EVENT_4** → `event_actions_fn[4]`:
   - `se_if_then_else` evaluates `se_check_event(1,3)` — EVENT_4 doesn't match, takes **else** branch
   - Calls `DISPLAY_EVENT_INFO` (shows event data 4.4)
3. `se_cond` also processes — EVENT_3 case matches first, EVENT_4 case matches on its event

### Phase 5: State 2 — Termination

1. State machine enters state 2
2. After 20 ticks, returns `SE_TERMINATE`
3. Tree terminates

## Component Interactions

```
┌─────────────────────────────────────────────────────────────────────┐
│                     SE_FUNCTION_INTERFACE                           │
│                                                                     │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────────┐ │
│  │ SE_EVENT_       │  │ SE_COND          │  │ SE_STATE_MACHINE    │ │
│  │ DISPATCH        │  │ (predicate       │  │                     │ │
│  │                 │  │  dispatch)       │  │  State 0:           │ │
│  │ USER_EVENT_1    │  │                 │  │   queue_event(1)    │ │
│  │   → if_then_    │  │ EVENT_1|3:      │  │   queue_event(2)    │ │
│  │     else(1,3)   │  │   log+display   │  │                     │ │
│  │   → state=1     │  │                 │  │  State 1:           │ │
│  │                 │  │ EVENT_2:        │  │   queue_event(3)    │ │
│  │ USER_EVENT_3    │  │   log+display   │  │   queue_event(4)    │ │
│  │   → state=2     │  │                 │  │                     │ │
│  │                 │  │ EVENT_4:        │  │  State 2:           │ │
│  │ USER_EVENT_4    │  │   log+display   │  │   → TERMINATE       │ │
│  │   → if_then_    │  │                 │  │                     │ │
│  │     else(1,3)   │  │ default:        │  │                     │ │
│  │                 │  │   reset (no-op) │  │                     │ │
│  │ default: halt   │  │                 │  │                     │ │
│  └─────────────────┘  └─────────────────┘  └─────────────────────┘ │
└─────────────────────────────────────────────────────────────────────┘
```

## C Test Harness

The C test harness (`advanced_primitive_test_main.c`) is identical in structure to the dispatch test harness. It implements the standard tick loop with event queue processing:

```c
do {
    result = s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    tick_count++;

    uint16_t event_count = s_expr_event_queue_count(tree);
    while (event_count > 0) {
        s_expr_event_pop(tree, &tick_type, &event_id, &event_data);

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

The harness loads the module from both ROM (compiled-in binary) and optionally from a `.bin` file on disk.

## Files

| File | Description |
|------|-------------|
| `state_machine.lua` | Lua DSL test definition (module: `advanced_primitive_test`) |
| `advanced_primitive_test_main.c` | C test harness with tick loop |
| `advanced_primitive_test.h` | Generated tree hashes |
| `advanced_primitive_test_bin_32.h` | Generated binary ROM |
| `advanced_primitive_test_records.h` | Generated record definitions |

## Key Concepts Demonstrated

1. **se_if_then_else with closures** — Predicate is a closure from `se_check_event`, not a boolean value
2. **se_cond parallel operation** — Lisp-style conditional dispatch runs alongside event dispatch and state machine
3. **Reusable action closures** — `event_displays_fn` table shared across multiple `se_if_then_else` invocations
4. **Multi-event predicates** — `se_check_event(USER_EVENT_1, USER_EVENT_3)` matches either event ID
5. **se_cond_default** — Mandatory fallback case using `SE_TRUE` predicate
6. **Event generation and handling** — State machine generates events consumed by both `se_event_dispatch` and `se_cond`

## Differences from Dispatch Test

| Aspect | Dispatch Test | Advanced Primitive Test |
|--------|--------------|------------------------|
| Module name | `dispatch_test` | `advanced_primitive_test` |
| Parallel composites | event_dispatch + field_dispatch + state_machine | event_dispatch + se_cond + state_machine |
| se_field_dispatch | Yes | No |
| se_if_then_else | No | Yes (inside event handlers) |
| se_cond | No | Yes (standalone instruction) |
| Event handler complexity | Simple log + display + set_field | Includes conditional branching |
| Reusable closures | No | Yes (`event_displays_fn` table) |

## Test Pass Criteria

The test passes when:

- Tree terminates normally (`SE_FUNCTION_TERMINATE`)
- State machine progresses through states 0 → 1 → 2
- Events are properly generated and handled
- `se_if_then_else` selects correct branch based on event ID
- `se_cond` matches correct case for each event
- Total ticks < max_ticks (100)

