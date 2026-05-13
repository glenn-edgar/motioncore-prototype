# SE_FUNCTION_INTERFACE - Tick Engine Interface Composite

## Overview

`se_function_interface` is the top-level composite that interfaces between the tick engine and the internal pipeline system. It executes all children in parallel (like `se_fork`) but translates result codes to `SE_FUNCTION_*` level for the external tick engine.

## Purpose

The S-Expression engine uses a three-tier result code system:

| Level | Codes | Range | Purpose |
|-------|-------|-------|---------|
| APPLICATION | `SE_CONTINUE`, `SE_HALT`, `SE_TERMINATE`, etc. | 0-5 | Fatal/global control |
| FUNCTION | `SE_FUNCTION_CONTINUE`, `SE_FUNCTION_HALT`, etc. | 6-11 | Tick engine interface |
| PIPELINE | `SE_PIPELINE_CONTINUE`, `SE_PIPELINE_HALT`, etc. | 12-17 | Internal composite coordination |

`se_function_interface` sits at the boundary:
- **Absorbs** PIPELINE codes (12-17) - handles internally
- **Propagates** FUNCTION codes (6-11) - passes to tick engine
- **Propagates** APPLICATION codes (0-5) - passes to tick engine

## Behavior

### Execution Model

- Invokes **all active children** on each tick (parallel execution)
- Children returning `SE_FUNCTION_HALT` block remaining siblings
- Children returning PIPELINE codes are handled internally
- Returns `SE_FUNCTION_*` codes to the tick engine

### Result Code Handling

| Child Returns | Function Interface Action |
|---------------|---------------------------|
| **APPLICATION (0-5)** | Propagate immediately to tick engine |
| **FUNCTION (6-11)** | Propagate immediately to tick engine |
| `SE_PIPELINE_CONTINUE` (12) | Child running - count as active |
| `SE_PIPELINE_HALT` (13) | Child running - count as active |
| `SE_PIPELINE_DISABLE` (16) | Child complete - terminate it |
| `SE_PIPELINE_TERMINATE` (14) | Child complete - terminate it |
| `SE_PIPELINE_RESET` (15) | Child complete - terminate and reset |
| `SE_PIPELINE_SKIP_CONTINUE` (17) | Skip remaining children this tick |

### Return Values to Tick Engine

| Condition | Returns |
|-----------|---------|
| All children complete | `SE_FUNCTION_DISABLE` |
| Children still running | `SE_FUNCTION_CONTINUE` |
| Child returns FUNCTION code | That code (propagated) |
| Child returns APPLICATION code | That code (propagated) |

## Lifecycle Events

### INIT
- Sets state to `FORK_STATE_RUNNING`
- Resets all callable children
- Returns `SE_FUNCTION_CONTINUE`

### TERMINATE
- Terminates all children
- Sets state to `FORK_STATE_COMPLETE`
- Returns `SE_FUNCTION_CONTINUE`

### TICK
- Invokes all active children
- Handles result codes as described above
- Returns appropriate `SE_FUNCTION_*` code

## Usage

### Lua DSL

```lua
se_function_interface(function()
    -- All children run in parallel
    se_fork_join(function()
        se_tick_delay(10)
    end)
    
    se_state_machine("state", cases)
    se_tick_delay(350)
    
    se_return_function_terminate()
end)
```

### Typical Tree Structure

```lua
start_tree("my_tree")
    use_record("my_blackboard")
    
    se_function_interface(function()
        -- Phase 1: Blocking initialization
        se_fork_join(function()
            se_log("Initializing...")
            se_tick_delay(10)
        end)
        
        -- Phase 2: Parallel execution
        se_fork(function()
            -- Background task
            se_tick_delay(100)
        end)
        
        se_state_machine("state", case_fn)
        se_tick_delay(350)  -- Overall timeout
        
        -- Termination
        se_return_function_terminate()
    end)
end_tree("my_tree")
```

## Blocking Behavior

Children that return `SE_FUNCTION_HALT` (like `se_fork_join` and `se_tick_delay`) block subsequent siblings:

```lua
se_function_interface(function()
    se_fork_join(function()     -- Returns SE_FUNCTION_HALT while running
        se_tick_delay(10)       -- Blocks siblings for 10 ticks
    end)
    
    -- These don't start until fork_join completes:
    se_log("After fork_join")
    se_state_machine("state", cases)
end)
```

**Why?** When a child returns `SE_FUNCTION_HALT` (code 7), it's `< SE_PIPELINE_CONTINUE` (12), so the function_interface propagates it immediately and exits the loop.

## Comparison with SE_FORK

| Aspect | `se_function_interface` | `se_fork` |
|--------|------------------------|-----------|
| **Purpose** | Tick engine interface | Internal parallel execution |
| **Returns** | `SE_FUNCTION_*` codes | `SE_PIPELINE_*` codes |
| **FUNCTION codes** | Propagates | Converts to PIPELINE |
| **Used as** | Top-level tree root | Nested composite |

## Important Restrictions

### Return Code Functions

Return code functions like `se_return_function_terminate()` should be placed **directly** inside `se_function_interface`, not inside nested composites:

**GOOD:**
```lua
se_function_interface(function()
    se_fork_join(function()
        se_tick_delay(10)
    end)
    se_return_function_terminate()  -- Correct: at function_interface level
end)
```

**BAD:**
```lua
se_function_interface(function()
    se_fork(function()
        se_tick_delay(10)
        se_return_function_terminate()  -- WRONG: inside se_fork (will hang)
    end)
end)
```

### One Per Tree

Typically, each tree has exactly one `se_function_interface` at the root level. It's the entry point from the tick engine.

## State Storage

- **state** (uint8_t): `FORK_STATE_RUNNING` or `FORK_STATE_COMPLETE`
- **user_flags** (uint16_t): Set to 0 (not used)

## Execution Flow Example

```
Tick Engine
    │
    ▼
se_function_interface
    │
    ├── se_fork_join ──► Returns SE_FUNCTION_HALT (blocking)
    │                    Loop exits, returns SE_FUNCTION_HALT to tick engine
    │
    ... (fork_join completes) ...
    │
    ├── se_fork_join ──► Returns SE_PIPELINE_DISABLE (complete)
    │                    Terminate child, continue loop
    │
    ├── se_fork ──────► Returns SE_PIPELINE_CONTINUE
    │                    Count as active, continue loop
    │
    ├── se_state_machine ► Returns SE_PIPELINE_HALT
    │                    Count as active, continue loop
    │
    ├── se_tick_delay ──► Returns SE_FUNCTION_HALT
    │                    Loop exits, returns SE_FUNCTION_HALT to tick engine
    │
    ... (tick_delay completes) ...
    │
    └── se_return_function_terminate ► Returns SE_FUNCTION_TERMINATE
                         Loop exits, returns SE_FUNCTION_TERMINATE to tick engine
```

## Error Handling

- Unknown result codes increment active count and continue
- Non-callable parameters are skipped
- Inactive children are skipped