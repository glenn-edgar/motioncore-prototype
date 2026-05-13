# S-Engine Predicate System

## Overview

The S-Engine predicate system provides composable boolean logic for behavior trees. Predicates are functions that return `true` or `false` and can be combined using logical operators (AND, OR, NOT, etc.) to build complex conditions.

## Architecture

### Three-Layer Design

```
┌─────────────────────────────────────────────────────────────────────┐
│                         LUA DSL LAYER                               │
│                                                                     │
│   se_pred_and(), se_pred_or(), se_pred_not(), etc.                 │
│   p_call_composite() - marks call as composite predicate            │
│   p_call() - simple predicate call                                  │
└─────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      BINARY PARAMETER STREAM                        │
│                                                                     │
│   [PRED_TAG | hash] [child1...] [child2...] [CLOSE]                │
│   Nested predicates encoded as children in parameter stream         │
└─────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────┐
│                       C RUNTIME LAYER                               │
│                                                                     │
│   se_pred_and(), se_pred_or(), se_pred_not()                       │
│   s_expr_invoke_pred() - recursively invokes child predicates       │
│   s_expr_skip_param() - skips over nested parameter structures      │
└─────────────────────────────────────────────────────────────────────┘
```

## Lua DSL Functions

### Simple Predicates

```lua
-- Call a simple predicate with no children
function se_pred(name)
    local c = p_call(name)
    end_call(c)
end

-- Constant predicates
function se_true()
    local c = p_call("SE_TRUE")
    end_call(c)
end

function se_false()
    local c = p_call("SE_FALSE")
    end_call(c)
end
```

### Composite Predicates

```lua
-- Composite predicates take child predicates as parameters
function se_pred_and()
    return p_call_composite("SE_PRED_AND")
end

function se_pred_or()
    return p_call_composite("SE_PRED_OR")
end

function se_pred_not()
    return p_call_composite("SE_PRED_NOT")  -- single child, inverts result
end

function se_pred_nor()
    return p_call_composite("SE_PRED_NOR")
end

function se_pred_nand()
    return p_call_composite("SE_PRED_NAND")
end

function se_pred_xor()
    return p_call_composite("SE_PRED_XOR")
end
```

### p_call vs p_call_composite

| Function | Usage | Children |
|----------|-------|----------|
| `p_call(name)` | Simple predicate, no nested predicates | Parameters only (int, field_ref, etc.) |
| `p_call_composite(name)` | Composite predicate with children | Can contain nested `p_call()` children |

**p_call_composite behavior:**

```lua
-- In s_expr_dsl.lua
function _G.p_call_composite(func_name)
    in_composite_block = true  -- Sets flag for validation
    return start_call(func_name, "p_call_composite")
end

function _G.end_call(node)
    local top = current_call_stack[#current_call_stack]
    if top.call_type == "p_call_composite" then 
        in_composite_block = false  -- Clears flag
    end
    -- ... rest of end_call
end
```

The `in_composite_block` flag enables validation that certain operations only occur inside composite predicates.

## DSL Usage Patterns

### Simple Predicate

```lua
-- User-defined predicate with parameter
local pred = p_call("TEST_BIT")
    int(0)  -- bit index parameter
end_call(pred)
```

### Composite AND

```lua
-- bit1 AND bit2
local pred = p_call("SE_PRED_AND")
    local p1 = p_call("TEST_BIT") int(1) end_call(p1)
    local p2 = p_call("TEST_BIT") int(2) end_call(p2)
end_call(pred)
```

### Composite OR

```lua
-- bit3 OR bit4
local pred = p_call("SE_PRED_OR")
    local p1 = p_call("TEST_BIT") int(3) end_call(p1)
    local p2 = p_call("TEST_BIT") int(4) end_call(p2)
end_call(pred)
```

### Composite NOT

```lua
-- NOT bit5
local pred = p_call("SE_PRED_NOT")
    local p1 = p_call("TEST_BIT") int(5) end_call(p1)
end_call(pred)
```

### Deeply Nested

```lua
-- (bit0 AND bit1) OR (bit2 AND bit3)
local pred = p_call("SE_PRED_OR")
    local and1 = p_call("SE_PRED_AND")
        local p1 = p_call("TEST_BIT") int(0) end_call(p1)
        local p2 = p_call("TEST_BIT") int(1) end_call(p2)
    end_call(and1)
    local and2 = p_call("SE_PRED_AND")
        local p3 = p_call("TEST_BIT") int(2) end_call(p3)
        local p4 = p_call("TEST_BIT") int(3) end_call(p4)
    end_call(and2)
end_call(pred)
```

### Using Helper Functions

```lua
-- Cleaner syntax with helper functions
local pred = se_pred_and()
    local p1 = p_call("TEST_BIT") int(1) end_call(p1)
    local p2 = p_call("TEST_BIT") int(2) end_call(p2)
end_call(pred)
```

## C Runtime Implementation

### Registration Table

```c
static const s_expr_func_entry_t builtin_predicates[] = {
    { SE_PRED_AND_HASH,  (void*)se_pred_and },
    { SE_PRED_OR_HASH,   (void*)se_pred_or },
    { SE_PRED_NOT_HASH,  (void*)se_pred_not },
    { SE_PRED_NOR_HASH,  (void*)se_pred_nor },
    { SE_PRED_NAND_HASH, (void*)se_pred_nand },
    { SE_PRED_XOR_HASH,  (void*)se_pred_xor },
    { SE_TRUE_HASH,      (void*)se_true },
    { SE_FALSE_HASH,     (void*)se_false },
};
```

### Predicate Function Signature

```c
typedef bool (*s_expr_pred_fn)(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
);
```

### SE_PRED_AND - Short-Circuit Conjunction

Returns `true` only if ALL child predicates return `true`. Short-circuits on first `false`.

```c
static bool se_pred_and(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    for (uint16_t i = 0; i < param_count; ) {
        if (s_expr_param_is_predicate(&params[i])) {
            if (!s_expr_invoke_pred(inst, params, i)) {
                return false;  // Short-circuit: first false → return false
            }
        }
        i = s_expr_skip_param(params, i);  // Skip to next parameter
    }
    return true;  // All children were true (or no children)
}
```

**Truth Table:**

| A | B | A AND B |
|---|---|---------|
| F | F | F |
| F | T | F |
| T | F | F |
| T | T | T |

### SE_PRED_OR - Short-Circuit Disjunction

Returns `true` if ANY child predicate returns `true`. Short-circuits on first `true`.

```c
static bool se_pred_or(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    for (uint16_t i = 0; i < param_count; ) {
        if (s_expr_param_is_predicate(&params[i])) {
            if (s_expr_invoke_pred(inst, params, i)) {
                return true;  // Short-circuit: first true → return true
            }
        }
        i = s_expr_skip_param(params, i);
    }
    return false;  // No children were true (or no children)
}
```

**Truth Table:**

| A | B | A OR B |
|---|---|--------|
| F | F | F |
| F | T | T |
| T | F | T |
| T | T | T |

### SE_PRED_NOT - Logical Negation

Inverts the result of its single child predicate.

```c
static bool se_pred_not(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    for (uint16_t i = 0; i < param_count; ) {
        if (s_expr_param_is_predicate(&params[i])) {
            return !s_expr_invoke_pred(inst, params, i);  // Invert first child
        }
        i = s_expr_skip_param(params, i);
    }
    return true;  // No child → return true (identity for NOT)
}
```

**Truth Table:**

| A | NOT A |
|---|-------|
| F | T |
| T | F |

### SE_PRED_NOR - Negated OR

Returns `true` only if ALL children are `false`. Equivalent to `NOT(A OR B)`.

```c
static bool se_pred_nor(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    return !se_pred_or(inst, params, param_count, event_type, event_id, event_data);
}
```

**Truth Table:**

| A | B | A NOR B |
|---|---|---------|
| F | F | T |
| F | T | F |
| T | F | F |
| T | T | F |

### SE_PRED_NAND - Negated AND

Returns `true` if ANY child is `false`. Equivalent to `NOT(A AND B)`.

```c
static bool se_pred_nand(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    return !se_pred_and(inst, params, param_count, event_type, event_id, event_data);
}
```

**Truth Table:**

| A | B | A NAND B |
|---|---|----------|
| F | F | T |
| F | T | T |
| T | F | T |
| T | T | F |

### SE_PRED_XOR - Exclusive OR

Returns `true` if EXACTLY ONE child is `true`.

```c
static bool se_pred_xor(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    int true_count = 0;
    
    for (uint16_t i = 0; i < param_count; ) {
        if (s_expr_param_is_predicate(&params[i])) {
            if (s_expr_invoke_pred(inst, params, i)) {
                true_count++;
                if (true_count > 1) return false;  // Early exit: more than one true
            }
        }
        i = s_expr_skip_param(params, i);
    }
    
    return (true_count == 1);  // Exactly one true
}
```

**Truth Table (2 inputs):**

| A | B | A XOR B |
|---|---|---------|
| F | F | F |
| F | T | T |
| T | F | T |
| T | T | F |

**Multi-input behavior:** Returns `true` if exactly one child is `true`, regardless of how many children.

### SE_TRUE / SE_FALSE - Constants

```c
static bool se_true(...) {
    return true;
}

static bool se_false(...) {
    return false;
}
```

## Key Runtime Functions

### s_expr_param_is_predicate

Checks if a parameter is a predicate call marker:

```c
bool s_expr_param_is_predicate(const s_expr_param_t* param) {
    return param->type == S_EXPR_PARAM_PRED;
}
```

### s_expr_invoke_pred

Recursively invokes a child predicate:

```c
bool s_expr_invoke_pred(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t index
) {
    // 1. Get predicate function hash from params[index]
    // 2. Look up function pointer in registration table
    // 3. Calculate child parameter range
    // 4. Call predicate function with child parameters
    // 5. Return boolean result
}
```

### s_expr_skip_param

Skips over a parameter (including nested structures):

```c
uint16_t s_expr_skip_param(const s_expr_param_t* params, uint16_t index) {
    // If params[index] is a nested structure (PRED, OPEN, etc.),
    // skip over all children until finding the matching CLOSE
    // Otherwise, skip single parameter
}
```

## Binary Parameter Stream

### Encoding Example

```lua
-- SE_PRED_AND(TEST_BIT(1), TEST_BIT(2))
local pred = p_call("SE_PRED_AND")
    local p1 = p_call("TEST_BIT") int(1) end_call(p1)
    local p2 = p_call("TEST_BIT") int(2) end_call(p2)
end_call(pred)
```

**Binary stream:**

```
[PRED | SE_PRED_AND_HASH]     -- SE_PRED_AND opens
  [PRED | TEST_BIT_HASH]      -- First child predicate
    [INT | 1]                 -- Parameter: bit index 1
  [CLOSE]                     -- End TEST_BIT
  [PRED | TEST_BIT_HASH]      -- Second child predicate
    [INT | 2]                 -- Parameter: bit index 2
  [CLOSE]                     -- End TEST_BIT
[CLOSE]                       -- End SE_PRED_AND
```

### Parameter Types

```c
S_EXPR_PARAM_PRED   = 0x0A,  // Predicate call marker
S_EXPR_PARAM_OPEN   = 0x05,  // Generic open bracket
S_EXPR_PARAM_CLOSE  = 0x06,  // Generic close bracket
S_EXPR_PARAM_INT    = 0x00,  // Integer parameter
```

## Usage with se_trigger_on_change

The predicate system is commonly used with `se_trigger_on_change` for edge detection:

```lua
se_trigger_on_change(0,  -- initial_state
    function()
        -- Predicate: (bit1 AND bit2)
        local pred = p_call("SE_PRED_AND")
            local p1 = p_call("TEST_BIT") int(1) end_call(p1)
            local p2 = p_call("TEST_BIT") int(2) end_call(p2)
        end_call(pred)
    end,
    function()
        -- Rising edge action (predicate went true)
        se_chain_flow(function()
            se_log("Both bits are now set!")
            se_return_continue()
        end)
    end,
    function()
        -- Falling edge action (predicate went false)
        se_chain_flow(function()
            se_log("At least one bit cleared!")
            se_return_continue()
        end)
    end
)
```

## User-Defined Predicates

Users can define custom predicates:

```c
bool test_bit(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    if (param_count < 1) return false;
    
    uint32_t* bitmap = (uint32_t*)inst->user_ctx;
    if (!bitmap) return false;
    
    int32_t bit_index = params[0].int_val;
    return (*bitmap & (1U << bit_index)) != 0;
}
```

Register in user functions:

```c
static const s_expr_func_entry_t user_predicates[] = {
    { TEST_BIT_HASH, (void*)test_bit },
};
```

## Predicate Summary Table

| Predicate | Children | Short-Circuit | Result |
|-----------|----------|---------------|--------|
| SE_PRED_AND | 0..N | Yes (first false) | All true → true |
| SE_PRED_OR | 0..N | Yes (first true) | Any true → true |
| SE_PRED_NOT | 1 | N/A | Inverts child |
| SE_PRED_NOR | 0..N | Yes (first true) | All false → true |
| SE_PRED_NAND | 0..N | Yes (first false) | Any false → true |
| SE_PRED_XOR | 0..N | Partial (>1 true) | Exactly one true → true |
| SE_TRUE | 0 | N/A | Always true |
| SE_FALSE | 0 | N/A | Always false |

## Design Principles

1. **Composability** - Predicates can be nested arbitrarily deep
2. **Short-Circuit Evaluation** - AND/OR stop early for efficiency
3. **Consistent Interface** - All predicates share the same function signature
4. **Hash-Based Lookup** - FNV-1a hashes enable fast function dispatch
5. **Stateless** - Predicates don't maintain state between calls
6. **User-Extensible** - Custom predicates use the same mechanism as builtins

