# cfl_avr — continue.md

**Status (2026-05-27 evening):** Engine refactor complete and event-driven.
Five Linux-native tests all pass under `make test`. Next session is the
**first hardware bring-up on Arduino Nano (ATmega328P / CH340)**.

## What landed today

- Event-driven dispatcher (refactor of earlier tick-walker shape)
- Locked semantics from a multi-round design dialog (see memory:
  [[cfl-avr-engine-2026-05-27]])
- Ten built-in node kinds covering the canonical patterns
- Five passing tests on Linux

### File layout

```
cfl_avr/
├── include/
│   ├── cfl_progmem.h   — PROGMEM + atomic-save/restore shims (no-op on Linux)
│   ├── cfl_types.h     — return codes, event ids, node kinds, descriptors
│   └── cfl_engine.h    — public API + cfl_engine_t struct
├── runtime/
│   └── cfl_engine.c    — full dispatcher, ~460 lines
├── tests/
│   ├── heartbeat/      — HB every 3 ticks + LED every tick
│   ├── state_machine/  — A→B→C chain enable + TERMINATE_SELF
│   ├── while_verify/   — WHILE polling + continuous VERIFY
│   ├── init_term/      — INIT_TERM session-scope RAII
│   └── isr_events/     — cfl_send_event_isr + user event ids
└── Makefile            — Linux test build (`make test`)
```

### Engine semantics (locked)

- **Event-driven** — engine pumps a FIFO queue of `(event_id, u16 data)` pairs.
- **INIT and TERMINATE are NOT queued** — engine-synthesized, targeted to a
  specific node. Return values ignored.
- **TIME_TICK and user events ARE queued** (broadcast to all enabled chains).
- **Return codes:** `CFL_CONTINUE / CFL_HALT / CFL_DISABLE / CFL_RESET / CFL_TERMINATE`.
- **Lazy INIT** — fires when first event reaches a not-yet-initialized armed node.
- **Reverse-order TERMINATE sweep** on chain disable.
- **Each chain stands alone** — RESET/TERMINATE return doesn't suppress event
  for sibling chains.
- **`cfl_chain_enable/disable/reset` are "other chains only"** — self-modify
  via APIs panics. Self uses return codes.
- **Queue overflow → panic_hook → reset.**
- **ISR-safe path:** `cfl_send_event_isr` skips the atomic guard (caller is in
  cli already). Modbus-in-ISR pattern works directly.

### Node kinds available

| Kind | Behavior |
|---|---|
| NOP | placeholder |
| M_CALL | fn called on every event; fn return drives dispatch |
| ONE_SHOT | INIT calls fn; TICK returns DISABLE |
| TIME_DELAY | INIT clears scratch; TIME_TICK accumulates data; ≥ param[0] → DISABLE |
| ENABLE_CHAINS | enables N chains by id on INIT, then DISABLE |
| DISABLE_CHAINS | disables N chains by id on INIT, then DISABLE |
| RESET_SELF | returns CFL_RESET (self-restart) |
| TERMINATE_SELF | returns CFL_TERMINATE (self-disable) |
| WHILE | polls fn; CONTINUE → HALT (still looping); DISABLE → exit |
| VERIFY | continuous monitor; OK → CONTINUE; anything else → RESET |
| INIT_TERM | INIT calls fn (acquire); TERM calls fn_idx_2 (release); TICK → CONTINUE |

## Tomorrow's task — heartbeat on ATmega328P Arduino Nano

### Hardware

- **DORHEA 4-pack Arduino Nano clones** — ATmega328P, CH340G USB-serial, 16 MHz, 5V.
- 32 KB flash (~30 KB usable after Optiboot), 2 KB SRAM, 1 KB EEPROM.
- LED on **PB5** (Arduino D13).
- USART0 on PD0 (RX) / PD1 (TX) — connected to the on-board CH340G; appears as
  `/dev/ttyUSB0` (or similar) when plugged in.

### Toolchain

- **avr-gcc + avr-libc + avrdude.** All apt-installable: `sudo apt install
  gcc-avr avr-libc avrdude`.
- Flash via the **Arduino bootloader**: `avrdude -c arduino -p atmega328p -P
  /dev/ttyUSB0 -b 57600 -U flash:w:heartbeat.hex`. Hold-down-reset not needed
  — Optiboot auto-resets on connection.
- **WSL2 caveat:** USB serial isn't passed through by default. Either
  (a) use `usbipd-win` to forward `/dev/ttyUSB0` into WSL, or
  (b) build in WSL, copy `.hex` to Windows side, flash from Windows with
  avrdude there.

### Concrete steps

1. **Sanity check the toolchain.** `avr-gcc --version`, plug in board,
   confirm `/dev/ttyUSB0` (or whatever) appears in `lsusb` + `dmesg | tail`.
   Optionally flash a stock 1-second-blink hex first to confirm the
   bootloader + USB path works end-to-end. **~15 min.**

2. **Write `runtime/cfl_main_atmega328.c`** — ~150 lines, covering:
   - Timer0 in CTC mode, OCR0A=249, /64 prescaler → 1 kHz interrupt
   - `ISR(TIMER0_COMPA_vect) { cfl_send_event_isr(&engine,
     CFL_EVENT_TIME_TICK, 1); }` (1 ms per tick)
   - USART0 init at 57600 baud + a `FILE` glue for `printf`
   - PB5 LED helper: `DDRB |= _BV(5); PORTB ^= _BV(5);`
   - Panic hook: write crash record to a 16 B `.noinit` struct, arm WDT for
     16 ms timeout, spin
   - `main()`: `cfl_engine_init(&engine); sei(); for (;;) {
     cfl_engine_pump(&engine); sleep_mode(); }` (sleep is optional for v1)

3. **Adjust the heartbeat test's user fns** for the AVR build:
   - `send_heartbeat`: `printf("HB %u\n", count);` over USART
   - `toggle_led`: `PORTB ^= _BV(5);`
   - Same module table, same chain shapes. Only the user fn bodies change.
   - Conditional compilation via `#ifdef __AVR__` in the test's main.c.

4. **AVR `Makefile` target** alongside the Linux build:
   ```make
   AVR_MCU   = atmega328p
   AVR_FCPU  = 16000000UL
   AVR_FLAGS = -mmcu=$(AVR_MCU) -DF_CPU=$(AVR_FCPU) -Os -Wall -Iinclude
   AVR_PORT ?= /dev/ttyUSB0
   $(BUILD_DIR)/heartbeat_avr.elf: <sources>
       avr-gcc $(AVR_FLAGS) -o $@ ...
   $(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf
       avr-objcopy -O ihex $< $@
   flash-heartbeat: $(BUILD_DIR)/heartbeat_avr.hex
       avrdude -c arduino -p $(AVR_MCU) -P $(AVR_PORT) -b 57600 \
               -U flash:w:$<
   ```

5. **Flash and observe.** Open `screen /dev/ttyUSB0 57600` (or PuTTY). LED
   should toggle every 1 ms (visibly indistinguishable from "always on" —
   we'll need a slower TIME_DELAY param to make it visibly blink. Suggest
   bumping the LED chain's RESET_SELF to be after a TIME_DELAY(250) for
   a 250 ms toggle = 2 Hz).

6. **If green:** port the other four tests one at a time. Each is one new
   module.c. Each takes 15–30 min.

### Memory budget on ATmega328P

- Engine flash: ~2.5–3 KB (classic AVR -Os)
- Tests + glue: ~1–2 KB
- avr-libc + printf + IRQ vectors: ~2–3 KB
- **Total flash: ~6–8 KB of 30 KB usable** — comfortable
- Engine SRAM state: ~220 B; stack peak ~256 B; printf buffer ~128 B;
  miscellaneous BSS ~150 B
- **Total SRAM: ~800 B of 2 KB** — comfortable

### Pre-bringup questions to answer

1. Is the user on Linux or WSL2 for flashing? If WSL2, has usbipd-win been
   set up? If not, plan B is flashing from Windows.
2. What baud rate for USART? 57600 matches Optiboot; 115200 is also common.
3. Should the heartbeat test on hardware run for a bounded count then halt,
   or run forever? Bounded is easier to verify; forever is closer to
   production reality.

### Followups (post-heartbeat)

- Port other four tests to AVR
- Add the `.noinit` crash-record dump on boot — verifies panic path works
- Stack paint + HWM measurement (port the [[samd21-stack-budget]] hardening
  pattern to ATmega328P)
- Eventually: cross-compile to AVR32SD32 Curiosity Nano (the original
  production target, with lockstep + ECC + functional-safety framework).
  Engine code unchanged; only `cfl_main_atmega328.c` → `cfl_main_avr32sd32.c`.

## Outstanding small items

- `node_synth_init` was inlined into `dispatch_chain` — leftover comment in
  the header still mentions it. Cleanup-pending, not a bug.
- The `panic_hook` field is in `cfl_engine_t` but no test currently sets it.
  AVR main will set it; Linux tests should probably set it to a function
  that calls `abort()` so test failures show stack traces.
- `CFL_KIND_O_CALL` alias was dropped in this refactor. If you have any
  external references to it, they'll need updating.

## Related memory

- [[cfl-avr-engine-2026-05-27]] — engine architecture + locked semantics
- [[s-engine-m-port-architecture-2026-05-12]] — the parent design discipline
  that cfl_avr departs from (Harvard + 2 KB SRAM made interpretation
  unaffordable; codegen→event-driven runtime was the resolution)
- [[four-chip-dongle-pivot-2026-05-11]] — broader chip-suite context;
  cfl_avr's eventual AVR32SD32 target slots into the safety-supervisor role
