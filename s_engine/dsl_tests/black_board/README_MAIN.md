# Blackboard Tutorial Test Harness

This test harness (`main.c`) validates the S-Expression engine's blackboard functionality by running the `black_board` DSL module through a comprehensive test suite.

## Overview

The test harness demonstrates:
- Loading modules from ROM (embedded binary) and file
- Engine initialization and function registration
- Engine-allocated vs external blackboard patterns
- Shared blackboard between multiple trees
- Error tracking and reporting

## Dependencies

### Header Files

| File | Purpose |
|------|---------|
| `s_engine_types.h` | Core type definitions |
| `s_engine_module.h` | Module management API |
| `s_engine_eval.h` | Tree evaluation API |
| `s_engine_loader.h` | Binary loader API |
| `s_engine_init.h` | Engine initialization |
| `s_engine_builtins.h` | Built-in function registration |
| `s_engine_node.h` | Node lifecycle API |
| `black_board.h` | Generated tree hash definitions |
| `black_board_bin_32.h` | Generated binary module (ROM) |
| `black_board_records.h` | Generated C struct definitions |

### External Function

```c
extern void black_board_register_all(s_expr_module_t* module);
```

User-provided function that registers all oneshot/main/pred functions for the module.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                         main()                               │
├─────────────────────────────────────────────────────────────┤
│  1. Setup allocator                                         │
│  2. Load from ROM → run_tutorial_tests()                    │
│  3. Load from file → run_tutorial_tests()                   │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                   run_tutorial_tests()                       │
├─────────────────────────────────────────────────────────────┤
│  Engine-Allocated Blackboard:                               │
│    • test_blackboard_access()                               │
│    • test_slot_access()                                     │
│    • test_array_access()                                    │
│    • test_nested_access()                                   │
│    • test_constants()                                       │
│                                                             │
│  External Blackboard:                                       │
│    • test_external_blackboard()                             │
│    • test_shared_blackboard()                               │
└─────────────────────────────────────────────────────────────┘
```

## Components

### Allocator

Simple wrapper around `malloc`/`free` with monotonic time provider:

```c
s_expr_allocator_t alloc = {
    .malloc = simple_malloc,
    .free = simple_free,
    .ctx = NULL,
    .get_time = linux_get_time  // CLOCK_REALTIME
};
```

### Callbacks

| Callback | Purpose |
|----------|---------|
| `debug_callback` | Prints `[DEBUG]` messages from `se_log()` |
| `error_callback` | Prints `[ERROR]` messages, increments `g_test_errors` |

### Module Loading

Two loading methods are demonstrated:

**From ROM (embedded binary)**:
```c
load_from_rom(&engine, &alloc, black_board_module_bin_32, BLACK_BOARD_MODULE_BIN_32_SIZE);
```

**From File**:
```c
load_from_file(&engine, &alloc, "black_board_32.bin");
```

Both methods perform:
1. Initialize engine with binary data
2. Register built-in functions
3. Register user functions via `black_board_register_all()`
4. Set debug/error callbacks
5. Validate all function hashes resolve

## Test Cases

### Engine-Allocated Blackboard Tests

These tests pass `NULL` for blackboard, letting the engine allocate and initialize with `use_defaults()`:

| Test | Tree | Description |
|------|------|-------------|
| `test_blackboard_access` | `demo_blackboard_access` | Direct struct field access |
| `test_slot_access` | `demo_slot_access` | Generic field_ref access |
| `test_array_access` | `demo_array_access` | String and array operations |
| `test_nested_access` | `demo_nested_access` | Embedded record fields |
| `test_constants` | `demo_constants` | Verify use_defaults() initialization |

### External Blackboard Tests

| Test | Description |
|------|-------------|
| `test_external_blackboard` | Bind pre-populated application struct |
| `test_shared_blackboard` | Multiple trees sharing same blackboard |

#### External Blackboard Test

Demonstrates binding application-owned data:

```c
ScalarDemo_t app_state = {
    .counter = 999,
    .flags = 0xABCD1234,
    .temperature = 25.5f,
    // ...
};

s_expr_tree_bind_blackboard(tree, &app_state, sizeof(app_state));
s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
// app_state is now modified by tree
```

Key points:
- Data persists after tree is freed
- Tree operates directly on application memory
- No copy overhead

#### Shared Blackboard Test

Demonstrates coordination between trees:

```c
ScalarDemo_t shared_data = {0};

// Both trees use same blackboard
s_expr_tree_bind_blackboard(tree1, &shared_data, sizeof(shared_data));
s_expr_tree_bind_blackboard(tree2, &shared_data, sizeof(shared_data));

// tree1 modifies shared_data
s_expr_node_tick(tree1, SE_EVENT_TICK, NULL);

// tree2 sees tree1's changes
s_expr_node_tick(tree2, SE_EVENT_TICK, NULL);
```

## Test Helper

```c
static bool run_tree_test(
    s_engine_handle_t* engine,
    s_expr_hash_t tree_hash,
    const char* test_name,
    void* blackboard,      // NULL = engine allocates
    uint16_t bb_size       // 0 = engine allocates
);
```

- Creates tree by hash
- Optionally binds external blackboard
- Runs single tick
- Reports pass/fail based on `g_test_errors`

## Output Format

```
╔════════════════════════════════════════════════════════════════╗
║           S-EXPRESSION ENGINE TUTORIAL TEST                    ║
╚════════════════════════════════════════════════════════════════╝

=== Loading module from ROM ===

=== Initializing Engine from ROM ===
✅ Module loaded successfully
   Trees:    5
   Records:  5
   Strings:  2
   Oneshot:  16
   Main:     0
   Pred:     0

=== Registering Functions ===
✅ Built-in functions registered
✅ Tutorial user functions registered
✅ Debug/error callbacks set

=== Validating Function Resolution ===
✅ All functions resolved successfully

╔════════════════════════════════════════════════════════════════╗
║           TUTORIAL TEST SUITE                                  ║
╚════════════════════════════════════════════════════════════════╝

╔════════════════════════════════════════╗
║    BLACKBOARD ACCESS TESTS             ║
╚════════════════════════════════════════╝

Testing demo_blackboard_access...
  Tree result: CONTINUE
  ✅ PASSED

...

╔════════════════════════════════════════════════════════════════╗
║           ALL TUTORIAL TESTS COMPLETE                          ║
╚════════════════════════════════════════════════════════════════╝

✅ All tests completed!
```

## Error Handling

| Error | Cause | Action |
|-------|-------|--------|
| Engine init failure | Bad binary data | Exit with error |
| Validation failure | Missing function hash | Print hash, exit |
| Tree creation failure | Invalid hash | Skip test, report |
| Verification failure | Value mismatch | Increment `g_test_errors` |

## Building

```bash
gcc -o black_board_test main.c black_board_user_functions.c \
    -I./include \
    -L./lib -ls_engine \
    -lm
```

## Running

```bash
# With binary file in current directory
./black_board_test

# ROM-only (file load will warn but continue)
./black_board_test
```

## Return Codes

| Code | Meaning |
|------|---------|
| 0 | All tests passed |
| 1 | Module load failure |