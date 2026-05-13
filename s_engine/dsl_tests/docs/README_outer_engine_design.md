# S-Expression Engine (s_engine) — Outer Engine Design Document

## 1. Overview

The outer engine provides the infrastructure layer that surrounds the inner engine's dispatch core. It handles everything that happens before the first tick and after the last: loading compiled binary modules from ROM or files, resolving function hashes to pointers, allocating tree instances, managing blackboards, and providing typed access to pointer slots, fields, and strings.

Where the inner engine is concerned with *executing* a program — dispatching functions, managing node state, propagating results — the outer engine is concerned with *preparing* a program for execution and providing the runtime services that executing functions depend on.

### Outer Engine Responsibilities

- **Module loading** — parse SEXB binary format from ROM, file, or compile-time definitions
- **Function binding** — resolve FNV-1a hashes in the module definition to C function pointers
- **Tree instance allocation** — create per-execution state with node arrays, pointer slots, and blackboards
- **Blackboard management** — auto-allocate typed records, bind external memory, provide field access by hash or string
- **Pointer slot management** — allocate, free, and track ownership of 64-bit storage slots
- **Parameter navigation** — `s_expr_skip_param()` and brace helpers consumed by the inner engine
- **String and constant access** — runtime access to interned string tables and ROM constant data

---

## 2. Architecture

### 2.1 Component Map

```
┌──────────────────────────────────────────────────────┐
│  s_engine_init.h / .c — High-Level Engine Handle      │
│  (load, register builtins, validate, create trees)    │
├──────────────────────────────────────────────────────┤
│  s_engine_module.h / .c — Module + Tree Management    │
│  ├─ Module init, function registration, validation    │
│  ├─ Tree create / free / bind blackboard              │
│  ├─ Blackboard field access (hash, string, offset)    │
│  ├─ Pointer slot access (alloc, free, get, set)       │
│  ├─ Node state access (flags, state, user_data)       │
│  ├─ String table + pool access                        │
│  └─ Parameter navigation (skip_param, brace helpers)  │
├──────────────────────────────────────────────────────┤
│  s_engine_loader.h / .c — Binary Format Parser        │
│  (SEXB header, directory, zero-copy param arrays)     │
└──────────────────────────────────────────────────────┘
```

### 2.2 Ownership Model

The engine uses explicit ownership throughout:

| Resource | Owner | Freed By |
|----------|-------|----------|
| `s_engine_handle_t` | Caller | Caller (stack or heap) |
| `s_expr_module_t` (function tables) | Engine handle | `s_engine_free()` |
| `s_expr_loaded_module_t` (parsed binary) | Engine handle | `s_engine_free()` → `s_expr_unload_module()` |
| `s_expr_tree_instance_t` | **Caller** | Caller via `s_expr_tree_free()` |
| Blackboard (auto-allocated) | Tree instance | `s_expr_tree_free()` |
| Blackboard (externally bound) | Caller | Caller (tree does not free) |
| Pointer slot (engine-allocated) | Tree instance | `s_expr_tree_free()` or `s_expr_pointer_free()` |
| Pointer slot (external) | Caller | Caller (tree does not free) |

The critical distinction: **trees are created by the engine but owned by the caller**. The caller must free all trees before calling `s_engine_free()`.

---

## 3. Engine Handle

### 3.1 `s_engine_handle_t`

The top-level handle bundles module state and loading context:

```c
typedef struct {
    s_expr_module_t           module;       // Initialized module
    s_expr_loaded_module_t*   loaded;       // Binary loader result (NULL if from def)
    s_expr_allocator_t        alloc;        // Allocator
    void*                     user_ctx;     // External context passed to all trees
    uint8_t                   error_code;   // Last error
} s_engine_handle_t;
```

### 3.2 Initialization Paths

Three ways to initialize the engine, all converging on `s_expr_module_init()`:

| Function | Source | Binary Ownership |
|----------|--------|-----------------|
| `s_engine_init_from_file()` | File path | Engine owns (loaded into RAM, freed on unload) |
| `s_engine_init_from_rom()` | ROM/flash pointer + size | Caller owns (must remain valid) |
| `s_engine_init_from_def()` | Compile-time `s_expr_module_def_t*` | No binary (definition used directly) |

### 3.3 High-Level Loaders

`s_engine_load_from_file()` and `s_engine_load_from_rom()` are convenience functions that perform the full initialization sequence in one call:

1. `memset` the handle
2. Load binary (file or ROM)
3. `s_engine_register_builtins()` — register built-in composites
4. Call user registration functions (array of callbacks)
5. Set debug callback if provided
6. `s_engine_validate()` — verify all function hashes resolved

These print diagnostic output via `printf` and return `false` on failure.

### 3.4 Lifecycle

```
s_engine_init_from_*()          ← Load + parse module definition
    │
    ▼
s_engine_register_builtins()    ← Bind built-in function tables
s_engine_register_main/pred/oneshot()  ← Bind user function tables
    │
    ▼
s_engine_validate()             ← Verify all hashes resolved
    │
    ▼
s_engine_create_tree()          ← Create tree instances (caller owns)
    │
    ▼
[... tick trees via inner engine ...]
    │
    ▼
s_expr_tree_free()              ← Caller frees each tree
    │
    ▼
s_engine_free()                 ← Free module + loaded binary
```

---

## 4. Binary Loader (SEXB Format)

### 4.1 Binary Layout

The SEXB format is a compact binary representation generated by the DSL. It is designed for zero-copy parameter access — param arrays are read directly from the binary without deserialization.

```
┌─────────────────────────┐  0
│  sexb_header_t (32 B)   │  Magic, version, counts, flags
├─────────────────────────┤  32
│  sexb_directory_t (32 B)│  8 section offsets
├─────────────────────────┤  64
│  Tree entries            │  24 bytes each
├─────────────────────────┤
│  Record entries          │  12 bytes each
├─────────────────────────┤
│  Field entries           │  12 bytes each
├─────────────────────────┤
│  String data             │  Length-prefixed, null-terminated, 4-byte aligned
├─────────────────────────┤
│  Constant entries        │  12 bytes each
├─────────────────────────┤
│  Constant data           │  Raw struct data
├─────────────────────────┤
│  Function hashes         │  uint32_t[] — oneshot, then main, then pred
├─────────────────────────┤
│  Parameter arrays        │  s_expr_param_t[] — zero-copy access
└─────────────────────────┘
```

### 4.2 Header

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;           // 0x42584553 "SEXB"
    uint16_t version;         // 0x0502
    uint16_t flags;           // SEXB_FLAG_64BIT, SEXB_FLAG_DEBUG
    uint32_t name_hash;       // Module name (FNV-1a)
    uint16_t tree_count;
    uint16_t record_count;
    uint16_t string_count;
    uint16_t const_count;
    uint16_t oneshot_count;
    uint16_t main_count;
    uint16_t pred_count;
    uint16_t reserved;
    uint32_t total_size;
} sexb_header_t;  // 32 bytes
```

Validation checks: magic, version, 64-bit flag match, total_size ≤ buffer size.

### 4.3 Zero-Copy Param Access

Tree param arrays point directly into the binary data. This means:

- For ROM loading: the binary must remain valid for the lifetime of the module.
- For file loading: the loaded buffer is owned by the `s_expr_loaded_module_t` and freed on unload.
- Param arrays require no deserialization — `s_expr_param_t` is packed identically in the binary and in memory.

### 4.4 String Parsing

Strings in the binary are length-prefixed (uint16_t), null-terminated, and padded to 4-byte alignment. The string table stores `const char*` pointers directly into the binary data (zero-copy).

### 4.5 Loaded Module Structure

```c
typedef struct {
    s_expr_module_def_t def;           // Points into allocated arrays
    s_expr_tree_def_t*    trees;       // Allocated
    s_expr_record_desc_t* records;     // Allocated
    s_expr_field_desc_t*  fields;      // Allocated (one block for all records)
    s_expr_hash_t*        oneshot_hashes;  // Allocated
    s_expr_hash_t*        main_hashes;     // Allocated
    s_expr_hash_t*        pred_hashes;     // Allocated
    const char**          string_table;    // Allocated (pointers into binary)
    const void**          constants;       // Allocated (pointers into binary)
    const uint8_t*        binary_data;     // ROM or owned buffer
    size_t                binary_size;
    bool                  binary_owned;    // If true, free on unload
    s_expr_allocator_t    alloc;
    uint8_t               error_code;
} s_expr_loaded_module_t;
```

`s_expr_unload_module()` frees all allocated arrays and, if `binary_owned`, the binary data buffer itself.

---

## 5. Module System

### 5.1 Module Initialization

`s_expr_module_init()` takes a `s_expr_module_def_t*` (from binary loader or compile-time) and allocates zeroed function pointer arrays sized to the definition's counts:

- `oneshot_fns[oneshot_count]`
- `main_fns[main_count]`
- `pred_fns[pred_count]`

These arrays are indexed by `func_index` from the param's union. The DSL assigns indices at compile time; the runtime fills the arrays by matching hashes.

### 5.2 Function Registration

Registration uses a hash-based table lookup. Each `s_expr_fn_table_t` contains an array of `{hash, fn_ptr}` pairs. Registration iterates the module's required hashes and fills any NULL slots that match:

```
Module needs:  main_hashes[0] = 0xABCD1234  (sequence)
               main_hashes[1] = 0xDEAD5678  (selector)

Table offers:  { 0xABCD1234, &se_sequence }
               { 0xDEAD5678, &se_selector }

After registration:
    main_fns[0] = &se_sequence
    main_fns[1] = &se_selector
```

Up to `MAX_REGISTRY_TABLES = 8` tables can be registered per function type. Tables are stored in static registries (module-level globals) used as a fallback during validation.

### 5.3 Validation

`s_expr_module_validate()` checks every function slot. For any remaining NULL slots, it does a second-pass lookup through all registered tables. If any slot is still NULL after this, validation fails with the hash and index of the first missing function.

---

## 6. Tree Instance Lifecycle

### 6.1 Creation

`s_expr_tree_create()` allocates all per-tree runtime state:

1. **Instance struct** — `s_expr_tree_instance_t`
2. **Node states** — `node_states[func_node_count]`, all set to ACTIVE
3. **Pointer array** — `pointer_array[pointer_count]`, zeroed
4. **Slot flags** — `slot_flags[pointer_count]`, parallel ownership tracking
5. **Blackboard** — auto-allocated if tree definition references a record hash

If the tree definition has a `defaults_index` (pointing to a ROM constant), the blackboard is initialized by `memcpy` from the constant data. Otherwise it is zeroed.

### 6.2 Blackboard Binding

Two modes:

- **Auto-allocated** (`blackboard_owned = true`): Created during `s_expr_tree_create()`, freed by `s_expr_tree_free()`.
- **Externally bound** (`blackboard_owned = false`): Set via `s_expr_tree_bind_blackboard()`. The tree does not free it. Replaces any existing auto-allocated blackboard (which is freed first).

### 6.3 User Context

`s_engine_create_tree()` automatically propagates `handle->user_ctx` to the tree via `s_expr_tree_set_user_ctx()`. Main functions retrieve this at runtime via `s_expr_tree_get_user_ctx()`, providing access to the external system handle (e.g., `cfl_runtime_handle_t*`).

### 6.4 Cleanup

`s_expr_tree_free()` frees in order:
1. Engine-allocated pointer slot contents (where `slot_flags[i] & ALLOCATED`)
2. Parameter stack (if created)
3. Slot flags array
4. Pointer array
5. Node states
6. Blackboard (if owned)
7. Instance struct itself

---

## 7. Blackboard Access

The blackboard provides typed record access for tree functions and external code. Three access patterns are supported:

### 7.1 By Offset (Fastest — Compile-Time)

Used by inner engine functions via FIELD params. The DSL resolves field names to offsets at compile time:

```c
#define S_EXPR_GET_FIELD(inst, param, type) \
    ((type*)((uint8_t*)(inst)->blackboard + (param)->field_offset))
```

Also available as `s_expr_blackboard_get_field_ptr(inst, field_offset)`.

### 7.2 By Hash (Runtime Lookup)

For external code that knows field hashes:

```c
void* s_expr_blackboard_get_field_by_hash(inst, field_hash);
bool  s_expr_blackboard_set_int(inst, field_hash, value);
int32_t s_expr_blackboard_get_int(inst, field_hash, default_value);
```

Performs a linear search through the record's field descriptors.

### 7.3 By String (Convenience)

For external code using field names directly. Computes FNV-1a hash internally, then delegates to the hash-based API:

```c
void* s_expr_blackboard_get_field_by_string(inst, "temperature");
bool  s_expr_blackboard_set_float_by_string(inst, "pressure", 101.3f);
```

---

## 8. Pointer Slot Management

Pointer slots (`s_expr_slot_t`) provide 64-bit persistent storage for `pt_m_call` functions. The outer engine manages their lifecycle and ownership.

### 8.1 Ownership Flags

Each slot has a parallel `slot_flags` byte:

| Flag | Value | Meaning |
|------|-------|---------|
| `NONE` | 0x00 | Empty slot |
| `ALLOCATED` | 0x01 | Engine allocated via `s_expr_pointer_alloc()` — tree frees it |
| `EXTERNAL` | 0x02 | User provided via `s_expr_set_ptr()` — tree does not free |

### 8.2 Access Patterns

**From pt_m_call functions** (inner engine context, `in_pointer_call == true`):

- `s_expr_get_pointer_slot(inst, param_index)` — raw slot access at `pointer_base + param_index`
- `s_expr_pointer_alloc(inst, param_index, size)` — allocate and store pointer
- `s_expr_pointer_free(inst, param_index)` — free allocated pointer
- `s_expr_get/set_u64/i64/f64()` — typed access to slot at `pointer_base`

**From external code** (direct index, no pointer call context required):

- `s_expr_tree_get_slot(inst, index)` — raw slot access by absolute index
- `s_expr_tree_slot_alloc(inst, index, size)` — allocate at absolute index
- `s_expr_tree_slot_set_ptr(inst, index, ptr)` — set external pointer
- `s_expr_tree_slot_free(inst, index)` — free allocated pointer

The external API is used for pre-initializing slots before ticking (e.g., binding driver handles, pre-allocated buffers).

---

## 9. Parameter Navigation

The outer engine provides the parameter skip function used by the inner engine:

```c
static inline uint16_t s_expr_skip_param(const s_expr_param_t* params, uint16_t idx) {
    uint8_t opcode = params[idx].type & S_EXPR_OPCODE_MASK;
    if (opcode == S_EXPR_PARAM_OPEN || S_EXPR_PARAM_OPEN_CALL || ... ) {
        return idx + params[idx].brace_idx + 1;  // skip entire structure
    }
    return idx + 1;  // skip single atom
}
```

This is the foundation for `s_expr_count_params()`, `s_expr_find_param()`, and `s_expr_iterate_params()` in the inner engine.

Additional navigation helpers:

- `s_expr_brace_contents(params, open_idx, &count)` — get contents between open/close braces
- `s_expr_call_func(params, open_idx)` — get function param from OPEN_CALL
- `s_expr_call_args(params, open_idx, &count)` — get argument span from OPEN_CALL

---

## 10. Allocator Interface

All allocation flows through the `s_expr_allocator_t` provided at initialization:

```c
typedef struct {
    s_expr_malloc_fn_t   malloc;     // void* (*)(void* ctx, size_t size)
    s_expr_free_fn_t     free;       // void  (*)(void* ctx, void* ptr)
    void*                ctx;        // Passed to malloc/free
    s_expr_time_fn_t     get_time;   // double (*)(void* ctx) — monotonic seconds
} s_expr_allocator_t;
```

The `ctx` pointer allows the allocator to use arena allocators, tracked heaps, or any custom memory manager. The `get_time` function enables time-based operations (timers, timeouts) without platform coupling.

Time access is available at both engine and tree level:

- `s_engine_get_time(handle)` — from engine handle
- `s_expr_tree_get_time(inst)` — from tree instance during callbacks

---

## 11. Static Registries

The module system uses static (file-scope) registry arrays for function tables:

```c
#define MAX_REGISTRY_TABLES 8

static s_expr_registry_t oneshot_registry;
static s_expr_registry_t main_registry;
static s_expr_registry_t pred_registry;
```

These are zeroed during `s_expr_module_init()` and populated by `s_expr_module_register_*()` calls. They serve as a secondary lookup during validation — if a function slot is still NULL after direct registration, the validator searches all registered tables as a fallback.

**Implication:** Only one module can be active at a time per process, since the registries are global. This is appropriate for embedded systems where a single module runs per MCU. For multi-module server deployments, the registries would need to become per-module.

---

## 12. Error Handling

### 12.1 Error Codes

Two error code namespaces:

**Module errors** (`S_EXPR_ERR_*`): Function resolution failures, allocation failures, invalid state.

**Loader errors** (`SEXB_ERR_*`): Binary format issues — bad magic, version mismatch, 64-bit mismatch, corrupt data, file I/O failures.

### 12.2 Error Reporting

The module stores the first error encountered during validation:

```c
struct s_expr_module {
    uint8_t       error_code;   // S_EXPR_ERR_*
    uint16_t      error_index;  // Index of failing function
    s_expr_hash_t error_hash;   // Hash of missing function
};
```

`s_engine_error_str()` returns a human-readable string by checking both the handle's error code and the module's error code.

### 12.3 EXCEPTION vs Return Codes

The outer engine uses a mixed strategy:

- **Precondition failures** (NULL handle, NULL allocator) → `EXCEPTION()` (fatal) + return error code
- **Operational failures** (file not found, allocation failed) → `EXCEPTION()` + return error code
- **Resolution failures** (missing function hash) → `printf` diagnostic + return error code

The `EXCEPTION()` macro is fatal on embedded targets (watchdog reset). On server/test targets, the return code allows the caller to handle the error. This dual behavior means the outer engine is slightly more forgiving than the inner engine — initialization can fail gracefully, but once execution begins, all errors are fatal.

---

## 13. Memory Budget (Outer Engine Overhead)

For a module with 10 trees, 20 functions (8 oneshot, 10 main, 2 pred), 5 records, 16 strings, 4 constants:

| Component | Size | Notes |
|-----------|------|-------|
| Module struct | ~80 bytes | Function pointer arrays + metadata |
| Oneshot fn table | 32 bytes | 8 × 4-byte pointers (32-bit) |
| Main fn table | 40 bytes | 10 × 4-byte pointers |
| Pred fn table | 8 bytes | 2 × 4-byte pointers |
| Loaded module struct | ~80 bytes | Pointers to allocated arrays |
| Tree def array | 200 bytes | 10 × 20 bytes |
| Record descriptors | 60 bytes | 5 × 12 bytes |
| String table | 64 bytes | 16 × 4-byte pointers |
| **Total module overhead** | **~564 bytes** | Excludes binary data in ROM |

Per-tree overhead is documented in the inner engine design document (~844 bytes for a typical 20-node tree).

---

## 14. Summary

The outer engine transforms a compiled SEXB binary into a live runtime environment ready for the inner engine to tick. It provides the module system that binds function hashes to pointers, the tree factory that allocates per-execution state, and the blackboard and slot management that executing functions depend on. Its ownership model is explicit — the engine owns module state, the caller owns trees — and its allocator interface allows deployment across bare-metal MCUs with arena allocators through to server processes with tracked heaps.

