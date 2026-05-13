# Function Dictionary Test — STM32F4 Peripheral Configuration

## Overview

This test demonstrates the **Function Dictionary** feature of the S-Expression Engine. A function dictionary is a collection of named, one-tick stack-based functions that can call each other internally to produce complex results. In this test, a small set of primitive I/O functions — `write_register` and `read_modify_write` — are composed into higher-level peripheral configuration routines that set up GPIO, UART, and SPI on a simulated STM32F4 microcontroller.

The key insight is that instead of writing many individual user-defined C functions for each peripheral register operation, only a single C callback (`write_register`) is needed. All the register address computation, bit manipulation, and sequencing logic lives in the dictionary as S-Expression tree nodes, using `quad_expr` expressions for arithmetic and bitwise operations.

## Dictionary Architecture

The dictionary is structured as a hierarchy of reusable functions:

```
init_all_peripherals          (top-level orchestrator)
  ├── enable_peripheral_clock   (clock setup, called 3x)
  │     └── read_modify_write     (bit manipulation)
  │           └── write_register    (C user function)
  ├── configure_gpio_pin        (GPIO register config)
  │     └── read_modify_write (x3: MODER, OSPEEDR, PUPDR)
  ├── configure_uart            (USART setup)
  │     ├── read_modify_write (x3: disable, config, enable)
  │     └── write_register    (x1: BRR baud rate)
  └── configure_spi             (SPI setup)
        ├── read_modify_write (x2: disable, enable)
        └── write_register    (x1: CR1 config)
```

### Dictionary Functions

| Function | Stack Params | Description |
|----------|-------------|-------------|
| `write_register` | addr, value | C callback that performs the hardware write |
| `read_modify_write` | addr, clear_mask, set_bits | Simulates read-modify-write: clears bits then sets bits |
| `enable_peripheral_clock` | clk_reg, periph_bit | Enables a peripheral clock via RCC register |
| `configure_gpio_pin` | port_base, pin, mode, speed, pull | Configures a GPIO pin's mode, speed, and pull-up/down |
| `configure_uart` | usart_base, baud_div, config_bits | Disables USART, sets baud rate, configures, re-enables |
| `configure_spi` | spi_base, clk_div, mode, bit_order | Disables SPI, builds CR1 register, writes config, re-enables |
| `init_all_peripherals` | (none) | Top-level: enables clocks, configures all peripherals |

## Dictionary Loading

The dictionary is loaded at tree construction time with `se_load_function_dict`:

```lua
se_load_function_dict("fn_dict", input_dictionary)
```

This stores the dictionary into the blackboard field `fn_dict`. The dictionary is a Lua table of `{name, builder_function}` pairs. Each builder function emits S-Expression tree nodes (using `se_call`, `quad_expr`, `quad_mov`, etc.) that define the function's behavior. Once loaded, the dictionary is available for the lifetime of the tree and can be called from anywhere in the S-Expression program.

## Calling Dictionary Functions

There are three ways to call dictionary functions:

### 1. Direct Call by Name (compile-time constant)

```lua
se_exec_dict_fn("fn_dict", "init_all_peripherals")
```

This is used from the main S-Expression program to invoke a dictionary function by a
name known at DSL compile time. The function name is hashed at compile time and embedded
directly in the generated code. The caller pushes any required parameters onto the stack
before calling. This is the simplest entry point from the tree's main program into the
dictionary.

### 2. Indirect Call via Hash Field (runtime variable)

```lua
se_set_hash_field("fn_hash", "init_all_peripherals")
se_exec_dict_fn_ptr("fn_dict", "fn_hash")
```

This two-step approach stores a function name hash into a blackboard field (`fn_hash`),
then calls `se_exec_dict_fn_ptr` which reads the hash from that field at runtime and
dispatches the corresponding dictionary function.

While the example above appears redundant with the direct call (since the function name
is a compile-time constant), these two functions exist because the hash field can be set
by any source at runtime — not just `se_set_hash_field`. In practice, the hash value
may come from:

- **An external tree** writing into the blackboard via `se_set_external_field`
- **A C callback** setting the field based on sensor input or protocol messages
- **A state machine** selecting different dictionary functions based on runtime conditions
- **An event handler** dispatching different operations based on event type

This is the mechanism that enables the **external tree calling pattern**: a parent tree
spawns a child tree containing a function dictionary, writes a function hash into the
child's `fn_hash` field, and ticks the child to execute that function. The child tree
does not need to know which function will be called at compile time.

### 3. Internal Call (dictionary function to dictionary function)

```lua
quad_mov(cv.addr, stack_push_ref())()
quad_mov(cv.current, stack_push_ref())()
se_exec_dict_internal("write_register")
```

Inside a dictionary function, `se_exec_dict_internal` calls another function within the same dictionary. Parameters are pushed onto the stack using `quad_mov` with `stack_push_ref()` as the destination. The called function receives these as its stack frame parameters. This is how `read_modify_write` calls `write_register`, and how `configure_gpio_pin` calls `read_modify_write`.

## The User Function: write_register

The only C callback in this test is `write_register`. It reads two parameters from the stack frame — the register address and the value to write — and prints them:

```c
void write_register(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    printf("write_register called\n");
    const s_expr_param_t* address_param = s_expr_stack_get_local(inst->stack, 0);
    printf("register address: 0x%08X\n", address_param->uint_val);
    const s_expr_param_t* value_param = s_expr_stack_get_local(inst->stack, 1);
    printf("register value: 0x%08X\n", value_param->uint_val);
}
```

In a production system, this function would perform the actual memory-mapped register write: `*(volatile uint32_t*)addr = value`. All the address computation and bit manipulation is handled by the dictionary functions, so this single C function serves every register write in the entire peripheral configuration sequence.

## Main Program Flow

The main program exercises both dictionary calling methods:

```lua
se_function_interface(function()
    -- 1. Initialize blackboard fields with peripheral configuration values
    se_set_field("uart_channel", 1)
    se_set_field("uart_baud", 0x0683)
    -- ... (GPIO, SPI fields)

    -- 2. Load the function dictionary
    se_load_function_dict("fn_dict", input_dictionary)

    -- 3. Direct call: invoke by compile-time name
    se_exec_dict_fn("fn_dict", "init_all_peripherals")

    -- 4. Log configuration results
    se_log("--- Configuration Results ---")
    se_log_slot_integer("config_state 0x%08X", "config_state")
    -- ...

    -- 5. Indirect call: set hash field, then dispatch via fn_ptr
    se_set_hash_field("fn_hash", "init_all_peripherals")
    se_exec_dict_fn_ptr("fn_dict", "fn_hash")

    -- 6. Terminate
    se_return_function_terminate()
end)
```

Steps 3 and 5 both invoke `init_all_peripherals`, but through different mechanisms.
Step 3 uses a compile-time constant hash. Step 5 demonstrates the indirect path where
the hash lives in a blackboard field — the same field that an external tree or C callback
would write to in a production system. Both paths produce identical results, confirming
that the two calling conventions are interchangeable.

## Control Flow Within the Dictionary

The dictionary supports the full range of S-Expression control flow constructs. This test demonstrates:

- **`se_if_then_else`** — Used in `init_all_peripherals` to conditionally configure UART and SPI based on blackboard field values. If `uart_channel` is non-zero, UART is configured; otherwise it is skipped.
- **`se_sequence_once`** — Ensures the initialization sequence runs exactly once.
- **`se_set_field` / `se_field_ne`** — Blackboard fields store configuration state and drive conditional logic.

Additionally, **`se_dispatch_event`** is available within dictionary functions for event-driven workflows, though it is not exercised in this particular test.

## Expression Compiler Usage

The dictionary functions use `quad_expr` to compile C-like expressions into quad operations at DSL build time:

```lua
quad_expr("shift = pin * 2", cv, {"t0"})()
quad_expr("mask = 3 << shift", cv, {"t0"})()
quad_expr("set_val = mode << shift", cv, {"t0"})()
quad_expr("reg_addr = port_base + 8", cv, {"t0"})()
```

The `frame_vars` function defines named locals and scratch variables with stack frame offsets, replacing raw `stack_local(N)` references with readable names. The expression compiler handles operator selection (integer arithmetic, bitwise operations), constant folding, and type inference automatically.

**Important constraint:** Values that will be read after a `stack_push_ref()` call must be stored in frame locals, not scratch (TOS) variables. The stack push advances the stack pointer, which can invalidate scratch-relative offsets.

## Test Results Explained

The test runs to completion in a single tick, producing 13 register writes (executed twice — once via direct call, once via indirect hash call):

### Clock Enables (RCC)

| Register | Value | Description |
|----------|-------|-------------|
| `0x40023830` (AHB1ENR) | `0x00000001` | Enable GPIOA clock (bit 0) |
| `0x40023844` (APB2ENR) | `0x00000010` | Enable USART1 clock (bit 4) |
| `0x40023844` (APB2ENR) | `0x00001000` | Enable SPI1 clock (bit 12) |

### GPIO PA5 Configuration

Pin 5 uses bit positions 10-11 (shift = pin × 2 = 10), with a 2-bit mask of `0xC00`.

| Register | Value | Description |
|----------|-------|-------------|
| `0x40020000` (MODER) | `0x00000800` | Alt-function mode (2 << 10) |
| `0x40020008` (OSPEEDR) | `0x00000800` | High speed (2 << 10) |
| `0x4002000C` (PUPDR) | `0x00000000` | No pull-up/pull-down (0 << 10) |

### USART1 Configuration

| Register | Value | Description |
|----------|-------|-------------|
| `0x4001100C` (CR1) | `0x00000000` | Disable USART (clear UE bit 13) |
| `0x40011008` (BRR) | `0x00000683` | Baud rate divisor for 115200 @ 16MHz |
| `0x4001100C` (CR1) | `0x0000200C` | Set UE, TE, RE (enable with TX+RX) |
| `0x4001100C` (CR1) | `0x00002000` | Enable USART (set UE bit 13) |

### SPI1 Configuration

SPI1 CR1 is at base address `0x40013000` (offset 0). Clock divider 2 maps to bits 5:3 = `0x10`.

| Register | Value | Description |
|----------|-------|-------------|
| `0x40013000` (CR1) | `0x00000000` | Disable SPI (clear SPE bit 6) |
| `0x40013000` (CR1) | `0x00000010` | CR1 = clk_div(2)<<3 \| mode(0) \| bit_order(0)<<7 |
| `0x40013000` (CR1) | `0x00000040` | Enable SPI (set SPE bit 6) |

### Final Blackboard State

After configuration completes, the blackboard fields confirm success:

| Field | Value | Meaning |
|-------|-------|---------|
| `config_state` | `0x00000004` | CONFIG_DONE |
| `peripherals_ready` | `0x00000001` | All peripherals initialized |
| `error_code` | `0x00000000` | No errors |

## Dictionary Calling Methods Summary

| Method | DSL Function | Hash Source | Use Case |
|--------|-------------|-------------|----------|
| Direct | `se_exec_dict_fn("fn_dict", "name")` | Compile-time constant | Known function, called from main program |
| Indirect | `se_exec_dict_fn_ptr("fn_dict", "fn_hash")` | Blackboard field (runtime) | Variable dispatch, external tree calls, event-driven selection |
| Internal | `se_exec_dict_internal("name")` | Compile-time constant | Dictionary function calling another dictionary function |

The indirect method is the key enabler for cross-tree dictionary invocation. A parent tree
can write any function hash into the child's `fn_hash` field via `se_set_external_field`,
then tick the child to execute that function. The child tree's dictionary serves as a
shared library of functions callable by any tree in the system.

## Key Design Pattern

This test illustrates a powerful pattern for embedded systems: **a minimal set of C hardware primitives composed through a dictionary of S-Expression functions**. The dictionary can be loaded once and called throughout the tree's lifetime. By moving register-level logic into the dictionary, the C codebase stays small (one `write_register` function), while the configuration logic remains flexible, readable, and modifiable without recompilation.