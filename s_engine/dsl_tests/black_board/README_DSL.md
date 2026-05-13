# Blackboard Tutorial DSL

This DSL file is a comprehensive tutorial demonstrating **record types, blackboard access patterns, and constant initialization** in the S-Expression engine.

## Overview

The `black_board` module covers all the fundamental data access patterns needed to build behavior trees that interact with typed data structures.

## Module Structure

```
black_board
├── Records
│   ├── ScalarDemo      (basic scalar types)
│   ├── ArrayDemo       (array types)
│   ├── Vector3         (simple record)
│   ├── Transform       (nested records)
│   └── LinkedNode      (pointer fields)
├── Constants
│   ├── default_vector
│   ├── default_transform
│   └── default_scalars
└── Trees
    ├── demo_blackboard_access
    ├── demo_slot_access
    ├── demo_array_access
    ├── demo_nested_access
    └── demo_constants
```

## Section 1: Scalar Field Types

### Supported Types

| Type | Size | Alignment | Description |
|------|------|-----------|-------------|
| `int32` | 4 bytes | 4 | Signed 32-bit integer |
| `uint32` | 4 bytes | 4 | Unsigned 32-bit integer |
| `float` | 4 bytes | 4 | 32-bit floating point |
| `int64` | 8 bytes | 8 | Signed 64-bit integer |
| `uint64` | 8 bytes | 8 | Unsigned 64-bit integer |
| `double` | 8 bytes | 8 | 64-bit floating point |

### Why No 8/16-bit Types?

The engine uses 32-bit minimum writes. Smaller types (`int8`, `uint8`, `int16`, `uint16`, `bool`, `char`) would corrupt adjacent fields during write operations.

### Example Record

```lua
RECORD("ScalarDemo")
    FIELD("counter", "int32")
    FIELD("flags", "uint32")
    FIELD("temperature", "float")
    FIELD("timestamp", "int64")
    FIELD("checksum", "uint64")
    FIELD("precise_value", "double")
END_RECORD()
```

## Section 2: Array Types

| Macro | Description | Minimum Size |
|-------|-------------|--------------|
| `CHAR_ARRAY(name, len)` | Character buffer | 4 bytes |
| `INT32_ARRAY(name, len)` | Array of int32 | 4 bytes |
| `FLOAT32_ARRAY(name, len)` | Array of float | 4 bytes |

### Example

```lua
RECORD("ArrayDemo")
    CHAR_ARRAY("name", 32)        -- 32-byte string buffer
    CHAR_ARRAY("short_tag", 4)    -- 4-byte tag
    INT32_ARRAY("int_values", 4)  -- 4 integers
    FLOAT32_ARRAY("float_values", 4)  -- 4 floats
END_RECORD()
```

## Section 3: Nested Records

Records can embed other records for hierarchical data:

```lua
RECORD("Vector3")
    FIELD("x", "float")
    FIELD("y", "float")
    FIELD("z", "float")
END_RECORD()

RECORD("Transform")
    FIELD("position", "Vector3")  -- embedded record
    FIELD("rotation", "Vector3")  -- embedded record
    FIELD("scale", "float")
END_RECORD()
```

Access nested fields with `nested_field_ref()`:

```lua
nested_field_ref("position.x")
nested_field_ref("rotation.y")
```

## Section 4: Pointer Fields

For runtime-only pointer storage (cannot be set from DSL):

```lua
RECORD("LinkedNode")
    FIELD("value", "int32")
    FIELD("pad", "uint32")
    PTR64_FIELD("next", "LinkedNode")  -- pointer to same type
    PTR64_FIELD("data", "void")        -- void pointer
END_RECORD()
```

## Section 5: Constants

Pre-initialized record values for `use_defaults()`:

```lua
CONST("default_vector", "Vector3")
    VALUE("x", 0.0)
    VALUE("y", 1.0)
    VALUE("z", 0.0)
END_CONST()

CONST("default_transform", "Transform")
    VALUE("position.x", 0.0)
    VALUE("position.y", 0.0)
    VALUE("position.z", 0.0)
    VALUE("rotation.x", 0.0)
    VALUE("rotation.y", 0.0)
    VALUE("rotation.z", 0.0)
    VALUE("scale", 1.0)
END_CONST()
```

## Tree Demonstrations

### Tree 1: Blackboard Access (`demo_blackboard_access`)

**Purpose**: Direct struct access via `inst->blackboard` cast.

**Pattern**: C functions cast blackboard to record type and access fields directly.

```lua
start_tree("demo_blackboard_access")
    use_record("ScalarDemo")
    use_defaults("default_scalars")
    
    local c1 = o_call("bb_write_verify_int32")
        int(100)    -- value to write
        int(100)    -- expected value
    end_call(c1)
end_tree("demo_blackboard_access")
```

**C Implementation Pattern**:
```c
void bb_write_verify_int32(...) {
    ScalarDemo* rec = (ScalarDemo*)inst->blackboard;
    rec->counter = s_expr_param_int(&params[0]);
    // verify against params[1]
}
```

### Tree 2: Slot Access (`demo_slot_access`)

**Purpose**: Generic field access via `field_ref()` parameter.

**Pattern**: Functions receive field offset, work with any compatible field.

```lua
start_tree("demo_slot_access")
    use_record("ScalarDemo")
    use_defaults("default_scalars")
    se_sequence(function()
        local c1 = o_call("slot_write_verify_int32")
            field_ref("counter")  -- field offset parameter
            int(42)               -- value to write
            int(42)               -- expected value
        end_call(c1)
    end)
end_tree("demo_slot_access")
```

**C Implementation Pattern**:
```c
void slot_write_verify_int32(...) {
    int32_t* field = S_EXPR_GET_FIELD(inst, &params[0], int32_t);
    *field = s_expr_param_int(&params[1]);
    // verify against params[2]
}
```

### Tree 3: Array Access (`demo_array_access`)

**Purpose**: String and array element manipulation.

**Patterns**:
- String write/verify for `CHAR_ARRAY`
- Element-by-element access for `INT32_ARRAY`
- Bulk write/verify for `FLOAT32_ARRAY`

```lua
-- String access
local c1 = o_call("slot_write_verify_string")
    field_ref("name")
    str("Hello, World!")
    str("Hello, World!")
end_call(c1)

-- Array element access
local c3 = o_call("slot_write_verify_int32_element")
    field_ref("int_values")
    int(0)      -- index
    int(100)    -- value
    int(100)    -- expected
end_call(c3)

-- Bulk array access
local c7 = o_call("slot_write_verify_float32_array")
    field_ref("float_values")
    flt(1.1) flt(2.2) flt(3.3) flt(4.4)  -- write values
    flt(1.1) flt(2.2) flt(3.3) flt(4.4)  -- expected values
end_call(c7)
```

### Tree 4: Nested Access (`demo_nested_access`)

**Purpose**: Access fields within embedded records.

```lua
start_tree("demo_nested_access")
    use_record("Transform")
    use_defaults("default_transform")
    se_sequence(function()
        local c1 = o_call("slot_write_verify_float")
            nested_field_ref("position.x")  -- nested path
            flt(10.0)
            flt(10.0)
        end_call(c1)
    end)
end_tree("demo_nested_access")
```

### Tree 5: Constants (`demo_constants`)

**Purpose**: Verify `use_defaults()` initializes blackboard correctly.

```lua
start_tree("demo_constants")
    use_record("Vector3")
    use_defaults("default_vector")  -- initialized to (0, 1, 0)
    se_sequence(function()
        -- Verify default y=1.0 without writing
        local c2 = o_call("slot_verify_float")
            field_ref("y")
            flt(1.0)    -- expected default
        end_call(c2)
    end)
end_tree("demo_constants")
```

**Important**: When using external blackboards, you must initialize with default values yourself. The engine only applies `use_defaults()` to engine-allocated blackboards.

## User Functions Required

| Function | Params | Description |
|----------|--------|-------------|
| `bb_write_verify_int32` | value, expected | Direct blackboard int32 write/verify |
| `bb_write_verify_uint32` | value, expected | Direct blackboard uint32 write/verify |
| `bb_write_verify_float` | value, expected | Direct blackboard float write/verify |
| `bb_write_verify_int64` | value, expected | Direct blackboard int64 write/verify |
| `bb_write_verify_uint64` | value, expected | Direct blackboard uint64 write/verify |
| `bb_write_verify_double` | value, expected | Direct blackboard double write/verify |
| `slot_write_verify_int32` | field_ref, value, expected | Slot-based int32 write/verify |
| `slot_write_verify_uint32` | field_ref, value, expected | Slot-based uint32 write/verify |
| `slot_write_verify_float` | field_ref, value, expected | Slot-based float write/verify |
| `slot_write_verify_int64` | field_ref, value, expected | Slot-based int64 write/verify |
| `slot_write_verify_uint64` | field_ref, value, expected | Slot-based uint64 write/verify |
| `slot_write_verify_double` | field_ref, value, expected | Slot-based double write/verify |
| `slot_write_verify_string` | field_ref, str, expected | String write/verify |
| `slot_write_verify_int32_element` | field_ref, index, value, expected | Array element write/verify |
| `slot_write_verify_float32_array` | field_ref, v0-v3, e0-e3 | Bulk float array write/verify |
| `slot_verify_float` | field_ref, expected | Read-only float verify |

## Access Pattern Comparison

| Pattern | Use Case | Flexibility | Performance |
|---------|----------|-------------|-------------|
| **Blackboard** | Known record type, direct access | Low | Fastest |
| **Slot** | Generic functions, any field | High | Slight overhead |
| **Nested** | Hierarchical data | Medium | Offset calculation |

## Files Generated

| File | Description |
|------|-------------|
| `black_board.h` | Tree hash definitions |
| `black_board_records.h` | C struct definitions |
| `black_board_bin_32.h` | Binary module (32-bit) |
| `black_board_bin_64.h` | Binary module (64-bit) |
| `black_board_32.bin` | Binary file (32-bit) |
| `black_board_64.bin` | Binary file (64-bit) |

## Usage Example (C)

```c
#include "black_board.h"
#include "black_board_bin_32.h"
#include "black_board_records.h"

// Engine-allocated blackboard (uses defaults)
s_expr_tree_instance_t* tree = s_expr_tree_create_by_hash(
    &engine->module,
    DEMO_CONSTANTS_HASH,
    0
);
s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
s_expr_tree_free(tree);

// External blackboard (shared state)
ScalarDemo_t app_data = {0};
s_expr_tree_instance_t* tree2 = s_expr_tree_create_by_hash(
    &engine->module,
    DEMO_SLOT_ACCESS_HASH,
    0
);
s_expr_tree_bind_blackboard(tree2, &app_data, sizeof(app_data));
s_expr_node_tick(tree2, SE_EVENT_TICK, NULL);
// app_data is now modified
s_expr_tree_free(tree2);
```