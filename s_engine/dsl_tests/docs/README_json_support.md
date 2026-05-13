# S-Engine JSON Dictionary System

## Overview

The S-Engine JSON dictionary system provides **zero-copy, compile-time JSON configuration** for embedded and real-time systems. Lua tables in DSL code are compiled directly into binary parameter arrays, enabling configuration data to live in ROM/Flash with no runtime parsing or memory allocation.

## Strategy

The system follows a three-layer architecture:
```
┌─────────────────────────────────────────────────────────────────┐
│  DSL Layer (Lua)                                                │
│  - se_load_dictionary(), se_dict_extract_*() helpers            │
│  - Compile-time validation and code generation                  │
├─────────────────────────────────────────────────────────────────┤
│  Oneshot Functions (C)                                          │
│  - SE_LOAD_DICTIONARY, SE_DICT_EXTRACT_INT, etc.                │
│  - Bridge between DSL and runtime libraries                     │
├─────────────────────────────────────────────────────────────────┤
│  Runtime Libraries (C)                                          │
│  - se_dict_string.h/c - String path navigation                  │
│  - se_dict_hash.h/c - Hash path navigation (faster)             │
│  - Zero-copy value extraction from binary params                │
└─────────────────────────────────────────────────────────────────┘
```

### Core Concept

1. **Load**: Store a physical pointer to the dictionary structure in a PTR64 blackboard field
2. **Extract**: Functions take that pointer and navigate the binary structure to extract values
3. **Store**: Helper functions write extracted values directly into blackboard fields

This approach eliminates:
- Runtime JSON parsing
- Dynamic memory allocation
- String comparisons (when using hash paths)
- Data copying

## File Organization
```
├── s_engine_helpers.lua      # DSL helper functions
├── s_engine_builtins.c       # Oneshot function implementations
├── se_dict_string.h/c        # String-path runtime library
├── se_dict_hash.h/c          # Hash-path runtime library
└── se_dict_extract.h/c       # Extraction oneshot functions
```

## DSL Helper Functions

### Dictionary Loading
```lua
-- Load dictionary with string keys (human-readable, slower lookup)
se_load_dictionary(blackboard_field, lua_table)

-- Load dictionary with hash keys (faster lookup, same data)
se_load_dictionary_hash(blackboard_field, lua_table)
```

### String Path Extraction

Navigate using dot-separated paths like `"system.config.timeout"`:
```lua
se_dict_extract_int(dict_field, path, dest_field)
se_dict_extract_uint(dict_field, path, dest_field)
se_dict_extract_float(dict_field, path, dest_field)
se_dict_extract_bool(dict_field, path, dest_field)
se_dict_extract_hash(dict_field, path, dest_field)
```

### Hash Path Extraction

Navigate using pre-computed key hashes (faster, no string parsing):
```lua
se_dict_extract_int_h(dict_field, {"key1", "key2", "key3"}, dest_field)
se_dict_extract_uint_h(dict_field, {"key1", "key2"}, dest_field)
se_dict_extract_float_h(dict_field, {"key1", "key2"}, dest_field)
se_dict_extract_bool_h(dict_field, {"key1", "key2"}, dest_field)
se_dict_extract_hash_h(dict_field, {"key1", "key2"}, dest_field)
```

### Bulk Extraction

Extract multiple values in one call:
```lua
se_dict_extract_all(dict_field, {
    {path = "system.timeout", dest = "timeout_ms", type = "int"},
    {path = "hardware.voltage", dest = "voltage", type = "float"},
    {path = "network.enabled", dest = "net_flag", type = "bool"}
})

se_dict_extract_all_h(dict_field, {
    {path = {"system", "timeout"}, dest = "timeout_ms", type = "int"},
    {path = {"hardware", "voltage"}, dest = "voltage", type = "float"},
})
```

## C Oneshot Functions

### Registration
```c
// Function table for registration
static const s_expr_fn_entry_named_t se_dict_oneshots[] = {
    {"SE_LOAD_DICTIONARY",      (void*)se_load_dictionary},
    {"SE_DICT_EXTRACT_INT",     (void*)se_dict_extract_int},
    {"SE_DICT_EXTRACT_UINT",    (void*)se_dict_extract_uint},
    {"SE_DICT_EXTRACT_FLOAT",   (void*)se_dict_extract_float},
    {"SE_DICT_EXTRACT_BOOL",    (void*)se_dict_extract_bool},
    {"SE_DICT_EXTRACT_HASH",    (void*)se_dict_extract_hash},
    {"SE_DICT_EXTRACT_INT_H",   (void*)se_dict_extract_int_h},
    {"SE_DICT_EXTRACT_UINT_H",  (void*)se_dict_extract_uint_h},
    {"SE_DICT_EXTRACT_FLOAT_H", (void*)se_dict_extract_float_h},
    {"SE_DICT_EXTRACT_BOOL_H",  (void*)se_dict_extract_bool_h},
    {"SE_DICT_EXTRACT_HASH_H",  (void*)se_dict_extract_hash_h},
};
```

### Parameter Layout

**SE_LOAD_DICTIONARY**:
```
[0] FIELD (PTR64)    - Blackboard field to store dictionary pointer
[1] OPEN_DICT        - Start of dictionary structure in params
```

**SE_DICT_EXTRACT_* (string path)**:
```
[0] FIELD (PTR64)    - Dictionary pointer field
[1] STR_IDX          - Path string (e.g., "system.config.timeout")
[2] FIELD            - Destination field for extracted value
```

**SE_DICT_EXTRACT_*_H (hash path)**:
```
[0] FIELD (PTR64)    - Dictionary pointer field
[1..N-1] STR_HASH    - Path key hashes
[N] FIELD            - Destination field for extracted value
```

## Runtime Libraries

### se_dict_string.h/c (String Path Navigation)

For human-readable paths with runtime string parsing:
```c
// Resolve path to parameter
const s_expr_param_t* se_dicts_resolve(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path,
    se_paths_context_t* ctx
);

// Typed extraction
ct_int_t se_dicts_get_int(dict, module_def, "path.to.value", default_val);
ct_uint_t se_dicts_get_uint(dict, module_def, "path.to.value", default_val);
ct_float_t se_dicts_get_float(dict, module_def, "path.to.value", default_val);
bool se_dicts_get_bool(dict, module_def, "path.to.value", default_val);
s_expr_hash_t se_dicts_get_hash(dict, module_def, "path.to.value", default_val);

// Dictionary iteration
se_dicts_iter_t iter;
se_dicts_iter_init(&iter, dict, module_def);
while (se_dicts_iter_next(&iter, &key_str, &key_hash, &value)) {
    // Process key-value pairs
}
```

### se_dict_hash.h/c (Hash Path Navigation)

For maximum performance with pre-computed hashes:
```c
// Resolve path using hash array
const s_expr_param_t* se_dicth_resolve(
    const s_expr_param_t* dict,
    const s_expr_hash_t* path_hashes,
    uint16_t path_depth,
    se_pathh_context_t* ctx
);

// Typed extraction
ct_int_t se_dicth_get_int(dict, path_hashes, depth, default_val);
ct_uint_t se_dicth_get_uint(dict, path_hashes, depth, default_val);
ct_float_t se_dicth_get_float(dict, path_hashes, depth, default_val);
bool se_dicth_get_bool(dict, path_hashes, depth, default_val);
s_expr_hash_t se_dicth_get_hash(dict, path_hashes, depth, module_def, default_val);

// Convenience macros for C code
ct_int_t val = se_dicth_get_int(dict, SE_PATH_H("system", "timeout"), 0);
```

## Usage Example

### DSL Code
```lua
local mod = start_module("zone_controller")

RECORD("zone_state")
    PTR64_FIELD("config_ptr", "void")
    FIELD("zone_id", "uint32")
    FIELD("timeout_ms", "uint32")
    FIELD("threshold", "float")
    FIELD("enabled", "uint32")
    FIELD("state_hash", "uint32")
END_RECORD()

local zone_config = {
    zone = {
        id = 42,
        timeout = 5000,
        threshold = 75.5,
        enabled = 1,
        state = "idle"
    },
    hardware = {
        gpio_pin = 17,
        adc_channel = 3
    }
}

start_tree("zone_init")
use_record("zone_state")

se_sequence(function()
    -- Load configuration pointer
    se_load_dictionary("config_ptr", zone_config)
    
    -- Extract values using string paths
    se_dict_extract_uint("config_ptr", "zone.id", "zone_id")
    se_dict_extract_uint("config_ptr", "zone.timeout", "timeout_ms")
    se_dict_extract_float("config_ptr", "zone.threshold", "threshold")
    se_dict_extract_bool("config_ptr", "zone.enabled", "enabled")
    se_dict_extract_hash("config_ptr", "zone.state", "state_hash")
    
    se_return_terminate()
end)

end_tree("zone_init")
return end_module(mod)
```

### Alternative with Hash Paths (Faster)
```lua
se_sequence(function()
    se_load_dictionary_hash("config_ptr", zone_config)
    
    se_dict_extract_uint_h("config_ptr", {"zone", "id"}, "zone_id")
    se_dict_extract_uint_h("config_ptr", {"zone", "timeout"}, "timeout_ms")
    se_dict_extract_float_h("config_ptr", {"zone", "threshold"}, "threshold")
    se_dict_extract_bool_h("config_ptr", {"zone", "enabled"}, "enabled")
    se_dict_extract_hash_h("config_ptr", {"zone", "state"}, "state_hash")
    
    se_return_terminate()
end)
```

## Creating Custom JSON Handlers

When the built-in extraction functions don't meet your needs, you can create custom handlers.

### Step 1: Define DSL Helper Function

In `s_engine_helpers.lua`:
```lua
-- Custom handler: extract array of integers into consecutive fields
function se_dict_extract_int_array(dict_field, path, dest_fields)
    validate_field_is_ptr64(dict_field)
    
    local call = o_call("USER_EXTRACT_INT_ARRAY")
        field_ref(dict_field)
        str(path)
        int(#dest_fields)  -- count
        for _, dest in ipairs(dest_fields) do
            field_ref(dest)
        end
    end_call(call)
end
```

### Step 2: Implement C Oneshot Function
```c
#include "se_dict_string.h"

void user_extract_int_array(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (param_count < 4) return;
    
    // Get dictionary pointer from blackboard field
    const s_expr_param_t* dict = get_dict_from_field(inst, &params[0]);
    if (!dict) return;
    
    // Get path string
    const char* path = get_string(inst, &params[1]);
    if (!path) return;
    
    // Get count
    uint16_t count = (uint16_t)params[2].int_val;
    
    // Navigate to array
    const s_expr_module_def_t* mod_def = inst->module ? inst->module->def : NULL;
    const s_expr_param_t* array = se_dicts_get_array(dict, mod_def, path);
    if (!array) return;
    
    // Iterate array and extract values
    se_arrays_iter_t iter;
    se_arrays_iter_init(&iter, array);
    
    const s_expr_param_t* value;
    uint16_t index;
    uint16_t dest_idx = 3;  // First destination field
    
    while (se_arrays_iter_next(&iter, &value, &index) && index < count) {
        if (dest_idx >= param_count) break;
        
        ct_int_t int_val = se_dicts_param_int(value, 0);
        write_int_to_field(inst, &params[dest_idx], int_val);
        dest_idx++;
    }
}
```

### Step 3: Register Function
```c
static const s_expr_fn_entry_named_t user_oneshots[] = {
    {"USER_EXTRACT_INT_ARRAY", (void*)user_extract_int_array},
};
```

### Step 4: Use in DSL
```lua
RECORD("sensor_data")
    PTR64_FIELD("config_ptr", "void")
    FIELD("sensor_0", "int32")
    FIELD("sensor_1", "int32")
    FIELD("sensor_2", "int32")
    FIELD("sensor_3", "int32")
END_RECORD()

local config = {
    sensors = {
        calibration = {10, 20, 30, 40}
    }
}

se_sequence(function()
    se_load_dictionary("config_ptr", config)
    se_dict_extract_int_array("config_ptr", "sensors.calibration", 
        {"sensor_0", "sensor_1", "sensor_2", "sensor_3"})
end)
```

## Performance Considerations

### String Paths vs Hash Paths

| Aspect | String Paths | Hash Paths |
|--------|--------------|------------|
| Lookup | O(n) string compare | O(n) hash compare |
| Path parsing | Runtime strtok | None (pre-computed) |
| Debugging | Human-readable | Requires hash lookup |
| Code size | Smaller | Slightly larger |
| Best for | Development, debugging | Production, hot paths |

### Memory Layout

Dictionaries are stored as flat arrays of `s_expr_param_t` structures:
```
OPEN_DICT [brace_idx=N]
  OPEN_KEY [str_hash=hash("key1")]
    INT [value=42]
  CLOSE
  OPEN_KEY [str_hash=hash("nested")]
    OPEN_DICT [brace_idx=M]
      OPEN_KEY [str_hash=hash("value")]
        FLOAT [value=3.14]
      CLOSE
    CLOSE_DICT
  CLOSE
CLOSE_DICT
```

### Typical Use Pattern
```lua
-- INIT: Load dictionary once
se_load_dictionary("config_ptr", config_table)

-- INIT: Extract all needed values once
se_dict_extract_int("config_ptr", "timeout", "timeout_field")
se_dict_extract_float("config_ptr", "threshold", "threshold_field")

-- TICK: Use blackboard fields directly (no dictionary access)
se_field_gt("sensor_value", "threshold_field")
```

## Binary Format

The JSON dictionary is compiled into binary `s_expr_param_t` tokens:

| Token Type | Description |
|------------|-------------|
| `OPEN_DICT` | Dictionary start, `brace_idx` points to `CLOSE_DICT` |
| `CLOSE_DICT` | Dictionary end |
| `OPEN_KEY` | Key entry, `str_hash` contains FNV-1a hash |
| `CLOSE` | Key entry end |
| `OPEN_ARRAY` | Array start |
| `CLOSE_ARRAY` | Array end |
| `INT` | Signed integer value |
| `UINT` | Unsigned integer value |
| `FLOAT` | Floating-point value |
| `STR_IDX` | String table index |
| `STR_HASH` | Pre-computed string hash |

## Error Handling

Resolution functions return status codes:
```c
typedef enum {
    SE_PATHS_OK = 0,
    SE_PATHS_NOT_FOUND,
    SE_PATHS_TYPE_MISMATCH,
    SE_PATHS_INVALID_INDEX,
    SE_PATHS_INVALID_PATH,
    SE_PATHS_NULL_DICT,
    SE_PATHS_NULL_PATH,
    SE_PATHS_NULL_MODULE,
} se_paths_status_t;
```

Context structures provide detailed error information:
```c
se_paths_context_t ctx;
se_paths_context_init(&ctx);

const s_expr_param_t* value = se_dicts_resolve(dict, mod, path, &ctx);
if (!value) {
    printf("Resolution failed at depth %d: %s\n", 
           ctx.depth, se_paths_status_name(ctx.status));
}
```

## Summary

The S-Engine JSON dictionary system provides:

- **Zero-copy** - Configuration lives in ROM, no runtime allocation
- **Type-safe** - Compile-time field validation in DSL
- **Flexible** - String paths for debugging, hash paths for performance
- **Extensible** - Easy to add custom extraction handlers
- **Embedded-friendly** - No dynamic memory, deterministic performance

