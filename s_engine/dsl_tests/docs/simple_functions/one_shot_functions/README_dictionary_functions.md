# Oneshot Dictionary Functions

Zero-copy, ROM-resident dictionary access for ChainTree S-expression nodes.

## Overview

The oneshot dictionary subsystem lets a ChainTree node load a JSON-style dictionary into a blackboard `PTR64_FIELD` and then extract typed values from it — all without dynamic allocation. The dictionary data lives directly in the compiled parameter array (ROM/binary), and the blackboard stores only a pointer to it. Extraction functions resolve a path into the dictionary and write the result to a destination blackboard field.

Two keying strategies are supported:

| Strategy | Dictionary loader | Extractor suffix | Path format | Best for |
|---|---|---|---|---|
| **String-keyed** | `se_load_dictionary` + `json()` | `se_dict_extract_*` | Dot-separated string `"system.config.timeout"` | Readability, debugging |
| **Hash-keyed** | `se_load_dictionary_hash` + `json_hash()` | `se_dict_extract_*_h` | Table of segment strings `{"system","config","timeout"}` | Runtime speed on constrained targets |

## Architecture

```
┌─────────────────────────────────────────────┐
│  LuaJIT DSL  (compile-time)                 │
│                                             │
│  se_load_dictionary("cfg", { ... })         │
│  se_dict_extract_int("cfg", "a.b.c", "x")  │
│                                             │
│  ► validates PTR64 field types              │
│  ► emits binary param arrays                │
└──────────────────┬──────────────────────────┘
                   │  compiled params (ROM)
                   ▼
┌─────────────────────────────────────────────┐
│  C Runtime  (execution-time)                │
│                                             │
│  se_load_dictionary()                       │
│    stores dict pointer → blackboard PTR64   │
│                                             │
│  se_dict_extract_int()                      │
│    reads dict pointer from blackboard       │
│    resolves path through dict params        │
│    writes typed value → dest blackboard     │
└─────────────────────────────────────────────┘
```

**Key properties:**

- **Zero-copy** — The dictionary pointer points directly into the param array. No parsing, no heap allocation.
- **ROM-safe** — Dictionary data is `const` and can reside in flash on bare-metal targets.
- **Compile-time validation** — The LuaJIT DSL layer checks that source and destination fields exist and have the correct types before any code is generated.

## LuaJIT DSL Reference

### Loading a Dictionary

```lua
-- String-keyed (use with se_dict_extract_* functions)
se_load_dictionary("config_ptr", {
    system = {
        irrigation = {
            zones = 120,
            master_valve = true
        },
        timeout_ms = 5000
    }
})

-- Hash-keyed (use with se_dict_extract_*_h functions)
se_load_dictionary_hash("config_ptr", {
    system = {
        irrigation = {
            zones = 120,
            master_valve = true
        },
        timeout_ms = 5000
    }
})
```

Both require `config_ptr` to be declared as a `PTR64_FIELD` in the tree's record. The DSL enforces this at compile time.

### String-Path Extraction

All string-path extractors share the same signature:

```lua
se_dict_extract_<type>(dict_field, dot_path, dest_field)
```

| Function | Extracted type | Dest field |
|---|---|---|
| `se_dict_extract_int` | signed integer | sized int field |
| `se_dict_extract_uint` | unsigned integer | sized uint field |
| `se_dict_extract_float` | float / double | float or double field |
| `se_dict_extract_bool` | boolean → 0/1 | int field |
| `se_dict_extract_hash` | hash value | uint32 or uint64 field |

Example:

```lua
se_dict_extract_int("config_ptr", "system.timeout_ms", "timeout")
se_dict_extract_bool("config_ptr", "system.irrigation.master_valve", "mv_enabled")
```

### Hash-Path Extraction

Hash-path extractors accept a table of path segment strings instead of a dot-separated string:

```lua
se_dict_extract_<type>_h(dict_field, path_keys_table, dest_field)
```

The available `_h` variants mirror the string-path set: `se_dict_extract_int_h`, `se_dict_extract_uint_h`, `se_dict_extract_float_h`, `se_dict_extract_bool_h`, `se_dict_extract_hash_h`.

Example:

```lua
se_dict_extract_int_h("config_ptr", {"system", "timeout_ms"}, "timeout")
```

### Storing Sub-Dictionary Pointers

To navigate into a nested structure and store a pointer to the sub-dictionary for later extraction:

```lua
-- String-path variant
se_dict_store_ptr("config_ptr", "system.irrigation", "irrig_ptr")

-- Hash-path variant
se_dict_store_ptr_h("config_ptr", {"system", "irrigation"}, "irrig_ptr")
```

Both source and destination fields must be `PTR64_FIELD`.

## C Runtime Reference

All C runtime functions share the standard ChainTree oneshot signature:

```c
void func(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t*   params,
    uint16_t                param_count,
    s_expr_event_type_t     event_type,
    uint16_t                event_id,
    void*                   event_data
);
```

### Parameter Layouts

**`se_load_dictionary`**

| Index | Type | Description |
|---|---|---|
| 0 | `FIELD` (PTR64) | Destination blackboard field |
| 1 | `OPEN_DICT` | Start of dictionary structure in param array |

**`se_dict_extract_*` (string-path)**

| Index | Type | Description |
|---|---|---|
| 0 | `FIELD` (PTR64) | Dictionary pointer field |
| 1 | `STR_IDX` | Path string (module string table index) |
| 2 | `FIELD` | Destination value field |

**`se_dict_extract_*_h` (hash-path)**

| Index | Type | Description |
|---|---|---|
| 0 | `FIELD` (PTR64) | Dictionary pointer field |
| 1..N−1 | `STR_HASH` | Path segment hashes |
| N | `FIELD` | Destination value field |

**`se_dict_store_ptr` / `se_dict_store_ptr_h`**

Same layouts as the corresponding extract variants, but the destination field is `PTR64` and receives a pointer to the resolved sub-element rather than a scalar value.

### Internal Helpers

| Helper | Purpose |
|---|---|
| `get_dict_from_field` | Read a dictionary pointer back from a PTR64 blackboard field |
| `get_string` | Look up a string in the module string table by `STR_IDX` |
| `write_int_to_field` | Write a sized signed integer (1/2/4/8 bytes) to a blackboard field |
| `write_uint_to_field` | Write a sized unsigned integer to a blackboard field |
| `write_float_to_field` | Write a float (4 bytes) or double (8 bytes) to a blackboard field |
| `write_hash_to_field` | Write a hash value (uint32 or uint64) to a blackboard field |
| `find_dest_field_index` | Scan backwards from end of params to find the last `FIELD` param |
| `collect_path_hashes` | Gather contiguous `STR_HASH` params into a path array (max depth 16) |

## Compile-Time Validation

The LuaJIT DSL layer performs the following checks before emitting any binary params:

- **Field existence** — Every referenced field name must exist in the tree's record definition.
- **PTR64 enforcement** — Dictionary source fields and `store_ptr` destination fields must be `PTR64_FIELD`. A clear error with a `Hint:` line is emitted on mismatch.
- **Path table validation** — Hash-path functions reject empty or non-table `path_keys` arguments.

## Typical Usage Pattern

```lua
-- 1. Define record with a PTR64 field for the dictionary
RECORD("zone_config",
    PTR64_FIELD("cfg", "void"),
    PTR64_FIELD("irrig", "void"),
    UINT32_FIELD("zone_count"),
    UINT32_FIELD("enabled")
)

-- 2. Load dictionary (oneshot, runs once)
se_load_dictionary("cfg", {
    system = {
        irrigation = {
            zones = 120,
            master_valve = true
        }
    }
})

-- 3. Extract values into typed blackboard fields
se_dict_extract_uint("cfg", "system.irrigation.zones", "zone_count")
se_dict_extract_bool("cfg", "system.irrigation.master_valve", "enabled")

-- 4. Or store a sub-dict pointer for repeated access
se_dict_store_ptr("cfg", "system.irrigation", "irrig")
se_dict_extract_uint("irrig", "zones", "zone_count")
```

## Design Rationale

Embedding configuration as dictionary literals in the param array avoids the need for a JSON parser, heap allocator, or file system on the target. This is particularly valuable on 32KB-class microcontrollers where every byte of RAM matters. The two keying strategies let you choose between human-readable paths during development and hash-based lookup for production deployments where ROM space and CPU cycles are tight.
