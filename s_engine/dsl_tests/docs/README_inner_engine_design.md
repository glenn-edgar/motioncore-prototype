# S-Expression Engine (s_engine) — Inner Engine Design Document

## 1. Overview

The s_engine is a compact, deterministic execution engine for evaluating flattened S-expression programs on resource-constrained embedded systems (ARM Cortex-M, 32KB+ RAM) and larger server targets (64-bit). It implements the inner evaluation core of the ChainTree architecture, providing unified control flow for behavior trees, state machines, and sequential pipelines.

The engine uses a **modified Tcl execution model**: programs are defined by a single root function that controls all evaluation. Parameters are tokenized by the DSL at compile time but **not evaluated** — evaluation is controlled by the builtin functions themselves, which decide how to interpret their arguments. This is analogous to Tcl's "everything is a command" philosophy, but with Lisp-style S-expression syntax for nesting, and a flat compiled representation for embedded efficiency.

There is no tree walker. The engine enters through a single root function via `s_expr_node_tick()`. That root function — typically a composite like `sequence`, `selector`, or `parallel` — explicitly invokes its children, which may in turn invoke their children. The program structure emerges entirely from how functions choose to evaluate their parameters.

### Key Design Principles

- **Zero heap allocation during tick** — all runtime state is pre-allocated; the tick path never calls malloc.
- **Deterministic execution** — identical inputs produce identical outputs; no hidden state or randomness.
- **Fail-fast via EXCEPTION macro** — invalid states halt the system (watchdog reset on embedded targets) rather than propagating corrupt data. NULL pointer errors are always fatal and terminate the engine immediately.
- **Parameters are data until evaluated** — functions receive tokenized but unevaluated parameter arrays and decide what to do with them: invoke as callables, read as values, pass to children, or ignore entirely.
- **Dual-width support** — the same source compiles for 32-bit (8-byte params) and 64-bit (16-byte params) via `MODULE_IS_64BIT`.

---

## 2. Architecture

### 2.1 Layer Diagram

```
┌─────────────────────────────────────────────────┐
│  Application / ChainTree Nodes                  │
│  (behavior tree composites, state machines)      │
├─────────────────────────────────────────────────┤
│  Outer Engine                                    │
│  (s_expr_skip_param, module creation, binding)   │
├─────────────────────────────────────────────────┤
│  s_engine_node.h  — Node lifecycle + children    │
│  (tick via root, child enumeration/invocation)   │
├─────────────────────────────────────────────────┤
│  s_engine_eval.h  — Dispatch core                │
│  (dispatch_main, dispatch_pred, dispatch_oneshot)│
├─────────────────────────────────────────────────┤
│  s_engine_stack.h — Per-tree parameter stack      │
│  (Lua-style indexing, frames, locals, scratch)    │
├─────────────────────────────────────────────────┤
│  s_engine_event_queue.h — Per-tree event queue    │
│  (circular buffer, tick_type + event_id + data)   │
├─────────────────────────────────────────────────┤
│  s_engine_types.h — Core type definitions         │
│  (s_expr_param_t, result codes, opcodes, flags)   │
├─────────────────────────────────────────────────┤
│  s_engine_module.h — Module lifecycle             │
│  (create, bind functions, allocate instances)     │
├─────────────────────────────────────────────────┤
│  s_engine_exception.h — Fatal error handler       │
│  (file/func/line/msg → spin for watchdog)         │
└─────────────────────────────────────────────────┘
```

### 2.2 Execution Model: Modified Tcl

The s_engine borrows from three language traditions:

| Tradition | What s_engine takes |
|-----------|-------------------|
| **Lisp** | S-expression nesting syntax, prefix notation, code-as-data |
| **Tcl** | Parameters are tokenized but not evaluated; functions control evaluation of their arguments |
| **Behavior Trees** | MAIN/ONESHOT/PRED function types, tick-driven lifecycle, result code propagation |

**Key difference from Tcl:** Parameters are compiled to a flat binary representation at build time by the DSL, not parsed at runtime from strings. This gives embedded-friendly O(1) access patterns with zero runtime parsing overhead.

**Key difference from Lisp:** There is no general-purpose `eval`. Each builtin function decides how to interpret its parameter list — some invoke children as callables, some read values, some do both. The evaluation strategy is a property of each function, not of the engine.

### 2.3 Program Structure

A program is a single root callable. All behavior emerges from how that root function and its descendants evaluate their parameters:

```
(sequence                          ← root MAIN function
    (init_hardware 0x40 0x01)      ← ONESHOT with value args
    (check_sensor threshold:42)    ← PRED with named constant
    (state_machine                 ← MAIN composite
        (state_idle ...)           ← MAIN child
        (state_active ...)))       ← MAIN child
```

The DSL compiles this to a flat param array. At runtime, `s_expr_node_tick()` invokes the root `sequence`. The sequence function calls `s_expr_child_invoke()` on each child in order. Each child function receives its own tokenized arguments and decides how to use them.

### 2.4 Inner / Outer Engine Boundary

The inner engine (this document) provides dispatch, lifecycle, stack, and event queue primitives. The **outer engine** provides:

- `s_expr_skip_param()` — skip over a logical parameter (atom or nested structure), used by the inner engine's `s_expr_count_params()`, `s_expr_find_param()`, and `s_expr_iterate_params()`.
- Module creation, function binding, and instance allocation.
- Higher-level coordination and scheduling.

### 2.5 Compilation Model

```
  YAML/S-expr DSL source
       │
       ▼
  Code Generator (Python)
       │
       ├─ Tokenizes parameters (but does not evaluate)
       ├─ Resolves function names to hash/index pairs
       ├─ Computes brace_idx offsets for O(1) structure skipping
       │
       ▼
  C source: module_def_t + tree_def_t + param arrays (ROM)
       │
       ▼
  s_engine links against generated ROM tables
       │
       ▼
  Runtime: module_t (RAM) binds function pointers to hashes
       │
       ▼
  Runtime: tree_instance_t (RAM) holds per-execution state
```

---

## 3. Core Data Structures

### 3.1 `s_expr_param_t` — The Universal Token

Every element in a tree's flat parameter array is an `s_expr_param_t`. These are **tokens**, not evaluated values — the distinction is central to the engine's Tcl-style model. A MAIN param is a reference to a callable, not a result. An INT param is a literal, not a computed value. Functions decide which tokens to evaluate and how.

The `type` byte encodes both the opcode (bits 5:0) and modifier flags (bits 7:6).

| Field | Size (32-bit) | Purpose |
|-------|---------------|---------|
| `type` | 1 byte | Opcode + flags |
| `index_to_pointer` | 1 byte | Index into `pointer_array[]` for `pt_m_call` |
| Union payload | 4 bytes | Value, indices, or brace offset |

**Total size:** 8 bytes (32-bit) or 16 bytes (64-bit), enforced by `_Static_assert`.

**Type byte layout:**

```
  Bit 7:  S_EXPR_FLAG_POINTER        (pt_m_call — uses pointer_array)
  Bit 6:  S_EXPR_FLAG_SURVIVES_RESET  (io_call — oneshot survives tree reset)
  Bits 5:0: Opcode (S_EXPR_OPCODE_MASK = 0x3F, 64 possible opcodes)
```

**Opcode categories:**

All opcodes are `#define` constants sharing a flat namespace (0x00–0x1C):

| Range | Category | Opcodes |
|-------|----------|---------|
| 0x00–0x03 | Primitive values | INT, UINT, FLOAT, STR_HASH |
| 0x04 | Legacy | SLOT |
| 0x05–0x06 | List structure | OPEN, CLOSE |
| 0x07 | Call structure | OPEN_CALL |
| 0x08–0x0A | Function refs | ONESHOT, MAIN, PRED |
| 0x0B–0x0E | Data access | FIELD, RESULT, STR_IDX, CONST_REF |
| 0x10–0x17 | Collections | DICT, KEY, ARRAY, TUPLE open/close pairs |
| 0x18–0x1C | Stack ops | STACK_TOS, STACK_LOCAL, NULL, STACK_PUSH, STACK_POP |

**Result codes** are a separate `s_expr_result_t` enum (values 0–17) with no overlap into the opcode space.

### 3.2 `s_expr_node_state_t` — Per-Node Runtime State

Each function node in a tree has a 4-byte state record:

| Field | Size | Purpose |
|-------|------|---------|
| `flags` | 1 byte | System flags (bits 3:0) + user flags (bits 7:4) |
| `state` | 1 byte | User state machine value (0–255) |
| `user_data` | 2 bytes | Dispatch tracking, counters, etc. |

**System flags:**

| Flag | Value | Meaning |
|------|-------|---------|
| `ACTIVE` | 0x01 | Node will be dispatched on tick |
| `INITIALIZED` | 0x02 | Node has received SE_EVENT_INIT |
| `EVER_INIT` | 0x04 | Node has ever been initialized (survives reset) |
| `ERROR` | 0x08 | Node is in error state |

### 3.3 `s_expr_slot_t` — 64-bit Pointer/Value Storage

An 8-byte union for `pt_m_call` functions that need persistent storage larger than the 4-byte `node_state_t` provides:

```c
typedef union {
    void*    ptr;
    uint64_t u64;
    int64_t  i64;
    double   f64;
} s_expr_slot_t;
```

Indexed by `index_to_pointer` in the param, accessed via `s_expr_get_user_u64()` / `s_expr_set_user_ptr()` etc. These accessors enforce that they are only called from within a `pt_m_call` context (`inst->in_pointer_call == true`).

### 3.4 `s_expr_tree_instance_t` — Per-Execution State

The central runtime structure containing:

- **Module reference** — shared function tables and allocator
- **Tree definition** — ROM param array pointer
- **Node states** — `node_states[node_count]` array
- **Pointer array** — `pointer_array[pointer_count]` for pt_m_call storage
- **Blackboard** — typed record binding for field access
- **Execution context** — current node index, event info, pointer call state
- **Parameter stack** — optional Lua-style stack for dynamic computation
- **Event queue** — circular buffer (inline, 16 slots) for deferred event delivery

---

## 4. Tick and Dispatch

### 4.1 Entry Point

The engine has a single entry point per tick:

```c
s_expr_result_t s_expr_node_tick(
    s_expr_tree_instance_t* inst,
    uint16_t event_id,
    void* event_data
);
```

This invokes the root function only. The root function is responsible for invoking its children, which invoke their children, and so on. There is no automatic traversal — all control flow is explicit, driven by the functions themselves.

### 4.2 Three Function Types

| Type | Signature | Returns | Lifecycle | Purpose |
|------|-----------|---------|-----------|---------|
| **MAIN** | `s_expr_main_fn_t` | `s_expr_result_t` | INIT → TICK* → TERMINATE | Long-lived nodes (composites, state machines) |
| **ONESHOT** | `s_expr_oneshot_fn_t` | void | Runs once per reset cycle | Setup, I/O binding, one-time actions |
| **PRED** | `s_expr_pred_fn_t` | bool | Stateless | Guards, conditions, predicates |

### 4.3 MAIN Function Three-Phase Lifecycle

```
dispatch_main(inst, func_param, args, ...)
    │
    ├─ PHASE 1: INITIALIZATION (first call only)
    │   if !(flags & INITIALIZED):
    │       set INITIALIZED
    │       call fn(inst, args, SE_EVENT_INIT, ...)
    │
    ├─ PHASE 2: REGULAR EVENT
    │   result = fn(inst, args, event_type, ...)
    │
    └─ PHASE 3: TERMINATION (if result == SE_PIPELINE_DISABLE)
        call fn(inst, args, SE_EVENT_TERMINATE, ...)
        clear ACTIVE flag
```

### 4.4 ONESHOT Execution Model

Oneshot functions run exactly once per lifecycle. The `check_flag` depends on the `S_EXPR_FLAG_SURVIVES_RESET` modifier:

- **Normal oneshot:** Checks `S_EXPR_NODE_FLAG_INITIALIZED`. Runs once after each reset.
- **io_call oneshot (survives reset):** Checks `S_EXPR_NODE_FLAG_EVER_INIT`. Runs once for the entire lifetime of the tree instance, surviving resets.

### 4.5 Result Code Hierarchy

Results are organized into three tiers for different scoping:

| Tier | Range | Meaning |
|------|-------|---------|
| **Application** (0–5) | SE_CONTINUE through SE_SKIP_CONTINUE | Propagate to application level |
| **Function** (6–11) | SE_FUNCTION_CONTINUE through SE_FUNCTION_SKIP_CONTINUE | Scoped to function internals |
| **Pipeline** (12–17) | SE_PIPELINE_CONTINUE through SE_PIPELINE_SKIP_CONTINUE | Scoped to composite node pipeline |

All NULL pointer errors and unrecoverable faults return `SE_TERMINATE`, which causes immediate engine termination.

### 4.6 Unevaluated Parameters and Brace Skipping

Because parameters are tokenized but not evaluated, functions receive flat spans of tokens and navigate them using `brace_idx` for O(1) skip-over of nested structures:

```
idx:     0          1         2    3    4
param: [OPEN_CALL] [MAIN:fn] [INT] [INT] [CLOSE]
        brace_idx=4

args = &params[2], arg_count = 4 - 0 - 2 = 2
```

A function receiving this param span sees two INT tokens as its arguments. It can read them as literal values, or — if it were a higher-order function — it could treat nested OPEN_CALL tokens as callables to invoke via `s_expr_child_invoke()`. The evaluation strategy is entirely up to the function.

### 4.7 Context Save/Restore

All dispatch functions save and restore `current_node_index`, `in_pointer_call`, and `pointer_base` on the C stack, enabling safe re-entrant invocation from composite nodes that call children.

---

## 5. Node API

### 5.1 Lifecycle

- `s_expr_node_tick()` — invoke root function with event
- `s_expr_node_reset()` — all nodes inactive except root
- `s_expr_node_terminate()` — backward walk, TERMINATE to initialized nodes
- `s_expr_node_full_reset()` — terminate then reset
- `s_expr_node_init_states()` — set root active only

### 5.2 Child Enumeration and Invocation

Functions that act as composites use these to control their children:

- `s_expr_child_count/index()` — enumerate logical children in the param array
- `s_expr_child_invoke()` — invoke Nth child, auto-detect type (primary API)
- `s_expr_child_invoke_main/pred/oneshot()` — type-specific invocation
- `s_expr_child_invoke_ex()` — invoke with explicit event context
- `s_expr_children_broadcast_ex()` — fan-out event to all children

### 5.3 Child Lifecycle Control

- `s_expr_child_reset/reset_recursive()` — clear child flags for re-initialization
- `s_expr_child_terminate()` — send TERMINATE to child
- `s_expr_children_terminate_all()` — terminate all children in reverse order
- `s_expr_children_reset_all()` — reset all children without TERMINATE

### 5.4 Eval-Layer Dispatch

The eval layer provides lower-level dispatch for direct param index access:

- `s_expr_invoke_main/oneshot/pred/any()` — dispatch a callable at a given param index
- `s_expr_tree_reset/terminate/full_reset()` — bulk lifecycle operations
- `s_expr_invoke_params()` — execute S-expression received via event_data

---

## 6. Parameter Stack

The optional per-tree stack (`s_expr_stack_t`) provides Lua-style 1-based indexing with negative indices from the top. It supports:

- **Typed push/pop** — int, uint, float, hash, pointer
- **Stack frames** — `push_frame(num_params, num_locals)` / `pop_frame()` for structured function calls
- **Manipulation** — insert, remove, replace, rotate, swap, dup, copy
- **Scratch area** — per-frame scratch space above locals via `poke()`
- **Type introspection** — `isint()`, `isfloat()`, `isnumeric()`, `isptr()`, etc.

**Frame layout:**

```
  ┌─────────────┐  ← base_ptr
  │ param 0     │
  │ param 1     │
  │ ...         │
  │ param N-1   │  ← num_params
  │ local 0     │
  │ local 1     │
  │ ...         │
  │ local M-1   │  ← num_locals
  ├─────────────┤  ← scratch_base
  │ scratch...  │
  │             │  ← sp
  └─────────────┘
```

Maximum frame depth: `S_EXPR_MAX_FRAMES = 16`.

---

## 7. Event Queue

A per-tree circular buffer (`S_EXPR_EVENT_QUEUE_SIZE = 16`) embedded inline in the tree instance for deferring events:

```c
typedef struct {
    uint16_t tick_type;
    uint16_t event_id;
    void*    event_data;
} s_expr_queued_event_t;
```

**API:** `s_expr_event_push()`, `s_expr_event_pop()`, `s_expr_event_queue_clear()`.

The queue uses head/count management. `event_data` pointers must remain valid until the event is consumed. All functions validate the `inst` pointer and will EXCEPTION on NULL (fatal, consistent with engine-wide policy).

---

## 8. Blackboard / Field Access

Trees can bind a typed record (struct) as a blackboard. FIELD-type params carry `field_offset` and `field_size`, enabling direct memory access into the bound record:

```c
#define S_EXPR_GET_FIELD(inst, param, type) \
    ((type*)((uint8_t*)(inst)->blackboard + (param)->field_offset))
```

Record and field descriptors (`s_expr_record_desc_t`, `s_expr_field_desc_t`) support runtime introspection via hash-based lookup.

---

## 9. Module System

### 9.1 Module Definition (ROM)

`s_expr_module_def_t` is generated by the DSL and contains:

- Tree definitions with param arrays
- Function hash tables (oneshot, main, pred) for binding
- Record descriptors for blackboard schemas
- String table for interned strings
- Constants table for ROM data references
- Maximum resource counts for pre-allocation

### 9.2 Module Instance (RAM)

`s_expr_module_t` holds:

- Resolved function pointer arrays (bound by hash lookup at init time)
- Allocator interface for runtime memory management
- Debug/error callback hooks
- Pool table for slot management

---

## 10. Exception Handling

The engine uses a `EXCEPTION(msg)` macro that expands to `cfl_exception_handler(__FILE__, __func__, __LINE__, msg)`. On embedded targets, this:

1. Prints file, function, line, and message via `puts()` (heap-safe)
2. Spins in an infinite loop waiting for the hardware watchdog to reset the system

This is a **fail-fast, never-recover** model appropriate for safety-critical embedded systems where corrupt state is worse than a restart. All NULL pointer dereference paths lead to EXCEPTION, which terminates the engine immediately.

---

## 11. Memory Budget

For a typical embedded deployment (32-bit, 20 function nodes, 8 pointer slots):

| Component | Size | Notes |
|-----------|------|-------|
| Node states | 80 bytes | 20 × 4 bytes |
| Pointer array | 64 bytes | 8 × 8 bytes |
| Slot flags | 8 bytes | 8 × 1 byte |
| Stack (64 slots) | 516 bytes | header + 64 × 8 bytes |
| Event queue | 96 bytes | 16 × 6 bytes (embedded in instance) |
| Instance struct | ~80 bytes | Pointers + context fields |
| **Total per tree** | **~844 bytes** | Excludes ROM param arrays |

Param arrays are in ROM and cost 8 bytes per param (32-bit mode).

---

## 12. Thread Safety

The engine is **not thread-safe**. A tree instance must be ticked from a single thread or protected by external synchronization. The event queue can be used to marshal cross-thread events, but the push/pop operations themselves are not atomic.

For multi-core embedded systems, each core should own its tree instances. Cross-core communication should go through NATS JetStream or MQTT at the ChainTree layer, not through shared s_engine state.

---

## 13. Summary

The s_engine inner core is a Tcl-inspired command dispatch engine operating on Lisp-style S-expressions compiled to flat parameter arrays. Programs are defined by a single root function invoked via `s_expr_node_tick()`. That function — and every function it calls — receives tokenized but unevaluated parameters, giving each builtin full control over argument interpretation. This model combines the composability of S-expressions with the simplicity of Tcl's evaluation rules, compiled to a zero-allocation flat representation suitable for deterministic embedded execution.

