# se_cond - Lisp-Style Conditional Dispatch

## Lisp Origins

`se_cond` is modeled directly on Common Lisp's `cond` special form. In Lisp, `cond` evaluates a series of test-expression pairs in order and executes the body of the first clause whose test returns non-nil:

```lisp
(cond
    ((> x 10) (do-something))
    ((< x 0)  (do-other))
    (t        (do-default)))
```

The `t` clause acts as a catch-all default — it always evaluates to true, so it matches when no prior clause does. This pattern provides priority-ordered conditional dispatch: conditions are checked top to bottom, first match wins, and a trailing `t` guarantees exhaustive coverage.

`se_cond` preserves these semantics exactly. Each case is a `(predicate, action)` pair. Predicates are evaluated in declaration order. The first predicate returning `true` has its action executed. A mandatory default case uses `SE_TRUE` (the equivalent of Lisp's `t`). Because `se_cond` operates in a tick-based runtime, the matched action persists across ticks — if the same predicate wins on successive ticks, the action continues running without reset.

## Lua DSL

### se_cond

The top-level function wraps all cases inside an `SE_COND` composite node. Cases are passed as a table of closures returned by `se_cond_case` and `se_cond_default`.

```lua
function se_cond(cases)
    -- Reset tracking
    cond_case_count = 0
    cond_has_default = false
    in_cond = true

    local success, err = pcall(function()
        local c = m_call("SE_COND")
            if type(cases) == "function" then
                cases()
            elseif type(cases) == "table" then
                for _, case_fn in ipairs(cases) do
                    case_fn()
                end
            else
                error("se_cond: cases must be function or table")
            end
        end_call(c)
    end)

    local case_count = cond_case_count
    local has_default = cond_has_default

    in_cond = false
    cond_case_count = 0
    cond_has_default = false

    if not success then error(err) end
    if case_count == 0 then error("se_cond: must have at least one case") end
    if not has_default then error("se_cond: must have a default case (use se_cond_default)") end
end
```

Validations enforced at DSL generation time:

- At least one case required
- Default case required (ensures exhaustive matching, like Lisp's `t`)
- Duplicate default detection
- Default must be the last case
- Cases must be inside `se_cond`

### se_cond_case

Each case pairs a predicate closure with an action closure. The predicate uses the unified predicate builder — all predicates return closures, no `function()` wrappers needed at the call site.

```lua
function se_cond_case(pred_fn, action_fn)
    return function()
        if not in_cond then
            error("se_cond_case: must be used inside se_cond")
        end
        if cond_has_default then
            error("se_cond_case: cannot add cases after se_cond_default (default must be last)")
        end
        cond_case_count = cond_case_count + 1
        pred_fn()
        action_fn()
    end
end
```

### se_cond_default

Default case uses `SE_TRUE` as the predicate, mirroring Lisp's `(t ...)` clause:

```lua
function se_cond_default(action_fn)
    return function()
        if not in_cond then
            error("se_cond_default: must be used inside se_cond")
        end
        if cond_has_default then
            error("se_cond_default: duplicate default case")
        end
        cond_has_default = true
        cond_case_count = cond_case_count + 1
        local pred = p_call("SE_TRUE")
        end_call(pred)
        action_fn()
    end
end
```

### Predicate Usage

Predicates use the unified predicate builder. All leaf predicates (`se_check_event`, `se_true`, `se_field_eq`, `se_pred`, `se_pred_with`, etc.) return closures. Simple predicates pass directly to `se_cond_case` with no wrapping:

```lua
se_cond({
    se_cond_case(
        se_check_event(USER_EVENT_1, USER_EVENT_3),
        function()
            se_chain_flow(function()
                se_log("Matched EVENT_1 or EVENT_3")
                se_return_pipeline_reset()
            end)
        end
    ),
    se_cond_case(
        se_field_eq("mode", 1),
        function()
            se_chain_flow(function()
                se_log("Mode is 1")
                se_return_pipeline_reset()
            end)
        end
    ),
    se_cond_default(
        function()
            se_chain_flow(function()
                se_log("No condition matched")
                se_return_pipeline_reset()
            end)
        end
    )
})
```

For composite predicates, use `pred_begin` / `pred_end` with `pred_close(id)` validation:

```lua
pred_begin()
    local or1 = se_pred_or()
        local and1 = se_pred_and()
            se_field_eq("mode", 1)
            se_field_gt("temp", 100)
        pred_close(and1)
        local not1 = se_pred_not()
            se_check_event(USER_EVENT_4)
        pred_close(not1)
    pred_close(or1)
local complex = pred_end()

se_cond({
    se_cond_case(
        complex,
        function()
            se_chain_flow(function()
                se_log("Complex condition met")
                se_return_pipeline_reset()
            end)
        end
    ),
    se_cond_default(
        function()
            se_chain_flow(function()
                se_log("Default")
                se_return_pipeline_reset()
            end)
        end
    )
})
```

## Binary Parameter Layout

Children are emitted as alternating `(predicate, action)` pairs. Both predicates and actions are wrapped in OPEN_CALL/CLOSE blocks and both count as logical children:

```
Child 0: predicate 0  (OPEN_CALL PRED)
Child 1: action 0     (OPEN_CALL MAIN)
Child 2: predicate 1  (OPEN_CALL PRED)
Child 3: action 1     (OPEN_CALL MAIN)
...
Child N-2: SE_TRUE    (OPEN_CALL PRED, default predicate)
Child N-1: action     (OPEN_CALL MAIN, default action)
```

The runtime expects an even number of children (pred-action pairs).

## C Runtime

### Scanning Strategy

The C runtime scans the raw parameter stream looking for predicate/action pairs. Both predicates and actions are counted as logical children. The `child_index` counter increments for every OPEN_CALL encountered, so the action's logical child index is captured directly after skipping the predicate.

This is the key insight that resolved the child-index mismatch bug: because predicates wrapped in OPEN_CALL are counted as logical children alongside actions, `matched_child * 2 + 1` does not give the correct index. Instead, tracking `child_index` as parameters are scanned naturally yields the correct action index.

### Implementation

```c
static s_expr_result_t se_cond(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_id; (void)event_data;

    if (event_type == SE_EVENT_TERMINATE) {
        s_expr_children_terminate_all(inst, params, param_count);
        s_expr_set_user_flags(inst, 0xFFFF);
        return SE_PIPELINE_CONTINUE;
    }

    if (event_type == SE_EVENT_INIT) {
        s_expr_set_user_flags(inst, 0xFFFF);
        return SE_PIPELINE_CONTINUE;
    }

    // Parameter layout: [pred0] [action0] [pred1] [action1] ...
    // Both predicates and actions are OPEN_CALL and counted as logical children.
    // Predicates are at even child indices: 0, 2, 4, ...
    // Actions are at odd child indices:    1, 3, 5, ...

    uint16_t active_child = s_expr_get_user_flags(inst);
    uint16_t matched_action = 0xFFFF;
    uint16_t child_index = 0;

    for (uint16_t i = 0; i < param_count; ) {
        if (s_expr_param_is_predicate(&params[i])) {
            bool result = s_expr_invoke_pred(inst, params, i);
            // Skip predicate
            i = s_expr_skip_param(params, i);
            child_index++;

            if (result && matched_action == 0xFFFF) {
                matched_action = child_index;  // action is next child
                break;
            }

            // Skip action
            i = s_expr_skip_param(params, i);
            child_index++;
        } else {
            i = s_expr_skip_param(params, i);
            child_index++;
        }
    }

    if (matched_action == 0xFFFF) {
        EXCEPTION("se_cond: no matching case (missing default)");
        return SE_PIPELINE_CONTINUE;
    }

    // Active child changed: terminate old, reset new
    if (matched_action != active_child) {
        if (active_child != 0xFFFF) {
            s_expr_child_terminate(inst, params, param_count, active_child);
            s_expr_child_reset_recursive(inst, params, param_count, active_child);
        }
        s_expr_child_terminate(inst, params, param_count, matched_action);
        s_expr_child_reset_recursive(inst, params, param_count, matched_action);
        s_expr_set_user_flags(inst, matched_action);
    }

    s_expr_result_t r = s_expr_child_invoke(inst, params, param_count, matched_action);

    // Non-PIPELINE codes (0-11): propagate to caller
    if (r < SE_PIPELINE_CONTINUE) {
        return r;
    }

    switch (r) {
        case SE_PIPELINE_CONTINUE:
        case SE_PIPELINE_HALT:
            return SE_PIPELINE_CONTINUE;
        case SE_PIPELINE_RESET:
            s_expr_child_terminate(inst, params, param_count, matched_action);
            s_expr_child_reset_recursive(inst, params, param_count, matched_action);
            return SE_PIPELINE_CONTINUE;
        case SE_PIPELINE_DISABLE:
        case SE_PIPELINE_TERMINATE:
        case SE_PIPELINE_SKIP_CONTINUE:
            return r;
        default:
            EXCEPTION("se_cond: unexpected result code");
            return SE_PIPELINE_CONTINUE;
    }
}
```

### Predicate Detection

The `s_expr_param_is_predicate` helper peeks inside OPEN_CALL blocks to identify predicates:

```c
static inline bool s_expr_param_is_predicate(const s_expr_param_t* param) {
    uint8_t opcode = param->type & S_EXPR_OPCODE_MASK;
    if (opcode == S_EXPR_PARAM_PRED) return true;
    if (opcode == S_EXPR_PARAM_OPEN_CALL) {
        return ((param + 1)->type & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_PRED;
    }
    return false;
}
```

## Behavior

### Each Tick

1. Iterate parameters, finding predicate/action pairs
2. For each predicate: evaluate it; if true and no prior match, record the action's child index, break
3. If `matched_action == 0xFFFF`: should never happen (default guarantees a match), raise EXCEPTION
4. If matched action differs from `active_child`: terminate old action, reset new action, update `user_flags`
5. Invoke matched action
6. Handle result code

### State Tracking

```
user_flags: Logical child index of the currently active action
            0xFFFF = no active action (initial state)
```

### Action Transitions

When a different predicate becomes the first true match:

```
Old action active (child 3) → New match (child 1):
1. Terminate action at child 3
2. Reset action at child 3
3. Terminate action at child 1 (clean slate)
4. Reset action at child 1
5. user_flags = 1
6. Invoke action at child 1
```

### Result Code Handling

| Action Returns | se_cond Action |
|----------------|----------------|
| Non-PIPELINE (0-11) | Propagate to caller |
| SE_PIPELINE_CONTINUE | Action running, return CONTINUE |
| SE_PIPELINE_HALT | Action paused, return CONTINUE |
| SE_PIPELINE_RESET | Terminate and reset action, return CONTINUE |
| SE_PIPELINE_DISABLE | Propagate to caller |
| SE_PIPELINE_TERMINATE | Propagate to caller |
| SE_PIPELINE_SKIP_CONTINUE | Propagate to caller |

## Usage Examples

### Event-Driven Priority Dispatch

```lua
se_cond({
    se_cond_case(
        se_check_event(USER_EVENT_1, USER_EVENT_3),
        function()
            se_chain_flow(function()
                se_log("Matched EVENT_1 or EVENT_3")
                se_return_pipeline_reset()
            end)
        end
    ),
    se_cond_case(
        se_check_event(USER_EVENT_2),
        function()
            se_chain_flow(function()
                se_log("Matched EVENT_2")
                se_return_pipeline_reset()
            end)
        end
    ),
    se_cond_default(
        function()
            se_chain_flow(function()
                se_log("No event matched")
                se_return_pipeline_reset()
            end)
        end
    )
})
```

### Field-Based Conditions

```lua
se_cond({
    se_cond_case(
        se_field_gt("temp", 100),
        function()
            se_chain_flow(function()
                se_log("Overtemp: reducing power")
                se_set_field("power", 50)
                se_return_pipeline_reset()
            end)
        end
    ),
    se_cond_case(
        se_field_lt("temp", 0),
        function()
            se_chain_flow(function()
                se_log("Undertemp: warming up")
                se_set_field("heater", 1)
                se_return_pipeline_reset()
            end)
        end
    ),
    se_cond_default(
        function()
            se_chain_flow(function()
                se_set_field("power", 100)
                se_return_pipeline_reset()
            end)
        end
    )
})
```

### User-Defined Predicates

```lua
se_cond({
    se_cond_case(
        se_pred_with("TEST_BIT", function() int(0) end),
        function()
            se_chain_flow(function()
                se_log("Bit 0 set")
                se_return_pipeline_reset()
            end)
        end
    ),
    se_cond_case(
        se_pred("SENSOR_READY"),
        function()
            se_chain_flow(function()
                se_log("Sensor ready")
                se_return_pipeline_reset()
            end)
        end
    ),
    se_cond_default(
        function()
            se_chain_flow(function()
                se_log("Idle")
                se_return_pipeline_halt()
            end)
        end
    )
})
```

### Composite Predicate with Builder

```lua
pred_begin()
    local and1 = se_pred_and()
        se_pred("SENSOR_READY")
        se_pred("CALIBRATED")
    pred_close(and1)
local ready_and_calibrated = pred_end()

se_cond({
    se_cond_case(
        ready_and_calibrated,
        function()
            se_chain_flow(function()
                se_log("Sensor ready and calibrated")
                se_return_continue()
            end)
        end
    ),
    se_cond_case(
        se_pred("SENSOR_READY"),
        function()
            se_chain_flow(function()
                se_log("Sensor ready but not calibrated")
                se_return_continue()
            end)
        end
    ),
    se_cond_default(
        function()
            se_chain_flow(function()
                se_log("Sensor not ready")
                se_tick_delay(10)
                se_return_pipeline_reset()
            end)
        end
    )
})
```

### Looping Alarm Pattern

```lua
se_cond({
    se_cond_case(
        se_pred("ALARM_ACTIVE"),
        function()
            se_chain_flow(function()
                se_set_field("led", 1)
                se_tick_delay(5)
                se_set_field("led", 0)
                se_tick_delay(5)
                se_return_pipeline_reset()  -- Loop while alarm active
            end)
        end
    ),
    se_cond_default(
        function()
            se_chain_flow(function()
                se_set_field("led", 0)
                se_return_pipeline_reset()
            end)
        end
    )
})
```

## Comparison with Other Constructs

### se_cond vs se_if_then_else

| Feature | se_cond | se_if_then_else |
|---------|---------|-----------------|
| Predicates | N (ordered) | 1 |
| Branches | N + default | 2 (then/else) |
| Priority | First match wins | Binary true/false |
| State tracking | Tracks active child via user_flags | None |
| Action lifetime | Persistent across ticks | Re-evaluated each tick |
| Lisp analogy | `cond` | `if` |

### se_cond vs se_field_dispatch

| Feature | se_cond | se_field_dispatch |
|---------|---------|-------------------|
| Dispatch key | Predicate result | Integer field value |
| Evaluation | Sequential, first-match | Direct case match |
| Dynamic | Predicates can use any logic | Field value only |
| Overhead | Evaluates predicates in order | Single field read + scan |
| Use case | Complex boolean conditions | Simple mode selection |
| Lisp analogy | `cond` | `case` |

### se_cond vs se_trigger_on_change

| Feature | se_cond | se_trigger_on_change |
|---------|---------|---------------------|
| Trigger | Predicate becomes first true | Predicate edge (rise/fall) |
| Actions | N (one per predicate) | 2 (rise/fall) |
| Re-evaluation | Every tick | Every tick (edge-sensitive) |
| Use case | Priority dispatch | Edge detection |

## Key Design Decisions

1. **Lisp cond semantics** — Evaluates in declaration order, first true wins, mandatory default mirrors `(t ...)`
2. **Persistent actions** — Active action continues across ticks as long as its predicate remains the first true match
3. **Clean transitions** — When a different predicate wins, old action is terminated and new action starts fresh
4. **child_index scanning** — Both predicates and actions count as logical children; the scan tracks child_index directly rather than computing `pair * 2 + 1`
5. **Selective PIPELINE handling** — CONTINUE and HALT are absorbed to keep the cond running; RESET triggers action restart; DISABLE, TERMINATE, and SKIP_CONTINUE propagate to the parent since se_cond is not in a position to interpret them
6. **Unified predicate builder** — All predicates return closures for consistent API; no user-facing `function()` wrappers needed

## Registration

Builtin function list in `s_expr_dsl.lua`:

```lua
"SE_COND",
```

C builtin table:

```c
{ SE_COND_HASH, (void*)se_cond },
```

## Files

| File | Description |
|------|-------------|
| `s_expr_primitives.c` | C runtime implementation |
| `s_expr_dsl_primitives.lua` | Lua DSL (se_cond, se_cond_case, se_cond_default) |
| `s_expr_dsl.lua` | Builtin registration (SE_COND) |