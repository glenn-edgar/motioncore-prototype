```markdown
# Callback Function Test

## Overview

This test demonstrates the S-expression engine's ability to store and execute 
callable s-expressions via blackboard pointer fields. A function is defined as 
a Lua closure, loaded into a PTR64 blackboard field at runtime, and then 
executed indirectly through that field reference.

## Mechanism

### se_load_function (oneshot)
Stores a pointer to a compiled s-expression callable into a PTR64 blackboard 
field. The callable lives in ROM (the compiled param array), so this is a 
simple pointer assignment — no allocation required.

- DSL: `se_load_function(blackboard_field, fn)`
- C function: `SE_LOAD_FUNCTION`
- Call type: `o_call` (oneshot — executes once)

### se_exec_function (main)
Reads the callable pointer from the blackboard PTR64 field and invokes it. 
Resets all callable node states before each invocation so the function can 
be called repeatedly. Maps `SE_PIPELINE_DISABLE` to `SE_PIPELINE_CONTINUE` 
so completion of the inner callable does not disable the caller.

- DSL: `se_exec_function(blackboard_field)`
- C function: `SE_EXEC_FN`
- Call type: `pt_m_call` (main with pointer slot)

## Test Structure

```
callback_function_blackboard (record)
└── fn_ptr: PTR64_FIELD — holds pointer to callable s-expression

callback_function (tree)
└── se_function_interface
    ├── se_log("callback test started")          -- oneshot
    ├── se_load_function("fn_ptr", fns)          -- oneshot: store callable
    ├── se_exec_function("fn_ptr")               -- main: invoke callable
    │   └── se_sequence_once                     -- the stored callable
    │       ├── se_log("callback function called")
    │       ├── se_log("do some stack work")
    │       └── se_log("call a dictionary function")
    └── se_return_function_terminate()
```

## Expected Output

```
[DEBUG] callback test started
[DEBUG] callback function called
[DEBUG] do some stack work
[DEBUG] call a dictionary function
Tick 1: result=FUNCTION_TERMINATE
```

## Key Design Points

- **Indirection**: The callable is not invoked directly in the tree. It is 
  stored as a pointer and dispatched at runtime, enabling callback patterns 
  and configurable behavior.

- **Node state reset**: `se_exec_function` resets all node states within the 
  callable before each invocation. This ensures oneshots re-fire and 
  sequence nodes re-initialize on repeated calls.

- **ROM storage**: The compiled s-expression lives in the module's const param 
  array. `se_load_function` stores a pointer — no heap allocation is needed 
  for the callable itself.

- **PTR64_FIELD**: Uses a 64-bit blackboard field to store the pointer, 
  ensuring portability across 32-bit and 64-bit targets.

## Related Functions

| DSL Function | C Function | Type | Purpose |
|---|---|---|---|
| `se_load_function` | `SE_LOAD_FUNCTION` | oneshot | Store callable pointer in blackboard |
| `se_exec_function` | `SE_EXEC_FN` | pt_m_call | Execute callable from blackboard |
| `se_load_function_dict` | `SE_LOAD_FUNCTION_DICT` | oneshot | Store dictionary pointer in blackboard |
| `se_exec_dict_fn` | `SE_EXEC_DICT_DISPATCH` | m_call | Execute dictionary entry (with blackboard) |
| `se_exec_dict_internal` | `SE_EXEC_DICT_INTERNAL` | m_call | Execute sibling dictionary entry (uses current_dict) |
```

