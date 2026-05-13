# Stack Frame and Quad Operations — DSL & Runtime Reference

## Theory of Operation

The S-Expression Engine executes behavior trees tick-by-tick. Each tick traverses the tree, invoking nodes according to their type. Stack frame management and quad operations integrate into this tick-driven model to provide local variables, scratch space, function calls with parameter passing, and three-address arithmetic — all within the constraints of an embedded execution environment.

### Tick-Based Execution Model

A single tick proceeds as follows:

1. The tree walker visits the root node (`se_function_interface`).
2. `se_function_interface` iterates its active children, invoking each one.
3. Each child returns a result code that determines its fate:
   - **APPLICATION codes (0–5):** Propagated immediately to the caller, ending the tick.
   - **FUNCTION codes (6–11):** Propagated immediately to the caller, ending the tick.
   - **PIPELINE codes (12–17):** Handled internally (continue, halt, disable, terminate, reset, skip).
4. Active children that return PIPELINE_CONTINUE or PIPELINE_HALT remain active for the next tick.
5. Children returning PIPELINE_DISABLE or PIPELINE_TERMINATE are terminated and removed.
6. When no active children remain, `se_function_interface` returns FUNCTION_DISABLE.

Within a single tick, `se_frame_allocate` pushes a stack frame before executing its children and pops it after. This means local variables and scratch space exist only for the duration of that one tick — they are recreated on every tick. Persistent state lives in the blackboard (record fields) or in node state bytes.

### Three-Layer Result Code Architecture

Result codes are organized into three propagation layers:

| Range | Layer | Scope |
|-------|-------|-------|
| 0–5 | APPLICATION | Exits the entire tree |
| 6–11 | FUNCTION | Exits current function boundary |
| 12–17 | PIPELINE | Internal to parent node only |

Each layer has the same six outcomes: CONTINUE, HALT, TERMINATE, RESET, DISABLE, SKIP_CONTINUE. The layer determines how far up the tree the result propagates.

### Stack Frame Lifecycle

Stack frames are transient within a tick. The frame lifecycle for `se_frame_allocate`:

```
TICK START
  ├── push_frame(num_params, num_locals)
  ├── push scratch slots (zeroed)
  ├── execute children (quads, control flow, calls)
  ├── pop_frame (restores SP to pre-frame position)
  └── return result
TICK END
```

For `se_call` (via `SE_STACK_FRAME_INSTANCE`), the lifecycle spans multiple ticks:

```
TICK N (INIT):     push_frame, capturing params already on stack
TICK N+1..M:       children execute with frame active
TICK M+1 (TERM):   copy return vars to temp, pop_frame, push returns
```

---

## Stack Architecture

### Frame Layout

When a stack frame is created, the stack pointer region is organized as:

```
base_ptr → [param 0] [param 1] ... [param N-1]
           [local 0] [local 1] ... [local M-1]  ← zeroed on init
           [scratch 0] [scratch 1] ...           ← TOS-relative
                                            ← SP (grows upward)
```

Each slot holds an `s_expr_param_t` (8 bytes on 32-bit, 16 bytes on 64-bit) containing a type tag and a value union.

### Addressing Modes

| Mode | DSL Function | Addressing | Lifetime |
|------|-------------|------------|----------|
| `stack_local(idx)` | `local_ref(idx)` | `base_ptr + idx` | Persistent across ticks (se_call only) |
| `stack_tos(offset)` | `tos_ref(offset)` | `SP - offset` (0 = top) | Within current tick only |
| `stack_push()` | `stack_push_ref()` | `*++SP = value` | Until consumed |
| `stack_pop()` | `stack_pop_ref()` | `value = *SP--` | Destructive read |

### Parameter Type Opcodes

| Opcode | Value | Description |
|--------|-------|-------------|
| `S_EXPR_PARAM_STACK_TOS` | 0x18 | Read/write relative to stack top |
| `S_EXPR_PARAM_STACK_LOCAL` | 0x19 | Read/write relative to frame base |
| `S_EXPR_PARAM_NULL` | 0x1A | Null/unused parameter slot |
| `S_EXPR_PARAM_STACK_PUSH` | 0x1B | Write and increment SP |
| `S_EXPR_PARAM_STACK_POP` | 0x1C | Read and decrement SP |

---

## DSL Functions

### se_frame_allocate(num_params, num_locals, scratch_depth, ...)

Allocates a stack frame per-tick around child nodes. The frame is pushed at the start of each tick and popped at the end, so locals and scratch are volatile across ticks.

**DSL:**
```lua
se_frame_allocate(0, 5, 5, function()
    local v = frame_vars({"a", "b", "c", "d", "e"}, {"t0", "t1", "t2", "t3", "t4"})
    quad_iadd(field_val("x"), int_val(1), v.a)()
end)
```

**Parameters:**
- `num_params` — Parameters already on stack (usually 0 for top-level frames)
- `num_locals` — Local variable slots to allocate (zeroed each tick)
- `scratch_depth` — Scratch slots pushed above locals
- `...` — Child functions to execute within the frame

**C Runtime (`se_frame_allocate`):**

On each TICK:
1. Calls `s_expr_stack_push_frame(stack, num_params, num_locals)`
2. Pushes `scratch_depth` zero-valued slots
3. Iterates children: oneshots and preds fire-and-forget, main nodes return pipeline results
4. Calls `s_expr_stack_pop_frame(stack)` before returning

On TERMINATE: terminates all children, does not touch the stack.

On INIT: returns PIPELINE_CONTINUE (no frame action).

**Critical detail:** Because the frame is pushed and popped every tick, local variables do not persist. Use blackboard fields for state that must survive across ticks.

### se_call(num_params, num_locals, scratch_depth, return_vars, body_fns)

Function call with parameter passing via the stack and return value retrieval. Creates a persistent frame that lives across multiple ticks.

**DSL:**
```lua
-- Push parameters before the call
quad_mov(field_val("x"), stack_push_ref())()
quad_mov(field_val("y"), stack_push_ref())()

-- Call with 2 params, 2 locals, 3 scratch, return locals 2 and 3
se_call(2, 2, 3, {2, 3}, {
    function()
        local cv = frame_vars({"p0", "p1", "r0", "r1"}, {"t0", "t1", "t2"})
        quad_fadd(cv.p0, cv.p1, cv.r0)()
        quad_fmul(cv.p0, float_val(2.0), cv.r1)()
    end
})

-- Pop return values (reverse order — last in return_vars is on top)
quad_mov(stack_pop_ref(), local_dest_1)()
quad_mov(stack_pop_ref(), local_dest_0)()
```

**Parameters:**
- `num_params` — Number of values already pushed onto the stack
- `num_locals` — Additional local slots (zeroed on init)
- `scratch_depth` — Compile-time scratch depth (for validation only)
- `return_vars` — List of local indices whose values are returned to caller
- `body_fns` — Table of functions forming the call body

**How it works:**

`se_call` emits the following tree structure:

```
SE_SEQUENCE_ONCE
  ├── SE_PUSH_STACK(num_params)     ← pushes param count marker
  ├── SE_STACK_FRAME_INSTANCE(...)  ← manages frame lifecycle
  └── body_fn children              ← user code
```

### se_stack_frame_instance (low-level)

The runtime node that manages the call frame lifecycle. Not typically called directly — `se_call` wraps it.

**C Runtime (`se_stack_frame_instance`):**

On INIT:
1. Pops the parameter count marker from the stack
2. Validates it matches `num_params`
3. Calls `s_expr_stack_push_frame(stack, num_params, num_locals)`
4. Sets node state to 1 (frame active)

On TICK: Returns PIPELINE_CONTINUE (frame stays active).

On TERMINATE:
1. If state == 1 (frame was active), calls `copy_return_vars_and_pop`
2. Resets node state to 0

**copy_return_vars_and_pop:**
1. Reads each local index from `return_vars` list
2. Copies values to a temporary buffer (avoids overlap corruption)
3. Calls `s_expr_stack_pop_frame` (restores SP to caller's position)
4. Pushes return values from temp buffer onto the caller's stack

Return values are pushed in list order, so `{2, 3}` pushes local 2 first, then local 3. The caller pops in reverse: first pop gets local 3, second pop gets local 2.

### se_sequence_once(...)

Executes all children in sequence within a single tick, then disables itself.

**DSL:**
```lua
se_sequence_once(
    function() quad_mov(int_val(0), field_val("x"))() end,
    function() quad_iadd(field_val("x"), int_val(1), field_val("y"))() end
)
```

**C Runtime (`se_sequence_once`):**

On TICK:
1. Iterates all children in order
2. Oneshots and preds: invoke and continue
3. Main nodes: invoke (pipeline results ignored, continues to next child)
4. After all children execute, terminates all initialized children
5. Returns PIPELINE_DISABLE

On TERMINATE: Terminates all initialized children, resets state.

On INIT: Sets state to 0.

This node is used internally by `se_call` to ensure the frame body and `SE_STACK_FRAME_INSTANCE` execute together, with proper terminate sequencing.

### se_function_interface(body_fn)

Top-level function entry point. Manages the lifecycle of all children as a fork-join with pipeline result handling.

**DSL:**
```lua
se_function_interface(function()
    action_1()
    action_2()
    se_return_terminate()
end)
```

**C Runtime (`se_function_interface`):**

On INIT: Sets state to RUNNING, resets all callable children.

On TICK:
1. Iterates active children
2. APPLICATION/FUNCTION results (0–11): propagated immediately, tick ends
3. PIPELINE results handled internally:
   - CONTINUE/HALT → child stays active
   - DISABLE/TERMINATE → child terminated
   - RESET → child terminated then reset (restarted)
   - SKIP_CONTINUE → child stays active, skip remaining children this tick
4. When active_count == 0, returns FUNCTION_DISABLE

On TERMINATE: Terminates all children.

### frame_vars(locals, scratch)

Compile-time helper that creates named references to stack frame slots.

**DSL:**
```lua
local v = frame_vars({"a", "b", "c"}, {"t0", "t1"})
-- v.a = function() stack_local(0) end
-- v.b = function() stack_local(1) end
-- v.c = function() stack_local(2) end
-- v.t0 = function() stack_tos(0) end
-- v.t1 = function() stack_tos(1) end
```

Returns a table of closure functions, each emitting the appropriate parameter type when called. These closures are used as arguments to quad operations.

### se_log_stack()

Debug utility that prints the current stack state.

**DSL:**
```lua
se_log_stack()
```

**C Runtime:** Prints capacity, free space, stack pointer, frame count, and current frame details (base_ptr, num_params, num_locals, scratch_base).

---

## Quad Operations

### Architecture

Quad operations are three-address instructions: `dest = op(src1, src2)`. They execute as oneshot nodes — fire once per tick, no persistent state.

**DSL core function:**
```lua
se_quad(opcode, src1_fn, src2_fn, dest_fn)
```

All four arguments are closure functions that emit parameters when called (the deferred emission pattern). The helpers return a closure that must be invoked with `()`:

```lua
quad_fadd(field_val("x"), float_val(1.0), local_ref(0))()
--                                                      ^^ fires the closure
```

### Parameter Factories

| Factory | Emits | Description |
|---------|-------|-------------|
| `field_val(name)` | `field_ref` | Blackboard field by name |
| `local_ref(idx)` | `stack_local` | Frame local by index |
| `tos_ref(offset)` | `stack_tos` | Scratch by TOS offset (0 = top) |
| `int_val(v)` | `int` | Integer constant |
| `uint_val(v)` | `uint` | Unsigned integer constant |
| `float_val(v)` | `flt` | Float constant |
| `const_val(name)` | `const_ref` | ROM constant reference |
| `hash_val(s)` | `str_hash` | FNV-1a hash of string |
| `null_val()` | `null_param` | Null/unused (for unary ops) |
| `stack_push_ref()` | `stack_push` | Write destination: push onto stack |
| `stack_pop_ref()` | `stack_pop` | Read source: pop from stack |

### C Runtime Read/Write

The runtime dispatches on the parameter type opcode to read or write values:

**`quad_read_int` / `quad_read_float`** — Source operand dispatch:

| Param Type | Action |
|-----------|--------|
| INT/UINT/FLOAT | Return literal value (with type coercion) |
| FIELD | Read from `blackboard + field_offset` |
| STACK_TOS | `s_expr_stack_peek_tos(stack, offset)` |
| STACK_LOCAL | `s_expr_stack_get_local(stack, index)` |
| STACK_POP | `s_expr_stack_pop(stack)`, return value |
| CONST_REF | Read from `module->constants[index]` |

**`quad_write_int` / `quad_write_float`** — Destination operand dispatch:

| Param Type | Action |
|-----------|--------|
| FIELD | Write to `blackboard + field_offset` |
| STACK_TOS | `s_expr_stack_poke(stack, offset, &param)` |
| STACK_LOCAL | `s_expr_stack_set_local(stack, index, &param)` |
| STACK_PUSH | `s_expr_stack_push(stack, &param)` — increments SP |

### SE_QUAD_OP Opcodes (Oneshot)

Integer arithmetic:

| Opcode | Value | Operation |
|--------|-------|-----------|
| IADD | 0x00 | dest = src1 + src2 |
| ISUB | 0x01 | dest = src1 - src2 |
| IMUL | 0x02 | dest = src1 × src2 |
| IDIV | 0x03 | dest = src1 / src2 |
| IMOD | 0x04 | dest = src1 % src2 |
| INEG | 0x05 | dest = -src1 |

Float arithmetic:

| Opcode | Value | Operation |
|--------|-------|-----------|
| FADD | 0x08 | dest = src1 + src2 |
| FSUB | 0x09 | dest = src1 - src2 |
| FMUL | 0x0A | dest = src1 × src2 |
| FDIV | 0x0B | dest = src1 / src2 |
| FMOD | 0x0C | dest = src1 % src2 |
| FNEG | 0x0D | dest = -src1 |

Bitwise (integer):

| Opcode | Value | Operation |
|--------|-------|-----------|
| BIT_AND | 0x10 | dest = src1 & src2 |
| BIT_OR | 0x11 | dest = src1 \| src2 |
| BIT_XOR | 0x12 | dest = src1 ^ src2 |
| BIT_NOT | 0x13 | dest = ~src1 |
| BIT_SHL | 0x14 | dest = src1 << src2 |
| BIT_SHR | 0x15 | dest = src1 >> src2 |

Integer comparison (dest = 1 or 0):

| Opcode | Value | Operation |
|--------|-------|-----------|
| ICMP_EQ | 0x20 | dest = (src1 == src2) |
| ICMP_NE | 0x21 | dest = (src1 != src2) |
| ICMP_LT | 0x22 | dest = (src1 < src2) |
| ICMP_LE | 0x23 | dest = (src1 <= src2) |
| ICMP_GT | 0x24 | dest = (src1 > src2) |
| ICMP_GE | 0x25 | dest = (src1 >= src2) |

Float comparison (dest = 1 or 0):

| Opcode | Value | Operation |
|--------|-------|-----------|
| FCMP_EQ | 0x28 | dest = (src1 == src2) |
| FCMP_NE | 0x29 | dest = (src1 != src2) |
| FCMP_LT | 0x2A | dest = (src1 < src2) |
| FCMP_LE | 0x2B | dest = (src1 <= src2) |
| FCMP_GT | 0x2C | dest = (src1 > src2) |
| FCMP_GE | 0x2D | dest = (src1 >= src2) |

Logical (dest = 1 or 0):

| Opcode | Value | Operation |
|--------|-------|-----------|
| LOG_AND | 0x30 | dest = src1 && src2 |
| LOG_OR | 0x31 | dest = src1 \|\| src2 |
| LOG_NOT | 0x32 | dest = !src1 |
| LOG_NAND | 0x33 | dest = !(src1 && src2) |
| LOG_NOR | 0x34 | dest = !(src1 \|\| src2) |
| LOG_XOR | 0x35 | dest = src1 xor src2 |

Move:

| Opcode | Value | Operation |
|--------|-------|-----------|
| MOVE | 0x40 | dest = src1 |

Float math functions:

| Opcode | Value | Operation |
|--------|-------|-----------|
| FSQRT | 0x50 | dest = sqrt(src1) |
| FPOW | 0x51 | dest = src1 ^ src2 |
| FEXP | 0x52 | dest = e^src1 |
| FLOG | 0x53 | dest = ln(src1) |
| FLOG10 | 0x54 | dest = log10(src1) |
| FLOG2 | 0x55 | dest = log2(src1) |
| FABS | 0x56 | dest = |src1| |

Trigonometric (radians):

| Opcode | Value | Operation |
|--------|-------|-----------|
| FSIN | 0x58 | dest = sin(src1) |
| FCOS | 0x59 | dest = cos(src1) |
| FTAN | 0x5A | dest = tan(src1) |
| FASIN | 0x5B | dest = asin(src1) |
| FACOS | 0x5C | dest = acos(src1) |
| FATAN | 0x5D | dest = atan(src1) |
| FATAN2 | 0x5E | dest = atan2(src1, src2) |

Hyperbolic:

| Opcode | Value | Operation |
|--------|-------|-----------|
| FSINH | 0x60 | dest = sinh(src1) |
| FCOSH | 0x61 | dest = cosh(src1) |
| FTANH | 0x62 | dest = tanh(src1) |

Integer math:

| Opcode | Value | Operation |
|--------|-------|-----------|
| IABS | 0x68 | dest = |src1| |
| IMIN | 0x69 | dest = min(src1, src2) |
| IMAX | 0x6A | dest = max(src1, src2) |

Float min/max:

| Opcode | Value | Operation |
|--------|-------|-----------|
| FMIN | 0x6C | dest = min(src1, src2) |
| FMAX | 0x6D | dest = max(src1, src2) |

### SE_P_QUAD_OP Opcodes (Predicate)

Predicate quad operations return a boolean (true/false) to the tree walker. They use the same opcode space for comparison, logical, and bitwise operations as SE_QUAD_OP but are registered as predicate functions (p_call).

They include all bitwise, comparison, and logical operations from SE_QUAD_OP, plus accumulate variants:

Integer comparison + accumulate (dest += result):

| Opcode | Value | Operation |
|--------|-------|-----------|
| ICMP_EQ_ACC | 0x40 | dest += (src1 == src2) |
| ICMP_NE_ACC | 0x41 | dest += (src1 != src2) |
| ICMP_LT_ACC | 0x42 | dest += (src1 < src2) |
| ICMP_LE_ACC | 0x43 | dest += (src1 <= src2) |
| ICMP_GT_ACC | 0x44 | dest += (src1 > src2) |
| ICMP_GE_ACC | 0x45 | dest += (src1 >= src2) |

Float comparison + accumulate (dest += result):

| Opcode | Value | Operation |
|--------|-------|-----------|
| FCMP_EQ_ACC | 0x48 | dest += (src1 == src2) |
| FCMP_NE_ACC | 0x49 | dest += (src1 != src2) |
| FCMP_LT_ACC | 0x4A | dest += (src1 < src2) |
| FCMP_LE_ACC | 0x4B | dest += (src1 <= src2) |
| FCMP_GT_ACC | 0x4C | dest += (src1 > src2) |
| FCMP_GE_ACC | 0x4D | dest += (src1 >= src2) |

Accumulate variants are useful for loop counters — the predicate returns the comparison result while simultaneously incrementing the destination, enabling patterns like `p_icmp_lt_acc(field_val("count"), uint_val(10), field_val("count"))` which tests `count < 10` and increments `count` in one operation.

### Composite Predicate Helpers

**Range check:**

```lua
-- Returns true if low <= src <= high
-- Requires 2 scratch slots for intermediates
p_icmp_in_range(src_fn, low_fn, high_fn, dest_fn, scratch1_fn, scratch2_fn)
p_fcmp_in_range(src_fn, low_fn, high_fn, dest_fn, scratch1_fn, scratch2_fn)
```

Expands to three predicate quads: `scratch1 = (low <= src)`, `scratch2 = (src <= high)`, `dest = scratch1 && scratch2`.

---

## Constraints and Rules

**Do not mix `stack_push`/`stack_pop` with `stack_tos` in the same sequence.** Push and pop modify SP, which invalidates TOS-relative offsets. Use push/pop only for call setup and return retrieval, after all scratch-based computation is complete.

**`se_frame_allocate` locals are volatile.** The frame is recreated every tick. Use blackboard fields for persistent state.

**`se_call` locals are persistent.** The frame created by `SE_STACK_FRAME_INSTANCE` lives across ticks until the call body completes and the terminate event fires.

**Return value pop order is reversed.** `return_vars = {2, 3}` pushes local 2 then local 3. First `stack_pop` gets local 3 (top), second gets local 2.

**Quad helpers return closures.** Always invoke with `()`:
```lua
quad_fadd(a, b, c)()   -- correct: emits the node
quad_fadd(a, b, c)     -- wrong: returns unevaluated closure
```

**`se_frame_allocate` is a pipeline node.** It does not create a function boundary. To create a function boundary (for FUNCTION_HALT propagation), wrap in `se_fork_join` or `se_function_interface`.

