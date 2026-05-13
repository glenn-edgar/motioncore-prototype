# S-Expression Engine Predicate Functions

## Overview

The S-Expression engine provides predicate functions that return boolean values for conditional control flow. Predicates are used with composites like `se_while`, `se_cond`, `se_if_then_else`, and `se_trigger_on_change`.

## Predicate Characteristics

Unlike MAIN functions, predicates:
- Return `bool` instead of result codes
- Are typically stateless (evaluate and return immediately)
- Do not receive explicit `SE_EVENT_INIT` or `SE_EVENT_TERMINATE` events from the engine

However, predicates **do** have a `node_index` and access to:
- `node_states[]` — flags, state, user_data
- Blackboard fields via field references

This allows certain predicates to maintain state across invocations.

---

## Predicate Builder System

The Lua DSL provides a stack-based predicate builder for composing complex predicates.

### Simple Predicates (Outside Builder)

Simple predicates can be used directly without the builder:

```lua
se_while(se_field_lt("counter", 10), ...)
se_cond_case(se_check_event(USER_EVENT_1), ...)
se_if_then_else(se_pred("SENSOR_READY"), ...)
```

### Composite Predicates (Inside Builder)

For complex boolean logic, use `pred_begin()` / `pred_end()`:

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
local complex_pred = pred_end()

se_cond_case(complex_pred, ...)
```

### Builder Functions

| Function | Description |
|----------|-------------|
| `pred_begin()` | Start building a composite predicate |
| `pred_end()` | Finish and return the predicate closure |
| `pred_close(id)` | Close a composite predicate (validates matching id) |

---

## Composite Predicates (Boolean Logic)

These predicates combine child predicates using boolean logic. They must be used inside `pred_begin()` / `pred_end()`.

### se_pred_and

**Purpose:** Returns true if ALL child predicates return true.

**Lua DSL:**
```lua
pred_begin()
    local id = se_pred_and()
        se_field_eq("mode", 1)
        se_field_gt("temp", 50)
        se_pred("SENSOR_READY")
    pred_close(id)
local pred = pred_end()
```

**Behavior:** Short-circuit evaluation — stops on first false.

**Truth table (2 inputs):**

| A | B | Result |
|---|---|--------|
| F | F | F |
| F | T | F |
| T | F | F |
| T | T | T |

---

### se_pred_or

**Purpose:** Returns true if ANY child predicate returns true.

**Lua DSL:**
```lua
pred_begin()
    local id = se_pred_or()
        se_check_event(USER_EVENT_1)
        se_check_event(USER_EVENT_2)
        se_field_eq("error", 1)
    pred_close(id)
local pred = pred_end()
```

**Behavior:** Short-circuit evaluation — stops on first true.

**Truth table (2 inputs):**

| A | B | Result |
|---|---|--------|
| F | F | F |
| F | T | T |
| T | F | T |
| T | T | T |

---

### se_pred_not

**Purpose:** Returns the inverse of the first child predicate.

**Lua DSL:**
```lua
pred_begin()
    local id = se_pred_not()
        se_pred("ALARM_ACTIVE")
    pred_close(id)
local pred = pred_end()
```

**Behavior:** Evaluates first child predicate, returns negation. Additional children are ignored.

**Truth table:**

| A | Result |
|---|--------|
| F | T |
| T | F |

---

### se_pred_nand

**Purpose:** Returns true if NOT all child predicates are true (NOT AND).

**Lua DSL:**
```lua
pred_begin()
    local id = se_pred_nand()
        se_field_eq("a", 1)
        se_field_eq("b", 1)
    pred_close(id)
local pred = pred_end()
```

**Behavior:** Equivalent to `NOT(AND(children))`.

**Truth table (2 inputs):**

| A | B | Result |
|---|---|--------|
| F | F | T |
| F | T | T |
| T | F | T |
| T | T | F |

---

### se_pred_nor

**Purpose:** Returns true if NO child predicates are true (NOT OR).

**Lua DSL:**
```lua
pred_begin()
    local id = se_pred_nor()
        se_check_event(ERROR_EVENT)
        se_field_eq("fault", 1)
    pred_close(id)
local pred = pred_end()
```

**Behavior:** Equivalent to `NOT(OR(children))`.

**Truth table (2 inputs):**

| A | B | Result |
|---|---|--------|
| F | F | T |
| F | T | F |
| T | F | F |
| T | T | F |

---

### se_pred_xor

**Purpose:** Returns true if EXACTLY ONE child predicate is true.

**Lua DSL:**
```lua
pred_begin()
    local id = se_pred_xor()
        se_field_eq("mode", 1)
        se_field_eq("mode", 2)
    pred_close(id)
local pred = pred_end()
```

**Behavior:** Counts true results, returns true only if count equals 1.

**Truth table (2 inputs):**

| A | B | Result |
|---|---|--------|
| F | F | F |
| F | T | T |
| T | F | T |
| T | T | F |

**Note:** For more than 2 inputs, XOR returns true only if exactly one is true (not the chained XOR of some languages).

---

## Constant Predicates

### se_true

**Purpose:** Always returns true.

**Lua DSL:**
```lua
se_true()
```

**Use cases:**
- Default case in `se_cond` (like Lisp's `t`)
- Placeholder during development
- Unconditional branches

---

### se_false

**Purpose:** Always returns false.

**Lua DSL:**
```lua
se_false()
```

**Use cases:**
- Disabling branches during development
- Testing conditional logic

---

## Event Predicates

### se_check_event

**Purpose:** Returns true if the current event_id matches any of the provided values.

**Param layout:** `[INT event_id_1] [INT event_id_2] ...`

**Lua DSL:**
```lua
-- Single event
se_check_event(USER_EVENT_1)

-- Multiple events (OR logic)
se_check_event(USER_EVENT_1, USER_EVENT_2, USER_EVENT_3)
```

**Behavior:** Iterates through parameters, returns true if any match the current `event_id`.

**Example:**
```lua
se_cond({
    se_cond_case(
        se_check_event(BUTTON_PRESS, TOUCH_EVENT),
        function()
            se_chain_flow(function()
                se_log("User input detected")
                se_return_pipeline_reset()
            end)
        end
    ),
    se_cond_default(...)
})
```

---

## Field Comparison Predicates

All field comparison predicates follow the pattern:
- Parameter 0: Field reference
- Parameter 1: Comparison value (INT or UINT)

### se_field_eq

**Purpose:** Returns true if field equals value.

**Param layout:** `[FIELD field_ref] [INT/UINT value]`

**Lua DSL:**
```lua
se_field_eq("mode", 1)
se_field_eq("state", 0)
```

**Behavior:** `return (*field == value)`

---

### se_field_ne

**Purpose:** Returns true if field does not equal value.

**Param layout:** `[FIELD field_ref] [INT/UINT value]`

**Lua DSL:**
```lua
se_field_ne("error_code", 0)
```

**Behavior:** `return (*field != value)` (implemented as `!se_field_eq`)

---

### se_field_gt

**Purpose:** Returns true if field is greater than value.

**Param layout:** `[FIELD field_ref] [INT/UINT value]`

**Lua DSL:**
```lua
se_field_gt("temperature", 100)
se_field_gt("counter", 0)
```

**Behavior:** `return (*field > value)`

---

### se_field_ge

**Purpose:** Returns true if field is greater than or equal to value.

**Param layout:** `[FIELD field_ref] [INT/UINT value]`

**Lua DSL:**
```lua
se_field_ge("level", 5)
```

**Behavior:** `return (*field >= value)`

---

### se_field_lt

**Purpose:** Returns true if field is less than value.

**Param layout:** `[FIELD field_ref] [INT/UINT value]`

**Lua DSL:**
```lua
se_field_lt("fuel", 10)
se_field_lt("retries", 3)
```

**Behavior:** `return (*field < value)`

---

### se_field_le

**Purpose:** Returns true if field is less than or equal to value.

**Param layout:** `[FIELD field_ref] [INT/UINT value]`

**Lua DSL:**
```lua
se_field_le("power", 50)
```

**Behavior:** `return (*field <= value)`

---

### se_field_in_range

**Purpose:** Returns true if field is within inclusive range [min, max].

**Param layout:** `[FIELD field_ref] [INT/UINT min] [INT/UINT max]`

**Lua DSL:**
```lua
se_field_in_range("temperature", 20, 80)
se_field_in_range("speed", 0, 100)
```

**Behavior:** `return (*field >= min && *field <= max)`

**Example:**
```lua
se_cond({
    se_cond_case(
        se_field_in_range("temp", 0, 20),
        function() se_chain_flow(function() se_log("Cold") end) end
    ),
    se_cond_case(
        se_field_in_range("temp", 21, 30),
        function() se_chain_flow(function() se_log("Normal") end) end
    ),
    se_cond_case(
        se_field_gt("temp", 30),
        function() se_chain_flow(function() se_log("Hot") end) end
    ),
    se_cond_default(...)
})
```

---

## User-Defined Predicates

### se_pred

**Purpose:** Invoke a user-defined predicate function by name (no parameters).

**Lua DSL:**
```lua
se_pred("SENSOR_READY")
se_pred("MOTOR_RUNNING")
se_pred("CALIBRATED")
```

**C registration:**
```c
static bool my_sensor_ready(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    // Custom logic
    return check_sensor_status();
}

// Register in predicate table
{ s_expr_hash("SENSOR_READY"), (void*)my_sensor_ready },
```

---

### se_pred_with

**Purpose:** Invoke a user-defined predicate function with parameters.

**Lua DSL:**
```lua
se_pred_with("TEST_BIT", function() int(0) end)
se_pred_with("CHECK_THRESHOLD", function() 
    field_ref("value")
    int(100) 
end)
```

**Example with bit testing:**
```lua
-- Test if bit 5 is set
se_pred_with("TEST_BIT", function() int(5) end)

-- C implementation
static bool test_bit(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    ...
) {
    if (param_count < 1) return false;
    uint8_t bit_index = (uint8_t)params[0].int_val;
    uint32_t* flags = get_flags_register();
    return (*flags & (1 << bit_index)) != 0;
}
```

---

## Stateful Counter Predicates

These predicates maintain state across invocations for loop counting. They use **self-initialization** since the engine does not send `SE_EVENT_INIT` to predicates.

### Self-Initialization Pattern

```c
uint8_t system_flag = s_expr_get_system_flags(inst);
if (!(system_flag & S_EXPR_NODE_FLAG_INITIALIZED)) {
    s_expr_set_system_flags(inst, system_flag | S_EXPR_NODE_FLAG_INITIALIZED);
    // One-time initialization here
}
```

This ensures initialization happens exactly once per predicate activation.

---

### se_state_increment_and_test

**Purpose:** Loop counter using node's `user_flags` storage (16-bit).

**Param layout:** `[UINT increment] [UINT limit]`

**Lua DSL:**
```lua
se_state_increment_and_test(increment, limit)
```

**Behavior:**
1. On first invocation (not initialized):
   - Set `S_EXPR_NODE_FLAG_INITIALIZED`
   - Set `user_flags` to 0
2. On every invocation:
   - `count = user_flags + increment`
   - Store `count` in `user_flags`
   - Return `(count <= limit)`

**Storage:** Uses the predicate's own `user_flags` (16-bit) — no blackboard field required.

**Example:**
```lua
-- Loop 5 times
se_while(se_state_increment_and_test(1, 5),
    function() se_log("Iteration") end,
    function() se_tick_delay(10) end
)
```

**Execution timeline:**
```
Invocation 1: init, count=0+1=1, return (1 <= 5) = true
Invocation 2: count=1+1=2, return (2 <= 5) = true
Invocation 3: count=2+1=3, return (3 <= 5) = true
Invocation 4: count=3+1=4, return (4 <= 5) = true
Invocation 5: count=4+1=5, return (5 <= 5) = true
Invocation 6: count=5+1=6, return (6 <= 5) = false → loop exits
```

**Parameters:**

| Parameter | Type | Range | Description |
|-----------|------|-------|-------------|
| `increment` | UINT | 1-65535 | Amount to add each invocation |
| `limit` | UINT | 0-65535 | Maximum value for true result |

---

#se_field_increment_and_test
Purpose: Loop counter using blackboard fields for storage, increment, and limit — all values are runtime-configurable.
Param layout: [FIELD counter_ref] [FIELD increment_ref] [FIELD limit_ref]
Lua DSL:
luase_field_increment_and_test(counter_field, increment_field, limit_field)
Parameters:
ParameterTypeDescriptioncounter_fieldstringBlackboard field name for the counterincrement_fieldstringBlackboard field name for the increment valuelimit_fieldstringBlackboard field name for the limit value
Behavior:

On first invocation (not initialized):

Set S_EXPR_NODE_FLAG_INITIALIZED
Set counter field to 0


On every invocation:

counter += increment
Return (counter <= limit)



Storage: Uses three blackboard fields (ct_int_t) — all visible to other functions and modifiable at runtime.
Example:
luase_chain_flow(function()
    -- Initialize fields
    se_set_field("counter", 0)
    se_set_field("increment", 1)
    se_set_field("limit", 10)
    
    se_while(se_field_increment_and_test("counter", "increment", "limit"),
        function()
            se_log_int("counter = %d", "counter")
        end,
        function()
            se_tick_delay(10)
        end
    )
    
    se_log("Loop complete")
    se_return_pipeline_disable()
end)
```

**Execution timeline (increment=1, limit=5):**
```
Invocation 1: init counter=0, counter=0+1=1, return (1 <= 5) = true
Invocation 2: counter=1+1=2, return (2 <= 5) = true
Invocation 3: counter=2+1=3, return (3 <= 5) = true
Invocation 4: counter=3+1=4, return (4 <= 5) = true
Invocation 5: counter=4+1=5, return (5 <= 5) = true
Invocation 6: counter=5+1=6, return (6 <= 5) = false → loop exits
Runtime Configuration:
Because all three parameters are field references, you can modify loop behavior at runtime:
luase_chain_flow(function()
    se_set_field("counter", 0)
    se_set_field("increment", 1)
    se_set_field("limit", 5)
    
    se_while(se_field_increment_and_test("counter", "increment", "limit"),
        function()
            se_log_int("Iteration %d", "counter")
            
            -- Double the increment after iteration 3
            se_cond({
                se_cond_case(se_field_eq("counter", 3), function()
                    se_set_field("increment", 2)
                    se_log("Doubled increment!")
                end),
                se_cond_default(function() end)
            })
        end
    )
end)
```

**Output:**
```
Iteration 1
Iteration 2
Iteration 3
Doubled increment!
Iteration 5   (jumped from 3+2=5)
Loop exits    (5+2=7 > 5)
Advantages:

Counter, increment, and limit all visible to other functions
Runtime modification of loop parameters
Can implement variable-rate loops
Useful for adaptive algorithms


Comparison: State vs Field Counter
Aspectse_state_increment_and_testse_field_increment_and_testStorageNode's user_flags (16-bit)Blackboard fields (ct_int_t)Counter visibilityPrivate to predicateVisible to all functionsIncrementFixed at compile timeRuntime-configurable fieldLimitFixed at compile timeRuntime-configurable fieldAuto-zeroedYes (on init)Yes (counter field on init)Max value65535Platform-dependent (32/64-bit)Parameters2 values (increment, limit)3 field names (counter, increment, limit)Use caseSimple fixed loopsDynamic loops, shared counters

Stateful Counters Quick Reference
PredicateParametersStorageRuntime Configse_state_increment_and_test(inc, limit)2 values16-bit node stateNose_field_increment_and_test(counter, inc, limit)3 field namesBlackboard fieldsYes
---
## Predicate Quick Reference

### Composite (Boolean Logic)

| Predicate | Description | Builder Required |
|-----------|-------------|------------------|
| `se_pred_and()` | All children true | Yes |
| `se_pred_or()` | Any child true | Yes |
| `se_pred_not()` | Negate first child | Yes |
| `se_pred_nand()` | NOT all children true | Yes |
| `se_pred_nor()` | NO children true | Yes |
| `se_pred_xor()` | Exactly one child true | Yes |

### Constants

| Predicate | Description |
|-----------|-------------|
| `se_true()` | Always true |
| `se_false()` | Always false |

### Events

| Predicate | Description |
|-----------|-------------|
| `se_check_event(id, ...)` | Current event matches any id |

### Field Comparisons

| Predicate | Description |
|-----------|-------------|
| `se_field_eq(field, val)` | field == val |
| `se_field_ne(field, val)` | field != val |
| `se_field_gt(field, val)` | field > val |
| `se_field_ge(field, val)` | field >= val |
| `se_field_lt(field, val)` | field < val |
| `se_field_le(field, val)` | field <= val |
| `se_field_in_range(field, min, max)` | min <= field <= max |

### User-Defined

| Predicate | Description |
|-----------|-------------|
| `se_pred(name)` | Call named predicate (no params) |
| `se_pred_with(name, params_fn)` | Call named predicate with params |

### Stateful Counters

| Predicate | Description |
|-----------|-------------|
| `se_state_increment_and_test(inc, limit)` | Counter in node state |
| `se_field_increment_and_test(field, inc, limit)` | Counter in blackboard |

---

## Usage Examples

### Simple Field Check

```lua
se_while(se_field_lt("retries", 3),
    function() se_log("Attempt") end,
    function() se_tick_delay(10) end,
    function() se_increment_field("retries") end
)
```

### Event-Driven Condition

```lua
se_cond({
    se_cond_case(
        se_check_event(ALARM_EVENT, CRITICAL_EVENT),
        function() se_chain_flow(...) end
    ),
    se_cond_default(...)
})
```

### Complex Boolean Logic

```lua
pred_begin()
    local or1 = se_pred_or()
        -- Condition 1: mode=1 AND temp>100
        local and1 = se_pred_and()
            se_field_eq("mode", 1)
            se_field_gt("temp", 100)
        pred_close(and1)
        -- Condition 2: emergency flag set
        se_field_eq("emergency", 1)
        -- Condition 3: NOT in safe mode
        local not1 = se_pred_not()
            se_field_eq("safe_mode", 1)
        pred_close(not1)
    pred_close(or1)
local alarm_condition = pred_end()

se_trigger_on_change(0, alarm_condition,
    function() se_chain_flow(...) end,  -- Rising edge
    function() se_chain_flow(...) end   -- Falling edge
)
```

### Loop with Visible Counter

```lua
se_function_interface(function()
    se_i_set_field("i", 0)
    
    se_while(se_field_increment_and_test("i", 1, 10),
        function() se_log_int("Processing item %d", "i") end,
        function() process_item() end,
        function() se_tick_delay(5) end
    )
    
    se_log("Processed 10 items")
    se_return_function_terminate()
end)
```

---

## Error Handling

All predicates use `EXCEPTION()` for parameter validation failures:

| Predicate | Error Condition |
|-----------|-----------------|
| Field predicates | Missing parameters, invalid field reference |
| `se_state_increment_and_test` | `param_count < 2`, invalid type |
| `se_field_increment_and_test` | `param_count < 3`, invalid field, invalid type |

Exceptions halt execution (Erlang-style fail-fast).

