# Why the S Engine
s_expr_param_t is a compact tagged-union token format for representing S-expression elements in the S_Engine control system. Each token encodes both its type and payload in a fixed-size structure optimized for embedded targets.
Origin: ChainTree and the Need for S_Engine
ChainTree Background
ChainTree is a behavior tree system developed before the S_Engine. It provides hierarchical control flow for embedded systems — sequences, selectors, parallel nodes, and state machines — with each leaf node implemented as a compiled C function.
ChainTree works well for high-level orchestration, but it struggles with modularity at the leaf level. Many embedded control tasks involve repetitive operations that differ only in parameters: configuring GPIO pins, setting up UART channels, reading ADC values, writing registers. Each variation requires its own C function, leading to:

Function explosion — hundreds of small C functions that do nearly identical things with different constants
Poor reuse — "set pin as output" can't easily be parameterized and shared across different ports
Tight coupling — the behavior tree structure is locked to specific hardware layouts
Difficult composition — combining boolean logic on hardware states (e.g., "wait until pin A AND pin B are both high") requires custom composite nodes

Even simple operations that differ only in a register address or bit mask become separate compiled functions, because ChainTree leaves have no built-in mechanism for parameterized, composable logic.
S_Engine as Microcode
The S_Engine was initially developed as a microcode layer for ChainTree leaf nodes. Instead of writing a separate C function for each hardware operation, a small set of C primitives (e.g., gpio_mode, write_register) could be composed through interpreted S-expression programs. ChainTree's virtual function table dispatches to either a native C function or an S_Engine program transparently:
Virtual Function Table
┌────────────────┬─────────────────────────────────────┐
│ Name           │ Implementation                      │
├────────────────┼─────────────────────────────────────┤
│ motor_init     │ C function: motor_init_fn()         │
│ sensor_read    │ C function: sensor_read_fn()        │
│ gpio_setup     │ S_Engine: gpio_setup_program[]      │
│ pump_cycle     │ S_Engine: pump_cycle_program[]      │
│ check_inputs   │ S_Engine: check_inputs_program[]    │
└────────────────┴─────────────────────────────────────┘
This eliminated the function explosion problem. A system that previously needed dozens of nearly-identical C leaf functions could instead share a single interpreter with parameterized token streams stored in ROM.
Standalone Engine
As the S_Engine matured, it became clear that it was capable of operating as a standalone control engine, not just as microcode beneath ChainTree. The S_Engine now supports:

Full behavior tree patterns (sequences, selectors, state machines, parallel nodes)
Stack-based parameter passing with frame variables
Function dictionaries for runtime-dispatched subroutines
Blackboard records for shared state
Cross-tree composition (spawning, ticking, and communicating between trees)
Expression compilation for arithmetic and bitwise operations

The S_Engine can be used as a microcode layer under ChainTree, as a standalone embedded control engine, or both in the same system — with ChainTree handling high-level orchestration and S_Engine handling parameterized leaf logic.




# S-Expression Parameter Token Format

## Overview

`s_expr_param_t` is a compact tagged-union token format for representing S-expression elements in the ChainTree control system. Each token encodes both its type and payload in a fixed-size structure optimized for embedded targets.

## Why S-Expressions?

### Evolution from Flow Control

The S_Engine wasn't the first approach. Earlier iterations used a flow control model with explicit opcodes for branching, looping, and sequencing — essentially a small bytecode VM. This was removed because S-expressions proved far more efficient in both code size and execution speed.

The flow control model required:
- Explicit branch targets and jump calculations
- Separate opcodes for if/else/while/for constructs
- Complex state tracking for nested control flow
- Redundant encoding of structure that was implicit in the tree

S-expressions encode control flow structurally. A `pipeline` node's children execute in sequence — no jump opcodes needed. An `if_then_else` node has exactly three children: predicate, consequent, alternative. The structure *is* the control flow.
```
Flow control approach (abandoned):
  LOAD_PRED 0
  JUMP_IF_FALSE label_else
  CALL func_a
  JUMP label_end
label_else:
  CALL func_b
label_end:

S-expression approach (current):
  (if_then_else
    (pred_0)
    (func_a)
    (func_b))
```

The S-expression version compiles to fewer tokens, executes faster (no branch misprediction), and is easier to debug (tree structure visible in token stream).

### Tcl-Like Evaluation Model

The S_Engine evaluation model is inspired by Tcl rather than Lisp. In Tcl, **the called function decides how to process its arguments**. Arguments are not evaluated before the call — the function receives them as unevaluated tokens and chooses what to do.

This is fundamentally different from Lisp's model where arguments are evaluated before the function sees them (unless explicitly quoted).

**Lisp model:**
```lisp
;; Arguments evaluated BEFORE my-if sees them
(my-if (expensive-predicate)    ; evaluated
       (side-effect-action)      ; evaluated - oops!
       (alternative))            ; evaluated - oops!

;; Must use macros or explicit quoting to prevent evaluation
(my-if (expensive-predicate)
       '(side-effect-action)     ; quoted - now a data structure
       '(alternative))           ; quoted
```

**Tcl/S_Engine model:**
```tcl
# Arguments passed as unevaluated blocks
# if_then_else decides when/whether to evaluate each
if_then_else {expensive-predicate} {side-effect-action} {alternative}
```

In the S_Engine, composite nodes like `if_then_else`, `pipeline`, `state_machine`, and `while_loop` receive their children as token ranges. The composite's C implementation decides:
- Which children to evaluate
- In what order
- How many times
- Whether to evaluate at all

This enables **creative control structures through new functions**, not through macros or quoting:
```lisp
;; retry_n: evaluate child up to N times until success
(retry_n 3
  (unreliable_network_call))

;; timeout: evaluate child, abort if exceeds duration
(timeout 5000
  (slow_operation))

;; parallel_race: evaluate all children, return when first completes
(parallel_race
  (sensor_a_read)
  (sensor_b_read)
  (timeout_fallback))

;; guarded_loop: evaluate body while predicate holds
(guarded_loop
  (temperature_below_threshold)
  (heater_on))
```

Each of these is just a C function that interprets its child token ranges appropriately. No macro system, no quoting rules, no evaluation order surprises.

### Avoiding Lisp's Quoting Complexity

Lisp's power comes with complexity. Quoting, quasiquoting, unquote, and unquote-splicing create a notation burden that's particularly painful for non-Lisp developers writing embedded control logic:
```lisp
;; Lisp: which things get evaluated when?
`(sequence
   ,@(loop for i from 0 to 3
           collect `(gpio-set ,port ,i ,(if (evenp i) 'HIGH 'LOW))))
```

The S_Engine sidesteps this entirely. Since functions control their own argument evaluation, there's no need for quoting mechanisms. What you write is what gets stored in the token stream. The DSL handles code generation without exposing Lisp's meta-syntactic complexity.

### The DSL: Making S-Expressions Palatable

Raw S-expressions are tedious to write and error-prone. The DSL provides a Python-based authoring environment that generates the token streams:
```python
# DSL source
with pipeline("motor_init"):
    oneshot("gpio_mode", PORTA, 0, OUTPUT_PP)
    oneshot("gpio_mode", PORTA, 1, OUTPUT_PP)
    oneshot("pwm_config", TIMER1, CH1, 20000, 8)
    oneshot("pwm_enable", TIMER1, CH1)
```

This compiles to:
```c
const s_expr_param_t motor_init_params[] = {
    { .type = 0x07, .brace_idx = 9 },                    // OPEN_CALL pipeline
    { .type = 0x08, .node_index = 0, .func_index = 1 },  // ONESHOT gpio_mode
    { .type = 0x00, .int_val = PORTA },
    { .type = 0x00, .int_val = 0 },
    { .type = 0x00, .int_val = OUTPUT_PP },
    // ... more tokens ...
    { .type = 0x06, .brace_idx = 0 },                    // CLOSE
};
```

The DSL provides:
- **Python syntax** — familiar to embedded developers, good tooling
- **Compile-time validation** — catch errors before flashing
- **Brace matching** — computed automatically, no manual index management
- **Function registration** — type checking for primitives
- **Multiple output formats** — C arrays, binary blobs, debug symbols

The developer writes natural Python code; the DSL emits efficient token streams. No Lisp knowledge required.

### Summary: Why This Approach

| Aspect | Flow Control VM | Lisp S-Expressions | Tcl-Style S_Engine |
|--------|-----------------|--------------------|--------------------|
| Control flow encoding | Explicit jumps | Structure + macros | Structure + functions |
| Argument evaluation | Eager | Eager (unless quoted) | Lazy (function decides) |
| New control structures | New opcodes | Macros | New C functions |
| Quoting complexity | N/A | High | None |
| Authoring | Assembly-like | Raw parens | Python DSL |
| Code size | Largest | Medium | Smallest |
| Debugging | Opaque | Readable | Readable |

---

## Size

| Platform | Size |
|----------|------|
| 32-bit   | 8 bytes |
| 64-bit   | 16 bytes |

## Structure Layout
```c
typedef struct {
    uint8_t  type;              // opcode (bits 0-5) + flags (bits 6-7)
    uint8_t  index_to_pointer;  // pointer array index for pt_m_call
    union { ... };              // 4 or 8 bytes depending on platform
} s_expr_param_t;
```

## The S_Engine: Microcode for ChainTree Virtual Functions

### The Problem: Behavior Trees and Hardware Composition

Traditional behavior trees work well for high-level control flow but struggle with low-level hardware operations. Consider initializing GPIO ports on a microcontroller:
```
HardwareInit [Sequence]
├── ConfigureGPIO_Port_A
│   ├── Set pin 0 as output, push-pull
│   ├── Set pin 1 as output, open-drain
│   ├── Set pin 2 as input, pull-up
│   └── Set pin 3 as input, floating
├── ConfigureGPIO_Port_B
│   ├── Set pin 0 as output
│   └── Set pin 1 as output
└── ConfigurePWM
    ├── Set timer prescaler
    ├── Set duty cycle
    └── Enable output
```

Each leaf node becomes a separate C function. For a system with dozens of GPIO configurations, ADC channels, and peripheral setups, this creates:

1. **Function explosion** — hundreds of tiny C functions that do nearly identical things
2. **Poor modularity** — can't easily reuse "set pin as output" across different ports
3. **Tight coupling** — behavior tree structure locked to specific hardware layout
4. **Difficult composition** — combining AND/OR logic on hardware states requires custom composite nodes

Even simple operations like "wait until pin A AND pin B are both high" require dedicated leaf functions or awkward tree structures.

### The Solution: S_Engine as Microcode Layer

The S_Engine provides a **microcode execution layer** that sits beneath ChainTree's virtual functions. Instead of each leaf node being a compiled C function, leaf nodes can reference S-expression programs that the S_Engine interprets.
```
ChainTree Architecture
┌─────────────────────────────────────────────────────────┐
│                    ChainTree Walker                     │
│            (behavior tree traversal engine)             │
└─────────────────────────┬───────────────────────────────┘
                          │
          ┌───────────────┼───────────────┐
          ▼               ▼               ▼
    ┌──────────┐    ┌──────────┐    ┌──────────┐
    │ C Leaf   │    │ C Leaf   │    │ Virtual  │
    │ Function │    │ Function │    │ Function │
    └──────────┘    └──────────┘    └─────┬────┘
                                          │
                                          ▼
                                   ┌──────────────┐
                                   │   S_Engine   │
                                   │  (microcode  │
                                   │  interpreter)│
                                   └──────────────┘
                                          │
                          ┌───────────────┼───────────────┐
                          ▼               ▼               ▼
                    ┌──────────┐    ┌──────────┐    ┌──────────┐
                    │ S-expr   │    │ S-expr   │    │ S-expr   │
                    │ Program  │    │ Program  │    │ Program  │
                    │ (ROM)    │    │ (ROM)    │    │ (ROM)    │
                    └──────────┘    └──────────┘    └──────────┘
```

### Virtual Function Expansion

A ChainTree "virtual function" is a named entry point that the tree walker can invoke. Without S_Engine, each virtual function maps directly to a C function pointer. With S_Engine, virtual functions can expand into interpreted S-expression programs:
```
Virtual Function Table
┌────────────────┬─────────────────────────────────────┐
│ Name           │ Implementation                      │
├────────────────┼─────────────────────────────────────┤
│ motor_init     │ C function: motor_init_fn()         │
│ sensor_read    │ C function: sensor_read_fn()        │
│ gpio_setup     │ S_Engine: gpio_setup_program[]      │
│ pump_cycle     │ S_Engine: pump_cycle_program[]      │
│ check_inputs   │ S_Engine: check_inputs_program[]    │
└────────────────┴─────────────────────────────────────┘
```

This allows:
- **C functions** for performance-critical or hardware-specific operations
- **S-expression programs** for composable, data-driven logic

### Hardware Configuration Example

Instead of writing separate C functions for each GPIO configuration:
```c
// Without S_Engine: explosion of tiny functions
void config_porta_pin0(void) { GPIO_SetMode(PORTA, 0, OUTPUT_PP); }
void config_porta_pin1(void) { GPIO_SetMode(PORTA, 1, OUTPUT_OD); }
void config_porta_pin2(void) { GPIO_SetMode(PORTA, 2, INPUT_PU); }
// ... dozens more ...
```

With S_Engine, define reusable primitives and compose them:
```lisp
;; DSL source (compiled to token stream)
(define gpio_setup
  (pipeline
    ;; Port A configuration
    (gpio_mode PORTA 0 OUTPUT_PP)
    (gpio_mode PORTA 1 OUTPUT_OD)
    (gpio_mode PORTA 2 INPUT_PU)
    (gpio_mode PORTA 3 INPUT_FLOAT)
    
    ;; Port B configuration  
    (gpio_mode PORTB 0 OUTPUT_PP)
    (gpio_mode PORTB 1 OUTPUT_PP)
    
    ;; PWM setup
    (pwm_config TIMER1 CH1 20000 8)  ;; 20kHz, 8-bit
    (pwm_duty TIMER1 CH1 0)          ;; start at 0%
    (pwm_enable TIMER1 CH1)))
```

The S_Engine interprets this token stream, calling a small set of primitive C functions (`gpio_mode`, `pwm_config`, etc.) with parameters from the token stream.

### Boolean Composition on Hardware

The S_Engine's predicate system enables composable boolean logic on hardware states without custom C code:
```lisp
;; Wait until both limit switches are triggered
(wait_until
  (and
    (gpio_read PORTA 4)      ;; limit switch A
    (gpio_read PORTA 5)))    ;; limit switch B

;; Complex safety interlock
(if_then_else
  (or
    (not (gpio_read EMERGENCY_STOP))
    (and
      (> (adc_read CURRENT_SENSE) 500)
      (< (adc_read TEMP_SENSE) 80)))
  (motor_disable)
  (motor_enable))
```

These compositions execute efficiently as token streams — no function call overhead per boolean operation, and the logic is data-driven rather than code-driven.

### Benefits for Embedded Systems

| Aspect | Traditional BT Leaves | S_Engine Microcode |
|--------|----------------------|-------------------|
| Code size | One C function per operation | Shared interpreter + token streams |
| Modularity | Poor — functions tightly coupled | High — primitives compose freely |
| Configuration | Recompile for changes | Data-driven, potentially loadable |
| Boolean logic | Custom composite nodes | Built-in AND/OR/NOT predicates |
| Debugging | Step through C code | Inspect token stream state |
| RAM usage | Call stack per function | Single interpreter context |

### Execution Model

The S_Engine executes token streams using the same lifecycle as ChainTree nodes:

1. **ONESHOT** — runs once when the S-expression program activates
2. **MAIN** — runs on each tick while active
3. **PRED** — boolean guard evaluated before execution

This maps directly to the `s_expr_param_t` function reference types (0x08, 0x09, 0x0A).

The S_Engine maintains minimal state per active program:
- Node flags (1 byte per node in the S-expression)
- Optional pointer slots for nodes that need persistent storage (DSL-controlled)

### Integration with ChainTree

The ChainTree walker doesn't know or care whether a virtual function is implemented in C or as an S-expression program. The dispatch layer handles this transparently:
```c
cfl_result_t invoke_virtual_function(uint16_t func_id, event_t event) {
    if (vtable[func_id].is_native) {
        // Direct C function call
        return vtable[func_id].c_func(context, event);
    } else {
        // S_Engine interpretation
        return s_engine_execute(
            vtable[func_id].tokens,
            vtable[func_id].params,
            vtable[func_id].node_count,
            event
        );
    }
}
```

This layered approach allows gradual migration — start with C functions, convert hot paths to S-expressions as patterns emerge, or vice versa for performance-critical sections.

---

## Type Byte Encoding
```
  7   6   5   4   3   2   1   0
+---+---+---+---+---+---+---+---+
| P | S |        OPCODE         |
+---+---+---+---+---+---+---+---+

P (bit 7): FLAG_POINTER        - token references pointer_array[]
S (bit 6): FLAG_SURVIVES_RESET - persists across system reset (io_call)
OPCODE (bits 0-5): 64 possible opcodes
```

### Flag Descriptions

**P Flag (FLAG_POINTER) - bit 7**

When set, indicates this function call uses an entry in the `pointer_array[]`. The `index_to_pointer` field specifies which slot. This supports functions that need to maintain a pointer or word-sized value across invocations, such as:
- Hardware peripheral handles
- Allocated resource references
- Cached computation results

Only functions explicitly marked in the DSL (e.g., `pt_m_call`) receive a pointer slot. This conserves memory since most functions don't need persistent pointer storage.

**S Flag (FLAG_SURVIVES_RESET) - bit 6**

When set, indicates this function's state persists across a system reset. Used for I/O-bound functions (`io_call`) that maintain external hardware state that shouldn't be reinitialized, such as:
- Serial port configurations
- Sensor calibration state
- Network connection handles

During a reset, functions without this flag have their node state cleared. Functions with this flag retain their `INITIALIZED` and `EVER_INIT` flags.

## Token Categories

### Primitive Values (0x00-0x03)

| Opcode | Name | Payload | Description |
|--------|------|---------|-------------|
| 0x00 | INT | `int_val` | Signed integer (32 or 64 bit) |
| 0x01 | UINT | `uint_val` | Unsigned integer |
| 0x02 | FLOAT | `float_val` | Float (32-bit) or double (64-bit) |
| 0x03 | STR_HASH | `str_hash` | Hashed string for fast comparison |

### List Structure (0x05-0x07)

| Opcode | Name | Payload | Description |
|--------|------|---------|-------------|
| 0x05 | OPEN | `brace_idx` | Start of list, index points to matching CLOSE |
| 0x06 | CLOSE | `brace_idx` | End of list, index points to matching OPEN |
| 0x07 | OPEN_CALL | `brace_idx` | Function invocation start |

### Function References (0x08-0x0A)

| Opcode | Name | Payload | Description |
|--------|------|---------|-------------|
| 0x08 | ONESHOT | `node_index`, `func_index` | Single-execution function |
| 0x09 | MAIN | `node_index`, `func_index` | Main tick function |
| 0x0A | PRED | `node_index`, `func_index` | Predicate function |

Function references use two 16-bit indices:
- `node_index` → runtime state in `node_states[]`
- `func_index` → entry in function dispatch table

#### Function Type Semantics

ChainTree nodes can have up to three function entry points, each serving a distinct purpose in the execution lifecycle:

**ONESHOT (0x08) - Initialization Function**

Executes exactly once when a node is first activated. Used for:
- Resource allocation
- Hardware initialization
- Initial state setup
- One-time configuration

The runtime tracks execution via `NODE_FLAG_INITIALIZED`. Once set, the oneshot function is skipped on subsequent ticks. If the node is reset (and doesn't have `FLAG_SURVIVES_RESET`), the flag clears and oneshot runs again on next activation.
```
First activation:  ONESHOT runs → sets INITIALIZED → MAIN runs
Subsequent ticks:  ONESHOT skipped → MAIN runs
After reset:       ONESHOT runs again (unless FLAG_SURVIVES_RESET)
```

**MAIN (0x09) - Tick Function**

Executes on every tick while the node is active. This is the primary execution path for:
- Periodic control logic
- State machine transitions
- Continuous monitoring
- Output updates

Returns a status that propagates up the behavior tree (SUCCESS, FAILURE, RUNNING, or ChainTree-specific codes like CONTINUE, HALT, RESET).

**PRED (0x0A) - Predicate Function**

A boolean guard that determines whether the node should execute. Evaluated before ONESHOT or MAIN:
- Returns true: node executes normally
- Returns false: node is skipped entirely

Used for:
- Conditional execution
- Resource availability checks
- Mode-dependent behavior
- Guard conditions in state machines
```
Execution order per tick:
1. PRED evaluated (if present)
   - false → skip node entirely
   - true  → continue
2. ONESHOT runs (if not yet initialized)
3. MAIN runs
```

#### Function Combinations

Not all nodes require all three functions. Common patterns:

| Pattern | ONESHOT | MAIN | PRED | Use Case |
|---------|---------|------|------|----------|
| Simple action | - | ✓ | - | Stateless operations |
| Guarded action | - | ✓ | ✓ | Conditional execution |
| Stateful action | ✓ | ✓ | - | Needs initialization |
| Full node | ✓ | ✓ | ✓ | Complex conditional stateful logic |
| One-time setup | ✓ | - | - | Pure initialization |

### Field/Data Access (0x0B-0x0E)

| Opcode | Name | Payload | Description |
|--------|------|---------|-------------|
| 0x0B | FIELD | `field_offset`, `field_size` | Record field reference |
| 0x0C | RESULT | `int_val` | Result value from computation |
| 0x0D | STR_IDX | `str_index`, `str_len` | String table reference |
| 0x0E | CONST_REF | `const_index`, `const_size` | ROM constant reference |

### Dictionary Structure (0x10-0x13)

| Opcode | Name | Payload | Description |
|--------|------|---------|-------------|
| 0x10 | OPEN_DICT | `brace_idx` | Begin key-value collection |
| 0x11 | CLOSE_DICT | `brace_idx` | End dictionary |
| 0x12 | OPEN_KEY | `str_hash` | Begin key-value pair (hash identifies key) |
| 0x13 | CLOSE_KEY | - | End key-value pair |

### Array Structure (0x14-0x15)

| Opcode | Name | Payload | Description |
|--------|------|---------|-------------|
| 0x14 | OPEN_ARRAY | `brace_idx` | Begin indexed collection |
| 0x15 | CLOSE_ARRAY | `brace_idx` | End array |

### Tuple Structure (0x16-0x17)

| Opcode | Name | Payload | Description |
|--------|------|---------|-------------|
| 0x16 | OPEN_TUPLE | `brace_idx` | Begin fixed-size heterogeneous group |
| 0x17 | CLOSE_TUPLE | `brace_idx` | End tuple |

## Node Runtime State

Every function node referenced in the token stream has an associated entry in `node_states[]`. This provides minimal per-node runtime storage.

### Node Flags (1 byte per node)
```c
S_EXPR_NODE_FLAG_ACTIVE        0x01  // Currently executing
S_EXPR_NODE_FLAG_INITIALIZED   0x02  // Oneshot function has completed
S_EXPR_NODE_FLAG_EVER_INIT     0x04  // Has been initialized at least once (lifetime)
S_EXPR_NODE_FLAG_ERROR         0x08  // Node is in error state
// Bits 4-7 available for user-defined flags
```

### Pointer Array (Optional, DSL-Controlled)

Most nodes only need the 1-byte flag state. However, some functions require persistent word-sized storage. The DSL controls allocation:
```
Regular function:     node_states[n] only (1 byte flags)
pt_m_call function:   node_states[n] + pointer_array[m] (1 byte + word)
io_call function:     FLAG_SURVIVES_RESET set, state persists across reset
pt_io_call function:  Both pointer storage and reset survival
```

This tiered approach conserves RAM on memory-constrained targets. A system with 100 nodes might only allocate 10 pointer slots for the functions that actually need them.

### Memory Layout Example
```
node_states[]:     [flags0][flags1][flags2]...[flagsN]   // N bytes
pointer_array[]:   [ptr0][ptr1][ptr2]...[ptrM]          // M words (M << N typically)

Token with P flag set:
  .node_index = 5        → node_states[5] for flags
  .index_to_pointer = 2  → pointer_array[2] for persistent value
```

## Predicate Macros

Type checking is done via bitmask predicates that handle flag bits correctly:
```c
S_EXPR_PARAM_IS_NUMERIC(t)      // INT, UINT, or FLOAT
S_EXPR_PARAM_IS_FUNC_REF(t)     // ONESHOT, MAIN, or PRED
S_EXPR_PARAM_IS_ANY_OPEN(t)     // Any opening bracket type
S_EXPR_PARAM_IS_ANY_CLOSE(t)    // Any closing bracket type
S_EXPR_PARAM_HAS_POINTER(t)     // Uses pointer_array[]
S_EXPR_PARAM_SURVIVES_RESET(t)  // Persists across reset
```

## Platform Adaptation

Types scale with platform word size:
```c
#if MODULE_IS_64BIT
    typedef int64_t  ct_int_t;
    typedef double   ct_float_t;
#else
    typedef int32_t  ct_int_t;
    typedef float    ct_float_t;
#endif
```

## DSL Output Examples
```c
// Integer literal
{ .type = 0x00, .int_val = 42 }

// Simple main function call to node 1, function 4
{ .type = 0x09, .index_to_pointer = 0, .node_index = 1, .func_index = 4 }

// Main function with pointer storage (pt_m_call)
{ .type = 0x89, .index_to_pointer = 3, .node_index = 2, .func_index = 7 }
//        ^^^^ P flag set (0x80 | 0x09)

// I/O function that survives reset (io_call)
{ .type = 0x49, .index_to_pointer = 0, .node_index = 5, .func_index = 12 }
//        ^^^^ S flag set (0x40 | 0x09)

// Pointer + survives reset (pt_io_call)
{ .type = 0xC9, .index_to_pointer = 1, .node_index = 8, .func_index = 15 }
//        ^^^^ P+S flags set (0x80 | 0x40 | 0x09)

// Field access: offset 8, size 4
{ .type = 0x0B, .field_offset = 8, .field_size = 4 }

// Open call bracket, matching close at index 7
{ .type = 0x07, .brace_idx = 7 }

// Dictionary key with hash
{ .type = 0x12, .str_hash = 0xA3B2C1D0 }
```

## Design Notes

- Brace matching is precomputed at compile time via `brace_idx`
- String hashing enables O(1) key lookup without string comparison
- The `index_to_pointer` field supports indirection for relocatable code
- All structured types (dict, array, tuple) use matched open/close pairs for unambiguous parsing
- Per-node storage is minimized: 1 byte flags for all nodes, pointer slots only where DSL specifies
- Reset behavior is explicit: `FLAG_SURVIVES_RESET` protects I/O state while allowing logic state to clear
- S_Engine provides composable microcode that eliminates the need for hundreds of small C leaf functions
- Tcl-like lazy evaluation enables new control structures through functions, not macros