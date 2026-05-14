# Dongle Class DSL Examples

Four skeleton DSL chain definitions, one per dongle class declared in the canonical `dongle_classes.lua` catalog. **Templates** — not yet wired into a build. The actual firmware for each chip starts from one of these as its `<class>.lua`, then fills in chip-specific user functions in `user_functions.c`.

| File | Class | Capabilities |
|---|---|---|
| `samd21_shell_v1.lua` | SAMD21 Xiao leaf | GPIO, PWM, 12-bit ADC, 10-bit DAC, counter, quadrature |
| `ra4m1_analytic_v1.lua` | RA4M1 Xiao leaf | Above + signal.* CMSIS-DSP (Goertzel, FFT, biquad, Welford, cross-corr) |
| `rp2350_router_v1.lua` | RP2350 Pico 2 W router | PIO RS-485 + can2040 CAN + slave roster + bus stats |
| `esp32c6_router_v1.lua` | ESP32-C6 router | Above wired + WiFi 6 + BLE 5 + Thread (IEEE 802.15.4) |

## What's universal across all four

Same scaffolding pattern at the top of every chain — read once, recognize everywhere:

```
se_function_interface
├── se_i_set_field("dongle_state", BOOT)        [init blackboard]
├── m_call("wdt_strobe")                        [hardware watchdog pet every tick]
├── m_call("handle_internal_events")            [host reattach + commissioning]
├── se_state_machine("dongle_state", { BOOT, OPERATIONAL }):
│   ├── BOOT case:
│   │   └── se_fork(retry_loop, ack_dispatch)
│   └── OPERATIONAL case:
│       └── se_fork(heartbeat, led, app_dispatch, [class-specific branches...])
└── se_return_halt()
```

The only thing that varies between classes:

1. **The class name** (and thus the `class_id` hash)
2. **The opcode set** in OPERATIONAL's `event_dispatch` branch
3. **Optional extra fork branches** in OPERATIONAL — analytic dongles tick DSP loops, routers monitor slave rosters, wireless dongles pump ESP-IDF events
4. **The blackboard schema** — routers/wireless carry more state fields than leaves

## What's chip-specific (NOT in these DSL files)

The DSL files declare BEHAVIOR. The matching `user_functions.c` per chip implements:

- `send_register` — reads chip-specific UID, builds REGISTER payload v2 (see `memory/dongle_class_identity_2026-05-13.md`)
- `send_heartbeat`, `send_pong` — emit frames via libcomm
- `toggle_led` — GPIO write to chip-specific LED pin
- `wdt_strobe`, `wdt_enable` — chip-specific WDT register pokes
- `handle_internal_events` — flash storage for commissioning is chip-specific
- All the `handle_*` user fns referenced from event_dispatch — chip-specific peripheral drivers

## How to use these examples

For Phase 2h+ work, when a new chip's firmware is being brought up:

```bash
# 1. Copy the appropriate skeleton into the chip's app directory
cp docs/dongle_classes/samd21_shell_v1.lua \
   samd21/apps/samd21_shell_v1/samd21_shell_v1.lua

# 2. Run the DSL compiler to emit C module ROM tables
cd s_engine/lua_dsl
luajit s_compile.lua ../../samd21/apps/samd21_shell_v1/samd21_shell_v1.lua \
   --helpers=s_engine_helpers.lua \
   --emit-c=samd21_shell_v1_module_rom.c \
   --outdir=../../samd21/apps/samd21_shell_v1/

# 3. Build firmware (Makefile per chip references the emitted .c + class_ids.h)
cd ../../samd21/apps/samd21_shell_v1
make
```

The chain stays portable; only `user_functions.c` and the Makefile know the chip.

## What's still TBD

These files are templates with placeholder user-fn names. Before they compile cleanly, the implementer needs to:

1. **Resolve user fn names** — the DSL compiler validates against `s_engine_builtins_*.h` for builtins and link-time for user fns. Some user fn names here are speculative (e.g., `handle_goertzel_lock_subscribe` — exact name choice is per implementer).

2. **Decide on payload-bearing event mechanism** — these examples assume engine event_data carries a pointer into the per-event-data buffer pool (option B in the design memory). The implementer sets up the pool in main.c.

3. **Resolve OP_SHELL_EXEC vs dedicated opcodes** — these examples use dedicated opcodes per command (faster dispatch, simpler chain). If `OP_SHELL_EXEC` with command_hash is preferred, collapse the per-command `se_event_case` blocks into one `se_event_case(OP_SHELL_EXEC, ...)` plus a C-side command_hash → handler dispatch table. Design choice — neither approach is uniquely correct.

4. **Add `OP_COMMISSION_SET` / `OP_COMMISSION_CLEAR` handling** — these examples reference the engine-internal event `EV_COMMISSION_SET = 0xFE01` but don't show the chain-side handler. Add the case to `handle_internal_events` or as a sibling. See design memory.

5. **Add the actual chip's send_register payload structure** — currently `samd21/apps/register_dongle/user_functions.c` builds the v1 payload (24 B). Update to v2 (38 B with class_id + instance_id + commissioning_state per design memory).

## Reading order

1. `samd21_shell_v1.lua` — simplest example, get the universal pattern
2. `ra4m1_analytic_v1.lua` — see how adding capability (signal.* + extra fork branch) cleanly extends the shape
3. `rp2350_router_v1.lua` — see how the router pattern (downstream bus, slave roster, bus stats) layers on
4. `esp32c6_router_v1.lua` — see maximum-capability fork (6 parallel branches, wireless event pump)

The shape is consistent. The differences are the value.
