# Function Dictionary & External Tree Test

## Overview

This test demonstrates hierarchical tree composition combined with a function dictionary
pattern. A parent tree (`call_tree`) spawns a child tree (`function_dictionary`) at runtime,
injects a command hash into the child's blackboard, and ticks the child to execute a
complete STM32F4 peripheral initialization sequence — all driven by dictionary-dispatched
functions that use stack-based parameter passing.

## Purpose

This test validates three core capabilities of the external tree system:

1. **External tree lifecycle management** — How to spawn, tick, and terminate a child tree
   from a parent tree using `se_spawn_tree`, `se_tick_tree`, and the automatic cleanup
   on `SE_EVENT_TERMINATE`.

2. **Cross-tree parameter passing** — How to write values into a child tree's blackboard
   from the parent using `se_set_external_field` with compile-time field offsets obtained
   via `get_field_offset`. This provides a clean, typed interface between trees without
   requiring shared global state.

3. **Shared dictionary invocation across trees** — How to call a specific dictionary entry
   in another tree by injecting a function hash into the child's blackboard before ticking.
   This is significant because it means a large function dictionary only needs to exist in
   one tree and can be invoked by any number of other trees. Without this pattern, every
   tree that needs dictionary functions would have to include its own copy, wasting ROM
   and increasing maintenance burden on memory-constrained embedded targets.

## Module Structure

**Module name:** `external_tree`

### Trees

| Tree | Record | Purpose |
|------|--------|---------|
| `function_dictionary` | `cpu_config_blackboard` | Contains a function dictionary with peripheral configuration routines. Executes whichever dictionary entry is named by `fn_hash`. |
| `call_tree` | `call_blackboard` | Parent tree that spawns `function_dictionary`, sets the command hash, ticks the child, and terminates. |

---

## Blackboard Records

### cpu_config_blackboard

| Field | Type | Purpose |
|-------|------|---------|
| `fn_dict` | `ptr64` | Pointer to the loaded function dictionary |
| `fn_hash` | `uint32` | Hash of the dictionary function to execute |
| `gpio_port` | `uint32` | GPIO port base address |
| `gpio_pin` | `uint32` | GPIO pin number |
| `gpio_mode` | `uint32` | GPIO mode (input/output/alt-fn/analog) |
| `gpio_speed` | `uint32` | GPIO speed setting |
| `gpio_pull` | `uint32` | GPIO pull-up/pull-down setting |
| `uart_channel` | `uint32` | UART channel number (0 = disabled) |
| `uart_baud` | `uint32` | UART baud rate divisor |
| `uart_parity` | `uint32` | UART parity setting |
| `uart_stop_bits` | `uint32` | UART stop bits |
| `uart_flow_ctrl` | `uint32` | UART flow control |
| `spi_channel` | `uint32` | SPI channel number (0 = disabled) |
| `spi_clock_div` | `uint32` | SPI clock divider |
| `spi_mode` | `uint32` | SPI mode (0-3) |
| `spi_bit_order` | `uint32` | SPI bit order (MSB/LSB first) |
| `config_state` | `uint32` | Configuration state machine state |
| `error_code` | `uint32` | Error code |
| `peripherals_ready` | `uint32` | Set to 1 when all peripherals are configured |
| `temp_reg_addr` | `uint32` | Scratch register address |
| `temp_reg_value` | `uint32` | Scratch register value |

### call_blackboard

| Field | Type | Purpose |
|-------|------|---------|
| `tree_pointer` | `ptr64` | Pointer to the spawned child tree instance |
| `dictionary_hash` | `uint32` | Hash value to inject into the child's `fn_hash` field |

---

## Function Dictionary Entries

The `function_dictionary` tree contains a dictionary of stack-based callable functions
that form a layered peripheral abstraction. Each function uses `se_call` with `frame_vars`
for safe local variable management.

### Call Hierarchy

```
init_all_peripherals
├── enable_peripheral_clock  (×3: GPIOA, USART1, SPI1)
│   └── read_modify_write
│       └── write_register
├── configure_gpio_pin
│   └── read_modify_write  (×3: MODER, OSPEEDR, PUPDR)
│       └── write_register
├── configure_uart  (conditional: uart_channel != 0)
│   ├── read_modify_write  (disable USART)
│   ├── write_register     (set BRR)
│   ├── read_modify_write  (set CR1 config)
│   └── read_modify_write  (enable USART)
└── configure_spi   (conditional: spi_channel != 0)
    ├── read_modify_write  (disable SPI)
    ├── write_register     (write CR1)
    └── read_modify_write  (enable SPI)
```

### Function Details

| Function | Stack Params | Locals | Description |
|----------|-------------|--------|-------------|
| `write_register` | addr, value | — | Low-level register write |
| `read_modify_write` | addr, clear_mask, set_bits | current, inv_mask | Read-modify-write: clears bits then sets bits |
| `enable_peripheral_clock` | clock_reg_addr, peripheral_bit | — | Calls `read_modify_write` to set a clock enable bit |
| `configure_gpio_pin` | port_base, pin, mode, speed, pull | shift, mask, reg_addr, set_val | Configures MODER, OSPEEDR, and PUPDR registers |
| `configure_uart` | usart_base, baud_div, config_bits | reg_addr | Disables USART, sets baud rate and config, re-enables |
| `configure_spi` | spi_base, clock_div, mode, bit_order | cr1 | Disables SPI, builds and writes CR1, re-enables |
| `init_all_peripherals` | — | — | Top-level orchestrator that enables clocks and configures all peripherals |

---

## STM32F4 Peripheral Constants

The test uses realistic STM32F4 base addresses and register offsets:

| Constant | Value | Description |
|----------|-------|-------------|
| `RCC_AHB1ENR` | `0x40023830` | AHB1 peripheral clock enable register |
| `RCC_APB2ENR` | `0x40023844` | APB2 peripheral clock enable register |
| `GPIOA_BASE` | `0x40020000` | GPIO Port A base address |
| `USART1_BASE` | `0x40011000` | USART1 base address |
| `SPI1_BASE` | `0x40013000` | SPI1 base address |

Register offsets follow the STM32F4 reference manual (e.g., MODER at +0, OSPEEDR at +8,
PUPDR at +12, USART BRR at +8, USART CR1 at +12).

---

## Parent Tree Flow (call_tree)

The `call_tree` demonstrates the external tree management pattern:

```
1. se_spawn_tree("tree_pointer", "function_dictionary", 128)
   → Creates child tree with stack capacity of 128

2. se_set_hash_field("dictionary_hash", "init_all_peripherals")
   → Stores the FNV-1a hash of "init_all_peripherals" in local blackboard

3. se_set_external_field("dictionary_hash", "tree_pointer", dictionary_offset)
   → Copies the hash value into the child tree's fn_hash field

4. se_tick_tree("tree_pointer")
   → Ticks the child tree, which executes init_all_peripherals

5. se_log("call_tree: called")
   → Confirms execution

6. se_return_function_terminate()
   → Terminates the parent tree
```

The key mechanism is `get_field_offset("cpu_config_blackboard", "fn_hash")` which retrieves
the byte offset of `fn_hash` in the child's blackboard at DSL compile time, allowing
`se_set_external_field` to write directly to the correct location.

---

## Key Design Patterns Demonstrated

### Stack-Based Parameter Passing
Dictionary functions receive parameters via the stack using `frame_vars` to declare
named parameters and locals. Values are pushed with `stack_push_ref()` before calling
`se_exec_dict_internal()`.

### Frame Variable Safety
All values that survive a `stack_push_ref()` call are stored in frame locals, not
scratch (TOS) variables. This is critical because `stack_push` advances `sp`, which
invalidates scratch-relative offsets.

### Expression Compilation
`quad_expr` compiles arithmetic expressions (shifts, masks, bitwise operations) into
sequences of quad operations, enabling register-manipulation math within the DSL.

### Conditional Peripheral Configuration
`se_if_then_else` with `se_field_ne` predicates allows peripheral configuration to be
skipped when the corresponding channel field is zero, demonstrating runtime branching.

### Cross-Tree Communication
The parent tree writes into the child's blackboard using a compile-time field offset,
providing a clean interface between trees without requiring the child to know about
the parent.

---

## Dependencies

- `se_spawn_tree` / `se_tick_tree` / `se_set_external_field` — External tree management
- `se_load_function_dict` / `se_exec_dict_fn_ptr` / `se_exec_dict_internal` — Function dictionary
- `se_call` / `frame_vars` / `stack_push_ref` — Stack-based calling convention
- `quad_expr` / `quad_mov` / `quad_not` / `quad_and` / `quad_or` — Expression compilation
- `se_function_interface` / `se_sequence_once` / `se_if_then_else` — Control flow
- `se_set_field` / `se_i_set_field` / `se_set_hash_field` — Blackboard access
- `get_field_offset` — Compile-time field offset lookup

