# Function Dictionary — DSL Helpers and Runtime Functions

## Overview

The function dictionary system provides a mechanism to store named collections of behavior tree nodes in a blackboard PTR64 field, then execute them by name at runtime using FNV-1a hash lookup. This enables data-driven dispatch patterns where the set of available actions is defined at compile time as a dictionary embedded in the binary, and selected at runtime by hash key.

The system has two layers: Lua DSL helpers that emit the dictionary structure at compile time, and C runtime functions that load, resolve, and execute dictionary entries during tree execution.

## Concept

A function dictionary is a compile-time dictionary where each key maps to one or more behavior tree nodes (oneshot calls, main calls, predicate calls, or any combination). At load time, the engine stores a pointer to the dictionary's location in the binary parameter stream into a PTR64 blackboard field. At execution time, the engine looks up a key by its FNV-1a hash, resets the nodes under that key (so they can be called repeatedly), and invokes them in sequence.

```
Compile time (Lua DSL):
    se_load_function_dict("fn_ptr", {
        {"idle",  function() se_log("Idle") end},
        {"run",   function() motor_start() end},
        {"error", function() se_log("Error") end},
    })

Binary (ROM):
    OPEN_CALL SE_LOAD_FUNCTION_DICT
        FIELD fn_ptr
        OPEN_DICT
            KEY "idle"  → [se_log nodes...]  CLOSE_KEY
            KEY "run"   → [motor nodes...]   CLOSE_KEY
            KEY "error" → [se_log nodes...]  CLOSE_KEY
        CLOSE_DICT
    CLOSE_CALL

Runtime (C):
    se_load_function_dict()  → stores dict pointer into blackboard[fn_ptr]
    se_exec_dict_dispatch()  → hash lookup → reset → invoke children
```

## Lua DSL Helpers

### se_load_function_dict(blackboard_field, func_list)

Emits a `SE_LOAD_FUNCTION_DICT` oneshot call that stores a pointer to a compiled dictionary into a PTR64 blackboard field.

**Parameters:**
- `blackboard_field` — name of a PTR64_FIELD in the current record (validated at compile time)
- `func_list` — array of `{"key_name", function() ... end}` pairs

**Compile-time validation:**
- Field must be PTR64 type
- func_list must be a non-empty table
- Each entry must be a two-element table: `{string, function}`
- Key strings must be non-empty
- Duplicate keys are rejected

**Example:**
```lua
se_load_function_dict("action_ptr", {
    {"start", function()
        se_log("Starting")
        motor_start(BRIDGE_FORWARD, "open_speed")
    end},
    {"stop", function()
        motor_kill()
        se_log("Stopped")
    end},
    {"status", function()
        local c = o_call("SEND_STATUS")
        end_call(c)
    end},
})
```

**What it emits:** An `o_call("SE_LOAD_FUNCTION_DICT")` containing a `field_ref` parameter followed by a dictionary structure. Each dictionary key contains the behavior tree nodes produced by calling the user's function closure. The dictionary is embedded directly in the binary parameter stream.

### se_exec_dict_fn(blackboard_field, key_name)

Emits a `SE_EXEC_DICT_DISPATCH` pt_main call that looks up a key in a previously loaded function dictionary and executes the nodes stored under that key.

**Parameters:**
- `blackboard_field` — name of the PTR64_FIELD containing the dictionary pointer (validated at compile time)
- `key_name` — string key to look up (converted to FNV-1a hash at compile time)

**Compile-time validation:**
- Field must be PTR64 type
- key_name must be a string

**Example:**
```lua
-- Load once (typically in an init sequence)
se_load_function_dict("cmd_ptr", {
    {"open",  function() se_set_field("open_request", 1) end},
    {"close", function() se_set_field("close_request", 1) end},
})

-- Execute by name (can be called repeatedly)
se_exec_dict_fn("cmd_ptr", "open")
se_exec_dict_fn("cmd_ptr", "close")
```

**What it emits:** A `pt_m_call("SE_EXEC_DICT_DISPATCH")` with two parameters — a `field_ref` to the blackboard PTR64 field and a `str_hash` of the key name. The hash is computed at compile time; no string comparison occurs at runtime.

**Note:** This is a `pt_m_call` (pointer-main), meaning it allocates a runtime pointer slot. This is required because the function maintains state across the init/tick/terminate lifecycle (it caches the dictionary pointer on init and clears it on terminate).

### se_exec_dict_internal(key_name)

Emits a `SE_EXEC_DICT_INTERNAL` pt_main call that executes a key from the *current dictionary context* — the dictionary that is already active from an enclosing `se_exec_dict_dispatch` call. This is used for nested dispatch within a dictionary entry's own execution.

**Parameters:**
- `key_name` — string key to look up (converted to FNV-1a hash at compile time)

**What it emits:** A `pt_m_call("SE_EXEC_DICT_INTERNAL")` with a single `str_hash` parameter.

## C Runtime Functions

### se_load_function_dict (oneshot)

```c
void se_load_function_dict(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params, uint16_t param_count,
    s_expr_event_type_t event_type, uint16_t event_id, void* event_data
);
```

**Purpose:** Stores a pointer to a dictionary embedded in the binary parameter stream into a PTR64 blackboard field. This is a zero-copy operation — no data is allocated or copied. The pointer points directly into ROM.

**Parameters expected:**
1. `FIELD` — offset of the PTR64 blackboard field
2. `OPEN_DICT` — start of the embedded dictionary

**Operation:**
1. Validates param_count >= 2
2. Validates param[0] is a FIELD opcode
3. Validates param[1] is an OPEN_DICT opcode
4. Computes the blackboard pointer location: `bb + field_offset`
5. Stores the address of the OPEN_DICT parameter into the PTR64 field

**Runtime cost:** A single pointer store. No allocation, no copying, no iteration.

### se_exec_dict_dispatch (pt_main)

```c
s_expr_result_t se_exec_dict_dispatch(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params, uint16_t param_count,
    s_expr_event_type_t event_type, uint16_t event_id, void* event_data
);
```

**Purpose:** Looks up a key in a function dictionary by hash, resets the nodes under that key, and invokes them. This is the primary dispatch function for executing named function dictionary entries.

**Parameters expected:**
1. `FIELD` — offset of the PTR64 blackboard field containing the dictionary pointer
2. `STR_HASH` — FNV-1a hash of the key to execute

**Lifecycle:**

| Event | Action |
|---|---|
| `SE_EVENT_INIT` | Reads the dictionary pointer from the blackboard field, validates it is a dictionary, caches it in `inst->current_dict` |
| `SE_EVENT_TICK` | Hash lookup → scan for content bounds → reset all callable nodes → invoke children in sequence |
| `SE_EVENT_TERMINATE` | Clears `inst->current_dict` to NULL |

**Tick execution detail:**
1. Computes the key hash from param[1]
2. Calls `se_dicth_find()` to locate the key's value in the dictionary (hash-based O(n) scan of dictionary keys)
3. Scans forward from the value to the `CLOSE_KEY` marker to determine the content span
4. Resets all `OPEN_CALL` nodes within the span via `s_expr_reset_recursive_at()` — this is what allows dictionary entries to be executed multiple times
5. Invokes children in order:
   - Oneshot and predicate calls: fire-and-forget (return value ignored)
   - Main calls: check return code; break on anything other than `PIPELINE_CONTINUE` or `PIPELINE_DISABLE`
6. Maps `PIPELINE_DISABLE` to `PIPELINE_CONTINUE` (disable is consumed, not propagated)

**Error handling:** Returns `SE_PIPELINE_TERMINATE` on: missing parameters, NULL blackboard, NULL dictionary pointer, invalid dictionary type, key not found, or empty key content.

### se_exec_dict_internal (pt_main)

```c
s_expr_result_t se_exec_dict_internal(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params, uint16_t param_count,
    s_expr_event_type_t event_type, uint16_t event_id, void* event_data
);
```

**Purpose:** Executes a key from the dictionary context already established by an enclosing `se_exec_dict_dispatch` call. Uses `inst->current_dict` directly without reading from the blackboard.

**Parameters expected:**
1. `STR_HASH` — FNV-1a hash of the key to execute

**Operation:** Identical to `se_exec_dict_dispatch`'s tick phase, except it reads from `inst->current_dict` directly rather than fetching the dictionary pointer from a blackboard field. This means it can only be used inside a context where `se_exec_dict_dispatch` has already initialized the dictionary context.

**Error handling:** Returns `SE_PIPELINE_TERMINATE` if `inst->current_dict` is NULL (no enclosing dispatch context).

## Key Design Decisions

**Zero-copy dictionary storage.** The `se_load_function_dict` oneshot stores a pointer directly into the binary parameter stream in ROM. The dictionary data is never copied to RAM. This means dictionary entries must be position-independent (they are, because the binary format uses relative offsets).

**Hash-based lookup.** Keys are converted to FNV-1a 32-bit hashes at compile time. Runtime lookup compares hashes only — no string comparison ever occurs at runtime. The compile-time DSL validates for duplicate keys.

**Node reset before invocation.** Dictionary entries can be called multiple times because `se_exec_dict_dispatch` and `se_exec_dict_internal` reset all callable nodes within a key's content span before each invocation. Without this reset, nodes with internal state (like `se_tick_delay` or `se_while`) would only execute correctly once.

**pt_main call type.** Both `se_exec_dict_dispatch` and `se_exec_dict_internal` are `pt_m_call` (pointer-main) functions. This means each instance allocates a runtime pointer slot (8 bytes). The slot is needed because `se_exec_dict_dispatch` caches the dictionary pointer across the init/tick/terminate lifecycle, and because both functions manage execution state for nodes that may take multiple ticks to complete.

**Dictionary context via inst->current_dict.** The `se_exec_dict_dispatch` function sets `inst->current_dict` on init and clears it on terminate. This provides a scoped context that `se_exec_dict_internal` can reference without needing its own blackboard field parameter. This enables nested dispatch patterns where an outer dispatch selects a top-level action, and inner dispatches select sub-actions from the same dictionary.

## Usage Patterns

### Simple named action dispatch
```lua
se_load_function_dict("actions_ptr", {
    {"start", function() motor_start(BRIDGE_FORWARD, "speed") end},
    {"stop",  function() motor_kill() end},
})

-- Later, in a state machine case:
se_exec_dict_fn("actions_ptr", "start")
```

### Data-driven state machine
```lua
se_load_function_dict("state_actions_ptr", {
    {"idle",    function() se_return_pipeline_halt() end},
    {"running", function()
        se_log("Running")
        se_tick_delay(100)
        se_return_pipeline_reset()
    end},
    {"error",   function()
        se_log("Error recovery")
        se_return_pipeline_terminate()
    end},
})

-- Dispatch based on a hash stored in a field:
se_exec_dict_fn("state_actions_ptr", "running")
```

### Nested dispatch (internal)
```lua
se_load_function_dict("cmd_ptr", {
    {"init", function()
        se_log("Initializing")
        se_exec_dict_internal("calibrate")  -- calls sibling key
    end},
    {"calibrate", function()
        se_log("Calibrating")
    end},
})
```

## License

MIT License — See repository LICENSE file.
