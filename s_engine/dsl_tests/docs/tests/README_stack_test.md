# stack_test.lua — Stack Frame and Quad Operations Test

## Overview

This test exercises the S-Expression Engine's stack frame management, quad arithmetic operations, and function call/return mechanism. It validates that stack-based local variables, scratch space, parameter passing, and return value retrieval all work correctly across nested frames.

The test runs three sequential actions inside `se_function_interface`, each progressively more complex.

## Record Definition

```
stack_test_state
├── int_val_1    (int32)    — integer workspace
├── int_val_2    (int32)    — integer workspace
├── int_val_3    (int32)    — integer workspace
├── uint_val_1   (uint32)   — unsigned workspace
├── uint_val_2   (uint32)   — unsigned workspace
├── uint_val_3   (uint32)   — unsigned workspace
├── float_val_1  (float)    — float workspace
├── float_val_2  (float)    — float workspace
├── float_val_3  (float)    — float workspace
└── loop_count   (uint32)   — loop iteration counter
```

## Test Actions

### action_1 — Integer Quad Operations with Frame Allocation

**Purpose:** Validate integer quad arithmetic using blackboard fields inside a stack frame, with a loop and tick delays.

**Structure:**
- `se_fork_join` → `se_while` (100 iterations) → `se_frame_allocate(0, 5, 5)`
- Each iteration delayed by 5 ticks

**Operations per iteration:**
1. `int_val_2 = int_val_1 + 1` (quad_iadd with float coercion)
2. `int_val_2 = int_val_1 + 5` (quad_iadd with uint)
3. `int_val_3 = int_val_2 - 2` (quad_isub)
4. `int_val_1 = int_val_3` (quad_mov)

**Net effect:** `int_val_1` increases by 3 each iteration (0, 3, 6, 9, ..., 297).

**Validates:**
- Integer quad operations (iadd, isub, mov)
- Blackboard field reads/writes from quads
- Frame allocation/deallocation across loop iterations
- Mixed type coercion (float literal to int)

### action_2 — Float Quad Operations with Frame Allocation

**Purpose:** Same structure as action_1 but with floating-point arithmetic.

**Structure:**
- `se_fork_join` → `se_while` (10 iterations) → `se_frame_allocate(0, 5, 5)`
- Each iteration delayed by 5 ticks

**Operations per iteration:**
1. `float_val_2 = float_val_1 + 1.0` (quad_fadd)
2. `float_val_2 = float_val_1 + 5.0` (quad_fadd, overwrites previous)
3. `float_val_3 = float_val_2 - 2.0` (quad_fsub)
4. `float_val_1 = float_val_3` (quad_mov)

**Net effect:** `float_val_1` increases by 3.0 each iteration (0.0, 3.0, 6.0, ..., 27.0).

**Validates:**
- Float quad operations (fadd, fsub, mov)
- Float blackboard field reads/writes
- Uint-to-float coercion in subtraction

### action_3 — Stack Locals, Scratch, and Function Call/Return

**Purpose:** Full integration test of stack frame locals, scratch variables, parameter passing via `stack_push`, function call with `se_call`, and return value retrieval via `stack_pop`.

**Structure:**
```
se_fork_join
└── se_while (10 iterations, using p_icmp_lt_acc on loop_count)
    └── se_frame_allocate(0 params, 5 locals, 5 scratch)
        ├── [compute using locals and scratch]
        ├── [push 2 params]
        ├── se_call(2 params, 2 locals, 3 scratch, returns {2, 3})
        │   └── [compute r0, r1 from p0, p1]
        ├── [pop 2 return values]
        └── [store results to blackboard]
```

**Outer frame variables:**
- Locals: `a`, `b`, `c`, `e`, `f` (indices 0–4)
- Scratch: `t0`, `t1`, `t2`, `t3`, `t4` (TOS offsets 0–4)

**Outer frame operations per iteration:**
1. `a = float_val_1 + 1.0`
2. `float_val_2 = a + 5.0`
3. `b = float_val_2`
4. `float_val_3 = b - 2.0`
5. `float_val_1 = float_val_3`

Net: `float_val_1 += 4.0` each iteration.

**Call setup:**
1. `stack_push(float_val_1)` — becomes p0
2. `stack_push(float_val_2)` — becomes p1

**Call frame variables:**
- Locals: `p0` (param 0), `p1` (param 1), `r0` (return 0), `r1` (return 1)
- Scratch: `ct0`, `ct1`, `ct2`

**Call operations:**
1. `ct0 = p0 + 1.0`
2. `ct1 = ct0 + 5.0`
3. `r0 = ct1 - 2.0` → result: `p0 + 4.0`
4. `r1 = p1 * 2.0` → result: `p1 * 2.0`

**Return:** `return_vars = {2, 3}` → copies locals 2 (`r0`) and 3 (`r1`) to caller's stack.

**Return value retrieval:**
1. `stack_pop() → v.e` (gets r1, last pushed is on top)
2. `stack_pop() → v.f` (gets r0)
3. `float_val_2 = v.e` (= r1)
4. `float_val_3 = v.f` (= r0)

**Expected values (iteration where float_val_1 enters as 36.0):**

| Step | Value |
|------|-------|
| float_val_1 (input) | 36.0 |
| a = 36.0 + 1.0 | 37.0 |
| float_val_2 = 37.0 + 5.0 | 42.0 |
| float_val_3 = 42.0 - 2.0 | 40.0 |
| float_val_1 = 40.0 | 40.0 |
| p0 = 40.0, p1 = 42.0 | — |
| r0 = (40+1+5) - 2 | 44.0 |
| r1 = 42 * 2 | 84.0 |
| call result float_val_2 | 84.0 |
| call result float_val_3 | 44.0 |

**Validates:**
- `stack_local()` reads/writes for local variables
- `stack_tos()` reads/writes for scratch variables
- `stack_push` destination mode in quad operations
- `stack_pop` source mode in quad operations
- `se_call` parameter passing via stack
- `SE_STACK_FRAME_INSTANCE` frame lifecycle (INIT → push frame, TERMINATE → copy returns, pop frame)
- `copy_return_vars_and_pop` correctly copying return values
- Return value ordering (LIFO pop)
- Nested frame allocation (outer `se_frame_allocate` + inner `se_call`)

## Test Sequence

```
1. Log initial stack state
2. Initialize int_val_1 = 0, float_val_1 = 0.0
3. action_1: 100 iterations of integer arithmetic (500 ticks)
4. action_2: 10 iterations of float arithmetic (50 ticks)
5. Reset int_val_1 = 0, float_val_1 = 0.0
6. action_3: 10 iterations of call/return test (50 ticks)
7. Log final stack state
8. Terminate
```

## Key DSL Features Exercised

| Feature | Where Used |
|---------|-----------|
| `se_frame_allocate` | All actions — creates stack frame with locals + scratch |
| `frame_vars` | action_3 — named references to locals and scratch |
| `quad_iadd/isub/mov` | action_1 — integer arithmetic |
| `quad_fadd/fsub/fmul/mov` | action_2, action_3 — float arithmetic |
| `stack_push_ref()` | action_3 — push call parameters |
| `stack_pop_ref()` | action_3 — retrieve return values |
| `se_call` | action_3 — function call with frame and returns |
| `p_icmp_lt_acc` | action_3 — predicate-based loop control |
| `se_state_increment_and_test` | action_1, action_2 — counter-based loop control |
| `se_while` | All actions — loop construct |
| `se_tick_delay` | All actions — multi-tick delay |
| `se_fork_join` | All actions — parallel execution wrapper |
| `se_log` / `se_log_slot_*` | All actions — debug output |

## Stack Layout During action_3 Call

```
                    ┌─────────────────────┐
                    │  scratch (ct0-ct2)  │ ← TOS during call body
                    ├─────────────────────┤
                    │  r1 (local 3)       │ ← return var
                    │  r0 (local 2)       │ ← return var
                    ├─────────────────────┤
                    │  p1 (param 1)       │ ← from stack_push
                    │  p0 (param 0)       │ ← from stack_push
  call frame ──►   ├═════════════════════┤ ← base_ptr (call)
                    │  scratch (t0-t4)    │ ← TOS during outer body
                    ├─────────────────────┤
                    │  f  (local 4)       │
                    │  e  (local 3)       │
                    │  c  (local 2)       │
                    │  b  (local 1)       │
                    │  a  (local 0)       │
  outer frame ──►  ├═════════════════════┤ ← base_ptr (outer)
                    │  (empty - 0 params) │
                    └─────────────────────┘
```

After `se_call` returns, return values `r0` and `r1` are copied to the caller's stack top via `copy_return_vars_and_pop`, then retrieved with `stack_pop`.
