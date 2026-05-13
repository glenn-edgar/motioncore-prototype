```markdown
# S_Engine Return Code Tests

## Overview

This test suite validates the S_Engine's return code system by defining minimal trees that return each possible result code. The tests verify that return codes propagate correctly from DSL source through compilation to runtime execution.

## Return Code Architecture

The S_Engine uses a symmetric 3-tier return code system with 6 codes per tier:

| Code | Application (0-5) | Function (6-11) | Pipeline (12-17) |
|------|-------------------|-----------------|------------------|
| CONTINUE | 0 | 6 | 12 |
| HALT | 1 | 7 | 13 |
| TERMINATE | 2 | 8 | 14 |
| RESET | 3 | 9 | 15 |
| DISABLE | 4 | 10 | 16 |
| SKIP_CONTINUE | 5 | 11 | 17 |

This design enables simple layer/code extraction: `base_code = result % 6`, `layer = result / 6`.

## Test Structure

### DSL Source (Lua)
```lua
local M = require("s_expr_dsl")
local mod = start_module("return_tests")
use_32bit()
set_debug(true)

-- Application result codes (0-5)
start_tree("return_continue_test")
    se_return_continue()
end_tree("return_continue_test")

start_tree("return_halt_test")
    se_return_halt()
end_tree("return_halt_test")

start_tree("return_terminate_test")
    se_return_terminate()
end_tree("return_terminate_test")

start_tree("return_reset_test")
    se_return_reset()
end_tree("return_reset_test")

start_tree("return_disable_test")
    se_return_disable()
end_tree("return_disable_test")

start_tree("return_skip_continue_test")
    se_return_skip_continue()
end_tree("return_skip_continue_test")

-- Function result codes (6-11)
start_tree("return_function_continue_test")
    se_return_function_continue()
end_tree("return_function_continue_test")

-- ... additional trees for each return code ...

local result = end_module(mod)
```

Each tree contains a single node that immediately returns a specific result code. This isolates the return code mechanism from any composite node logic.

### Generated Outputs

The DSL compiler produces:

| File | Purpose |
|------|---------|
| `return_tests_32.bin` | Binary module for file-based loading |
| `return_tests_bin_32.h` | C header with embedded binary (ROM loading) |
| `return_tests.h` | Tree hash definitions for lookup |
| `return_tests_user_functions.h` | User function stubs (if any) |

### Tree Hash Definitions
```c
// From return_tests.h

// Application result code trees
#define RETURN_CONTINUE_TEST_HASH                0x...
#define RETURN_HALT_TEST_HASH                    0x...
#define RETURN_TERMINATE_TEST_HASH               0x...
#define RETURN_RESET_TEST_HASH                   0x...
#define RETURN_DISABLE_TEST_HASH                 0x...
#define RETURN_SKIP_CONTINUE_TEST_HASH           0x...

// Function result code trees
#define RETURN_FUNCTION_CONTINUE_TEST_HASH       0x...
#define RETURN_FUNCTION_HALT_TEST_HASH           0x...
#define RETURN_FUNCTION_TERMINATE_TEST_HASH      0x...
#define RETURN_FUNCTION_RESET_TEST_HASH          0x...
#define RETURN_FUNCTION_DISABLE_TEST_HASH        0x...
#define RETURN_FUNCTION_SKIP_CONTINUE_TEST_HASH  0x...

// Pipeline result code trees
#define RETURN_PIPELINE_CONTINUE_TEST_HASH       0x...
#define RETURN_PIPELINE_HALT_TEST_HASH           0x...
#define RETURN_PIPELINE_TERMINATE_TEST_HASH      0x...
#define RETURN_PIPELINE_RESET_TEST_HASH          0x...
#define RETURN_PIPELINE_DISABLE_TEST_HASH        0x...
#define RETURN_PIPELINE_SKIP_CONTINUE_TEST_HASH  0x...
```

Trees are identified by FNV-1a hashes of their names, enabling O(1) lookup without string comparison.

---

## Runtime Interface

### Engine Initialization

The S_Engine provides two general-purpose loading functions:

**Function Signatures**
```c
typedef void (*s_engine_user_register_fn)(s_engine_handle_t* engine);
typedef void (*s_engine_debug_callback_fn)(s_expr_tree_instance_t* inst, const char* msg);

bool s_engine_load_from_file(
    s_engine_handle_t* engine,
    s_expr_allocator_t* alloc,
    const char* filepath,
    s_engine_debug_callback_fn debug_cb,
    size_t user_fn_count,
    s_engine_user_register_fn* user_fns
);

bool s_engine_load_from_rom(
    s_engine_handle_t* engine,
    s_expr_allocator_t* alloc,
    const uint8_t* binary_data,
    size_t binary_size,
    s_engine_debug_callback_fn debug_cb,
    size_t user_fn_count,
    s_engine_user_register_fn* user_fns
);
```

**ROM Loading (Embedded Binary)**
```c
#include "return_tests_bin_32.h"

bool result = s_engine_load_from_rom(
    &engine,
    &alloc,
    return_tests_module_bin_32,
    RETURN_TESTS_MODULE_BIN_32_SIZE,
    debug_callback,
    0,      // No user functions
    NULL
);
```

Used for embedded systems where the module is compiled into flash.

**File Loading**
```c
bool result = s_engine_load_from_file(
    &engine,
    &alloc,
    "return_tests_32.bin",
    debug_callback,
    0,      // No user functions
    NULL
);
```

Used for development or systems with filesystem access.

**With User Functions**
```c
s_engine_user_register_fn user_fns[] = {
    register_sensor_functions,
    register_motor_functions,
    register_protocol_functions
};

bool result = s_engine_load_from_file(
    &engine,
    &alloc,
    "robot_controller.bin",
    debug_callback,
    3,
    user_fns
);
```

### Allocator Interface

The S_Engine uses a pluggable allocator for all dynamic memory:
```c
typedef struct {
    void* (*malloc)(void* ctx, size_t size);
    void  (*free)(void* ctx, void* ptr);
    void* ctx;                        // User context passed to malloc/free
    double (*get_time)(void* ctx);    // Monotonic time source
} s_expr_allocator_t;
```

This allows integration with custom memory managers, arena allocators, or RTOS heap implementations.
```c
// Example: Simple malloc wrapper
static void* simple_malloc(void* ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void simple_free(void* ctx, void* ptr) {
    (void)ctx;
    free(ptr);
}

static double linux_get_time(void* ctx) {
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

s_expr_allocator_t alloc = {
    .malloc   = simple_malloc,
    .free     = simple_free,
    .ctx      = NULL,
    .get_time = linux_get_time
};
```

### Tree Instantiation

Create a tree instance by hash:
```c
s_expr_tree_instance_t* tree = s_expr_tree_create_by_hash(
    &engine.module,
    RETURN_CONTINUE_TEST_HASH,
    0                              // Flags (reserved)
);

if (!tree) {
    printf("Failed to create tree\n");
    return;
}
```

Each tree instance has its own:
- Node state array (`node_states[]`)
- Pointer array (if any nodes require it)
- Execution context

Multiple instances of the same tree definition can exist simultaneously.

### Tree Execution

Tick the tree with an event:
```c
s_expr_result_t result = s_expr_node_tick(
    tree,
    SE_EVENT_TICK,                 // Event type
    NULL                           // Event data (optional)
);
```

The tick function:
1. Delivers the event to the root node
2. Executes the tree according to its structure
3. Returns the propagated result code

### Event Types
```c
typedef enum {
    SE_EVENT_INIT,      // Initialize tree (run oneshot functions)
    SE_EVENT_TICK,      // Normal execution tick
    SE_EVENT_TERM,      // Termination request
    SE_EVENT_RESET,     // Reset request
} s_expr_event_t;
```

### Tree Cleanup
```c
s_expr_tree_free(tree);
```

Releases node states, pointer slots, and the instance structure.

### Engine Cleanup
```c
s_engine_free(&engine);
```

Releases all module resources, function tables, and allocated memory.

---

## Test Execution Flow
```
┌─────────────────────────────────────────────────────────────────┐
│                        Test Runner                              │
└─────────────────────────────────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│ 1. Initialize allocator                                         │
│    - malloc/free wrappers                                       │
│    - time source                                                │
└─────────────────────────────────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│ 2. Load module (ROM or file)                                    │
│    - Parse binary header                                        │
│    - Map tree/function/string tables                            │
│    - Register built-in functions                                │
│    - Register user functions (if any)                           │
│    - Set debug callback (if provided)                           │
│    - Validate all function references                           │
└─────────────────────────────────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│ 3. For each test tree:                                          │
│    a. Create tree instance by hash                              │
│    b. Tick with SE_EVENT_TICK                                   │
│    c. Verify returned result code                               │
│    d. Free tree instance                                        │
└─────────────────────────────────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│ 4. Free engine                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Test Cases

### Application Result Codes (0-5)

| Test | DSL Function | Expected Result | Scope |
|------|--------------|-----------------|-------|
| `return_continue_test` | `se_return_continue()` | `SE_CONTINUE` (0) | ChainTree |
| `return_halt_test` | `se_return_halt()` | `SE_HALT` (1) | ChainTree |
| `return_terminate_test` | `se_return_terminate()` | `SE_TERMINATE` (2) | ChainTree |
| `return_reset_test` | `se_return_reset()` | `SE_RESET` (3) | ChainTree |
| `return_disable_test` | `se_return_disable()` | `SE_DISABLE` (4) | ChainTree |
| `return_skip_continue_test` | `se_return_skip_continue()` | `SE_SKIP_CONTINUE` (5) | ChainTree |

These codes pass through to the ChainTree walker unchanged.

### Function Result Codes (6-11)

| Test | DSL Function | Expected Result | Scope |
|------|--------------|-----------------|-------|
| `return_function_continue_test` | `se_return_function_continue()` | `SE_FUNCTION_CONTINUE` (6) | S-expression function |
| `return_function_halt_test` | `se_return_function_halt()` | `SE_FUNCTION_HALT` (7) | S-expression function |
| `return_function_terminate_test` | `se_return_function_terminate()` | `SE_FUNCTION_TERMINATE` (8) | S-expression function |
| `return_function_reset_test` | `se_return_function_reset()` | `SE_FUNCTION_RESET` (9) | S-expression function |
| `return_function_disable_test` | `se_return_function_disable()` | `SE_FUNCTION_DISABLE` (10) | S-expression function |
| `return_function_skip_continue_test` | `se_return_function_skip_continue()` | `SE_FUNCTION_SKIP_CONTINUE` (11) | S-expression function |

These codes are handled at the S_Engine function boundary.

### Pipeline Result Codes (12-17)

| Test | DSL Function | Expected Result | Scope |
|------|--------------|-----------------|-------|
| `return_pipeline_continue_test` | `se_return_pipeline_continue()` | `SE_PIPELINE_CONTINUE` (12) | Composite node |
| `return_pipeline_halt_test` | `se_return_pipeline_halt()` | `SE_PIPELINE_HALT` (13) | Composite node |
| `return_pipeline_terminate_test` | `se_return_pipeline_terminate()` | `SE_PIPELINE_TERMINATE` (14) | Composite node |
| `return_pipeline_reset_test` | `se_return_pipeline_reset()` | `SE_PIPELINE_RESET` (15) | Composite node |
| `return_pipeline_disable_test` | `se_return_pipeline_disable()` | `SE_PIPELINE_DISABLE` (16) | Composite node |
| `return_pipeline_skip_continue_test` | `se_return_pipeline_skip_continue()` | `SE_PIPELINE_SKIP_CONTINUE` (17) | Composite node |

These codes are handled internally by composite nodes (pipeline, sequence, state_machine, etc.).

---

## Debug Output

With `set_debug(true)` in the DSL and a debug callback registered, execution traces are available:
```c
static void debug_callback(s_expr_tree_instance_t* inst, const char* msg) {
    (void)inst;
    printf("  [DEBUG] %s\n", msg);
}
```

Example output:
```
Testing RETURN_CONTINUE...
  [DEBUG] TICK: return_continue_test
  [DEBUG] INVOKE: se_return_continue -> SE_CONTINUE
  result: 0 (expected SE_CONTINUE=0)
```

---

## Result Code Reference
```c
typedef enum {
    // APPLICATION RESULT CODES (0-5) - pass through to ChainTree
    SE_CONTINUE           = 0,
    SE_HALT               = 1,
    SE_TERMINATE          = 2,
    SE_RESET              = 3,
    SE_DISABLE            = 4,
    SE_SKIP_CONTINUE      = 5,
    
    // FUNCTION RESULT CODES (6-11) - handled at function boundary
    SE_FUNCTION_CONTINUE      = 6,
    SE_FUNCTION_HALT          = 7,
    SE_FUNCTION_TERMINATE     = 8,
    SE_FUNCTION_RESET         = 9,
    SE_FUNCTION_DISABLE       = 10,
    SE_FUNCTION_SKIP_CONTINUE = 11,
    
    // PIPELINE RESULT CODES (12-17) - handled by composite nodes
    SE_PIPELINE_CONTINUE      = 12,
    SE_PIPELINE_HALT          = 13,
    SE_PIPELINE_TERMINATE     = 14,
    SE_PIPELINE_RESET         = 15,
    SE_PIPELINE_DISABLE       = 16,
    SE_PIPELINE_SKIP_CONTINUE = 17,
} s_expr_result_t;
```

---

## Building and Running
```bash
# Compile DSL to binary
lua return_tests.lua

# Build test executable
gcc -o return_tests_runner \
    main.c \
    s_engine_*.c \
    -I. -lm

# Run tests
./return_tests_runner
```

Expected output:
```
╔════════════════════════════════════════════════════════════════╗
║           S-EXPRESSION ENGINE TEST SUITE                       ║
╚════════════════════════════════════════════════════════════════╝

Loading module from ROM...

=== Initializing Engine ===
✅ Module loaded successfully
   Trees:    18
   Records:  0
   Strings:  0
   Oneshot:  0
   Main:     18
   Pred:     0

=== Registering Functions ===
✅ Built-in functions registered
✅ Debug callback set

=== Validating Function Resolution ===
✅ All functions resolved successfully

╔════════════════════════════════════════════════════════════════╗
║                    RETURN VALUE TESTS                          ║
╚════════════════════════════════════════════════════════════════╝

--- Application Result Codes (0-5) ---

Testing SE_CONTINUE...
  ✅ PASS: CONTINUE (0)
Testing SE_HALT...
  ✅ PASS: HALT (1)
Testing SE_TERMINATE...
  ✅ PASS: TERMINATE (2)
Testing SE_RESET...
  ✅ PASS: RESET (3)
Testing SE_DISABLE...
  ✅ PASS: DISABLE (4)
Testing SE_SKIP_CONTINUE...
  ✅ PASS: SKIP_CONTINUE (5)

--- Function Result Codes (6-11) ---

Testing SE_FUNCTION_CONTINUE...
  ✅ PASS: FUNCTION_CONTINUE (6)
Testing SE_FUNCTION_HALT...
  ✅ PASS: FUNCTION_HALT (7)
Testing SE_FUNCTION_TERMINATE...
  ✅ PASS: FUNCTION_TERMINATE (8)
Testing SE_FUNCTION_RESET...
  ✅ PASS: FUNCTION_RESET (9)
Testing SE_FUNCTION_DISABLE...
  ✅ PASS: FUNCTION_DISABLE (10)
Testing SE_FUNCTION_SKIP_CONTINUE...
  ✅ PASS: FUNCTION_SKIP_CONTINUE (11)

--- Pipeline Result Codes (12-17) ---

Testing SE_PIPELINE_CONTINUE...
  ✅ PASS: PIPELINE_CONTINUE (12)
Testing SE_PIPELINE_HALT...
  ✅ PASS: PIPELINE_HALT (13)
Testing SE_PIPELINE_TERMINATE...
  ✅ PASS: PIPELINE_TERMINATE (14)
Testing SE_PIPELINE_RESET...
  ✅ PASS: PIPELINE_RESET (15)
Testing SE_PIPELINE_DISABLE...
  ✅ PASS: PIPELINE_DISABLE (16)
Testing SE_PIPELINE_SKIP_CONTINUE...
  ✅ PASS: PIPELINE_SKIP_CONTINUE (17)

╔════════════════════════════════════════════════════════════════╗
║                        TEST SUMMARY                            ║
╠════════════════════════════════════════════════════════════════╣
║  Passed: 18                                                    ║
║  Failed:  0                                                    ║
║  Total:  18                                                    ║
╚════════════════════════════════════════════════════════════════╝

✅ ALL TESTS PASSED
```
```