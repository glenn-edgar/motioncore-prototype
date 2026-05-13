# s_expr_compiler — Expression Compiler for ChainTree Quad Operations

## Overview

`s_expr_compiler.lua` is a compile-time expression compiler that translates C-like
arithmetic expressions into sequences of quad and p_quad operations for the
S-Expression DSL engine. It runs inside LuaJIT during tree compilation, not at
runtime on the target. The generated quad operations become binary tree nodes
that execute on platforms ranging from 32KB ARM Cortex-M microcontrollers to
multi-gigabyte servers.

The compiler replaces hand-written chains of `quad_iadd`, `quad_fsub`, `quad_mov`,
etc. with readable infix expressions while producing identical output.

```lua
-- Before: 4 lines of raw quad calls
quad_iadd(field_val("x"), uint_val(5), field_val("tmp"))()
quad_imul(field_val("tmp"), field_val("y"), field_val("tmp"))()
quad_isub(field_val("tmp"), uint_val(2), field_val("result"))()

-- After: 1 line
quad_expr("@result = (@x + 5) * @y - 2", v, {"t0", "t1"})()
```

## Architecture

```
Expression string
       │
       ▼
   Tokenizer ──────► Token stream
       │
       ▼
   Parser (Pratt) ──► AST
       │
       ▼
   Constant Folder ─► Optimized AST
       │
       ▼
   Type Inference ──► float/int decision per node
       │
       ▼
   Code Generator ──► {fn, args} operation list
       │
       ▼
   Closure ─────────► Emits quad DSL nodes when called
```

All phases run at DSL compile time in LuaJIT. The returned closure, when
invoked with `()`, emits binary tree node parameters into the compilation
stream. No parsing or AST traversal happens at runtime on the target.

## API Reference

### frame_vars(locals, scratch)

Creates a variable table mapping names to stack frame parameter emitters.
Must be called inside `se_frame_allocate` where the stack frame exists.

```lua
local v = frame_vars(
    {"x:float", "y:float", "count:int"},   -- locals (stack_local)
    {"t0:float", "t1:int", "t2:float"}     -- scratch (stack_tos)
)
```

Each declaration is `"name"` or `"name:type"` where type is `float` or `int`
(default: `int`). The type annotation drives the compiler's type inference
for operations involving that variable.

Returns a table where `v[name]` is a parameter-emitting closure, plus metadata
fields `v[name.."_is_float"]`, `v[name.."_type"]`, `v._local_count`, and
`v._scratch_count`.

**Important:** This `frame_vars` replaces the simpler version in
`s_engine_helpers.lua`. The helpers file must not redefine `frame_vars`
after loading the compiler, or type annotations will be lost.

### quad_expr(expr_str, vars, scratch_names)

Compiles a single assignment expression into quad operations.

```lua
quad_expr("dest = expression", v, {"t0", "t1"})()
```

**Parameters:**
- `expr_str` — Assignment in the form `"dest = expr"` or compound `"dest += expr"`.
  Destination can be a variable name or `@field_name`.
- `vars` — Table from `frame_vars()`.
- `scratch_names` — List of scratch variable names from `vars` for intermediate results.

**Returns:** A closure. Call it with `()` to emit the compiled quad operations.

**Examples:**
```lua
-- Simple field arithmetic
quad_expr("@result = @x + 5", v, {"t0"})()

-- Float with parenthesized subexpression
quad_expr("@out = (@a + 1.0) * sqrt(@b)", v, {"t0", "t1", "t2"})()

-- Variable-to-variable
quad_expr("result = x + y", v, {"t0"})()

-- Pure assignment (emits quad_mov)
quad_expr("@field_a = @field_b", v, {})()

-- Compound assignment
quad_expr("@counter += 1", v, {"t0"})()

-- Constant folding: compiles to single quad_mov of 14
quad_expr("result = 2 + 3 * 4", v, {})()
```

### quad_pred(expr_str, vars, scratch_names)

Compiles a boolean expression into p_quad (predicate) operations for use
as conditions in `se_if_then`, `se_while`, and other control structures.

```lua
se_while(quad_pred("@count < 100", v, {}), function()
    -- loop body
end)

se_if_then(quad_pred("@x > 5.0 && @y <= 10.0", v, {"t0", "t1"}), function()
    -- then branch
end)
```

Comparisons emit `p_icmp_*` / `p_fcmp_*` and logical connectives emit
`p_log_and` / `p_log_or` instead of the oneshot `quad_*` variants.

### quad_multi(expr_str, vars, scratch_names)

Compiles multiple semicolon-separated assignments as a single batch.
Variables assigned in earlier statements are available to later ones.

```lua
quad_multi(
    "a = @sensor + 1.0; "  ..
    "@filtered = a * 0.5; " ..
    "b = @filtered - @offset",
    v, {"t0", "t1"}
)()
```

Each statement is compiled independently via `quad_expr`, so scratch is
reused across statements. Local variables (`a`, `b`) persist within the
enclosing `se_frame_allocate` scope.

### quad_pred_acc(conditions, vars, count_var)

Compiles multiple independent comparison conditions with accumulate semantics.
Each true condition increments a counter variable. Useful for multi-condition
evaluation where you need a count of passing conditions rather than a
short-circuit boolean.

```lua
quad_pred_acc(
    {"@temp > 100", "@pressure > 50.0", "@flow_rate < 2.0"},
    v, "alarm_count"
)()
-- alarm_count = number of true conditions (0–3)
```

**Restrictions:** Each condition must be a single comparison with leaf operands
(variables, fields, or constants). No compound expressions in individual conditions.

### quad_expr_debug / quad_pred_debug

Debug variants that print the AST before and after constant folding, the
generated operation sequence, and scratch usage, then return a working closure.

```lua
quad_expr_debug("result = (x + 5.0) * y - 2.0", v, {"t0", "t1"})()
-- Output:
--   === quad_expr_debug: result = (x + 5.0) * y - 2.0 ===
--     AST (before fold): (((x + 5) * y) - 2)
--     AST (after fold):  (((x + 5) * y) - 2)
--     1: quad_fadd(<ref>, <ref>, <ref>)
--     2: quad_fmul(<ref>, <ref>, <ref>)
--     3: quad_fsub(<ref>, <ref>, <ref>)
--     scratch used: 2 / 2
--   ===
```

## Expression Syntax

### Operand Types

| Syntax | Type | Example | Emits |
|--------|------|---------|-------|
| `@name` | Blackboard field | `@sensor_val` | `field_val("sensor_val")` |
| `name` | Frame variable | `x`, `t0` | `vars[name]` closure |
| `42` | Integer literal | `42`, `0` | `uint_val(42)` |
| `-5` | Negative integer | `-5` | `int_val(-5)` |
| `3.14` | Float literal | `3.14`, `0.0` | `float_val(3.14)` |
| `3f` | Float (suffix) | `3f`, `42f` | `float_val(3.0)` |

### Operators (by precedence, low to high)

| Prec | Operators | Category | Result Type |
|------|-----------|----------|-------------|
| 1 | `\|\|` | Logical OR | int |
| 2 | `&&` | Logical AND | int |
| 3 | `\|` | Bitwise OR | int |
| 4 | `^` | Bitwise XOR | int |
| 5 | `&` | Bitwise AND | int |
| 6 | `==` `!=` | Equality | int (0/1) |
| 7 | `<` `<=` `>` `>=` | Relational | int (0/1) |
| 8 | `<<` `>>` | Shift | int |
| 9 | `+` `-` | Additive | float if either operand float |
| 10 | `*` `/` `%` | Multiplicative | float if either operand float |
| 11 | `-x` `!x` `~x` | Unary (prefix) | inherits / int / int |

All operators are left-associative. Parentheses override precedence.

### Compound Assignment

Supported for both variables and `@field` destinations:

```
+=  -=  *=  /=  %=  &=  |=  ^=  <<=  >>=
```

Desugared at compile time: `@x += 5` becomes `@x = @x + (5)`.

### Built-in Functions

| Function | Arity | Float | Int | Notes |
|----------|-------|-------|-----|-------|
| `sqrt(x)` | 1 | ✓ | — | Always float |
| `abs(x)` | 1 | ✓ | ✓ | |
| `sin(x)` | 1 | ✓ | — | Always float |
| `cos(x)` | 1 | ✓ | — | Always float |
| `tan(x)` | 1 | ✓ | — | Always float |
| `asin(x)` | 1 | ✓ | — | Always float |
| `acos(x)` | 1 | ✓ | — | Always float |
| `atan(x)` | 1 | ✓ | — | Always float |
| `exp(x)` | 1 | ✓ | — | Always float |
| `log(x)` | 1 | ✓ | — | Always float |
| `log10(x)` | 1 | ✓ | — | Always float |
| `log2(x)` | 1 | ✓ | — | Always float |
| `sinh(x)` | 1 | ✓ | — | Always float |
| `cosh(x)` | 1 | ✓ | — | Always float |
| `tanh(x)` | 1 | ✓ | — | Always float |
| `neg(x)` | 1 | ✓ | ✓ | |
| `min(a,b)` | 2 | ✓ | ✓ | |
| `max(a,b)` | 2 | ✓ | ✓ | |
| `pow(a,b)` | 2 | ✓ | — | Always float |
| `atan2(a,b)` | 2 | ✓ | — | Always float |

Functions without an integer variant (`fn_i`) always produce float results
regardless of argument types.

## Type Inference

The compiler infers float vs. integer for each operation bottom-up through
the AST. No explicit casts are needed.

**Rules:**

1. Numeric literals: float if they contain `.` or `f` suffix, otherwise int.
2. Variables: float if `frame_vars` declared with `:float`, otherwise int.
3. Field references (`@field`): default to int. Use float literals on the
   other side of the operation to force float dispatch.
4. Arithmetic (`+` `-` `*` `/` `%`): float if **either** operand infers float.
5. Bitwise (`&` `|` `^` `<<` `>>`): always int.
6. Comparison (`==` `!=` `<` `<=` `>` `>=`): always int result (0 or 1),
   but operand comparison uses `fcmp` if either side is float.
7. Logical (`&&` `||` `!`): always int.
8. Function calls: float if function has no `fn_i` variant, otherwise
   follows argument types.

**Example type dispatch:**
```lua
quad_expr("@out = @x + 5", v, {"t0"})()     -- int + int → quad_iadd
quad_expr("@out = @x + 5.0", v, {"t0"})()   -- int + float → quad_fadd
quad_expr("@out = @x + 5f", v, {"t0"})()    -- int + float → quad_fadd
```

**Tip:** When operating on float blackboard fields, use float literals
(`1.0` not `1`) to ensure the compiler selects float operations. The
compiler has no access to the record schema at compile time, so `@field`
alone does not carry type information.

## Constant Folding

The compiler evaluates constant subexpressions at compile time using LuaJIT
math, reducing them to single literal values. This eliminates runtime
operations and can reduce scratch usage.

```lua
-- Folds to: quad_mov(14, result)
quad_expr("result = 2 + 3 * 4", v, {})()

-- Folds 1.0 + 5.0 → 6.0, then compiles (p0 + 6.0) - 2.0
-- Uses 1 scratch instead of 2
quad_expr("r0 = (p0 + 1.0 + 5.0) - 2.0", cv, {"ct0"})()

-- Folds sqrt(2.0) at compile time
quad_expr("scale = sqrt(2.0) * @amplitude", v, {"t0"})()
```

Folding applies to all operators, all unary/binary math functions, and
propagates through nested expressions. Division by zero and `sqrt` of
negative values are not folded (left for runtime).

## Scratch Variables

Complex expressions need temporary storage for intermediate results. The
compiler allocates scratch variables from the list you provide, in order.
Scratch is freed after the parent operation consumes the intermediate, so
deep expression trees may reuse slots.

**How many scratch slots do I need?**

For a simple binary operation on two leaves: 0 extra (result goes to dest).
Each level of nesting where both sides are non-leaf adds 1 scratch.

```lua
-- 0 scratch: both operands are leaves
quad_expr("@out = @a + @b", v, {})()

-- 0 scratch: one operand is leaf, result goes to dest
quad_expr("@out = @a + 5", v, {})()

-- 1 scratch: (a+5) needs a temp, then temp*b goes to dest
quad_expr("@out = (@a + 5) * @b", v, {"t0"})()

-- 2 scratch: both sides of * are non-leaf
quad_expr("@out = (@a + 5) * (@b - 2)", v, {"t0", "t1"})()

-- Function call: argument computation may need scratch
quad_expr("@out = sqrt(@a + @b)", v, {"t0"})()
```

If you provide too few scratch variables, the compiler errors at compile time
with a message showing how many were needed. Over-allocating is safe but
wastes stack space on constrained targets.

## Integration

### Loading

The compiler is loaded by `s_engine_helpers.lua` at startup:

```lua
dofile("s_engine_equation.lua")  -- loads s_expr_compiler.lua
```

This defines `frame_vars`, `quad_expr`, `quad_pred`, `quad_multi`,
`quad_pred_acc`, and the debug variants as globals.

### frame_vars Conflict

**Critical:** `s_engine_helpers.lua` historically defines its own simpler
`frame_vars` function (without type annotations) that overwrites the
compiler's version. This must be removed from the helpers file. The
compiler's `frame_vars` handles both typed (`"x:float"`) and untyped
(`"x"`) declarations, defaulting untyped to `int`.

Without type annotations, the compiler cannot determine whether local/scratch
variables are float or int, and will default all operations to integer.
Field references (`@field`) are unaffected since they bypass the vars table.

### Usage Pattern

```lua
se_frame_allocate(0, num_locals, num_scratch, function()
    local v = frame_vars(
        {"a:float", "b:float", "result:float"},
        {"t0:float", "t1:float"}
    )

    -- Arithmetic assignment
    quad_expr("a = @sensor_1 + @offset", v, {"t0"})()

    -- Multi-statement batch
    quad_multi("b = a * 0.95; @output = b + @bias", v, {"t0"})()

    -- Predicate for control flow
    se_if_then(quad_pred("@output > @threshold", v, {}), function()
        quad_expr("@alarm = 1", v, {})()
    end)

    -- Compound assignment
    quad_expr("@counter += 1", v, {"t0"})()
end)
```

### Inside se_call

Call bodies create their own `frame_vars` scope. Parameters are locals at
the start of the frame, return values are locals at designated indices.

```lua
se_call(2, 2, 3, {2, 3}, {
    function()
        local cv = frame_vars(
            {"p0:float", "p1:float", "r0:float", "r1:float"},
            {"ct0:float", "ct1:float", "ct2:float"}
        )
        quad_expr("r0 = (p0 + 1.0 + 5.0) - 2.0", cv, {"ct0", "ct1"})()
        quad_expr("r1 = p1 * 2.0", cv, {"ct0"})()
        se_return_pipeline_terminate()
    end
})
```

## Compiler Internals

### AST Node Types

| Tag | Fields | Produced By |
|-----|--------|-------------|
| `num` | `value`, `is_float` | Numeric literals |
| `var` | `name` | Identifiers not followed by `(` |
| `field` | `name` | `@identifier` tokens |
| `binop` | `op`, `left`, `right` | Binary operators |
| `unop` | `op`, `operand` | Unary `-`, `!`, `~` |
| `call` | `name`, `args` | `ident(args)` |

### Code Generation

The `emit(node, dest_name)` function recursively walks the AST:

1. **Leaf nodes** with `dest_name`: emit `quad_mov` from source to destination.
2. **Leaf nodes** without `dest_name`: return the parameter-emitting closure directly (no quad op).
3. **Binary operations**: emit children first (allocating scratch for non-leaf intermediates),
   then emit the operation targeting `dest_name` or a fresh scratch slot.
4. **Function calls**: emit argument sub-trees, then emit the function quad with
   all argument refs followed by the destination ref.

The scratch allocator is a simple stack: `alloc_scratch()` increments an index
and returns the next name from the scratch list, `free_scratch()` decrements it.
A high-water mark tracks peak usage for the debug output.

### Operation List

The code generator produces a list of `{fn = "quad_name", args = {ref, ref, ...}}`
entries. The outer closure iterates this list, resolving each `fn` from `_G` and
calling it with the argument refs. Each quad function returns a closure that is
immediately invoked with `()` to emit the tree node.

## Version History

### v1.1
- `@field_name` works as assignment destination in `quad_expr` and `quad_multi`
- `ref_for()` detects `@` prefix and delegates to `field_val()`
- Leaf-to-dest assignment emits `quad_mov` (previously silently dropped)
- Compound assignment patterns extended for `@field` destinations
- Destination regex updated to `@?[%w_.]+`

### v1.0
- Initial release with full expression parsing, type inference, constant folding
- `quad_expr`, `quad_pred`, `quad_multi`, `quad_pred_acc` API
- Debug variants for AST inspection
- Typed `frame_vars` with `:float` / `:int` annotations
