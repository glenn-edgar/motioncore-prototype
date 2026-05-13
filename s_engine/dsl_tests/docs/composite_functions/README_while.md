# SE_WHILE - Conditional Loop Composite

## Overview

`se_while` implements a conditional loop that evaluates a predicate and, while true, executes a body to completion before re-evaluating. The body is wrapped in an implicit `se_fork_join`, allowing multiple children to run in parallel within each iteration.

## Lua DSL

```lua
function se_while(condition, ...)
    local children = {...}
    local w = m_call("SE_WHILE")
    condition()
    se_fork_join(unpack(children))
    end_call(w)
end
```

**Parameters:**
- `condition` - A predicate function that returns true/false
- `...` - One or more child functions forming the loop body (wrapped in se_fork_join)

## Param Layout

```
[PRED condition] [MAIN se_fork_join(...children)]
```

- Child 0: Predicate function
- Child 1: Main body (se_fork_join containing the loop children)

## State Machine

| State | Name | Description |
|-------|------|-------------|
| 0 | `EVAL_PRED` | Evaluate predicate, transition to RUN_BODY or exit |
| 1 | `RUN_BODY` | Tick body until completion, then back to EVAL_PRED |

```
┌─────────────────────────────────────────────┐
│                                             │
│    ┌──────────┐    true    ┌──────────┐    │
│    │EVAL_PRED │───────────▶│ RUN_BODY │    │
│    └──────────┘            └──────────┘    │
│         │                       │          │
│         │ false                 │ body     │
│         │                       │ complete │
│         ▼                       │          │
│   SE_PIPELINE_DISABLE           │          │
│                                 │          │
│         ◀───────────────────────┘          │
└─────────────────────────────────────────────┘
```

## Lifecycle Events

### INIT
- Sets state to `EVAL_PRED`
- Returns `SE_PIPELINE_CONTINUE`

### TERMINATE
- Terminates body child if initialized
- Returns `SE_PIPELINE_CONTINUE`

### TICK
- Executes state machine (see below)

## Tick Behavior

### State: EVAL_PRED

1. Invoke predicate via `s_expr_child_invoke_pred`
2. If predicate returns **false**: return `SE_PIPELINE_DISABLE` (loop complete)
3. If predicate returns **true**:
   - Reset body recursively (prepare for new iteration)
   - Set state to `RUN_BODY`
   - **Fall through** to RUN_BODY on same tick

### State: RUN_BODY

1. Invoke body via `s_expr_child_invoke`
2. Handle result:

| Body Returns | Action |
|--------------|--------|
| **Non-PIPELINE (0-11)** | Propagate immediately (fatal) |
| `SE_PIPELINE_CONTINUE` | Return `SE_FUNCTION_HALT` (body running) |
| `SE_PIPELINE_HALT` | Return `SE_FUNCTION_HALT` (body running) |
| `SE_PIPELINE_SKIP_CONTINUE` | Return `SE_FUNCTION_HALT` (body running) |
| `SE_PIPELINE_DISABLE` | Terminate body, reset, go to EVAL_PRED, return `SE_PIPELINE_HALT` |
| `SE_PIPELINE_TERMINATE` | Terminate body, reset, go to EVAL_PRED, return `SE_PIPELINE_HALT` |
| `SE_PIPELINE_RESET` | Terminate body, reset, go to EVAL_PRED, return `SE_PIPELINE_HALT` |

## Return Codes

| Condition | Returns |
|-----------|---------|
| Predicate false | `SE_PIPELINE_DISABLE` |
| Body still running | `SE_FUNCTION_HALT` |
| Body iteration complete | `SE_PIPELINE_HALT` |
| Fatal error from body | Propagated (0-11) |

## Predicate Handling

The predicate (child 0):
- Receives INIT event on first invocation
- Has node state (flags, state, user_data)
- Is **not** reset between iterations
- Can maintain state across loop iterations (e.g., counter-based predicates)

This allows stateful predicates like:

```lua
-- Predicate that counts iterations internally
se_while(se_state_increment_and_test(5),
    function() se_log("Iteration") end
)
```

## Body Handling

The body (child 1):
- Is wrapped in `se_fork_join` by the DSL
- **Is reset recursively** at the start of each iteration
- Runs to completion before predicate re-evaluates
- All children within execute in parallel (fork_join semantics)

## Usage Examples

### Simple Counter Loop

```lua
se_while(se_field_increment_and_test("counter", 5),
    function() se_log("counter: %d", "counter") end,
    function() se_tick_delay(10) end
)
```

**Execution:**
```
Tick 1:  counter=0, pred=true, body starts, log fires, delay starts
Tick 2-10: delay running
Tick 11: delay complete, body complete, pred re-eval
Tick 11: counter=1, pred=true, body resets, log fires, delay starts
...
Tick 51: counter=5, pred=true, body starts
Tick 61: body complete, pred re-eval
Tick 61: counter=6, pred=false, SE_PIPELINE_DISABLE
```

### Parallel Actions Per Iteration

```lua
se_while(some_condition,
    function() se_log("Action A") end,
    function() se_log("Action B") end,
    function() se_tick_delay(10) end,
    function() se_set_field("iteration_complete", 1) end
)
```

All children run in parallel within each iteration via the implicit fork_join.

### Nested Loops

```lua
se_while(outer_condition,
    function()
        se_while(inner_condition,
            function() se_log("Inner iteration") end
        )
    end,
    function() se_log("Outer iteration complete") end
)
```

### Event-Driven Loop

```lua
se_while(se_field_less_than("error_count", 3),
    function() se_send_request() end,
    function() se_wait_for_response() end,
    function() 
        se_if(response_is_error,
            function() se_increment_field("error_count") end
        )
    end
)
```

## Comparison with se_chain_flow Reset

| Aspect | `se_while` | `se_chain_flow` + RESET |
|--------|------------|-------------------------|
| **Loop control** | Predicate function | Explicit RESET return |
| **Condition check** | Before each iteration | None (always loops) |
| **Body execution** | Parallel (fork_join) | Sequential (chain) |
| **Exit condition** | Predicate returns false | TERMINATE or DISABLE |
| **State machine** | Yes (2 states) | No |

## Important Notes

### Fall-Through Optimization

When the predicate returns true, `se_while` falls through from EVAL_PRED to RUN_BODY on the same tick. This avoids wasting a tick just for state transition.

### Body Reset Each Iteration

The body is reset recursively before each iteration. All oneshots, delays, and child state within the body start fresh each time through the loop.

### Predicate Not Reset

The predicate maintains its state across iterations. This is intentional — predicates like `se_field_increment_and_test` need to track iteration count. If you need a fresh predicate evaluation each time, design the predicate to be stateless.

### Return Code: SE_FUNCTION_HALT vs SE_PIPELINE_HALT

- `SE_FUNCTION_HALT`: Body is still running (delay in progress, etc.)
- `SE_PIPELINE_HALT`: Body iteration complete, returning to predicate

This distinction matters when `se_while` is nested inside other composites.

## State Storage

- **state**: 8-bit state machine (EVAL_PRED or RUN_BODY)
- **user_data**: Not used
- **pointer_array**: Not used


