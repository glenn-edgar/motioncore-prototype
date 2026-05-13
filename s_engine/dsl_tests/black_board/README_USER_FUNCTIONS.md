# Blackboard User Functions

This file (`black_board_user_functions.c`) implements the oneshot functions required by the `black_board` DSL module. All functions follow a **write-then-verify** pattern for testing blackboard access.

## Overview

The implementation provides 16 oneshot functions organized into four categories:

| Category | Count | Purpose |
|----------|-------|---------|
| Blackboard Access | 6 | Direct struct field access |
| Slot Access | 6 | Generic field_ref access |
| Array Access | 3 | String and array operations |
| Verify Only | 1 | Read-only verification |

## Function Signature

All oneshot functions share the same signature:

```c
void function_name(
    s_expr_tree_instance_t* inst,      // Tree instance
    const s_expr_param_t* params,       // Parameter array
    uint16_t param_count,               // Number of parameters
    s_expr_event_type_t event_type,     // TICK, INIT, TERMINATE, USER
    uint16_t event_id,                  // User-defined event ID
    void* event_data                    // User-defined event data
);
```

## Blackboard Access Functions

Direct access via `inst->blackboard` cast to known record type.

### Parameter Layout
```
params[0] = value to write
params[1] = expected value
```

### Functions

| Function | Field | Type |
|----------|-------|------|
| `bb_write_verify_int32` | `counter` | int32_t |
| `bb_write_verify_uint32` | `flags` | uint32_t |
| `bb_write_verify_float` | `temperature` | float |
| `bb_write_verify_int64` | `timestamp` | int64_t |
| `bb_write_verify_uint64` | `checksum` | uint64_t |
| `bb_write_verify_double` | `precise_value` | double |

### Example Implementation

```c
void bb_write_verify_int32(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    ...
) {
    // Validate
    if (!inst || !inst->blackboard || param_count < 2) {
        report_error(inst, __func__, "invalid args");
        return;
    }
    
    // Cast blackboard to known type
    ScalarDemo* rec = (ScalarDemo*)inst->blackboard;
    
    // Extract parameters
    int32_t write_val = (int32_t)s_expr_param_int(&params[0]);
    int32_t expected = (int32_t)s_expr_param_int(&params[1]);
    
    // Write
    rec->counter = write_val;
    
    // Verify
    if (rec->counter != expected) {
        report_verify_fail(inst, __func__);
    }
}
```

## Slot Access Functions

Generic access via `field_ref()` parameter using `S_EXPR_GET_FIELD` macro.

### Parameter Layout
```
params[0] = field_ref (contains offset and size)
params[1] = value to write
params[2] = expected value
```

### Functions

| Function | Type |
|----------|------|
| `slot_write_verify_int32` | int32_t |
| `slot_write_verify_uint32` | uint32_t |
| `slot_write_verify_float` | float |
| `slot_write_verify_int64` | int64_t |
| `slot_write_verify_uint64` | uint64_t |
| `slot_write_verify_double` | double |

### Example Implementation

```c
void slot_write_verify_float(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    ...
) {
    if (!inst || !inst->blackboard || param_count < 3) {
        report_error(inst, __func__, "invalid args");
        return;
    }
    
    // Generic field access via offset
    float* field = S_EXPR_GET_FIELD(inst, &params[0], float);
    float write_val = (float)s_expr_param_float(&params[1]);
    float expected = (float)s_expr_param_float(&params[2]);
    
    *field = write_val;
    
    if (!float_eq(*field, expected)) {
        report_verify_fail(inst, __func__);
    }
}
```

### S_EXPR_GET_FIELD Macro

```c
#define S_EXPR_GET_FIELD(inst, param, type) \
    ((type*)((uint8_t*)(inst)->blackboard + (param)->field_offset))
```

## Array Access Functions

### slot_write_verify_string

Writes string to `CHAR_ARRAY` field.

**Parameter Layout**:
```
params[0] = field_ref (CHAR_ARRAY)
params[1] = string to write (STR_IDX)
params[2] = expected string (STR_IDX)
```

**Key Points**:
- Uses `s_expr_param_string()` to lookup string from string table
- Respects field size, truncates if necessary
- Null-terminates the string

### slot_write_verify_int32_element

Writes single element to `INT32_ARRAY` field.

**Parameter Layout**:
```
params[0] = field_ref (INT32_ARRAY)
params[1] = index
params[2] = value to write
params[3] = expected value
```

**Key Points**:
- Calculates array length from `field_size / sizeof(int32_t)`
- Performs bounds checking
- Reports error on out-of-bounds access

### slot_write_verify_float32_array

Bulk write to `FLOAT32_ARRAY` field (4 elements).

**Parameter Layout**:
```
params[0] = field_ref (FLOAT32_ARRAY)
params[1..4] = values to write
params[5..8] = expected values
```

## Verify-Only Function

### slot_verify_float

Read-only verification without writing.

**Parameter Layout**:
```
params[0] = field_ref
params[1] = expected value
```

**Use Case**: Verify `use_defaults()` initialization worked correctly.

## Helper Functions

### Error Reporting

```c
static void report_error(s_expr_tree_instance_t* inst, const char* func, const char* msg);
static void report_verify_fail(s_expr_tree_instance_t* inst, const char* func);
```

Reports errors via `inst->module->error_fn` callback. Test harness counts errors via `g_test_errors`.

### Float Comparison

```c
static bool float_eq(float a, float b) {
    return fabsf(a - b) < 1e-6f;
}

static bool double_eq(double a, double b) {
    return fabs(a - b) < 1e-12;
}
```

Epsilon-based comparison to handle floating-point representation differences.

## Function Table Registration

### Data Structures

```c
// Named entries (for hash computation)
static const s_expr_fn_entry_named_t tutorial_oneshot_named[] = {
    { "bb_write_verify_int32", (void*)bb_write_verify_int32 },
    // ...
};

// Hash-indexed entries (filled by init)
static s_expr_fn_entry_t tutorial_oneshot_entries[TUTORIAL_ONESHOT_COUNT];

// Table structure
static s_expr_fn_table_t tutorial_oneshot_table = {
    .entries = tutorial_oneshot_entries,
    .count = TUTORIAL_ONESHOT_COUNT
};
```

### Initialization

```c
void tutorial_init_function_tables(void) {
    s_expr_build_fn_table(
        tutorial_oneshot_named,
        tutorial_oneshot_entries,
        TUTORIAL_ONESHOT_COUNT
    );
}
```

Computes FNV-1a hashes for all function names and populates the hash-indexed table.

### Registration

```c
const s_expr_fn_table_t* tutorial_get_oneshot_table(void) {
    return &tutorial_oneshot_table;
}
```

Returns table for registration with module:

```c
// In main.c
tutorial_init_function_tables();
s_expr_module_register_oneshot(&engine->module, tutorial_get_oneshot_table());
```

## Record Structures

The file includes local copies of record structures. In production, use the generated `black_board_records.h`:

```c
typedef struct {
    int32_t  counter;
    uint32_t flags;
    float    temperature;
    int64_t  timestamp;
    uint64_t checksum;
    double   precise_value;
} ScalarDemo;

typedef struct {
    char    name[32];
    char    short_tag[4];
    int32_t int_values[4];
    float   float_values[4];
} ArrayDemo;

typedef struct {
    float x;
    float y;
    float z;
} Vector3;

typedef struct {
    Vector3 position;
    Vector3 rotation;
    float   scale;
} Transform;
```

## Parameter Accessor Functions

| Function | Returns | Usage |
|----------|---------|-------|
| `s_expr_param_int(p)` | `ct_int_t` | Signed integer value |
| `s_expr_param_uint(p)` | `ct_uint_t` | Unsigned integer value |
| `s_expr_param_float(p)` | `ct_float_t` | Float/double value |
| `s_expr_param_string(def, p)` | `const char*` | String from string table |

## Blackboard vs Slot Access Comparison

| Aspect | Blackboard | Slot |
|--------|------------|------|
| Type safety | Compile-time | Runtime |
| Flexibility | Fixed fields | Any field |
| Performance | Direct access | Offset calculation |
| Code reuse | Per-record type | Generic |
| Use case | Known record, performance critical | Reusable functions |

## DSL Usage

**Blackboard access** (hardcoded field):
```lua
local c1 = o_call("bb_write_verify_int32")
    int(100)    -- value
    int(100)    -- expected
end_call(c1)
```

**Slot access** (generic field):
```lua
local c1 = o_call("slot_write_verify_int32")
    field_ref("counter")   -- which field
    int(42)                -- value
    int(42)                -- expected
end_call(c1)
```

## Adding New Functions

1. Implement function with standard signature
2. Add to `tutorial_oneshot_named[]` array
3. Recompile - hash is computed automatically
4. Use in DSL with `o_call("function_name")`