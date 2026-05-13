# stack_test_equations — Expression Compiler Integration Test

## Purpose

Validates the expression compiler (`s_expr_compiler.lua` v1.1) by rewriting the
original `stack_test.lua` (which used raw quad helper calls) to use `quad_expr`,
`quad_multi`, and `quad_pred` with C-like expression syntax. Exercises the full
compilation pipeline: parsing, type inference, constant folding, scratch
allocation, and code generation for both blackboard fields (`@field`) and
stack frame locals/scratch.

## Compiler Features Exercised

| Feature | Where Tested |
|---------|-------------|
| `quad_expr` with `@field` destination | Actions 1, 2 |
| `quad_expr` with local destination | Action 3 (call body) |
| `quad_expr` pure assignment (leaf → dest mov) | `@float_val_1 = @float_val_3` |
| `quad_multi` semicolon-separated statements | Action 3 outer frame |
| `quad_expr` subexpression with parentheses | `r0 = (p0 + 1.0 + 5.0) - 2.0` |
| Float type inference from literal suffix | `1.0`, `5.0`, `2.0` → `quad_fadd` |
| Integer type inference (no suffix) | `1`, `5`, `2` → `quad_iadd` |
| Typed `frame_vars` with `:float` / `:int` | All actions |
| Scratch variable allocation | `{"t0", "t1"}` passed to compiler |
| Constant folding | `1.0 + 5.0` folded to `6.0` at compile time |
| `stack_push` / `stack_pop` parameter passing | Action 3 call setup/return |
| `se_call` with `frame_vars` inside call body | Action 3 |

## Blackboard Record

```
stack_test_state:
    int_val_1    int32
    int_val_2    int32
    int_val_3    int32
    uint_val_1   uint32
    uint_val_2   uint32
    uint_val_3   uint32
    float_val_1  float
    float_val_2  float
    float_val_3  float
    loop_count   uint32
```

## Test Actions

### Action 1 — Integer Arithmetic (`quad_expr` with `@field`)

Loops 100 times within `se_frame_allocate(0, 5, 5)`. Each iteration:

```
@int_val_2 = @int_val_1 + 1       (overwritten next line)
@int_val_2 = @int_val_1 + 5
@int_val_3 = @int_val_2 - 2
@int_val_1 = @int_val_3            (pure assignment, emits quad_mov)
```

Net per iteration: `int_val_1 += 3`. After 100 iterations: `int_val_1 = 300`.

The `+ 1` and `+ 5` literals have no decimal point, so the compiler selects
`quad_iadd` / `quad_isub` (integer ops). The final statement `@int_val_1 = @int_val_3`
is a leaf-to-dest assignment that generates a `quad_mov`.

### Action 2 — Float Arithmetic (`quad_expr` with `@field`)

Loops 10 times within `se_frame_allocate(0, 5, 5)`. Each iteration:

```
@float_val_2 = @float_val_1 + 1.0     (overwritten next line)
@float_val_2 = @float_val_1 + 5.0
@float_val_3 = @float_val_2 - 2.0
@float_val_1 = @float_val_3
```

Net per iteration: `float_val_1 += 3.0`. After 10 iterations: `float_val_1 = 30.0`.

The `1.0`, `5.0`, `2.0` literals trigger float type inference, selecting
`quad_fadd` / `quad_fsub`.

### Action 3 — Call/Return with `quad_multi` and `quad_expr`

Loops 10 times using `p_icmp_lt_acc` as the loop predicate. Each iteration:

**Outer frame** (`se_frame_allocate(0, 5, 5)`):

```lua
quad_multi("a = @float_val_1 + 1.0; @float_val_2 = a + 5.0; b = @float_val_2", v, {"t0","t1"})
quad_multi("@float_val_3 = b - 2.0; @float_val_1 = @float_val_3", v, {"t0"})
```

This compiles 5 statements into a sequence of `quad_fadd`, `quad_fsub`, and
`quad_mov` operations. The local variables `a` and `b` are stack locals that
persist within the frame. Net effect: `float_val_1 += 4.0` per iteration.

**Call setup** (stack_push/stack_pop):

```
push float_val_1       -- parameter p0
push float_val_2       -- parameter p1
```

**Call body** (`se_call(2, 2, 3, {2, 3})`):

```lua
quad_expr("r0 = (p0 + 1.0 + 5.0) - 2.0", cv, {"ct0", "ct1"})   -- r0 = p0 + 4.0
quad_expr("r1 = p1 * 2.0", cv, {"ct0"})                          -- r1 = p1 * 2.0
```

The parenthesized subexpression `(p0 + 1.0 + 5.0)` is compiled with constant
folding: `1.0 + 5.0` folds to `6.0`, reducing to `(p0 + 6.0) - 2.0` which
uses one scratch instead of two.

**Return retrieval** (LIFO order):

```
pop → v.e    (gets r1 = p1 * 2.0)
pop → v.f    (gets r0 = p0 + 4.0)
store v.e → float_val_2
store v.f → float_val_3
```

## Expected Values (Action 3)

| Iter | float_val_1 (start) | float_val_2 | float_val_3 | float_val_1 (end) | call r0 (fv3) | call r1 (fv2) |
|------|---------------------|-------------|-------------|-------------------|---------------|---------------|
| 1    | 0.0                 | 6.0         | 4.0         | 4.0               | 8.0           | 12.0          |
| 2    | 4.0                 | 10.0        | 8.0         | 8.0               | 12.0          | 20.0          |
| 3    | 8.0                 | 14.0        | 12.0        | 12.0              | 16.0          | 28.0          |
| ...  | +4.0/iter           | fv1+6       | fv1+4       | fv3               | fv1+8         | fv2*2         |
| 9    | 32.0                | 38.0        | 36.0        | 36.0              | 40.0          | 76.0          |
| 10   | 36.0                | 42.0        | 40.0        | 40.0              | 44.0          | 84.0          |

Formulas:
- `float_val_2 = float_val_1 + 1.0 + 5.0 = float_val_1 + 6.0`
- `float_val_3 = float_val_2 - 2.0 = float_val_1 + 4.0`
- `float_val_1 (end) = float_val_3`
- `call r0 = float_val_1(end) + 4.0`
- `call r1 = float_val_2 * 2.0`

## Execution Structure

```
se_function_interface
├── init: int_val_1=0, float_val_1=0
├── action_1: se_while(100) → se_frame_allocate → integer quad_expr
├── action_2: se_while(10) → se_frame_allocate → float quad_expr
├── reinit: int_val_1=0, float_val_1=0
├── action_3: se_while(10) → se_frame_allocate
│   ├── quad_multi (outer computation)
│   ├── stack_push × 2
│   ├── se_call(2,2,3,{2,3})
│   │   └── quad_expr × 2 (call body)
│   ├── stack_pop × 2
│   └── quad_mov × 2 (store results)
└── se_return_terminate
```

## Tick Budget

Each iteration of actions 1, 2, and 3 includes `se_tick_delay(5)`, consuming
5 ticks for the delay plus 1 tick each for frame push and the computation tick.
Total ticks: approximately 100×7 + 10×7 + 10×7 = 840.

## Compiler Bugs Found and Fixed (v1.0 → v1.1)

1. **`@field` destinations not supported**: `quad_expr("@int_val_2 = @int_val_1 + 5")`
   failed because `ref_for()` didn't recognize the `@` prefix. Fixed by adding
   field detection in `ref_for()`: names starting with `@` delegate to `field_val()`.

2. **Leaf-to-dest assignment silently dropped**: `quad_expr("b = @float_val_2")`
   generated zero operations because `emit()` short-circuited on leaf nodes
   without checking `dest_name`. Fixed by emitting `quad_mov` when a leaf has
   a destination.

3. **Destination regex too restrictive**: The pattern `[%w_@.]+` was updated to
   `@?[%w_.]+` to properly capture `@field.path` as a destination.

4. **Compound assignment patterns**: Added `@field` variants so `@counter += 1`
   desugars correctly to `@counter = @counter + (1)`.


