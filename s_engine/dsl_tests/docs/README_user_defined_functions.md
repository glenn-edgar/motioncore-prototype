# User-Defined External Functions

## Overview

The S-Expression DSL supports **external functions** that are not built into the system. When the DSL encounters a function call that isn't a registered builtin, it treats it as an external function and generates the necessary scaffolding for user implementation.

This allows extending the engine with application-specific functionality like:
- Hardware control
- Custom I/O operations
- Application-specific logic
- Integration with external systems

## How It Works

### 1. Declare in Lua DSL

Use `o_call` (oneshot), `m_call` (main), or `p_call` (predicate) with any function name:

```lua
-- Custom oneshot functions
local o0 = o_call("CFL_DISABLE_CHILDREN")
end_call(o0)

local o1 = o_call("CFL_ENABLE_CHILD")
    int(2)  -- Parameter: child index
end_call(o1)
```

### 2. DSL Generates Scaffolding

The DSL compiler generates three files:

#### `xxx_user_functions.h` - Function Prototypes

```c
#ifndef STATE_MACHINE_TEST_USER_FUNCTIONS_H
#define STATE_MACHINE_TEST_USER_FUNCTIONS_H

#include "s_engine_types.h"

// Oneshot function prototypes
void cfl_disable_children(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
);

void cfl_enable_child(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
);

#endif
```

#### `xxx_user_registration.c` - Registration Tables

```c
#include "state_machine_test.h"
#include "state_machine_test_user_functions.h"
#include "s_engine_module.h"

// Oneshot function entries (hash -> function pointer)
static s_expr_fn_entry_t state_machine_test_oneshot_entries[] = {
    { 0x5839B05B, (void*)cfl_disable_children },
    { 0xD42E3453, (void*)cfl_enable_child },
};

static const s_expr_fn_table_t state_machine_test_oneshot_table = {
    .entries = state_machine_test_oneshot_entries,
    .count = 2
};

// Register all user functions with module
void state_machine_test_register_all(s_expr_module_t* module) {
    s_expr_module_register_oneshot(module, &state_machine_test_oneshot_table);
}
```

### 3. User Implements Functions

Create a source file (e.g., `user_functions.c`) with implementations:

```c
#include "state_machine_test_user_functions.h"
#include "s_engine_exception.h"
#include <stdio.h>

void cfl_disable_children(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)inst; (void)params; (void)param_count;
    (void)event_id; (void)event_data;
    
    // Oneshots typically only act on INIT (first invocation)
    if (event_type != SE_EVENT_INIT) {
        return;
    }
    
    printf("cfl_disable_children\n");
    // TODO: Actual implementation
}

void cfl_enable_child(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)inst; (void)event_id; (void)event_data;
    
    if (event_type != SE_EVENT_INIT) {
        return;
    }
    
    // Validate parameters
    if (param_count < 1) {
        EXCEPTION("cfl_enable_child: need at least one parameter");
        return;
    }
    
    if (params[0].type != S_EXPR_PARAM_INT && 
        params[0].type != S_EXPR_PARAM_UINT) {
        EXCEPTION("cfl_enable_child: first parameter must be integer");
        return;
    }
    
    uint16_t child_index = params[0].int_val;
    printf("cfl_enable_child: enabling child %d\n", child_index);
    // TODO: Actual implementation
}
```

### 4. Register at Engine Load

Pass the registration callback when loading the engine:

```c
// Registration callback array
s_engine_user_register_fn user_fns[] = {
    state_machine_test_register_all
};

// Load engine with user functions
s_engine_load_from_rom(
    &engine,
    &alloc,
    rom_data,
    rom_size,
    debug_callback,
    1,          // user_fn_count
    user_fns    // user function registration callbacks
);
```

## Function Types

### Oneshot Functions

Declared with `o_call()`, return `void`:

```lua
local o = o_call("MY_ONESHOT")
    int(42)
    str_ptr("hello")
end_call(o)
```

```c
void my_oneshot(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
);
```

### Main Functions

Declared with `m_call()`, return `s_expr_result_t`:

```lua
local m = m_call("MY_MAIN")
    int(100)
end_call(m)
```

```c
s_expr_result_t my_main(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
);
```

### Predicate Functions

Declared with `p_call()`, return `bool`:

```lua
local p = p_call("MY_PREDICATE")
    field_ref("value")
end_call(p)
```

```c
bool my_predicate(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
);
```

## Function Name Convention

- **Lua DSL**: Use `UPPER_SNAKE_CASE` (e.g., `CFL_ENABLE_CHILD`)
- **C function**: Use `lower_snake_case` (e.g., `cfl_enable_child`)

The DSL automatically converts the name and generates a hash for runtime lookup.

## Accessing Parameters

Parameters are passed as an array of `s_expr_param_t`:

```c
void my_function(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    ...
) {
    // Check parameter count
    if (param_count < 2) {
        EXCEPTION("my_function: need 2 parameters");
        return;
    }
    
    // Access integer parameter
    if (params[0].type == S_EXPR_PARAM_INT) {
        int32_t value = params[0].int_val;
    }
    
    // Access string parameter
    const char* str = s_expr_get_string(inst, &params[1]);
    
    // Access field reference
    int32_t* field_ptr = S_EXPR_GET_FIELD(inst, &params[2], int32_t);
}
```

## Parameter Types

| Lua DSL | C Type | Access |
|---------|--------|--------|
| `int(n)` | `S_EXPR_PARAM_INT` | `params[i].int_val` |
| `uint(n)` | `S_EXPR_PARAM_UINT` | `params[i].uint_val` |
| `flt(n)` | `S_EXPR_PARAM_FLOAT` | `params[i].float_val` |
| `str_ptr("s")` | `S_EXPR_PARAM_STR_IDX` | `s_expr_get_string(inst, &params[i])` |
| `field_ref("f")` | `S_EXPR_PARAM_FIELD` | `S_EXPR_GET_FIELD(inst, &params[i], type)` |

## Event Handling

External functions receive lifecycle events:

```c
void my_oneshot(..., s_expr_event_type_t event_type, ...) {
    switch (event_type) {
        case SE_EVENT_INIT:
            // First invocation - do the work
            break;
            
        case SE_EVENT_TERMINATE:
            // Cleanup (rare for oneshots)
            break;
            
        case SE_EVENT_TICK:
            // Only for MAIN functions
            break;
    }
}
```

### Oneshot Event Pattern

Oneshots typically only act on `SE_EVENT_INIT`:

```c
void my_oneshot(...) {
    if (event_type != SE_EVENT_INIT) {
        return;  // Ignore other events
    }
    
    // Do the work
}
```

### Main Event Pattern

Main functions handle all three events:

```c
s_expr_result_t my_main(...) {
    if (event_type == SE_EVENT_INIT) {
        // Initialize state
        return SE_PIPELINE_CONTINUE;
    }
    
    if (event_type == SE_EVENT_TERMINATE) {
        // Cleanup
        return SE_PIPELINE_CONTINUE;
    }
    
    // SE_EVENT_TICK - do the work
    // Return appropriate result code
    return SE_PIPELINE_DISABLE;  // Complete
}
```

## File Organization

```
project/
├── state_machine.lua              # DSL source
├── generated/
│   ├── state_machine_test.h       # Tree hashes, constants
│   ├── state_machine_test_bin_32.h  # Binary ROM data
│   ├── state_machine_test_records.h # Record definitions
│   ├── state_machine_test_user_functions.h    # Function prototypes
│   └── state_machine_test_user_registration.c # Registration tables
└── src/
    ├── user_functions.c           # User implementations
    └── main.c                     # Test harness
```

## Example: Chain Flow Control Functions

The state machine test uses two external oneshots for chain flow control:

### CFL_DISABLE_CHILDREN

Disables all children in the current chain flow context:

```lua
local o0 = o_call("CFL_DISABLE_CHILDREN")
end_call(o0)
```

### CFL_ENABLE_CHILD

Enables a specific child by index:

```lua
local o1 = o_call("CFL_ENABLE_CHILD")
    int(0)  -- Enable child at index 0
end_call(o1)
```

These would typically interact with a chain flow composite to control which branches are active.

## Summary

1. **Declare** external functions in Lua DSL using `o_call`, `m_call`, or `p_call`
2. **Generate** scaffolding with DSL compiler
3. **Implement** functions in C matching the generated prototypes
4. **Register** via callback when loading the engine

This pattern allows clean separation between:
- **DSL/Tree definition** (Lua)
- **Engine core** (builtin functions)
- **Application logic** (user-defined functions)