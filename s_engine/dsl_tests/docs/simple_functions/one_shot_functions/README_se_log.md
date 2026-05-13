# SE_LOG - Debug Logging Oneshot

## Overview

`se_log` is a ONESHOT function that outputs a timestamped debug message. It fires once when invoked and completes immediately within the same tick.

## Purpose

Provides debug/trace logging within behavior trees for:
- Tracking execution flow
- Debugging state transitions
- Monitoring composite node behavior
- Development and testing

## Parameters

| Parameter | Type | Description |
|-----------|------|-------------|
| `message` | string | The message to log |

## Behavior

As a ONESHOT function:
- Fires **once** when first invoked
- Completes **immediately** (same tick)
- Sets `INITIALIZED` flag to prevent re-firing
- Does not block execution flow

### Output Format

```
[<timestamp>] <message>
```

Example:
```
[1769889708.801323] Fork Join Test Started
```

### Output Destination

1. **Debug callback** (if registered): Calls `mod->debug_fn(inst, buf)`
2. **Fallback to printf** (if no callback and `S_ENGINE_NO_STDIO` not defined)

## Lua DSL

### Basic Usage

```lua
se_log("Starting initialization")
```

### Conditional Debug Logging

```lua
se_debug_log("Verbose debug info")  -- Only logs if is_debug() returns true
```

The `se_debug_log` variant checks a debug flag at **compile time**, so debug messages can be stripped from production builds.

## Usage Examples

### State Machine Tracing

```lua
se_case(0, function()
    se_sequence(function()
        se_log("State 0")           -- Entry trace
        se_tick_delay(10)
        se_set_field("state", 1)
        se_log("State 0 terminated") -- Exit trace
        se_return_pipeline_disable()
    end)
end)
```

### Phase Markers

```lua
se_function_interface(function()
    se_log("Phase 1: Initialization")
    se_fork_join(function()
        se_tick_delay(10)
    end)
    
    se_log("Phase 2: Main execution")
    se_state_machine("state", cases)
    
    se_log("Phase 3: Shutdown")
    se_return_function_terminate()
end)
```

### Debugging Parallel Execution

```lua
se_fork(function()
    se_log("Fork child 1 started")
    se_tick_delay(10)
    se_log("Fork child 1 done")
end)

se_log("Main thread continues")  -- Logs same tick as "Fork child 1 started"
```

## Timestamp Source

Uses the allocator's `get_time` callback:

```c
double timestamp = mod->alloc.get_time(mod->alloc.ctx);
```

Typically returns seconds since epoch with microsecond precision.

## Debug Callback Registration

Register a debug callback when loading the engine:

```c
static void debug_callback(s_expr_tree_instance_t* inst, const char* msg) {
    printf("  [DEBUG] %s\n", msg);
}

// When loading:
s_engine_load_from_rom(&engine, &alloc, rom_data, rom_size, debug_callback, ...);
```

## ONESHOT Behavior in Composites

### In se_sequence

Fires immediately, sequence advances to next child same tick:

```lua
se_sequence(function()
    se_log("A")         -- Tick 1: logs, advances
    se_log("B")         -- Tick 1: logs, advances
    se_tick_delay(10)   -- Tick 1-10: waits
    se_log("C")         -- Tick 11: logs
end)
```

### In se_fork / se_fork_join

Fires once on first tick, skipped on subsequent ticks:

```lua
se_fork_join(function()
    se_log("Started")     -- Tick 1: logs
    se_tick_delay(10)     -- Tick 1-10: waits
    se_log("Finished")    -- Tick 11: logs
end)
```

### In se_function_interface

Fires immediately, doesn't block siblings:

```lua
se_function_interface(function()
    se_log("Start")       -- Tick 1: logs
    se_tick_delay(10)     -- Tick 1: starts, blocks
    se_log("End")         -- Tick 11: logs
end)
```

## Error Handling

| Error | Behavior |
|-------|----------|
| Missing message parameter | `EXCEPTION("SE_LOG: param_count < 1")` |
| String not found | `EXCEPTION("SE_LOG: msg not found")` |
| No debug callback | Falls back to `printf` (if enabled) |
| No timestamp source | Uses `0.0` as timestamp |

## Buffer Size

Messages are truncated to 256 characters (including timestamp):

```c
char buf[256];
snprintf(buf, sizeof(buf), "[%.6f] %s", timestamp, msg);
```

## Comparison with Other Oneshots

| Function | Purpose | Parameters |
|----------|---------|------------|
| `se_log` | Debug logging | string message |
| `se_set_field` | Set blackboard field | field name, value |
| `se_return_*` | Return result code | none |

All oneshots:
- Fire once
- Complete immediately
- Don't block execution
- Use `o_call` in Lua DSL

