# cfl_avr — chain-flow engine for AVR

An event-driven chain-flow engine designed for 8-bit Harvard architectures
(AVR family, originally targeting ATmega328P/AVR32SD32). Direct port of a
20-year-tested pattern first deployed on PSoC4 4 KB SRAM.

This README is the **design rationale** — the *why* behind every architectural
choice. If you find yourself thinking "wouldn't it be simpler if we just...",
read the relevant lesson section first. Most "simpler" ideas were already
tried and discarded over the multi-round design dialog that produced this
engine.

For the user-facing API and node-kind reference, see `include/cfl_engine.h`
and `include/cfl_types.h`. For tomorrow's work, see `continue.md`.

---

## What it is

A runtime that executes **chains of nodes** in response to **events**. Each
chain is a small sequence (typically 2–10 nodes); the engine pumps a FIFO of
`(event_id: u8, data: u16)` pairs and broadcasts each event to every enabled
chain.

Nodes have **kinds** (NOP, M_CALL, TIME_DELAY, WHILE, VERIFY, INIT_TERM, …)
that determine how they react to events. Each node has up to two function
indexes + a slice of a packed `uint16_t` parameter pool. The engine + node
descriptors are compile-time const (PROGMEM); per-node state is bit-packed
into three small RAM bitmaps plus one `uint16_t` scratch per node.

A 100-chain / 100-node program costs ~50 B of RAM for engine state. Each
chain costs zero CPU when idle.

## What it isn't

- **Not an RTOS.** No threads, no preemption, no priorities. Single pump call
  drains the queue in main-context order.
- **Not an interpreter.** The DSL emits C arrays at build time. The chip runs
  tables, not parsed source.
- **Not a state machine library.** Chains are sequences; *programs* of chains
  form state machines via ENABLE_CHAINS/DISABLE_CHAINS transitions.
- **Not portable to anything but small embedded.** The whole point is fitting
  in 2 KB SRAM with Harvard PROGMEM penalties. On a Linux box, just use
  coroutines.

---

## Why it exists (the divergence from s_engine)

`s_engine/` is the sibling project — a similar DSL targeting larger chips
(SAMD21, RA4M1, RP2350). It uses **tick-walking interpretation**: every tick,
walk every node, dispatch.

That model assumes:
1. You can afford ~24 B of per-node instance state
2. PROGMEM is cheap to dereference (it isn't on AVR)
3. Tick budget can absorb walking N nodes (it can't at low MHz)

cfl_avr departs by:
1. **Bit-packed bitmaps** — per-node RAM cost drops 10×
2. **Indexes-not-pointers** — eliminates LPM penalty for cross-table refs
3. **Event-driven** — idle chains cost zero CPU; only enabled chains receive
   events

The trade: cfl_avr is less expressive (5 return codes, 11 node kinds, no
nested DSL). For the chain-flow workload, that's all you need.

---

## Locked design lessons

The following are decisions that came out of a ~31-round design dialog
informed by Glenn's PSoC4-era war stories. They are **not negotiable without
new evidence** — they each have a known failure mode that motivated them.

For the full prose discussion, see memory: `cfl-chain-flow-design-lessons`.

### 1. Indexes, not pointers (Harvard penalty)
All cross-table references are `uint8_t` indexes into RAM tables. PROGMEM is
read once per dispatch step, never chased.

### 2. Bit-packed bitmaps for chain/node state
- `chain_enable[]` — 1 bit per chain
- `node_enable[]` — 1 bit per node
- `node_initialized[]` — 1 bit per node
- `node_scratch[]` — 16-bit mutable slot per node (the only per-node RAM beyond bits)

### 3. Event-driven dispatch
TIME_TICK is just another event. Engine pumps a FIFO; idle chains cost zero
CPU. Bursty I/O (Modbus RX) is natural — ISR pushes event, main drains.

### 4. Each chain stands alone
A RESET/TERMINATE/DISABLE return from chain A does **not** suppress event
delivery to chain B. Chains are compositional.

### 5. Chain control APIs are "other chains only"
`cfl_chain_enable / disable / reset` **panic** if called on `current_chain`.
Self-modify uses return codes (`CFL_RESET`, `CFL_TERMINATE`).
Reason: self-disable mid-dispatch invalidates the dispatcher's iterator.

### 6. INIT and TERMINATE are NOT queued
Engine-synthesized targeted callbacks. Return values are **IGNORED**.
Reason: queueing creates races; ignoring returns prevents init-time loops.

### 7. Lazy INIT
INIT fires on first event delivery to an armed-but-uninitialized node, not
eagerly on enable. Lets engine state settle before user code runs.

### 8. Reverse-order TERMINATE sweep
On chain disable, walk nodes last-to-first, fire TERMINATE on each initialized
node, then clear enable. Mirrors init order — required for INIT_TERM RAII.

### 9. TERMINATE fires BEFORE the enable bit clears
Gives node a chance to release acquired state. Single biggest improvement
over PSoC, where `cf_disable_chain` just bit-zeroed with no cleanup.

### 10. Continuous VERIFY
VERIFY runs on every event, not one-shot. If fn returns CONTINUE → OK;
anything else → chain RESET. Watchdog pattern: voltage range, ack-pending
count, stall detection, etc.

### 11. State-based-on-links didn't work; controlling chains did
Each "state" of a state machine = one chain. Transitions are ENABLE_CHAINS +
DISABLE_CHAINS return codes. Don't try to encode states as links within one
chain — it doesn't compose.

### 12. ONE_SHOT discards fn return; M_CALL propagates
For "fire once and terminate the chain", use **M_CALL** returning
CFL_TERMINATE, not ONE_SHOT. ONE_SHOT is "do work, then go away (DISABLE
this node)" — it can't influence chain control.

### 13. Queue overflow → panic
Save cause to `.noinit`, arm WDT, reset. No silent drop. Silent drops hide
load-dependent bugs at the worst possible time.

### 14. ISR-safe push skips cli/sei
`cfl_send_event_isr` doesn't bracket the queue write — caller is already
atomic. Main-context API `cfl_send_event` MUST use the guard. Naming
discipline is the only defense; pick the wrong one → corrupt queue.

### 15. Skip suspend/resume; skip change_state
Both were in PSoC. Neither got used. Every feature you don't add is one you
don't debug.

### 16. Five return codes total
`CONTINUE / HALT / DISABLE / RESET / TERMINATE`. Sufficient to express every
observed PSoC chain. Resist SUSPEND, PAUSE, RESTART_NEXT_TICK.

### 17. Time ticks are events too
Don't special-case time. TIME_TICK is broadcast with `data` = ticks elapsed.
Hardware timer ISR pushes via `cfl_send_event_isr`.

### 18. `data` is 16 bits, meaning event-specific
Don't grow the queue slot for richer payloads — it explodes RAM on small
chips. Pass an index into a user-side table if you need more.

### 19. Engine vs user separation
Engine: dispatch, bitmaps, queue, INIT/TERMINATE synthesis, panic.
User: actual work in `cfl_user_fn_t` callbacks.
Clean boundary; user_fns table indexed by 8-bit `fn_idx`.

### 20. Codegen-friendly, not interpreter-friendly
DSL (Python/Lua) emits C arrays of `cfl_node_desc_t`. Chip runs the table;
no parse tree at runtime.

---

## Architecture at a glance

```
+--------------------------------------------------------------+
|  PROGMEM (flash, const)                                      |
|  - cfl_chain_desc_t chains[]   (4 B/chain)                   |
|  - cfl_node_desc_t  nodes[]    (5 B/node)                    |
|  - uint16_t         params[]   (packed param pool)           |
+--------------------------------------------------------------+
|  RAM (per-engine, ~50 B + N×(0.4 + 2) bytes for N nodes)     |
|  - cfl_engine_t       engine                                 |
|    - chain_enable[]     (1 bit/chain)                        |
|    - node_enable[]      (1 bit/node)                         |
|    - node_initialized[] (1 bit/node)                         |
|    - node_scratch[]     (2 B/node)                           |
|    - event queue        (16 slots × 3 B = 48 B)              |
|    - current_chain      (self-modify guard)                  |
+--------------------------------------------------------------+
|  User code (RAM, BSS/data)                                   |
|  - cfl_user_fn_t user_fns[] (function-pointer table, RAM)    |
|  - Globals the fns touch (counters, peripherals, …)          |
+--------------------------------------------------------------+

  ISR ────────►  cfl_send_event_isr(eng, event_id, data)
                       │
                       ▼
                 [ event queue ]
                       │
  main ──► cfl_engine_pump(eng)
              │
              ▼
        for each queued event:
          for each enabled chain c:
            current_chain = c
            for each enabled node n in c (with INIT if first hit):
              status = user_fns[node.fn_idx](eng, n, event_id, data)
              dispatch(status):
                CONTINUE  → next node
                HALT      → stop walking this chain
                DISABLE   → TERMINATE n, clear n.enable, next node
                RESET     → reverse-TERMINATE all init'd nodes, INIT first
                TERMINATE → reverse-TERMINATE sweep, clear chain.enable
            current_chain = NONE
```

---

## File layout

```
cfl_avr/
├── README.md            — this file (design rationale)
├── continue.md          — current pickup point (tomorrow: ATmega328P bring-up)
├── Makefile             — Linux test build
├── include/
│   ├── cfl_progmem.h    — PROGMEM shims + atomic save/restore (no-op on Linux)
│   ├── cfl_types.h      — return codes, event ids, node kinds, descriptors
│   └── cfl_engine.h     — public API + cfl_engine_t
├── runtime/
│   └── cfl_engine.c     — full dispatcher (~460 lines)
└── tests/               — 5 Linux-native test modules (all PASS)
    ├── heartbeat/
    ├── state_machine/
    ├── while_verify/
    ├── init_term/
    └── isr_events/
```

## Building & testing

Linux test build:
```
$ make test
```

AVR cross-compile target (planned, see `continue.md`):
```
$ make flash-heartbeat AVR_PORT=/dev/ttyUSB0
```

## Cross-architecture portability

Engine code (`runtime/cfl_engine.c` + headers) compiles unchanged on:
- Linux x86_64 (test harness)
- AVR (ATmega328P, AVR32SD32 — production)
- ARM Cortex-M (any of the s_engine chips, if/when needed)

Only `runtime/cfl_main_<chip>.c` is chip-specific (~150 lines: timer ISR,
USART glue, LED pin, panic hook + WDT). Each port adds one of these.

## Related

- `../s_engine/` — sibling DSL for larger chips (tick-walking interpreter)
- Memory: `cfl-chain-flow-design-lessons` — the full prose discussion of the
  20 locked lessons
- Memory: `cfl-avr-engine-2026-05-27` — implementation summary + file
  locations + tomorrow's pickup point
- Memory: `feedback-design-dialog-style` — the multi-round-confirmation
  dialog pattern used to lock cfl_avr's semantics

## License & origin

Engine pattern: Glenn Edgar, originally PSoC4 (~2005), ported and refined
across many chips since. cfl_avr is the AVR-Harvard-specific instantiation.
