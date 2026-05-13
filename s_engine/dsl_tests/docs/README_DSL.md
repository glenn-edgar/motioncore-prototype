# S-Expression Engine DSL v5.3

A LuaJIT-based domain-specific language for defining behavior trees, state machines, and sequential control flows that compile to zero-copy binary modules for embedded systems.

## Overview

The S-Expression Engine DSL provides a high-level Lua API for defining structured control flow that compiles to efficient binary bytecode. The system targets embedded platforms from 32KB ARM Cortex-M microcontrollers to full ARM64/AMD64 systems with 8GB+ RAM.

**Key Features:**
- Zero-copy binary loading (cast pointer directly from ROM)
- Two binary formats: 32-bit (8-byte params) and 64-bit (16-byte params)
- FNV-1a 32-bit hash-based function dispatch
- Typed blackboard fields with compile-time layout
- Nested record support with embedded structures
- Dict/list/array/tuple data structures
- Composable predicate API
- Expression compiler with C-like syntax, constant folding, and type inference
- Three-tier result codes (application, function, pipeline)
- Stack frame management with compile-time bounds checking
- Parent offset tracking (v5.3) for O(1) upward navigation in binary param stream

---

## File Structure

### Core DSL Files

| File | Purpose |
|------|---------|
| `s_expr_dsl.lua` | Main DSL library — type system, hash functions, module/record/tree/call/param APIs |
| `s_expr_generators.lua` | C header and binary module generators (loaded by s_expr_dsl.lua) |
| `s_expr_debug.lua` | Debug output generation (loaded by s_expr_dsl.lua) |
| `s_compile.lua` | Command-line compiler that processes DSL files |
| `s_engine_helpers.lua` | Modular loader — includes all helper sub-modules in dependency order |
| `s_expr_compiler.lua` | Expression compiler — parses C-like expressions into quad/p_quad operations |

### Helper Sub-Modules (se_helpers_dir/)

Loaded by `s_engine_helpers.lua` in dependency order:

| File | Purpose |
|------|---------|
| `s_engine_equation.lua` | Equation support (loaded first) |
| `se_field_validation.lua` | Compile-time field validation (ptr64, numeric, type checks) |
| `se_result_codes.lua` | Application, function, and pipeline result code emitters |
| `se_predicates.lua` | Predicate builder, composite/leaf predicates, typed value emission |
| `se_oneshot.lua` | Log, field set/inc/dec, push stack, external field set |
| `se_control_flow.lua` | Sequence, if/then/else, fork, while, cond, trigger-on-change |
| `se_timing_events.lua` | Tick/time delays, wait, verify, event queueing |
| `se_state_machine.lua` | State machine dispatch, event dispatch, case helpers |
| `se_dictionary.lua` | Dictionary/JSON loading, string-path and hash-path extraction |
| `se_quad_ops.lua` | Quad operations — arithmetic, comparison, logical, math, trig |
| `se_p_quad_ops.lua` | Predicate quad operations — boolean comparisons, accumulate, range |
| `se_stack_frame.lua` | Stack frame instance, se_call wrapper, frame allocation |
| `se_function_dict.lua` | Function dictionary load/exec, tree spawning, function pointers |
| `se_chain_tree.lua` | Chain tree functions |

### Reference DSL Files

| File | Purpose |
|------|---------|
| `s_expr_tutorial.lua` | Basic record types, field access, arrays, constants |
| `state_machine.lua` | State machine patterns using `se_state_machine` and `se_field_dispatch` |

### Generated Output Files

| File Pattern | Content |
|--------------|---------|
| `<base>_records.h` | C struct definitions for records |
| `<base>.h` | Module header with hashes and string table |
| `<base>_debug.h` | Debug hash-to-name mappings |
| `<base>_user_functions.h` | User function prototypes |
| `<base>_user_registration.c` | Function registration code |
| `<base>_32.bin` / `<base>_64.bin` | Binary module for runtime loading |
| `<base>_bin_32.h` / `<base>_bin_64.h` | Binary as C array for ROM embedding |
| `<base>_dump_32.h` / `<base>_dump_64.h` | Human-readable parameter dump |

---

## DSL Structure

### Module Definition

Every DSL file follows this structure:

```lua
local M = require("s_expr_dsl")
local mod = start_module("module_name")
use_32bit()  -- or use_64bit()
set_debug(true)  -- optional

-- Events (optional)
EVENTS({
    SENSOR_READY = 0x0001,
    TIMEOUT      = 0x0002,
})

-- Records (data structures)
RECORD("my_record")
    FIELD("counter", "int32")
    FIELD("temperature", "float")
END_RECORD()

-- Constants (pre-initialized records)
CONST("defaults", "my_record")
    VALUE("counter", 0)
    VALUE("temperature", 20.0)
END_CONST()

-- Trees (behavior trees)
start_tree("my_tree")
    use_record("my_record")
    use_defaults("defaults")
    -- tree content (exactly ONE top-level node required in v5.3)
    se_sequence(function()
        -- children
    end)
end_tree("my_tree")

return end_module(mod)
```

**v5.3 tree validation:** Each tree must have exactly one top-level node. Multiple top-level nodes produce a detailed error message suggesting appropriate containers (SE_SEQUENCE, SE_FORK, SE_STATE_MACHINE, SE_CHAIN_FLOW).

---

## Record Types

### Scalar Fields

```lua
RECORD("ScalarDemo")
    FIELD("counter", "int32")      -- 32-bit signed integer
    FIELD("flags", "uint32")       -- 32-bit unsigned integer
    FIELD("temperature", "float")  -- 32-bit float
    FIELD("timestamp", "int64")    -- 64-bit signed integer
    FIELD("checksum", "uint64")    -- 64-bit unsigned integer
    FIELD("precise", "double")     -- 64-bit float
END_RECORD()
```

**Note:** Sub-32-bit types (`int8`, `uint8`, `int16`, `uint16`, `bool`, `char`) are NOT allowed in `FIELD()` because 32-bit writes would corrupt adjacent fields. Use `CHAR_ARRAY()` for strings.

### Array Fields

```lua
RECORD("ArrayDemo")
    CHAR_ARRAY("name", 32)           -- Character buffer (min 4 bytes)
    INT32_ARRAY("int_values", 4)     -- Array of 4 int32
    FLOAT32_ARRAY("float_values", 4) -- Array of 4 float
END_RECORD()
```

### Embedded Records

```lua
RECORD("Vector3")
    FIELD("x", "float")
    FIELD("y", "float")
    FIELD("z", "float")
END_RECORD()

RECORD("Transform")
    FIELD("position", "Vector3")  -- Embedded record
    FIELD("rotation", "Vector3")
    FIELD("scale", "float")
END_RECORD()
```

### Pointer Fields

```lua
RECORD("LinkedNode")
    FIELD("value", "int32")
    FIELD("pad", "uint32")
    PTR64_FIELD("next", "LinkedNode")  -- Always 64-bit storage
    PTR64_FIELD("data", "void")
END_RECORD()
```

---

## Tree Definition

### Basic Structure

```lua
start_tree("tree_name")
    use_record("blackboard_record")      -- Associate blackboard type
    use_defaults("constant_name")        -- Optional: initialize from constant

    -- Exactly ONE top-level node (v5.3 requirement)
    se_sequence(function()
        -- tree content
    end)

end_tree("tree_name")
```

### Call Types

| Function | Type | Returns | Use Case |
|----------|------|---------|----------|
| `o_call("NAME")` | Oneshot | void | Fire-once initialization |
| `io_call("NAME")` | Init-oneshot | void | Survives reset, runs once |
| `m_call("NAME")` | Main | result code | Primary execution |
| `p_call("NAME")` | Predicate | bool | Condition checking |
| `pt_m_call("NAME")` | Pointer main | result code | Allocates pointer slot |
| `p_call_composite("NAME")` | Composite pred | bool | Multi-child predicate |

### Parameter Functions

```lua
local c = m_call("FUNCTION_NAME")
    int(42)              -- 32/64-bit signed integer
    uint(0xDEADBEEF)     -- 32/64-bit unsigned integer
    flt(3.14159)         -- 32/64-bit float
    str("hello")         -- String (indexed in string table)
    str_ptr("world")     -- String pointer
    str_hash("key")      -- Pre-computed FNV-1a hash
    field_ref("counter") -- Field offset reference
    nested_field_ref("position.x")  -- Nested field path
    const_ref("defaults")           -- Constant reference
    stack_local(0)       -- Stack frame local variable
    stack_tos(0)         -- Stack top-of-stack offset
    stack_push()         -- Push onto stack
    stack_pop()          -- Pop from stack
    null_param()         -- Null/unused parameter
end_call(c)
```

### Data Structures

```lua
-- List
local l = list_start("my_list")
    int(1)
    int(2)
    int(3)
list_end(l)

-- Dictionary
local d = dict_start("my_dict")
    local k1 = key("name")
        str("value")
    key_end(k1)
dict_end(d)

-- Array
local a = array_start("my_array")
    int(10)
    int(20)
array_end(a)

-- Tuple
local t = tuple_start("my_tuple")
    int(1)
    str("two")
tuple_end(t)

-- JSON shorthand
json({ key1 = "value1", key2 = 123, nested = { a = 1, b = 2 } })
json_hash({ key1 = "value1" })  -- Uses hash keys for faster lookup
```

### Inline Constructors for JSON/Dict Values

These helpers create structured values inside `json()` or dictionary contexts:

```lua
json({
    action = main("MY_FUNC", 42, 3.14),     -- Inline m_call
    init   = oneshot("SETUP"),               -- Inline o_call
    check  = pred("IS_READY"),               -- Inline p_call
    target = field("counter"),               -- Field reference
    path   = nfield("position.x"),           -- Nested field reference
    config = const("defaults"),              -- Constant reference
    lookup = hash("my_key"),                 -- String hash
    items  = list(1, 2, 3),                  -- Explicit list
    pair   = tuple("name", 42),              -- Explicit tuple
})
```

---

## Helper Functions

### Result Codes

Three tiers of result codes, each with six variants:

```lua
-- Application level (affect the current tree tick)
se_return_continue()        -- SE_CONTINUE (0)
se_return_halt()            -- SE_HALT (1)
se_return_terminate()       -- SE_TERMINATE (2)
se_return_reset()           -- SE_RESET (3)
se_return_disable()         -- SE_DISABLE (4)
se_return_skip_continue()   -- SE_SKIP_CONTINUE (5)

-- Function level (affect enclosing function)
se_return_function_continue()
se_return_function_halt()
se_return_function_terminate()
se_return_function_reset()
se_return_function_disable()
se_return_function_skip_continue()

-- Pipeline level (affect enclosing pipeline/chain)
se_return_pipeline_continue()
se_return_pipeline_halt()
se_return_pipeline_terminate()
se_return_pipeline_reset()
se_return_pipeline_disable()
se_return_pipeline_skip_continue()
```

### Control Flow

```lua
-- Execute children in order; stop on non-CONTINUE
se_sequence(function()
    se_log("Step 1")
    se_tick_delay(100)
    se_log("Step 2")
end)

-- Execute once (survives reset tracking)
se_sequence_once(function()
    se_log("Runs exactly once")
end)

-- Conditional execution
se_if_then(
    function() se_field_gt("temperature", 100) end,  -- predicate
    function() se_log("Too hot!") end                 -- then branch
)

se_if_then_else(
    function() se_field_eq("mode", 1) end,  -- predicate
    function() se_log("Mode 1") end,        -- then
    function() se_log("Other mode") end     -- else
)

-- Edge detection
se_on_rising_edge(
    function() se_field_gt("temp", 50) end,
    function() se_log("Temperature crossed threshold") end
)

se_on_falling_edge(
    function() se_field_gt("temp", 50) end,
    function() se_log("Temperature dropped below threshold") end
)

-- Parallel execution
se_fork(
    function() se_log("Branch A") end,
    function() se_log("Branch B") end
)

-- Fork and wait for all children to complete
se_fork_join(
    function() se_tick_delay(100) end,
    function() se_tick_delay(200) end
)

-- Pipeline processing
se_chain_flow(
    function() --[[ stage 1 ]] end,
    function() --[[ stage 2 ]] end
)

-- While loop
se_while(
    function() se_field_lt("counter", 10) end,  -- condition
    function() se_increment_field("counter", 1) end
)

-- Lisp-style conditional dispatch
se_cond({
    se_cond_case(
        function() se_field_eq("mode", 0) end,
        function() se_log("Idle") end
    ),
    se_cond_case(
        function() se_field_eq("mode", 1) end,
        function() se_log("Running") end
    ),
    se_cond_default(
        function() se_log("Unknown mode") end
    ),
})

-- Function interface wrapper
se_function_interface(function()
    -- body
end)
```

### Oneshot Operations

```lua
se_log("Debug message")                    -- Log string
se_log_slot_integer("Counter: ", "count")  -- Log with int field value
se_log_slot_float("Temp: ", "temperature") -- Log with float field value

se_set_field("counter", 42)               -- Set field value (oneshot)
se_i_set_field("state", 0)                -- Set field on init only (io-oneshot)
se_set_hash_field("mode", "running")       -- Set field to hash of string

se_increment_field("counter", 1)           -- Increment field
se_decrement_field("counter", 1)           -- Decrement field

se_push_stack(function() int(42) end)      -- Push value onto stack
se_log_stack()                             -- Log stack contents

-- Set a field in another tree's blackboard
se_set_external_field("value_field", "tree_ptr", offset)
```

### Timing, Delays, and Events

```lua
se_tick_delay(100)                         -- Wait 100 ticks
se_time_delay(2.5)                         -- Wait 2.5 seconds

-- Wait for predicate to become true
se_wait(function() se_field_eq("ready", 1) end)

-- Wait with timeout
se_wait_timeout(
    function() se_field_eq("ready", 1) end,  -- predicate
    5.0,                                      -- timeout seconds
    true,                                     -- reset on timeout
    function() se_log("Timed out!") end       -- error handler
)

-- Wait for specific event
se_wait_event(0x0001, 1)       -- Wait for event 0x0001, count=1
se_wait_event_once(0x0001)     -- Shorthand: wait for one occurrence

-- Verify predicate with time/event budget
se_verify(
    function() se_field_gt("temp", 0) end,
    true,
    function() se_log("Verification failed") end
)

se_verify_and_check_elapsed_time(10.0, true, function()
    se_log("Time budget exceeded")
end)

se_verify_and_check_elapsed_events(0x0001, 5, true, function()
    se_log("Event budget exceeded")
end)

-- Queue an event
se_queue_event(event_type, event_id, "slot_name")
```

### State Machine and Dispatch

```lua
-- State machine: index-based dispatch (array index = state value)
actions_fn = {}

actions_fn[1] = function()  -- State 0
    se_log("State 0")
    se_tick_delay(100)
    se_set_field("state", 1)
    se_return_halt()
end

actions_fn[2] = function()  -- State 1
    se_log("State 1")
    se_return_terminate()
end

se_state_machine("state", actions_fn)

-- Field dispatch with explicit case values and duplicate detection
case_fn = {}

case_fn[1] = function()
    se_case(0, function()
        se_sequence(function()
            se_log("Case 0")
            se_set_field("state", 1)
            se_return_halt()
        end)
    end)
end

case_fn[2] = function()
    se_case(1, function()
        se_log("Case 1")
        se_return_terminate()
    end)
end

case_fn[3] = function()
    se_case('default', function()
        se_log("Default case")
        se_return_halt()
    end)
end

se_field_dispatch("state", case_fn)

-- Event dispatch
se_event_dispatch({
    function() se_event_case(0x0001, function() se_log("Event 1") end) end,
    function() se_event_case(0x0002, function() se_log("Event 2") end) end,
    function() se_event_case("default", function() se_log("Other") end) end,
})
```

### Predicates

#### Leaf Predicates

```lua
se_true()                                    -- Always true
se_false()                                   -- Always false
se_field_eq("mode", 1)                       -- field == value
se_field_ne("mode", 0)                       -- field != value
se_field_gt("counter", 10)                   -- field > value
se_field_ge("counter", 10)                   -- field >= value
se_field_lt("counter", 100)                  -- field < value
se_field_le("counter", 100)                  -- field <= value
se_field_in_range("temp", 20, 80)            -- min <= field <= max
se_check_event(0x0001, 0x0002)               -- Check for events
se_field_increment_and_test("counter", "inc_field", "test_field")
se_state_increment_and_test(1, 100)          -- Increment and test threshold

-- Custom predicate
se_pred("MY_CUSTOM_PRED")
se_pred_with("MY_PRED", function() int(42) end)
```

#### Composite Predicates (Predicate Builder)

```lua
pred_begin()
    local or_id = se_pred_or()
        se_field_gt("temperature", 100)
        local and_id = se_pred_and()
            se_field_lt("pressure", 50)
            se_field_eq("mode", 1)
        pred_close(and_id)
    pred_close(or_id)
local my_pred = pred_end()

-- Use the predicate
se_if_then(my_pred, function()
    se_log("Condition met")
end)
```

Composite operators: `se_pred_or()`, `se_pred_and()`, `se_pred_nor()`, `se_pred_nand()`, `se_pred_xor()`, `se_pred_not()`

### Dictionary/JSON Operations

```lua
-- Load dictionary into a PTR64 blackboard field
se_load_dictionary("config_ptr", {
    speed = 100,
    mode = "auto",
    limits = { min = 0, max = 255 }
})

-- Load with hash keys (faster lookup, smaller binary)
se_load_dictionary_hash("config_ptr", { speed = 100 })

-- Extract values using string paths
se_dict_extract_int("config_ptr", "speed", "speed_field")
se_dict_extract_float("config_ptr", "gain", "gain_field")
se_dict_extract_uint("config_ptr", "flags", "flags_field")
se_dict_extract_bool("config_ptr", "enabled", "enabled_field")
se_dict_extract_hash("config_ptr", "mode", "mode_field")

-- Extract values using hash paths (for json_hash dictionaries)
se_dict_extract_int_h("config_ptr", {"limits", "max"}, "max_field")
se_dict_extract_float_h("config_ptr", {"gains", "kp"}, "kp_field")

-- Store a dict sub-pointer into another PTR64 field
se_dict_store_ptr("config_ptr", "limits", "limits_ptr")
se_dict_store_ptr_h("config_ptr", {"nested", "section"}, "section_ptr")

-- Batch extraction
se_dict_extract_all("config_ptr", {
    { path = "speed",    dest = "speed_field",  type = "int" },
    { path = "gain",     dest = "gain_field",   type = "float" },
    { path = "enabled",  dest = "flag_field",   type = "bool" },
})

se_dict_extract_all_h("config_ptr", {
    { path = {"gains", "kp"}, dest = "kp_field", type = "float" },
    { path = {"gains", "ki"}, dest = "ki_field", type = "float" },
})
```

### Function Dictionary and Tree Spawning

```lua
-- Load a dictionary of named functions
se_load_function_dict("fn_dict_ptr", {
    {"idle",   function() se_log("Idle") end},
    {"run",    function() se_log("Running") end},
    {"error",  function() se_log("Error") end},
})

-- Execute a function by name from the dictionary
se_exec_dict_fn("fn_dict_ptr", "idle")

-- Execute using a hash stored in a field
se_exec_dict_fn_ptr("fn_dict_ptr", "command_hash_field")

-- Load a single function into a pointer field
se_load_function("fn_ptr", function()
    se_sequence(function()
        se_log("Loaded function")
    end)
end)

-- Execute a loaded function
se_exec_function("fn_ptr")

-- Spawn and tick a child tree
se_spawn_tree("child_tree_ptr", "child_tree_name", 256)
se_tick_tree("child_tree_ptr")
```

### Stack Frame Management

```lua
-- Low-level: allocate a stack frame for a node subtree
se_frame_allocate(num_params, num_locals, scratch_depth,
    function() --[[ body ]] end
)

-- High-level: call with stack frame, parameter passing, and return values
se_call(
    2,              -- num_params (pushed by caller before call)
    3,              -- num_locals
    2,              -- scratch_depth
    {0, 1},         -- return_vars (indices copied back after call)
    {               -- body functions
        function()
            -- stack_local(0) = param 0
            -- stack_local(1) = param 1
            -- stack_local(2..4) = locals
            -- stack_tos(0..1) = scratch
        end
    }
)

-- Stack frame instance (low-level primitive)
se_stack_frame_instance(num_params, num_locals, scratch_depth, return_vars)
```

### Quad Operations (Oneshot Arithmetic)

Quad operations execute as oneshot nodes with `(src1, src2, dest)` semantics. Each wrapper returns a closure; call `()` to emit the node.

#### Value Reference Helpers

```lua
int_val(42)              -- Integer literal
uint_val(0xFF)           -- Unsigned integer literal
float_val(3.14)          -- Float literal
field_val("temperature") -- Blackboard field reference
local_ref(0)             -- Stack local by index
tos_ref(0)               -- Stack TOS by offset
const_val("defaults")    -- Constant reference
hash_val("key")          -- String hash
null_val()               -- Null parameter
stack_push_ref()         -- Push operation
stack_pop_ref()          -- Pop operation
```

#### Integer Arithmetic

```lua
quad_iadd(src1, src2, dest)()   -- dest = src1 + src2
quad_isub(src1, src2, dest)()   -- dest = src1 - src2
quad_imul(src1, src2, dest)()   -- dest = src1 * src2
quad_idiv(src1, src2, dest)()   -- dest = src1 / src2
quad_imod(src1, src2, dest)()   -- dest = src1 % src2
quad_ineg(src, dest)()          -- dest = -src
quad_iabs(src, dest)()          -- dest = |src|
quad_imin(src1, src2, dest)()   -- dest = min(src1, src2)
quad_imax(src1, src2, dest)()   -- dest = max(src1, src2)
```

#### Float Arithmetic

```lua
quad_fadd(src1, src2, dest)()   quad_fsub(src1, src2, dest)()
quad_fmul(src1, src2, dest)()   quad_fdiv(src1, src2, dest)()
quad_fmod(src1, src2, dest)()   quad_fneg(src, dest)()
quad_fabs(src, dest)()          quad_fmin(src1, src2, dest)()
quad_fmax(src1, src2, dest)()
```

#### Float Math/Trig/Hyperbolic

```lua
quad_sqrt(src, dest)()     quad_pow(src1, src2, dest)()
quad_exp(src, dest)()      quad_log(src, dest)()
quad_log10(src, dest)()    quad_log2(src, dest)()
quad_sin(src, dest)()      quad_cos(src, dest)()
quad_tan(src, dest)()      quad_asin(src, dest)()
quad_acos(src, dest)()     quad_atan(src, dest)()
quad_atan2(y, x, dest)()   quad_sinh(src, dest)()
quad_cosh(src, dest)()     quad_tanh(src, dest)()
```

#### Bitwise, Comparison, Logical, Move

```lua
-- Bitwise
quad_and(a, b, dest)()   quad_or(a, b, dest)()
quad_xor(a, b, dest)()   quad_not(src, dest)()
quad_shl(a, b, dest)()   quad_shr(a, b, dest)()

-- Integer comparison (dest = 0 or 1)
quad_ieq(a, b, dest)()   quad_ine(a, b, dest)()
quad_ilt(a, b, dest)()   quad_ile(a, b, dest)()
quad_igt(a, b, dest)()   quad_ige(a, b, dest)()

-- Float comparison (dest = 0 or 1)
quad_feq(a, b, dest)()   quad_fne(a, b, dest)()
quad_flt(a, b, dest)()   quad_fle(a, b, dest)()
quad_fgt(a, b, dest)()   quad_fge(a, b, dest)()

-- Logical
quad_log_and(a, b, dest)()   quad_log_or(a, b, dest)()
quad_log_not(src, dest)()    quad_log_nand(a, b, dest)()
quad_log_nor(a, b, dest)()   quad_log_xor(a, b, dest)()

-- Move
quad_mov(src, dest)()
```

### Predicate Quad Operations

Predicate quads emit `p_call("SE_P_QUAD")` nodes. Same value reference helpers as quad ops.

```lua
-- Integer/float comparison (predicate versions)
p_icmp_eq(a, b, dest)()   p_fcmp_eq(a, b, dest)()
p_icmp_ne(a, b, dest)()   p_fcmp_ne(a, b, dest)()
p_icmp_lt(a, b, dest)()   p_fcmp_lt(a, b, dest)()
p_icmp_le(a, b, dest)()   p_fcmp_le(a, b, dest)()
p_icmp_gt(a, b, dest)()   p_fcmp_gt(a, b, dest)()
p_icmp_ge(a, b, dest)()   p_fcmp_ge(a, b, dest)()

-- Comparison + accumulate (dest += result)
p_icmp_eq_acc(a, b, dest)()   p_fcmp_eq_acc(a, b, dest)()
-- ... all six comparison variants for both int and float

-- Logical
p_log_and(a, b, dest)()   p_log_or(a, b, dest)()
p_log_not(src, dest)()    p_log_nand(a, b, dest)()
p_log_nor(a, b, dest)()   p_log_xor(a, b, dest)()

-- Bitwise
p_bit_and(a, b, dest)()   p_bit_or(a, b, dest)()
p_bit_xor(a, b, dest)()   p_bit_not(src, dest)()
p_bit_shl(a, b, dest)()   p_bit_shr(a, b, dest)()

-- Range checks (composite: two comparisons + logical AND)
p_icmp_in_range(src, low, high, dest, scratch1, scratch2)()
p_fcmp_in_range(src, low, high, dest, scratch1, scratch2)()
```

---

## Expression Compiler (s_expr_compiler.lua)

The expression compiler parses C-like expressions at DSL compile time and generates sequences of quad/p_quad operations, with constant folding and automatic type inference.

### Setup

```lua
local v = frame_vars(
    {"x:float", "y:float", "result:float", "count:int"},  -- locals
    {"t0:float", "t1:float", "t2:float"}                  -- scratch
)
```

### Arithmetic Expressions

```lua
-- Simple assignment (dest can be a variable or @field)
quad_expr("result = (x + 5.0) * y - 2.0", v, {"t0", "t1"})()
quad_expr("@temperature = x + 1.0", v, {"t0"})()

-- Compound assignment
quad_expr("count += 1", v, {})()
quad_expr("@sensor_value *= 0.95", v, {"t0"})()

-- Multi-statement
quad_multi("a = x + 1; b = a * y; result = b - 2", v, {"t0", "t1"})()
```

### Predicate Expressions

```lua
-- Boolean expression for use in se_if_then, se_while, etc.
quad_pred("x > 5.0 && y <= 10.0", v, {"t0", "t1"})()

-- Accumulate: independently test multiple conditions, count passes
quad_pred_acc({"x > 5", "y <= 10", "z == 0"}, v, "pass_count")()
```

### Supported Operations

Arithmetic: `+`, `-`, `*`, `/`, `%`. Bitwise: `&`, `|`, `^`, `~`, `<<`, `>>`. Comparison: `==`, `!=`, `<`, `<=`, `>`, `>=`. Logical: `&&`, `||`, `!`. Unary: `-`, `!`, `~`.

Math functions: `sqrt`, `abs`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `exp`, `log`, `log10`, `log2`, `sinh`, `cosh`, `tanh`, `neg`, `min`, `max`, `pow`, `atan2`.

Field references use `@field_name` syntax in both expressions and as assignment destinations.

### Debug Variants

```lua
quad_expr_debug("result = x * y + 1.0", v, {"t0"})()  -- Prints AST + ops
quad_pred_debug("x > 5.0", v, {"t0"})()
```

---

## Field Validation

Compile-time validation helpers used by dictionary and function_dict modules:

```lua
validate_field_exists(field_name, func_name)       -- Error if field not in record
validate_field_is_ptr64(field_name, func_name)      -- Error if not PTR64_FIELD
validate_field_is_numeric(field_name, func_name)    -- Error if not int/uint/float/double
validate_field_type(field_name, expected, func_name) -- Error if wrong type
```

These are called automatically by helper functions like `se_load_dictionary`, `se_exec_dict_fn`, `se_spawn_tree`, etc.

---

## Result Codes

| Code | Name | Value | Meaning |
|------|------|-------|---------|
| SE_CONTINUE | Continue | 0 | Keep processing siblings |
| SE_HALT | Halt | 1 | Stop this tick, resume next tick |
| SE_TERMINATE | Terminate | 2 | Shut down the tree |
| SE_RESET | Reset | 3 | Reset current node |
| SE_DISABLE | Disable | 4 | Disable current node |
| SE_SKIP_CONTINUE | Skip Continue | 5 | Skip to next sibling |
| SE_FUNCTION_CONTINUE | Function Continue | 6 | Continue at function level |
| SE_FUNCTION_HALT | Function Halt | 7 | Halt at function level |
| SE_FUNCTION_TERMINATE | Function Terminate | 8 | Terminate function |
| SE_FUNCTION_RESET | Function Reset | 9 | Reset function |
| SE_FUNCTION_DISABLE | Function Disable | 10 | Disable function |
| SE_FUNCTION_SKIP_CONTINUE | Function Skip Continue | 11 | Skip continue at function level |
| SE_PIPELINE_CONTINUE | Pipeline Continue | 12 | Continue pipeline |
| SE_PIPELINE_HALT | Pipeline Halt | 13 | Halt pipeline |
| SE_PIPELINE_TERMINATE | Pipeline Terminate | 14 | Terminate pipeline |
| SE_PIPELINE_RESET | Pipeline Reset | 15 | Reset pipeline |
| SE_PIPELINE_DISABLE | Pipeline Disable | 16 | Disable pipeline |
| SE_PIPELINE_SKIP_CONTINUE | Pipeline Skip Continue | 17 | Skip continue in pipeline |

---

## Compilation

### Basic Usage

```bash
luajit s_compile.lua input.lua --helpers=s_engine_helpers.lua --all --outdir=output/
```

### Command Line Options

| Option | Description |
|--------|-------------|
| `--header=<file>` | Generate main C header |
| `--user=<file>` | Generate user function header |
| `--reg=<file>` | Generate user registration code |
| `--records=<file>` | Generate records header |
| `--debug=<file>` | Generate debug header |
| `--binary=<file>` | Generate binary module (.bin) |
| `--binary-h=<file>` | Generate binary as C header |
| `--dump-h=<file>` | Generate parameter dump header |
| `--helpers=<file>` | Load helper functions |
| `--dump` | Print debug dump to stdout |
| `--all` | Generate all text outputs |
| `--all-bin` | Generate all outputs including binary |
| `--outdir=<dir>` | Output directory |
| `--32bit` | Force 32-bit mode (default) |
| `--64bit` | Force 64-bit mode |

### Examples

```bash
# Generate all files for 32-bit target
luajit s_compile.lua state_machine.lua --helpers=s_engine_helpers.lua --all-bin --outdir=generated/

# Generate only binary header for 64-bit
luajit s_compile.lua my_module.lua --binary-h=my_module_bin_64.h --64bit

# Debug dump only
luajit s_compile.lua my_module.lua --dump
```

---

## Engine Built-in Functions

These are registered automatically via `s_engine_register_builtins()` and the helper module loader:

| DSL Helper | Engine Function | Type | Description |
|------------|-----------------|------|-------------|
| `se_sequence` | `SE_SEQUENCE` | main | Execute children in order |
| `se_sequence_once` | `SE_SEQUENCE_ONCE` | main | Execute children once |
| `se_fork` | `SE_FORK` | main | Execute children in parallel |
| `se_fork_join` | `SE_FORK_JOIN` | main | Fork and wait for all |
| `se_chain_flow` | `SE_CHAIN_FLOW` | main | Pipeline processing |
| `se_if_then_else` | `SE_IF_THEN_ELSE` | main | Conditional execution |
| `se_trigger_on_change` | `SE_TRIGGER_ON_CHANGE` | main | Edge detection |
| `se_while` | `SE_WHILE` | main | While loop |
| `se_cond` | `SE_COND` | main | Lisp-style conditional |
| `se_function_interface` | `SE_FUNCTION_INTERFACE` | main | Function wrapper |
| `se_state_machine` | `SE_STATE_MACHINE` | main | State dispatch |
| `se_field_dispatch` | `SE_FIELD_DISPATCH` | main | Value-based dispatch |
| `se_event_dispatch` | `SE_EVENT_DISPATCH` | main | Event-based dispatch |
| `se_tick_delay` | `SE_TICK_DELAY` | pt_main | Wait N ticks |
| `se_time_delay` | `SE_TIME_DELAY` | pt_main | Wait N seconds |
| `se_wait` | `SE_WAIT` | main | Wait for predicate |
| `se_wait_timeout` | `SE_WAIT_TIMEOUT` | pt_main | Wait with timeout |
| `se_wait_event` | `SE_WAIT_EVENT` | pt_main | Wait for event |
| `se_verify` | `SE_VERIFY` | main | Verify predicate |
| `se_set_field` | `SE_SET_FIELD` | oneshot | Set blackboard field |
| `se_log` | `SE_LOG` | oneshot | Debug output |
| `se_quad` | `SE_QUAD` | oneshot | Quad arithmetic op |
| `se_p_quad` | `SE_P_QUAD` | predicate | Predicate quad op |
| `se_spawn_tree` | `SE_SPAWN_TREE` | pt_main | Spawn child tree |
| `se_tick_tree` | `SE_TICK_TREE` | main | Tick child tree |
| `se_frame_allocate` | `SE_FRAME_ALLOCATE` | main | Allocate stack frame |

---

## Binary Format

The binary module format (v5.3) provides:

- **Magic**: `0x42584553` ("SEXB")
- **Version**: `0x0503`
- **Direct s_expr_param_t structs** — no decoding needed
- **Zero-copy loading** — cast pointer directly from ROM
- **Position-independent** — no absolute addresses
- **Parent offset** (v5.3) — O(1) upward navigation from any brace token to its enclosing OPEN_CALL

### 32-bit vs 64-bit

| Mode | Param Size | Pointer Size | Use Case |
|------|------------|--------------|----------|
| 32-bit | 8 bytes | 4 bytes | ARM Cortex-M, ESP32 |
| 64-bit | 16 bytes | 8 bytes | ARM64, AMD64, servers |

---

## Version History

### v5.3
- Tree validation: exactly one top-level node required per tree
- Parent offset tracking in all brace tokens for O(1) upward navigation
- Better error messages for tree structure violations

### v5.2
- Split generators into separate files
- Added array/tuple data structures
- Added `str_hash()`, `key()`/`key_end()`, `key_hash()`
- Modular helper loader with sub-module architecture

---

## License

MIT License — See individual repository LICENSE files.