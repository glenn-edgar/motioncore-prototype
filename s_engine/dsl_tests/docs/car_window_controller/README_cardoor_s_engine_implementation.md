# Door Window Controller — DSL Implementation Guide

## Overview

`door_window_controller.lua` is an S-Expression Engine DSL v5.3 implementation of a car power window controller built around the Infineon TLE7269G H-bridge motor driver. The DSL file compiles to a binary behavior tree that runs on the S-Expression Engine runtime, targeting 32-bit ARM Cortex-M microcontrollers.

All control logic, sequencing, state transitions, and coordination are handled entirely by the S-Expression Engine's built-in functions (`SE_*`). The user functions listed in `door_window_controller_user_functions.h` are strictly I/O functions — they read sensors, write to hardware registers, and format messages. They contain no control flow or decision logic.

## Disclaimer — Interrupt-Level Safety

In a production implementation, several I/O and safety-critical functions would be placed in hardware interrupt handlers rather than called from the behavior tree tick loop:

**Handled at ISR level (not in the tree tick):**
- **Overcurrent detection** — The TLE7269G's current sense output drives a comparator interrupt that disables the H-bridge within microseconds, independent of the tree tick rate
- **TLE7269G fault pin** — A GPIO interrupt on the chip's fault output kills motor drive immediately; the ISR sets the `fault_pin_active` blackboard flag for the tree to handle recovery
- **Emergency stop hardware input** — A dedicated GPIO interrupt disables the H-bridge outputs directly, then sets the `emergency` flag
- **UART RX** — Byte-level serial reception fills a ring buffer via interrupt; the tree checks `serial_data_avail` each tick
- **ADC conversion complete** — Current and temperature readings are sampled by ADC interrupts and stored; the tree reads the latest values from the blackboard
- **Timer/PWM** — Hardware timer interrupts generate the motor PWM signal and provide the system tick

**Handled by the behavior tree (tick-level):**
- All state machine logic and state transitions
- Motor startup/shutdown sequencing (bridge mode transitions, brake-before-freewheel timing)
- Command parsing and serial dispatch
- Concurrent monitor evaluation (reading ISR-set flags)
- Auto-reverse sequencing after obstruction
- Diagnostic data collection and status reporting
- Fault flag management and manual reset handling

The tree structure remains fully intact — ISRs set blackboard flags and post events, the tree reads them deterministically each tick. This gives microsecond hardware protection with deterministic, testable software logic.

## Architecture

The compiled tree has a single root node (`se_function_interface`) containing a four-branch `se_fork`:

```
se_function_interface
└── se_fork
    ├── Branch 1: Serial Command Handler    (se_chain_flow pipeline)
    ├── Branch 2: Motor State Machine       (se_state_machine, 6 cases)
    ├── Branch 3: Status Reporter           (se_chain_flow pipeline)
    └── Branch 4: Diagnostics Monitor       (se_fork of 3 se_while loops)
```

### Branch 1 — Serial Command Handler

A `se_chain_flow` pipeline that:
1. Waits for `serial_data_avail` flag (set by UART ISR)
2. Calls `READ_SERIAL_MESSAGE` and `PARSE_MESSAGE_TYPE` (user I/O functions)
3. Dispatches via `se_field_dispatch` on `serial_msg_type` to one of 6 cases (open, close, stop, status, emergency, default)

Each command case sets the appropriate request flag and posts an event, then issues `se_return_pipeline_reset()` to restart the pipeline for the next command.

### Branch 2 — Motor State Machine

A `se_state_machine` dispatching on the `motor_state` field with six cases:

| State | Value | Description |
|---|---|---|
| IDLE | 0 | Motor killed, freewheel, polls for open/close/emergency requests via `se_cond` |
| OPENING | 1 | Drives motor forward; runs 3 concurrent monitors; waits for completion; shuts down; transitions based on outcome |
| CLOSING | 2 | Drives motor reverse; same monitor/shutdown pattern; obstruction triggers auto-reverse |
| AUTO_REVERSE | 3 | Brief reverse pulse at half speed (anti-pinch); returns to idle |
| EMERGENCY | 4 | Immediate motor kill; waits for manual reset; clears all fault flags; returns to idle |
| default | — | Invalid state recovery; forces transition to emergency |

**Opening and Closing** share a common pattern built from parameterized helper functions:
- `motor_start(direction, speed_field)` — sets bridge mode, PWM, enables motor
- `concurrent_monitors(guard_flag, limit_pred, done_flag, done_event, auto_reverse)` — forks three monitoring loops (current, limit switch, obstruction)
- `wait_for_completion(position_flag, timeout_msg)` — 30-second timeout + OR predicate on five stop conditions
- `motor_shutdown(status_msg)` — disable, brake hold, freewheel, status message
- `transition_after_open()` / `transition_after_close()` — `se_cond` dispatch to next state

### Branch 3 — Status Reporter

A `se_chain_flow` pipeline that waits for either a status request flag or a periodic 1-second event (composite OR predicate), then calls six user I/O functions to read diagnostics, assemble, and transmit a status message. Resets the pipeline to wait again.

### Branch 4 — Diagnostics Monitor

A `se_fork` of three `se_while` loops, each running continuously while `system_shutdown == 0`:

| Monitor | Condition | Action |
|---|---|---|
| Thermal | `temperature > 85.0` | Post thermal shutdown + emergency stop events |
| Voltage | `voltage < 9.0` OR `voltage > 16.0` | Post voltage error + emergency stop events |
| TLE7269G Fault | `fault_pin_active == 1` | Read fault status register, post emergency stop |

## User Functions (I/O Only)

The generated header `door_window_controller_user_functions.h` declares 17 user functions. These are all hardware I/O — they read from or write to peripherals. The S-Expression Engine built-in functions handle all control flow, sequencing, state management, and coordination.

### Oneshot Functions (14)

These execute once per invocation and return void:

| Function | Purpose | Hardware |
|---|---|---|
| `read_serial_message` | Read bytes from UART RX buffer | UART peripheral |
| `parse_message_type` | Parse command code from message buffer | Software (no I/O) |
| `disable_motor_tle7269g` | Disable TLE7269G motor outputs | SPI/GPIO to TLE7269G |
| `enable_motor_tle7269g` | Enable TLE7269G motor outputs | SPI/GPIO to TLE7269G |
| `set_bridge_mode` | Set H-bridge direction (fwd/rev/brake/freewheel) | SPI/GPIO to TLE7269G |
| `set_motor_pwm` | Set PWM duty cycle for motor speed | Timer peripheral |
| `send_status_message` | Transmit status string over UART | UART TX |
| `read_diagnostics_tle7269g` | Read TLE7269G diagnostic registers | SPI to TLE7269G |
| `read_current_position` | Read window position (encoder/potentiometer) | ADC or encoder |
| `read_motor_current` | Read motor current from sense resistor/TLE7269G | ADC |
| `read_temperature` | Read chip/ambient temperature sensor | ADC or I2C |
| `assemble_status_message` | Format diagnostic data into message buffer | Software (no I/O) |
| `send_serial_status` | Transmit assembled status over UART | UART TX |
| `read_fault_status_tle7269g` | Read TLE7269G fault register after fault pin | SPI to TLE7269G |

### Predicate Functions (3)

These return `bool` and are called by the engine's monitoring loops:

| Function | Purpose | Hardware |
|---|---|---|
| `check_limit_switch_open` | Check fully-open limit switch state | GPIO input |
| `check_limit_switch_closed` | Check fully-closed limit switch state | GPIO input |
| `check_motor_stall` | Detect motor stall (current spike / zero speed) | ADC + software |

## Blackboard Record

The `door_controller_bb` record defines the shared state visible to all tree branches:

| Category | Fields | Size |
|---|---|---|
| Motor state | `motor_state`, `bridge_mode`, `motor_pwm`, `motor_enabled` | 16 bytes |
| Sensor readings | `motor_current`, `max_current`, `position`, `temperature`, `voltage` | 20 bytes |
| Thresholds | `open_speed`, `close_speed`, `thermal_threshold`, `min_voltage`, `max_voltage` | 20 bytes |
| Status flags | `fully_open`, `fully_closed`, `emergency`, `obstruction`, `over_current`, `system_shutdown`, `manual_reset`, `fault_pin_active` | 32 bytes |
| Command flags | `open_request`, `close_request`, `stop_request`, `status_request`, `auto_reverse_req` | 20 bytes |
| Serial I/O | `serial_data_avail`, `serial_msg_type` | 8 bytes |
| Pointers | `config_ptr`, `diag_ptr` (PTR64) | 16 bytes |
| **Total** | **31 fields** | **132 bytes** |

## DSL Helper Function Design

The implementation uses Lua helper functions to eliminate code duplication and keep the tree readable. These are compile-time abstractions — they emit DSL nodes when called and have zero runtime overhead.

**Motor primitives:** `disable_motor()`, `enable_motor()`, `apply_bridge_mode()`, `set_pwm_field()`, `set_pwm_value()`, `send_status()` — each wraps a single `o_call` with its parameters.

**Composite sequences:** `motor_start()`, `motor_shutdown()`, `motor_kill()`, `clear_all_faults()` — bundle 4–8 DSL calls that always occur together.

**Parameterized monitors:** `monitor_current(guard_flag)`, `monitor_limit_switch(guard_flag, pred_name, done_flag, done_event)`, `monitor_obstruction(guard_flag, with_auto_reverse)` — return closures parameterized for opening vs. closing.

**Predicates:** `pred_any_stop(position_flag)` — composite OR predicate across five stop conditions; `pred_not_shutdown()` — guard for diagnostic loops.

**Transitions:** `transition_after_open()`, `transition_after_close()` — `se_cond` dispatch blocks for post-motion state routing.

### Sequencing Rule

- **`se_sequence`** — used for straight-through sequential DSL calls with no blocking or return codes
- **`se_chain_flow`** — used when the body contains `se_wait`, `se_tick_delay`, `se_time_delay`, or any `se_return_*` result code

## Compiled Binary Size

### Flash (ROM)

| Metric | Value |
|---|---|
| **Binary size** | **10,072 bytes** |
| S-expression parameters | 1,065 |
| Parameter size (32-bit) | 1,065 × 8 = 8,520 bytes |
| String table + headers | ~1,552 bytes |

The binary is zero-copy — the engine casts a pointer directly to the ROM data with no decoding, copying, or dynamic allocation at load time.

### RAM

| Component | Calculation | Bytes |
|---|---|---|
| Node execution state | ~250 nodes × 4 bytes | ~1,000 |
| pt_main pointer slots | 4 × 8 bytes | 32 |
| Blackboard | 22 int32 + 7 float + 2 ptr64 | 132 |
| **Total runtime RAM** | | **~1,164** |

### Summary

| Resource | Value |
|---|---|
| Flash (binary tree in ROM) | 10,072 bytes |
| RAM (runtime state) | ~1.2 KB |
| User functions (I/O only) | 14 oneshot + 3 predicate |
| Built-in engine functions | All control flow, sequencing, state dispatch |
| Blackboard fields | 31 (132 bytes) |
| Behavior tree nodes | ~250 |
| S-expression parameters | 1,065 |

This fits comfortably on a 32KB Flash / 4KB RAM Cortex-M0 target alongside the engine runtime, user function implementations, peripheral drivers, ISR handlers, and application stack.

## Build

```bash
luajit s_compile.lua door_window_controller.lua \
    --helpers=s_engine_helpers.lua --all-bin --outdir=generated/
```

Generated outputs:

| File | Purpose |
|---|---|
| `door_window_controller_records.h` | C struct definition for `door_controller_bb` |
| `door_window_controller.h` | Module header with hash table and string table |
| `door_window_controller_debug.h` | Debug hash-to-name mappings |
| `door_window_controller_user_functions.h` | User function prototypes (I/O functions) |
| `door_window_controller_user_registration.c` | Function registration code |
| `door_window_controller_32.bin` | Binary module for runtime loading |
| `door_window_controller_bin_32.h` | Binary as C array for ROM embedding |
| `door_window_controller_dump_32.h` | Human-readable parameter dump |

## License

MIT License — See repository LICENSE file.
