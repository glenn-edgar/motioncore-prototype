# External Tree Management Functions

## Overview

These three function pairs (DSL + C) provide the ability to spawn, tick, and interact with
external tree instances from within a parent tree. This enables hierarchical tree composition
where a parent tree can create child trees at runtime, forward events to them, and share
data between blackboards.

All three functions operate through a shared `ptr64` blackboard field that holds the pointer
to the child tree instance.

---

## se_spawn_tree

### DSL

```lua
se_spawn_tree(tree_pointer, tree_name, stack_size)
```

**Parameters:**
- `tree_pointer` — `ptr64` field in the blackboard that will store the child tree instance pointer.
- `tree_name` — String name of the tree to create (resolved via hash lookup in the module).
- `stack_size` — Unsigned integer specifying the parameter stack capacity for the child tree. Use `0` for no stack.

**DSL call type:** `pt_m_call` (uses a private 64-bit pointer slot for ownership tracking).

### C Implementation

```c
s_expr_result_t se_spawn_tree(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
);
```

**Behavior by event type:**

| Event | Action |
|-------|--------|
| `SE_EVENT_INIT` | Creates the child tree via `s_expr_tree_create_by_hash`, optionally creates a stack, initializes node states, and stores the pointer in both the `pt_m_call` slot and the blackboard field. |
| `SE_EVENT_TERMINATE` | Terminates and frees the child tree, clears both the `pt_m_call` slot and the blackboard field. |
| All others | Returns `SE_PIPELINE_CONTINUE` (no-op). |

**Error conditions:**
- Returns `SE_PIPELINE_TERMINATE` if parameters are missing, incorrectly typed, or if tree creation fails.

---

## se_tick_tree

### DSL

```lua
se_tick_tree(tree_pointer)
```

**Parameters:**
- `tree_pointer` — `ptr64` field in the blackboard holding the child tree instance pointer (previously set by `se_spawn_tree`).

**DSL call type:** `m_call` (standard main function, no private pointer slot).

### C Implementation

```c
s_expr_result_t se_tick_tree(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
);
```

**Behavior by event type:**

| Event | Action |
|-------|--------|
| `SE_EVENT_INIT` | Performs a full reset (`s_expr_node_full_reset`) on the child tree. |
| `SE_EVENT_TERMINATE` | Terminates the child tree (`s_expr_node_terminate`). |
| All others | Forwards the current `event_id` and `event_data` to the child tree via `s_expr_node_tick`, then drains the child's event queue, ticking for each queued event. Returns the last result code. |

**Event queue draining:**
After the initial tick, any events that the child tree has pushed onto its own event queue
are popped and dispatched via `s_expr_node_tick` in FIFO order. The result code from the
final tick (whether from the initial event or the last queued event) is returned to the caller.

**Error conditions:**
- Returns `SE_PIPELINE_TERMINATE` if the tree pointer field is missing, incorrectly typed, or NULL.

---

## se_set_external_field

### DSL

```lua
se_set_external_field(value_field, tree_pointer, dictionary_offset)
```

**Parameters:**
- `value_field` — `uint32` field in the local blackboard containing the value to write.
- `tree_pointer` — `ptr64` field in the local blackboard holding the target tree instance pointer.
- `dictionary_offset` — Unsigned integer byte offset into the target tree's blackboard where the value will be written.

**DSL call type:** `o_call` (oneshot, executes once).

### C Implementation

```c
void se_set_external_field(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
);
```

**Behavior:**
Reads a `uint32` value from `value_field` in the local blackboard and writes it to the
target tree's blackboard at the specified byte offset. This enables the parent tree to
push configuration or state data into a child tree's blackboard without the child needing
to know about the parent.

**Error conditions:**
- Raises an exception if parameters are missing, incorrectly typed, or if either the
  target tree instance or its blackboard is NULL.

---

## Typical Usage Pattern

```lua
-- Define blackboard fields
local bb = define_record("my_record", {
    field_u32("command_value"),
    field_ptr64("child_tree"),
})

-- In tree definition:
-- 1. Spawn the child tree (runs once on init, cleans up on terminate)
se_spawn_tree(bb.child_tree, "worker_tree", 32)

-- 2. Push a value into the child's blackboard before ticking
se_set_external_field(bb.command_value, bb.child_tree, 0)

-- 3. Tick the child tree each cycle (forwards events, drains queue)
se_tick_tree(bb.child_tree)
```

## Dependencies

- `s_engine_types.h` — Core type definitions
- `s_engine_module.h` — `s_expr_tree_create_by_hash`, `s_expr_tree_free`
- `s_engine_node.h` — `s_expr_node_tick`, `s_expr_node_init_states`, `s_expr_node_terminate`, `s_expr_node_full_reset`
- `s_engine_stack.h` — `s_expr_tree_create_stack`
- `s_engine_event_queue.h` — `s_expr_event_queue_count`, `s_expr_event_pop`
- `s_engine_eval.h` — `s_expr_set_user_ptr`, `s_expr_get_user_ptr`
- `s_engine_exception.h` — `EXCEPTION` macro