# SE_SET_FIELD - Blackboard Field Setter Oneshot

## Overview

`se_set_field` is a ONESHOT function that sets a blackboard field to a specified value. It supports integers, unsigned integers, floats, and string hashes - all 32-bit values that can be stored in the same field.

## Variants

| Function | Behavior |
|----------|----------|
| `se_set_field` | Normal oneshot - fires once per tree cycle |
| `se_i_set_field` | IO oneshot - survives tree reset, fires once ever |

Use `se_i_set_field` for initialization that should only happen once, even if the tree resets.

## Parameters

| Parameter | Type | Description |
|-----------|------|-------------|
| `target_field` | field_ref | Blackboard field to set |
| `value` | int/uint/float/string | Value to assign |

### Value Type Handling

The Lua DSL automatically determines the parameter type:

```lua
local function emit_typed_value(value)
    local t = type(value)
    if t == "number" then
        if math.floor(value) == value then
            if value < 0 then
                int(value)   -- Negative integer
            else
                uint(value)  -- Non-negative integer
            end
        else
            flt(value)       -- Floating point
        end
    elseif t == "string" then
        str_hash(value)      -- String becomes hash
    elseif t == "boolean" then
        uint(value and 1 or 0)  -- Boolean as 0/1
    end
end
```

## Behavior

As a ONESHOT function:
- Fires **once** when invoked
- Completes **immediately** (same tick)
- Writes directly to blackboard memory

### C Implementation

```c
static void se_set_field(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_type; (void)event_id; (void)event_data;
    
    if (param_count < 2) return;
    
    int32_t* field_ptr = S_EXPR_GET_FIELD(inst, &params[0], int32_t);
    if (!field_ptr) return;
    *field_ptr = (int32_t)params[1].int_val;
}
```

**Note**: The function treats all values as 32-bit and writes via `int_val`. This works because:
- `int_val`, `uint_val`, `float_val` share the same memory location in the parameter union
- All are 32-bit values
- The blackboard field receives the raw 32 bits

## Lua DSL

### Integer Values

```lua
se_set_field("counter", 42)      -- Positive → uint
se_set_field("offset", -10)      -- Negative → int
se_i_set_field("state", 0)       -- Initialization (survives reset)
```

### Float Values

```lua
se_set_field("temperature", 98.6)
se_set_field("sensor_reading", 3.14159)
```

### String Hashes

```lua
se_set_field("current_mode", "IDLE")     -- Hash of "IDLE"
se_set_field("status", "RUNNING")        -- Hash of "RUNNING"
```

### Boolean Values

```lua
se_set_field("enabled", true)   -- Stored as 1
se_set_field("active", false)   -- Stored as 0
```

## Usage Examples

### State Machine Transitions

```lua
se_case(0, function()
    se_sequence(function()
        se_log("State 0")
        se_tick_delay(10)
        se_set_field("state", 1)  -- Transition to state 1
        se_return_pipeline_disable()
    end)
end)
```

### Event Data Setup

```lua
se_chain_flow(function()
    se_log("Generating event")
    se_tick_delay(20)
    
    -- Set event data before queuing
    se_set_field("event_data_1", 1.1)
    se_set_field("event_data_2", 2.2)
    
    se_queue_event(USER_EVENT_TYPE, USER_EVENT_1, "event_data_1")
    se_queue_event(USER_EVENT_TYPE, USER_EVENT_2, "event_data_2")
    
    se_return_pipeline_reset()
end)
```

### Initialization (IO Variant)

```lua
se_function_interface(function()
    -- These only fire once, even if tree resets
    se_i_set_field("state", 0)
    se_i_set_field("counter", 0)
    se_i_set_field("mode", "IDLE")
    
    se_state_machine("state", cases)
end)
```

## Accessing Event Data via Field Offset

When events are queued with `se_queue_event`, the `event_data` contains the **field offset**. Here's how to access it in a user-defined oneshot:

```c
void display_event_info(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count);
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    // Skip regular ticks - only process user events
    if (event_id == SE_EVENT_TICK) {
        return;
    }
    
    printf("[display_event_info] Event type: %d, Event ID: %d\n", 
           event_type, event_id);
    
    // event_data is the field offset (cast from void*)
    uint16_t offset = (uint16_t)(size_t)event_data;
    printf("[display_event_info] Field offset: %d\n", offset);
    
    // Access the actual value in the blackboard
    float* value = (float*)((uint8_t*)inst->blackboard + offset);
    printf("[display_event_info] Value: %f\n", *value);
}
```

### Key Pattern: Offset to Pointer

```c
// Generic pattern to access event data
uint16_t offset = (uint16_t)(size_t)event_data;
void* field_ptr = (uint8_t*)inst->blackboard + offset;

// Cast to appropriate type
int32_t* as_int = (int32_t*)field_ptr;
float* as_float = (float*)field_ptr;
uint32_t* as_uint = (uint32_t*)field_ptr;
```

## Record Definition

Fields must be defined in the blackboard record:

```lua
RECORD("my_blackboard")
    FIELD("state", "int32")
    FIELD("counter", "int32")
    FIELD("temperature", "float")
    FIELD("mode_hash", "uint32")
    FIELD("event_data_1", "float")
    FIELD("event_data_2", "float")
END_RECORD()
```

## se_set_field vs se_i_set_field

| Aspect | `se_set_field` | `se_i_set_field` |
|--------|----------------|------------------|
| DSL call | `o_call` | `io_call` |
| Flag | Normal oneshot | `SURVIVES_RESET` (0x40) |
| Fires | Once per tree activation | Once ever |
| Use case | Runtime updates | Initialization |

### When Tree Resets

```lua
se_function_interface(function()
    se_i_set_field("init_value", 100)  -- Fires once, never again
    se_set_field("runtime_value", 0)   -- Fires each time tree starts
    
    se_chain_flow(function()
        -- ...
        se_return_pipeline_reset()  -- Tree resets here
    end)
end)
```

After `se_return_pipeline_reset()`:
- `se_i_set_field` does NOT fire again
- `se_set_field` fires again

## Error Handling

| Error | Behavior |
|-------|----------|
| Missing parameters | Returns silently |
| Invalid field reference | Returns silently (NULL check) |
| Wrong field type | Writes raw 32 bits (may cause issues) |

## Comparison with Other Field Functions

| Function | Purpose | Parameters |
|----------|---------|------------|
| `se_set_field` | Set field to constant value | field_ref, value |
| `se_i_set_field` | Set field once (survives reset) | field_ref, value |
| `se_copy_field` | Copy one field to another | dest_field, src_field |
| `field_ref` | Reference field (for other functions) | field_name |

## Implementation Notes

The function uses `S_EXPR_GET_FIELD` macro to get a typed pointer:

```c
int32_t* field_ptr = S_EXPR_GET_FIELD(inst, &params[0], int32_t);
```

This macro:
1. Extracts the field offset from the parameter
2. Adds it to the blackboard base address
3. Returns a typed pointer

All 32-bit value types (int32, uint32, float, hash) can be written through the same `int32_t*` pointer because they occupy the same memory size.
