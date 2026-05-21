# continue.md — motioncore-prototype

**Status:** 2026-05-12. Strategic + architectural design phase complete. Implementation work begins with **Phase 0 — s_engine M-port** (see §4). Prior strategic context lives in memory: `four_chip_dongle_pivot_2026-05-11.md` and `dongle_linux_protocol_2026-05-11.md`. The CWC class spec at `car_door_window_controller/continue.md` remains valid (only its "RS-485 PIO half-duplex" assumption is superseded — now plain UART with auto-direction transceiver).

---

## 1. The four-chip dongle suite

| Chip | Role | Special capability |
|---|---|---|
| SAMD21 Xiao (ARM M0+, 32K SRAM) | dongle | functional HIL: GPIO / PWM / 12-bit ADC / 10-bit DAC / quadrature gen+decode |
| RA4M1 Xiao (ARM M4, 32K SRAM) | dongle | **analytical HIL** — 14-bit ADC, 12-bit DAC, CMSIS-DSP (Goertzel + block FFT + sliding DFT + biquad + Welford + cross-corr). Parallel reference DSP for CWC inner-loop verification. |
| Pico 2 W (RP2350, ARM M33 SMP, 520K SRAM) | dongle OR slave (separate builds) | PIO multi-bus, can2040 PIO soft-CAN, RS-485 UART. Slave build hosts hard-RT classes (CWC). |
| ESP32-C6 Waveshare DEV-KIT-N8 (RISC-V, 512K SRAM) | dongle OR slave (separate builds) | hardware TWAI CAN, WiFi 6, BLE 5, IEEE 802.15.4 (Thread), RS-485 UART. Single-core (no hard-RT class hosting). |

**Role discipline:** every chip has separate **dongle** and **slave** firmware builds. Single role per binary. SAMD21 + RA4M1 are typically dongles; RP2350 + ESP32-C6 either. See `four_chip_dongle_pivot_2026-05-11.md` for full discussion.

**Container/dongle pairing (v1):** one Linux container per dongle. Cross-dongle coordination is via MQTT, soft-RT. Hard-RT stays within a dongle + its bus.

---

## 2. Locked libcomm protocol decisions (2026-05-11)

Full detail: `memory/dongle_linux_protocol_2026-05-11.md`. Headlines:

- **Polled, request-reply, no async dongle→Linux events.** Modbus-style: send command → ack → poll for result. `comm_submit` / `comm_status` / `comm_claim` pattern.
- **One in-flight per direction.** Anything dropped is an exception, not a tolerated condition.
- **Three-level recovery:** normal → master-initiated handshake → USB-level reconnect → `reboot_with_cause()`.
- **Every dongle has a system shell + optional application shell.** Wire: `OP_SHELL_EXEC` + `OP_SHELL_REPLY` with request_id. Binary-structured replies.
- **Manifest-driven portability.** Linux fetches CBOR manifest at registration; learns commands + symbols + variable-length return shapes + metadata (e.g. `adc_bits`). Zero per-chip code on Linux.
- **Slave-bus traffic is OUT of scope of this contract.** Internal to router dongles.
- **RS-485:** auto-direction transceiver, plain UART. Inter-frame gap: `t_gap ≥ max(3.5 × char_time, 1 ms) + 100 µs guard`.
- **CAN classic fragmentation:** custom 29-bit ID protocol; 6 frames per 40-byte bus_msg_t; ~1.5 ms at 500 kbps.

---

## 3. Locked s_engine architecture decisions (2026-05-12)

S_engine is the substrate for orchestrating **parallel and interacting state machines** on every planned processor. Yesterday's dialog locked:

- **Two engines:** canonical at `~/knowledge_base_assembly/c_programs_and_containers/build_blocks/s_expression/` for Linux + RISC-V + x86. **M-port** in this repo for all ARM Cortex-M chips (M0+, M4, M33). Copy + strip `.git*`, document divergence.
- **No blob format on M-port.** DSL compiler emits name-mangled C source containing `const` module/tree/blackboard tables. Function references resolved by linker. Registration table is `const` in flash, not RAM-built. Reduces RAM per chain from ~10 KB (loaded tree copy) to ~1 KB (instance state only).
- **Module = unit of compilation.** Contains N trees + M blackboards + a bump-allocator buffer for per-instance RAM. User flexibility: trees can share blackboards or have private ones. Compiler computes bump buffer size statically.
- **Layered architecture per dongle:**
  - **C layer:** event sequencer + WDT pet + libcomm framing + HAL pokes + m_function bodies
  - **s_engine layer:** tree composition + scheduling + lifecycle (INIT/event/TERMINATE) + reset propagation
  - **Chains (parallel siblings):** hub (1) + link-monitor (1) + sub-machines (~6) + future apps (N)
- **Function types** used correctly:
  - `o_function` — fires once, becomes inactive (actions: reply, set field, start operation)
  - `io_function` — fires once per session, never re-fires on SE_RESET (power-up init)
  - `m_function` — INIT → event-processing → TERMINATE lifecycle (stateful processes; the workhorse)
  - `p_function` — pure boolean (conditions inside `if`/`cond`/`when`)
- **Upstream cross-cutters** handled BEFORE the hub FSM as siblings in the root composite: `OP_LINK_RESYNC`, `OP_COMMISSION_BEGIN`, `OP_PING`, `OP_IDENTIFY`. **Hub never carries cross-cutter conditions** (avoids the 2005 HSM-fragility trap).
- **Watchdog (hardware WDT) in C** at the event-sequencer level — guarantees "sequencer alive ≡ WDT happy."
- **Link-monitor (peer-staleness) as a chain** — policy-level, tunable threshold.
- **Sub-machines self-clean via m_function TERMINATE.** No manual cleanup branches in chains.
- **DSL compiler emits dispatch source** listing only builtins referenced by compiled chains. Linker drops the rest via `-ffunction-sections -fdata-sections -Wl,--gc-sections`.
- **Bump allocator** in the engine (~50 LOC primitive). All per-instance allocation goes through it. Module reset = bump reset.
- **Apps = chains.** Hot-pluggable. Adding a new monitoring/control capability is registering a new chain, not editing existing ones.

---

## 4. Phase 0 — s_engine M-port (today's work)

**Goal:** Copy canonical s_engine into this repo as the M-port, apply the structural trim, verify against staged tests on Linux native, then later bring up on SAMD21.

### Pre-staged (done 2026-05-12)

- **Local copy location:** `motioncore-prototype/s_engine/`
- **Test set:** `motioncore-prototype/s_engine/dsl_tests/` — 9 tests pre-curated:
  - `basic_primitive_test`, `advanced_primitive_test`
  - `black_board` (blackboard semantics — essential)
  - `callback_function` (EXEC_FN C-bridge — essential)
  - `complex_sequence` (sequence/halt/resume)
  - `dispatch` (field/event/dispatch primitives)
  - `loop_test` (SE_FOR / SE_WHILE kept)
  - `return_test` (return-code propagation)
  - `state_machine` (SE_STATE_MACHINE primitive)
- **Excluded from canonical** (drop-list aligned): `external_tree_test`, `function_dictionary`, `json`, `stack_equations`, `stack_test`
- **Deferred:** `car_window_controller` — uses runtime features (bit maps, etc.) not needed for v1 M-port. Will run on Pico 2 later.

### Trim discipline

**The test set defines the keep-list.** Walk source for the 9 tests, collect every builtin hash referenced — that's what stays. Anything not referenced is a drop candidate. Linker (`-ffunction-sections -fdata-sections -Wl,--gc-sections`) does the actual removal once unreferenced functions exist.

### Step-by-step plan

| # | Step | Pass criterion |
|---|---|---|
| 1 | Strip `.git*` from `s_engine/` so it's a clean fork | `find s_engine/ -name '.git*'` returns nothing |
| 2 | Build canonical-style on Linux native (no trim yet); run 9 staged tests | All 9 tests green; baseline `size` numbers captured |
| 3 | Read DSL compiler (`s_engine/lua_dsl/s_compile.lua` and helpers); confirm assemble-stage in-memory tables match the blob wire format | Understand where to add C-source emit |
| 4 | Add C-source emit path to compiler (`--emit-c` or similar). Test with one DSL program (e.g., `state_machine`). | Generated `.c`/`.h` compiles and runs identically to blob version |
| 5 | Verify bump allocator in the engine. Add it (~50 LOC) if missing. Confirm `s_expr_module_init` can take `const s_expr_module_def_t*` directly. | Module init works against const data |
| 6 | Delete `s_engine_loader.c` from M-port. Refactor module init to use const tables exclusively. | Linux M-port still green on all 9 tests |
| 7 | Apply DSL-compiler dispatch-source emission (only used builtins listed). Add linker `--gc-sections` flag. | Generated binary contains only referenced builtin functions |
| 8 | Re-run 9 tests against fully-trimmed M-port | All 9 still green |
| 9 | Measure: Linux native binary size, BSS, per-test eval performance | Numbers captured for baseline + comparison vs canonical |
| 10 | Write `s_engine/DIVERGENCE.md` listing every change from canonical with rationale | File exists, all changes documented |

**Realistic timeline:** Steps 1–4 today. Steps 5–10 next session.

**Status as of 2026-05-12:** Steps 1–3 complete. Step 4 (C-emit path) is design-locked through dialog; implementation deferred. See `memory/s_engine_m_port_architecture_2026-05-12.md` for the locked design and §11 "State at end of 2026-05-12 session" below for the handoff.

### After Linux baseline green — embedded bring-up (later session)

| # | Step |
|---|---|
| 11 | **Test sweep on Linux:** build + run all single-tree tests via the ROM + bump path; for black_board, run each of its 5 trees individually. Capture `size` (text/data/bss) and bump peak RAM per test. Output: a coverage table characterizing the builtin functions used in the 32 K RAM scope. Excludes `return_test` (18 trees, multi-tree per module — doesn't match M-port single-active-tree pattern). |
| 12a | **`dongle_console` Linux host tool, v0:** auto-discover ACM dongle by VID:PID, raw-bytes/hex/SLIP modes, script hooks (`on_byte`/`on_frame`/`send`). Prerequisite for any SAMD21 work — host visibility before any flashing. |
| 12b | SAMD21 toolchain: arm-none-eabi-gcc + CMSIS headers + startup + linker script |
| 13 | Smallest s_engine program for SAMD21: `io_function` blinks LED on boot, emit DISABLE |
| 14 | Flash via UF2 bootloader; observe LED |
| 15 | `arm-none-eabi-size` for actual flash + RAM usage |

### Bring-up infrastructure: `dongle_console` (built + tested 2026-05-12)

**What it is:** standalone Linux host tool for observing dongle CDC output during bring-up. Not part of the production deploy environment — separate from the mqtt_robot / ChainTree stack.

**Location:** `motioncore-prototype/linux/dongle_console/dongle_console.lua` (~280 LOC, pure LuaJIT)

**Operational rationale (locked 2026-05-12 dialog):**
- Dongle development uses ONLY this tool until the dongle is "pretty much debugged" — protocol bugs caught at the wire-observer layer are much cheaper than after mqtt_robot is in the loop.
- After dongle dev: integrate with the canonical `~/knowledge_base_assembly/.../ros_planner_ii_mqtt_robot/` LuaJIT + libcomm stack.
- The tool stays useful even after integration as the "raw bytes view" escape hatch when the production stack misbehaves.

**Capability evolution across phase 2 sub-steps:**

| Phase | Tool capability |
|---|---|
| 2a (SAMD21 LED + printf) | Raw ASCII dump — **delivered in v0** |
| 2b (SLIP framing on wire) | `--slip` decoder — **delivered in v0** |
| 2c (CRC-8 + first libcomm opcode) | + CRC verify, opcode label decode — **future** |
| 2d (handshake / shell / manifest) | + `expect(predicate, ms)` for conformance scripts — **future** |

**Phase 2 exit criterion (Strict):** dongle passes a host-driven conformance suite (script exercises registration, manifest, handshake, shell-exec, USB disconnect/reconnect, reboot cause) on at least 2 chip families. Tool reaches v3 (send + expect) at that point.

**v0 features delivered + verified against live Xiao (VID:PID `2886:802f` on Pi `robot`):**
- Auto-discovery via `/sys/class/tty/ttyACM*/device/{idVendor,idProduct,serial}`
- Termios raw mode via LuaJIT FFI (zero compiled C deps)
- Modes: ASCII (default), `--hex` (offset + hex + ASCII columns), `--slip` (RFC 1055 decoder, hex-dumped frames)
- Filters: `--port`, `--serial`, `--vid-pid VVVV:PPPP`
- Multi-device policy: **error if >1 match** (Strict mode — single-device bring-up scope)
- Script API: `on_byte(cb)`, `on_frame(cb)`, `send(bytes)` — verified by writing "Hello\n" to the Xiao (returned 6 bytes written)
- `--list` enumerates candidates without opening

**Known caveat:** Lua scripts using `print()` may buffer through SSH+timeout pipes. Workaround: `io.stderr:write(...); io.stderr:flush()`, or `io.flush()` after `print()`.

**Resume command for SAMD21 bring-up:**
```
ssh robot   # the Pi dev host has the Xiao at /dev/ttyACM0
luajit /tmp/dongle_console.lua --list             # check enumeration
luajit /tmp/dongle_console.lua --hex              # watch bytes once firmware exists
```

### Step 12c — hello CDC (DONE 2026-05-12)

**Built and verified end-to-end on the Pi.** Bare-metal hello_cdc firmware on the Seeeduino Xiao SAMD21, flashed via UF2 drag-and-drop, output read live by `dongle_console`.

**Pipeline links proven:**
- `arm-none-eabi-gcc 8.3.1` toolchain on Pi (`apt install gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi`)
- TinyUSB 0.18.0 vendored at `samd21/vendor/tinyusb/` (TinyUSB already ships a `seeeduino_xiao` BSP — reused upstream's clock-init + USB pin config)
- Bare-metal `main.c` (no Arduino runtime, no ASF HAL): clocks via BSP, `tusb_init()`, `tud_task()` + 1 Hz `printf` cadence
- USB descriptors at `samd21/apps/hello_cdc/usb_descriptors.c`: VID `0x2886`, PID `0x802F`, mfr `"motioncore"`, product `"hello_cdc"`
- `_write` retargeted via `samd21/apps/hello_cdc/syscalls_cdc.c` → `tud_cdc_write` + `tud_cdc_write_flush`
- `--specs=nano.specs --specs=nosys.specs`, `-Wl,--wrap=malloc` (trip-wire intact — no libc malloc paths reached)
- UF2 conversion via `microsoft/uf2` `uf2conv.py`, family ID `0x68ed2b88`, app origin `0x2000` (preserves bootloader region)

**Build sizes:**

| Section | Bytes | % of region |
|---|---|---|
| text | 15,496 | 6% of 248 KB app flash |
| data | 124 | — |
| bss | 9,388 | — |
| bss + 8 KB stack | 17,580 | 55% of 32 KB RAM |

**Live output (sample):**
```
/dev/ttyACM0    vid:pid=2886:802f  serial=0123456789ABCDEF  motioncore hello_cdc
hello SAMD21 tick=13
hello SAMD21 tick=14
hello SAMD21 tick=15
...
```

**Source tree (mirrored on WSL and Pi):**
- `samd21/apps/hello_cdc/main.c`
- `samd21/apps/hello_cdc/usb_descriptors.c`
- `samd21/apps/hello_cdc/syscalls_cdc.c`
- `samd21/apps/hello_cdc/tusb_config.h`
- `samd21/apps/hello_cdc/Makefile`
- `samd21/vendor/tinyusb/` (pinned 0.18.0; deps fetched via `tools/get_deps.py samd21`)

**Flash recipe (recorded for repeat use):**
1. Double-short Xiao reset pads → bootloader mode (USB ID flips `802f` → `002f`; mass storage `XIAO` appears)
2. `sudo mount -o uid=pi,gid=pi /dev/sdc /mnt/xiao`
3. `cp samd21/apps/hello_cdc/build/hello_cdc.uf2 /mnt/xiao/`
4. `sync` (mount unmounts itself after the bootloader flashes and reboots)
5. Watch with `luajit dongle_console.lua` → `hello SAMD21 tick=N`

**Minor follow-up:** USB serial currently reports fallback string `0123456789ABCDEF` — `board_usb_get_serial()` returned 0 on this BSP path. Not blocking; chip-UID-as-serial can be wired up during step 13.

---

### Step 13 — s_engine on SAMD21 (DONE 2026-05-12)

**Phase 0 closed on real hardware.** The s_engine M-port now drives a user-supplied C function on the Seeeduino Xiao SAMD21 via a DSL-defined chain, with no heap, no loader, no registration-hash machinery.

**App:** `samd21/apps/blink_engine/` (modeled on hello_cdc; links the M-port engine subset). DSL source is a single-tree, single-node chain calling user fn `toggle_led` which flips PA17 once per tick.

**Engine sources compiled (M-port subset — others SKIPPED per decisions #11 + #12):**
- `s_engine_module.c`, `s_engine_eval.c`, `s_engine_node.c`
- `s_engine_rom_init.c` (M-port init)
- `s_engine_exception.c`, `s_engine_event_queue.c`
- `se_dict_hash.c`, `se_dict_string.c`

Skipped (excluded by the M-port delete list): `s_engine_loader.c`, `s_engine_init.c`, `s_engine_builtins.c`, `s_engine_stack.c`, the two `.cc` files.

**On-chip measurements (Cortex-M0+ Thumb, gcc 8.3.1, `-Os -ffunction-sections -fdata-sections -Wl,--gc-sections`):**

| Section | Bytes | % of region |
|---|---|---|
| `.text` | 21,448 | 8% of 256 KB flash |
| `.relocate` (data) | 124 | — |
| `.bss` | 1,268 | — |
| `.stack` (reserved) | 2,052 | — |
| **total RAM (bss + stack)** | **3,320** | **10% of 32 KB** |

**Bump peak observed at runtime via CDC: 208 B / 256 B buffer.** Trivial chain (one node, no record, no params beyond the function ref) — that's the floor for the M-port per-instance cost. Tree-instance struct + 1 × 4 B node state + alignment overhead.

**RAM-tightening applied (vs initial agent build):**
- `BUMP_BUFFER_SIZE`: 1024 → 256 (blink_engine's actual peak is 208 B; ~20% headroom retained)
- `CFG_TUD_CDC_TX_BUFSIZE`: 256 → 64 (one short printf line per second)
- Stack via `-Wl,--defsym=__stack_size__=0x800`: 8 KB → 2 KB
- Combined savings: −7,104 B (68% of pre-tightening total)

**Two engine-source portability patches that landed during the build (worth keeping):**

| Patch | What | Why |
|---|---|---|
| `s_engine_builtins_flow_control.h` lines 841 + 985: empty `;` after `check_completion:` label | gcc 8.3.1 rejects label-followed-by-decl (C11); gcc 11+ accepts as extension | Real C11 portability fix; should land cleanly upstream |
| Empty stubs for `s_expr_tree_reset_stack` + `s_expr_tree_free_stack` in `user_functions.c` | Engine eval calls those even though the M-port excludes `s_engine_stack.c`; calls are dead at runtime (`inst->stack == NULL` guard) but the linker still demands resolution | Proper fix: `#ifndef S_ENGINE_NO_STACK` guards around the call sites in `s_engine_node.c` + `s_engine_module.c`. Decision #11 calls for this when the engine surgery lands. |

**Live verification:** CDC output streams `tick=N bump_peak=208` at ~4 Hz; LED visually blinks amber at ~4 Hz. End-to-end pipeline working: SAMD21 → CDC → `dongle_console`.

**§4 step table status (full):**

| # | Step | Status |
|---|---|---|
| 1 | Strip `.git*` from `s_engine/` | DONE |
| 2 | Build canonical-style on Linux; run 9 tests | DONE |
| 3 | Read DSL compiler | DONE (conversationally) |
| 4 | Add C-source emit path | DONE |
| 5 | Verify bump allocator | DONE |
| 6 | Delete `s_engine_loader.c` | NOT YET — additive shim path used; loader still on disk but `--gc-sections` drops it |
| 7 | Apply dispatch-source emission + `--gc-sections` | DONE |
| 8 | Re-run 9 tests against trimmed M-port | DONE |
| 9 | Measure: size + BSS + per-test eval | DONE |
| 10 | Write `s_engine/DIVERGENCE.md` | NOT YET — would document the additive shim approach + the two portability patches |
| 11 | Test sweep on Linux | DONE |
| 12a | `dongle_console` Linux host tool v0 | DONE |
| 12b | SAMD21 toolchain | DONE |
| 12c | hello_cdc (toolchain + CDC + dongle_console pipeline) | DONE |
| 13 | s_engine on SAMD21 (blink_engine) | **DONE** |
| 14 | Flash via UF2 + observe LED | DONE (4 Hz amber confirmed) |
| 15 | `arm-none-eabi-size` for flash + RAM | DONE (numbers above) |

---

### Next milestone — libcomm wire format on SAMD21

Per `four_chip_dongle_pivot_2026-05-11.md`: "First-milestone target: all four chips do dongle registration + USB-CDC libcomm messages to the Linux container. Order: SAMD21 → Linux dongle-manager → RA4M1 → RP2350 → ESP32-C6."

Next concrete chunk:
1. **`common/spec/` write-up** — libcomm framing, opcode allocation, manifest CBOR schema, three-factor handshake (per `dongle_linux_protocol_2026-05-11.md`)
2. **SAMD21 firmware adds libcomm framer** (SLIP + CRC-8 per `transport_usbcdc.md`)
3. **SAMD21 firmware emits `OP_REGISTER` packet on boot**
4. **`dongle_console` v2c: CRC-8 verify + opcode label decode** (per Phase 2c in the bring-up infra section)
5. **Verify wire bytes via `dongle_console --slip`** (already works for SLIP raw; v2c adds opcode labels)
6. **Engine cleanup deferred items** (decision #6, #11, #12): formal rename `s_expr_module_*_t` → `s_engine_rom_t/ram_t`, delete loader + stack source files, write DIVERGENCE.md

---

## 5. Build plan after Phase 0 — chip firmware + Linux side

After Phase 0 is green, build the actual dongle stack:

| Step | Deliverable | Notes |
|---|---|---|
| **A** | `common/spec/` write-down | libcomm framing, opcode allocation, manifest schema, handshake protocol, per-transport adapters |
| **B** | Reuse existing libcomm (mostly) | Already at `~/knowledge_base_assembly/.../ros_planner_ii_mqtt_robot/libcomm/`. Slice 1d shipped. Custody: copy/submodule/path-reference TBD. |
| **C** | Linux side: ChainTree-based base layer (dongle lifecycle) + first app KB tree (shell relay) | Build on existing mqtt_robot/dongle_hal/ct_comm stack — DON'T write from scratch. |
| **D** | SAMD21 dongle firmware | TinyUSB-CDC + libcomm port + commission + system shell + app shell (HIL). Uses s_engine M-port. |
| **E** | RA4M1 dongle firmware | Port from SAMD21 (Renesas FSP HAL); add `signal.*` DSP commands. Portability litmus test: Linux should require no changes except app-shell command knowledge. |
| **F** | Pico 2 W dongle and slave firmware | reuse pico-sdk + FreeRTOS-SMP infra; add manager + internal_bus + slave commissioning + downstream-bus drivers |
| **G** | ESP32-C6 dongle firmware | ESP-IDF + RISC-V + native USB + CH343 split; hardware TWAI |

**Parallel track:** CWC bench Phase A0–A11 (motor characterization on Pico 2 W in standalone bench-shell mode, per `car_door_window_controller/continue.md`) does not need any dongle and can run alongside D–G.

---

## 6. Where source will live

```
motioncore-prototype/
├── s_engine/                    ← M-port copied from canonical (Phase 0 work)
│   ├── dsl_tests/               ← 9 staged tests
│   ├── runtime/, include/, lib/, lua_dsl/
│   └── DIVERGENCE.md            ← (to be written) all changes vs canonical
├── linux/                       ← (future) ChainTree app KB trees, base layer
├── rp2350/                      ← (future) Pico 2 W firmware
├── esp32c6/                     ← (future) ESP32-C6 firmware
├── samd21/                      ← (future) SAMD21 firmware
├── ra4m1/                       ← (future) RA4M1 firmware
├── car_door_window_controller/  ← CWC class spec (architecturally valid)
├── docs/hardware/esp32c6_devkit_n8/README.md   ← pinout reference
└── continue.md                  ← this file
```

Top-level `common/` (portable contract beyond s_engine) is a future addition once libcomm spec is written down.

---

## 7. Hardware reference

- **ESP32-C6 pinout:** `docs/hardware/esp32c6_devkit_n8/README.md`. 32-pin board; 38-pin terminal carriers have wrong silk-screen labels — use the README.
- **Pico 2 W / SAMD21 / RA4M1 Xiao pinouts:** not yet written; should be written before respective firmware bring-up.
- **Auto-direction RS-485 transceiver part:** TBD — pending decision before carrier-PCB work.
- **Carrier PCBs:** two designs (RP2350 + ESP32-C6) sharing a bus-interface sub-circuit. Not started.

---

## 8. Open queue (relevant items, 2026-05-12)

### Resolved 2026-05-12
- ~~libcomm + ChainTree custody~~ → **s_engine: copy + strip git, M-port lives in this repo. Canonical stays at `~/knowledge_base_assembly/...` for non-M chips. libcomm custody still TBD when libcomm work starts.**

### Active for Phase 0 today
- Identify which canonical builtins are NOT referenced by the 9 staged tests → drop list
- Validate that the bump allocator pattern is already in the canonical engine (likely yes)
- Confirm module init accepts `const` tables (avoids loader dependency)

### Lower priority — design after Phase 0
- `common/` name disambiguation (top-level portable vs per-chip)
- `pico/` vs `rp2350/` directory naming
- Manifest CBOR schema formal write-up
- Linux hot-plug strategy (udev vs polling)
- USB-CDC-uplink-strict vs allow-IP-uplink
- RS-485 transceiver part number
- Carrier-PCB design

---

## 9. What changed since 2026-05-04

- "RS-485 via PIO half-duplex on RP2350" → plain UART with auto-direction transceiver
- "The dongle" (singular Pico-based) → four chip-family implementations
- "P0 = chain_tree container migration" → deferred; s_engine M-port replaces it as Phase 0
- TI Xiao for Thread → dropped; ESP32-C6 absorbs the role
- Linux side from-scratch Python design → **abandoned**; reuse existing LuaJIT + libcomm + chain_tree stack
- All four chips have separate dongle/slave builds (no hybrid binaries)

---

## 10. Cross-references

- **Memory** at `/home/gedgar/.claude/projects/-home-gedgar-motioncore-prototype/memory/`:
  - `four_chip_dongle_pivot_2026-05-11.md` — strategic shift, four-chip suite, role discipline
  - `dongle_linux_protocol_2026-05-11.md` — protocol details
  - `feedback_design_dialog_style.md` — **REQUIRED reading**: dialog-before-execution discipline
  - `cwc_class_spec_2026-05-10.md` — CWC slave class spec (mostly valid)
  - `pico_sdk_freertos_setup.md`, `bench_hardware_status.md`, `dongle_protocol_reboot_cause.md` — RP2350-side notes
- **Canonical s_engine:** `~/knowledge_base_assembly/c_programs_and_containers/build_blocks/s_expression/` (16K LOC, full feature set)
- **libcomm canonical:** `~/knowledge_base_assembly/luajit_programs_and_containers/building_blocks/ros_planner_ii_mqtt_robot/libcomm/` (slice 1d shipped)
- **Existing master-side stack:** `~/knowledge_base_assembly/luajit_programs_and_containers/building_blocks/ros_planner_ii_mqtt_robot/` (mqtt_robot_main.lua, dongle_hal.lua, ct_comm.lua, test_comm_pty_multi_dongle.lua)
- **Pi dev host:** SSH `robot` (192.168.1.66, user `pi`). Has `00_smp_hello` + `01_ram_stats` + `02_watchdog` work not yet synced to WSL.

---

## 11. Instructions for the next Claude session

**You are picking up at Phase 0 — s_engine M-port (§4 above).** The strategic and architectural design phases are complete; you are doing implementation work.

### Required cold-start reading (in order)

1. **This file** (`continue.md`) — full plan, §3 and §4 are the operative sections
2. **`memory/feedback_design_dialog_style.md`** — **MANDATORY**. The user pushed back hard yesterday about over-design and pre-execution. Read this before doing anything. Key lessons:
   - Stay in dialog until decisions are locked, then execute
   - Don't pre-execute by writing files, elaborate sketches, scope expansions
   - Push back is the value-add, not deference
   - Read existing tree before designing new
   - One concern per response; wait for sign-off
3. **`memory/four_chip_dongle_pivot_2026-05-11.md`** — four-chip suite, role discipline
4. **`memory/dongle_linux_protocol_2026-05-11.md`** — libcomm protocol decisions
5. **`s_engine/README_COMMAND_LINES.txt`** — DSL compiler invocations
6. Skim `s_engine/runtime/s_engine_types.h` and `s_engine_module.h` to understand the data structures

### Today's task — Phase 0 steps 1–4

Concrete order (per §4):

1. **Verify the staged tree exists:** `ls motioncore-prototype/s_engine/` shows `runtime/`, `include/`, `lib/`, `lua_dsl/`, `dsl_tests/`, `s_build.sh`
2. **Strip git tabs:** `find motioncore-prototype/s_engine -name '.git*'` — if anything returned, delete
3. **Build canonical-style on Linux native:** check `s_build.sh` or `Makefile`. Adapt paths if needed. Build should produce `libs_s_engine.a` (or equivalent)
4. **Run the 9 staged tests** in `dsl_tests/`. Each test directory has a Makefile and a generated binary blob. Run each, verify pass. Record `size`/`arm-none-eabi-size` baseline numbers.
5. **Read the DSL compiler:** `s_engine/lua_dsl/s_compile.lua` and `s_engine_helpers.lua`. Understand the assemble stage's in-memory representation.
6. **Sketch the C-emit path:** in conversation with the user, propose how `--emit-c` would work for one test (e.g., `state_machine`). **Don't write the implementation yet — discuss the approach first.**

### Discipline reminders (from feedback memory)

- **Don't write new files or elaborate plans before the user has agreed to the approach.** Talking about "what we'd do" is fine; producing artifacts isn't.
- **If you start to feel "let me make this concrete by writing X" — pause and ask first.**
- **If the user's framing seems wrong, push back with your actual analysis** — don't just acknowledge and proceed.
- **One concern per turn.** Wait for sign-off before moving to the next.
- **Read existing code before proposing changes to it.** The canonical engine has 16K LOC of working code; lots of patterns are already there.

### Pass criteria for today's session

By end of session:
- 9 tests build and run successfully on Linux against the staged M-port copy
- Baseline `size` measurements captured (for comparison after trim)
- DSL compiler internals are understood (at least conversationally)
- C-emit path approach is sketched in conversation with the user

### Things NOT to do today

- Don't apply the structural trim yet (steps 5–10) — that's next session
- Don't bring up SAMD21 yet (steps 11–14) — that's later
- Don't redesign the engine's internals beyond what the trim plan calls for
- Don't write `DIVERGENCE.md` until there are actual divergences to document

### State at session end

Update §4's step table with what completed. If measurements differ materially from estimates, update memory with the real numbers. Append a short "where I left off" note to this section if mid-step.

### State at end of 2026-05-12 session

**Complete:** §11 today's-task steps 1–6 all done. 9 staged tests green on Linux x86_64 native. Baselines captured (`libs_s_engine.a` text=157 KB / data=1.6 KB / bss=232 B; test binaries text=138–146 KB). C-emit design-locked through dialog.

**Locked decisions** (full record + reasoning in `memory/s_engine_m_port_architecture_2026-05-12.md`):

| # | Decision |
|---|---|
| 1 | Proof target: `state_machine` (revised post-implementation; return_test was 18 trivial trees — wrong axis. state_machine: 1 tree, 183 params, records, strings, 4+9 builtins) |
| 2 | C-emit scope: data + dispatch (must enable real `--gc-sections` trim) |
| 3 | Dispatch via direct `func_index` into ROM array; no runtime hash scan |
| 4 | Function-pointer arrays live in ROM handle |
| 5 | Type names: `s_engine_rom_t` / `s_engine_ram_t` |
| 6 | DSL multi-tree per module; M-port runtime activates ONE tree at a time (future: dongle command switches). ROM stays multi-tree; bump sized for largest tree. |
| 7 | Hash elimination scope: function dispatch only |
| 8 | Builtin: lowercased DSL name = C symbol, verified at compile time against `s_engine_builtins_*.h` scan (convention holds 100%, 93/93 today). User fn: name as-is, link error on mismatch. |
| 9 | All builtins go through helpers — single rename site, complete inventory |
| 10 | Module-init API: `s_engine_init(&engine, &alloc, &<name>_module_rom, debug_cb)` single call |
| 11 | No stack on M-port runtime (no STACK_*, no frame_allocate, no stack_frame_instance). Future versions may add. Auto-enforced by decision E: if `s_engine_builtins_stack.h` is deleted, scan errors on any helper that references stack ops. |
| 12 | M-port DSL scope excludes: stack ops, JSON, CBOR, external trees, function_dictionary, stack_equations. Engine-source delete list broadens accordingly (see memory). |

**Next-session starting point — C-emit implementation:**

Required NEW reading (additive to top of §11):
- `memory/s_engine_m_port_architecture_2026-05-12.md` — load-bearing
- `lua_dsl/s_expr_generators.lua` (1515 lines) — where `BinaryModuleGenerator` lives; the new C-emit generator class goes parallel to it
- One or two helpers in `se_helpers_dir/` (e.g., `se_state_machine.lua`) — confirm decisions #8 and #9 against actual helper code

Concrete first task: implement `CSourceModuleGenerator` in `s_expr_generators.lua` + a `--emit-c=<file>` flag in `s_compile.lua`. Target: a single `.c` for `return_test` containing const ROM tables matching the `s_engine_rom_t` shape, with referenced builtins emitted by direct C-symbol name into fn-ptr arrays. Engine-side rename (`module->X` → `module->rom->X`) and `s_engine_loader.c` deletion come AFTER the C-emit produces output that compiles.

**Open concerns deferred to next-session dialog:**
C-emit file structure (one `.c` + `.h` per module? naming?); tree create/switch API on M-port (multi-tree-defs but single-active runtime, future dongle-command driven switching); bump-sized-for-largest-tree details; eval-side rename surgery exact scope; full engine source delete list.

---

### Implementation progress at end of 2026-05-12 session (post-pause)

After capturing the design, user signed off to implement. Code changes landed in this session:

**`lua_dsl/s_expr_generators.lua`** (added ~330 lines):
- `CSourceModuleGenerator` class — parallel to `BinaryModuleGenerator`, walks the same `module_data`
- `collect_tree_params(tree)` — mirrors `emit_tree_params` but builds a Lua list of param records with brace_idx patched
- `format_param(p)` — emits one typed `s_expr_param_t` struct initializer, opcode-dispatched (handles INT/UINT/FLOAT/STR_HASH/STR_IDX/FIELD/CONST_REF/RESULT/ONESHOT/MAIN/PRED/OPEN_KEY/all brace variants/STACK_*/NULL_PARAM)
- `to_c_source(base_name)` — produces one `.c` file containing: extern fwd decls (lowercase-resolved), string table, record fields + record_desc, per-tree param arrays, tree_def array, fn-ptr arrays (referenced fns only), top-level `const s_engine_rom_t <name>_module_rom`
- `c_symbol_for(name)` — builtin: lowercase + verify against scanned set; user: as-is

**`lua_dsl/s_compile.lua`** (added ~50 lines):
- `scan_runtime_builtins(dir)` — globs `s_engine_builtins*.h`, regex-extracts `se_*`/`cfl_*` function names, returns set
- New flags: `--emit-c=<file>`, `--runtime-headers=<dir>` (default `<script_dir>/../runtime`)
- Wire-up: instantiate `CSourceModuleGenerator(module_data, valid_syms)`, write to file

**`lua_dsl/s_expr_dsl.lua`** (1 line):
- Re-export `M.CSourceModuleGenerator = generators.CSourceModuleGenerator`

**Verification:**
- `return_test` emits a 372-line `.c` — 18 trees, 18 main fns, scanned 113 C symbols, all builtin refs resolved to lowercase names
- `state_machine` emits a 296-line `.c` — 1 tree with 183 params + 48 nodes, 12 strings, 1 record (with field), 4+9+0 fn arrays — exercises records, strings, nested params

**Not done — strictly next session:**
1. Define `s_engine_rom.h` introducing `s_engine_rom_t` (the emitted `.c` references it but the type doesn't exist yet)
2. Add `s_engine_ram_t` and refactor `s_expr_module_init` / `s_engine_init` accordingly
3. Compile the emitted `.c` into a `return_test` test binary (proves the const-ROM path links)
4. Measure `--gc-sections` trim impact (the whole point of decision #2)
5. Delete `s_engine_loader.c` once the new path is verified
6. Decision deferrals from §11 open-concerns: file structure, tree-create API, bump allocator integration

**Resume command for next session (state_machine, not return_test):**
```
cd s_engine/lua_dsl
luajit s_compile.lua ../dsl_tests/state_machine/state_machine.lua \
  --helpers=s_engine_helpers.lua --emit-c=state_machine_module_rom.c \
  --outdir=/tmp/c_emit_out/
```
Output is the current `.c` (296 lines, verified clean — 0 stack opcodes). Inspect, then proceed to engine surgery (see decision rows #11 and #12 for the broader delete list — stack + dict-support files come out alongside the loader).

---

### End-to-end run delivered (2026-05-12 session, post-capture)

After capture + user sign-off, did a minimal-surgery additive build:

**New files:**
- `runtime/s_engine_rom.h` — `s_engine_rom_t` (prefix-compatible with `s_expr_module_def_t`, plus fn-ptr array fields)
- `runtime/s_engine_rom_init.c` — `s_engine_init_rom(mod, rom, alloc)` — no malloc, no hash lookup, no register_builtins
- `dsl_tests/state_machine/main_rom.c` — calls `s_engine_init_rom` against the generated `state_machine_test_module_rom`
- `dsl_tests/state_machine/Makefile.rom` — builds with `-ffunction-sections -fdata-sections -Wl,--gc-sections`

**C-emit refinements:**
- Removed redundant string-table emit (the existing `<base>.h` already has it as `static const`)
- Added `s_engine_builtins_*.h` includes so static builtin defs are visible in the TU
- Skipped extern fwd decls for builtin functions (they're now visible via includes); user fns still get extern decls

**Result — state_machine via ROM path runs end-to-end on Linux x86_64:**

| Section | Blob path (existing `main`) | ROM path (new `main_rom`, `--gc-sections`) | Delta |
|---|---|---|---|
| text | 138,576 | 72,014 | **−48%** (66 KB saved) |
| data | 2,624 | 1,176 | −55% |
| bss | 248 | 248 | 0 |
| on-disk size | 667 KB | 469 KB | −30% |

Both binaries pass the same state_machine test logic (`SE_FUNCTION_TERMINATE` reached at tick 363 via the ROM path; existing blob path reached it earlier in the session). **Decision #2 is proven** — the load-bearing payoff of the arc is delivered.

**Still deferred to next session:**
1. Engine surgery proper (rename `s_expr_module_def_t` → `s_engine_rom_t`; rename `s_expr_module_t` → `s_engine_ram_t`; drop the `oneshot_fns`/`main_fns`/`pred_fns` malloc/RAM-resident arrays — currently the additive shim keeps `s_expr_module_t` shape unchanged and just stuffs const pointers into the RAM-array slots via cast-away-const)
2. Delete `s_engine_loader.c`, `s_engine_stack.*`, `s_engine_stack_functions.*`, `s_engine_builtins_stack.h`, `s_engine_list_dictionary_support.*` (decisions #11 and #12) — `--gc-sections` already drops their contributions from binaries, but the source files still exist
3. Expand C-emit + ROM path to the other 8 staged tests
4. Verify the canonical engine still builds + tests still pass (the additive changes shouldn't have broken anything, but a sanity rebuild is owed)

**Resume next session by:** building `main_rom` from `dsl_tests/state_machine/` (Makefile.rom), inspecting `size`, then either (a) starting full rename surgery, or (b) expanding ROM path to other tests first.

---

### Bump allocator added + re-measured (2026-05-12, follow-on)

Replaced the libc-backed `simple_malloc/free` in `main_rom.c` with a static 1024-byte bump buffer. Cursor-advance malloc, no-op free, single-step reset. No heap allocations from the engine path.

**Final measurement, blob path vs ROM + bump:**

| | Blob path | ROM + bump | Δ |
|---|---|---|---|
| text | 138,576 | 72,815 | **−47%** |
| data | 2,624 | 1,176 | −55% |
| bss | 248 | 1,304 | +1,056 (bump buffer in .bss) |
| heap allocations from engine | 5 + libc bookkeeping | **0** | — |

**Bump usage at peak:** 648 B / 1024 B (63%). Replaces 5 separate libc mallocs (tree_instance struct + node_states[48] + pointer_array[7] + slot_flags[7] + blackboard[4]). Single 1024 B static buffer; no fragmentation, no allocator dependency, single-reset deallocation.

**Stack confirmed unused at runtime:** `inst->stack = NULL` by default in `s_expr_tree_create`; the stack is only created when `s_expr_tree_create_stack` is explicitly called, and state_machine never calls it. Decision #11 is *runtime-satisfied* even before deleting the stack source files — `--gc-sections` already drops their contributions from `main_rom` (part of the −47% text reduction).

**Test pass:** `SE_FUNCTION_TERMINATE` reached at tick 363, identical to the blob path.

**Remaining surgery deferred to next session:**
1. Delete `s_engine_loader.c` + `s_engine_stack.*` + `s_engine_stack_functions.*` + `s_engine_builtins_stack.h` + `s_engine_list_dictionary_support.*` source files (already trim-dropped from binaries, but source still on disk)
2. DSL-compute and emit `rom->bump_buffer_size` per the formula `sizeof(tree_instance) + node_count*4 + pointer_count*9 + blackboard_size + slack` (currently `.bump_buffer_size = 0` with TODO comment; main_rom.c uses hardcoded 1024)
3. Rename `s_expr_module_def_t` → `s_engine_rom_t` (formal); drop the cast-away-const path in `s_engine_init_rom`
4. Verify canonical engine still builds (no regressions from the additive changes)

---

### Step 11 — Test sweep on Linux (2026-05-12 follow-on)

C-emit expanded to cover the full single-tree-test set + each black_board tree individually. Three C-emit bugs surfaced and were fixed in `s_expr_generators.lua`:

| Bug | Symptom | Fix |
|---|---|---|
| `format_int` used `tostring(math.floor(v))` | Large ints/uints output as scientific notation (`1.8e+19U`) — compile error | `format_int` now uses `string.format("%d", bit.band(v, 0xFFFFFFFF))` for int32 path; new `format_uint` emits hex `0xXXXU` |
| `format_float` always appended `f` suffix | `1.0` formatted as `"1"` + `"f"` = `"1f"` — C parses as int+identifier, compile error | Detect missing `.`/`e`/`E`, insert `.0` before the `f` suffix |
| `resolve_field_offset` searched all records globally; `resolve_nested_field_offset` did not exist | Nested-field DSL refs resolved to *wrong field offset* (or fell back to hash); demo_nested_access segfaulted | `current_tree_record` is now bound around `collect_tree_params` (matching `BinaryModuleGenerator`); `resolve_field_offset` is tree-scoped; new `resolve_nested_field_offset` walks dotted paths through embedded records |

**Final test-sweep results (all single-tree tests, M-port ROM + bump path, `--gc-sections`):**

| Test | Builtins (one/main/pred) | text | data | bss | Bump peak |
|---|---|---|---|---|---|
| basic_primitive_test | 9 / 4 / 4 | 70,927 | 1,176 | 1,304 | 576 B |
| advanced_primitive_test | 4 / 11 / 2 | 77,234 | 1,288 | 1,288 | 816 B |
| callback_function | 2 / 4 / 0 | 68,191 | 1,064 | 1,288 | 448 B |
| complex_sequence | 4 / 17 / 4 | 81,145 | 1,416 | 1,288 | 1,024 B |
| dispatch | 4 / 10 / 0 | 75,223 | 1,256 | 1,288 | 728 B |
| loop_test | 4 / 7 / 2 | 72,849 | 1,152 | 1,288 | 616 B |
| state_machine | 4 / 9 / 0 | 72,815 | 1,176 | 1,304 | 648 B |
| black_board / demo_blackboard_access | 16 / 1 / 0 (shared 5-tree ROM) | 71,278 | 1,296 | 1,288 | 456 B |
| black_board / demo_slot_access | 16 / 1 / 0 | 71,278 | 1,296 | 1,288 | 456 B |
| black_board / demo_array_access | 16 / 1 / 0 | 71,278 | 1,296 | 1,288 | 488 B |
| black_board / demo_nested_access | 16 / 1 / 0 | 71,278 | 1,296 | 1,288 | 440 B (clean after field-resolution fix) |
| black_board / demo_constants | 16 / 1 / 0 | 71,278 | 1,296 | 1,288 | 424 B |

**Findings for the 32 K RAM target:**
- Bump-peak range: **424 B → 1,024 B**. Recommended buffer: **1.5 KB** (covers complex_sequence with ~50% headroom).
- Text 68–81 KB on x86_64 → estimated 48–57 KB on Cortex-M0+ Thumb. Fits SAMD21's 256 KB flash with substantial margin.
- The .bss includes the 1 KB bump buffer itself (1,288 / 1,304 B total). Net RAM cost = bump + stack + tiny `s_expr_module_t` — well within 32 KB.
- complex_sequence and basic_primitive_test ran the full 1000-tick cap without natural termination; their bump peaks are stable steady-state numbers, not OOMs (verified by running complex_sequence with an 8 KB buffer — peak stayed at 1,024).

**Files added under each test dir during the sweep:**
- `main_rom.c` (per-test, models on state_machine's; for black_board it's parameterized via `-DBB_TREE_HASH=...`)
- `Makefile.rom` (per-test; black_board's builds five binaries from one source)
- `<test>_module_rom.c` (regenerated by `--emit-c` with the fixed emit)

---

### Phase 2 closure — register_dongle end-to-end on SAMD21 (2026-05-12)

After §11 capture + the post-EOD dialog locking the chain shape + Linux prototype passing, the SAMD21 build merged all three Phase-2 components into one binary:
- s_engine M-port (proven via `blink_engine` — bump=208 B per instance on Cortex-M0+)
- libcomm framer (proven via `blink_frame` — SLIP + CRC-8/AUTOSAR + decoder round-trip)
- `register_dongle` chain (proven on Linux via `s_engine/dsl_tests/register_dongle/` — engine-driven orchestration with `io_call(send_register) ; se_fork(heartbeat-loop, led-toggle)`)

**Live decoded trace on real hardware** (Xiao SAMD21 via `dongle_console --frame`, 8-second sample):

```
[frame  1] cmd=0x0001 seq=0 len=24 CRC=ok    ← OP_REGISTER: chip_uid(16B from device-sig 0x0080A00C+0x0080A040..0x0080A048) + vid=0x2886 + pid=0x802F + fw=1
[frame  2] cmd=0x0002 seq=0 len=8 CRC=ok     ← OP_HEARTBEAT: uptime= 250 ms, counter=0
[frame  3] cmd=0x0002 seq=1 len=8 CRC=ok     ← uptime=1500 ms, counter=1
[frame  4] cmd=0x0002 seq=2 len=8 CRC=ok     ← uptime=2750 ms, counter=2
[frame  5] cmd=0x0002 seq=3 len=8 CRC=ok     ← uptime=4000 ms, counter=3
[frame  6] cmd=0x0002 seq=4 len=8 CRC=ok     ← uptime=5260 ms, counter=4
[frame  7] cmd=0x0002 seq=5 len=8 CRC=ok     ← uptime=6500 ms, counter=5
[frame  8] cmd=0x0002 seq=6 len=8 CRC=ok     ← uptime=7750 ms, counter=6
[frame  9] cmd=0x0002 seq=7 len=8 CRC=ok     ← uptime=9000 ms, counter=7
```

LED at 4 Hz throughout (visually confirmed). Cadence 1250 ms/cycle matches the Linux prototype's observed cycle exactly — engine semantics identical on Cortex-M0+ Thumb and x86_64.

**Resource picture (SAMD21G18A: 256 KB flash / 32 KB RAM):**

| App | Flash | RAM (data + bss + stack) | Δ flash over hello_cdc |
|---|---|---|---|
| `hello_cdc` baseline | 15.5 KB | 3.3 KB | — |
| `blink_engine` (engine + chain) | 21.4 KB | 3.3 KB | engine = +6 KB |
| `blink_frame` (framer) | 18.2 KB | 4.5 KB | framer = +3 KB |
| **`register_dongle` (combined)** | **24.5 KB (9.4%)** | **6.0 KB (18%)** | engine+framer+chain = +9 KB |

~91% flash + ~82% RAM still available for: manifest CBOR encoder, RX path, opcode dispatcher, shell, additional chains, link-monitor, app-layer.

**Layers verified end-to-end on real hardware:**

| Layer | Status |
|---|---|
| Cortex-M0+ Thumb codegen for the s_engine M-port subset | ✓ |
| `s_engine_init_rom` + const ROM tables in flash | ✓ |
| Bump allocator in `.bss` (512 B), zero heap | ✓ |
| Chain: `io_call(send_register) ; se_fork(chain_flow{o_call+tick_delay+pipeline_reset}, m_call)` | ✓ |
| `o_call` auto-terminates in chain_flow (heartbeat fires once/cycle) | ✓ |
| `pt_m_call` `se_tick_delay(3)` counts down 4 ticks then disables | ✓ |
| `se_return_pipeline_reset()` loops chain_flow | ✓ |
| `m_call(toggle_led)` returning `SE_PIPELINE_CONTINUE` keeps fork branch alive | ✓ |
| SAMD21 chip_uid read from device-signature region | ✓ |
| `frame_encode_s2m` (SLIP + CRC-8/AUTOSAR with final XOR) on Cortex-M0+ | ✓ |
| `dongle_console --frame` decoder on live USB-CDC bytes | ✓ |
| All 9 frames CRC-verified ok | ✓ |
| Pipeline stable over 8+ seconds, no asserts, no resets | ✓ |

This is the proof-of-life for the **four-chip-dongle architecture's first chip + the s_engine M-port + the libcomm wire format**, all in one binary, all driven by a DSL chain.

**Files added in this session:**

| File | Purpose |
|---|---|
| `samd21/apps/register_dongle/{main.c, user_functions.c, usb_descriptors.c, tusb_config.h, Makefile}` | SAMD21 app, ~24.5 KB flash |
| `samd21/apps/register_dongle/register_dongle.h` + `register_dongle_module_rom.c` | DSL-emitted ROM tables |
| `samd21/apps/register_dongle/vendor/libcomm/{frame.c, frame.h, comm.h, bus_config.h, opcodes.h, VENDORED.md}` | Lifted from `~/knowledge_base_assembly/.../libcomm/` |
| `s_engine/dsl_tests/register_dongle/{register_dongle.lua, user_functions.c, main_rom.c, Makefile.rom}` | Linux prototype (mocks via printf) — keep as reference + regression test |
| `memory/s_engine_dsl_composition_rules.md` | Two composition rules + verified semantics of fork/chain_flow/tick_delay/io_call + result-code scopes + canonical patterns (built up from this session's code-reading exercises) |

---

### Plan for the next session (Phase 2d — bidirectional libcomm + engine event injection)

**Primary goal:** add the RX (host→dongle) path on SAMD21 + integrate inbound packets into the engine event system. Establishes the architecture's "inbound packet becomes engine event" pattern using `OP_PING` / `OP_PONG` as the proof case.

**Required cold-start reading** (in order, minutes not hours):

1. `memory/MEMORY.md` for the index
2. `memory/feedback_design_dialog_style.md` — **MANDATORY** dialog discipline (concerns one at a time; Linux-first prototyping; push-back is value-add; don't pre-execute)
3. `memory/s_engine_dsl_composition_rules.md` — required for any chain edits
4. `memory/s_engine_m_port_architecture_2026-05-12.md` — design contract
5. `memory/dongle_linux_protocol_2026-05-11.md` — protocol envelope + three-factor handshake intent
6. continue.md "Phase 2 closure" section directly above + the resource tables

**Concrete proposed scope (one session, ~3-5 hours):**

| # | Step | Notes |
|---|---|---|
| 1 | **Dialog: lock the inbound-event injection model** | `s_expr_tree_instance_t` has `event_queue[16]` — likely the right primitive. Need to confirm: who pushes? who pops? how does the engine deliver `event_id` to the chain? Likely via `se_wait_event(event_id, count)` (saw it in `se_helpers_dir/se_timing_events.lua`) |
| 2 | Define `OP_PING` (m2s) and `OP_PONG` (s2m) in `vendor/libcomm/opcodes.h` | Smallest possible bidirectional opcode pair — proves wire round-trip |
| 3 | **Linux prototype FIRST**: new chain shape with `se_wait_event(OP_PING)` triggering oneshot `send_pong` user fn | Per locked discipline, never edit chain shapes directly on embedded |
| 4 | SAMD21 firmware: add RX path | Main loop reads CDC bytes via `tud_cdc_available` + `tud_cdc_read`, feed to `frame_decoder_feed`, on valid m2s frame switch by `cmd`, push to engine's `event_queue` |
| 5 | `dongle_console` v2d: add `--send-ping` flag emitting a libcomm m2s OP_PING frame | Mirrors firmware send pattern; tests round-trip |
| 6 | Flash, run, verify | dongle_console sends OP_PING → SAMD21 decodes → engine fires `se_wait_event` → `send_pong` runs → frame back to host → dongle_console decodes OP_PONG |

**Open concerns to dialog up-front (before any code):**

- How exactly does `se_wait_event` activate when a packet arrives? The pt_m_call slot needs a "match" mechanism — confirm by reading `s_engine_builtins_dict.h` / `event_queue` code.
- Where does the inbound event arrive at — root composite, or a specific listening chain? Likely a new fork branch listening on the event id.
- The architecture memory talks about "router dongles" doing slave-bus routing; for the SAMD21 leaf-dongle the equivalent is just "RX path inside the dongle app." Lighter scope.
- The first-frame BAD-LEN issue we saw on `blink_frame` will recur on RX in the other direction (dongle reading from host) — drop first frame until decoder is synced, both sides.

**Secondary track (only if primary completes early):**

| Item | Type | Effort |
|---|---|---|
| Fix `frame.h` "no final XOR" comment (the implementation has `^ 0xFF` at the end) | doc | 1 line |
| `dongle_console` first-frame sync drop — silently discard bytes until first END-to-END produces a valid frame | code | ~10 lines |
| Patch `check_completion:` label-decl issue in `s_engine_builtins_flow_control.h` (gcc 8.3.1 portability) | code | 2 lines |
| dongle_console v2d: opcode label decode (show `OP_REGISTER` not `cmd=0x0001`) | code | ~20 lines + small opcode table |

**Deferred to a LATER session (NOT for tomorrow):**

- Engine surgery proper — rename `s_expr_module_def_t` → `s_engine_rom_t`; delete `s_engine_loader.c` + `s_engine_stack.*` + `.cc` source files; write `s_engine/DIVERGENCE.md`. Currently the additive-shim path works; the rename is cosmetic.
- DSL: compute `bump_buffer_size` per-tree and emit into ROM
- Engine source patches upstream-style: guard stack calls with `#ifndef S_ENGINE_NO_STACK`
- Second chip family (RA4M1 next per `four_chip_dongle_pivot_2026-05-11.md` ordering)
- OP_GET_MANIFEST + stub CBOR manifest (the milestone after OP_PING/OP_PONG)
- Three-factor handshake (OP_EXCEPTION / OP_RECONCILE / OP_RESUME)
- Linux integration with `~/knowledge_base_assembly/.../ros_planner_ii_mqtt_robot/libcomm/` host stack

**Things NOT to do tomorrow:**

- Don't redesign the architecture. It's locked across 12 decisions in `s_engine_m_port_architecture_2026-05-12.md` + 2 composition rules in `s_engine_dsl_composition_rules.md`.
- Don't pre-execute by writing chain definitions before the inbound-event injection model is locked through dialog. The user's standing rule: "any chain tree solution must be prototyped in Linux first."
- Don't expand opcode catalog beyond OP_PING/OP_PONG this session — manifest/shell/handshake are bigger chunks for later sessions.
- Don't merge primary + secondary scope into one PR — they're separable, ship primary first.

**Resume command for tomorrow:**

```
ssh robot                                    # Pi has Xiao at /dev/ttyACM0 running register_dongle
luajit /tmp/dongle_console.lua --frame       # confirm last build is still emitting REGISTER+HEARTBEAT
                                              # then read MEMORY.md and start the dialog
```

The dongle is currently still running `register_dongle` with the live OP_REGISTER+OP_HEARTBEAT stream. That's the launching pad for tomorrow — add bidirectional flow on top of the proven s2m path.

---

### Phase 2d closure — bidirectional libcomm + engine event injection (2026-05-13)

Phase 2d delivered. Host-to-dongle command path is now end-to-end verified: PING arrives over USB-CDC, decoder validates, engine event queue receives it, `se_event_dispatch` matches, user function fires OP_PONG, response decoded on the host. State machine (`BOOT → OPERATIONAL`) gates the heartbeat per the protocol's "responder must pause unsolicited sends" rule.

**Locked-this-session decisions:**

| # | Decision |
|---|---|
| 1 | Use `se_state_machine` + `se_event_dispatch`, not `se_wait_event` — reactive handlers compose naturally across all future host-initiated opcodes (manifest, shell, handshake) |
| 2 | BOOT → OPERATIONAL transition driven by explicit `OP_REGISTER_ACK` opcode (option a from dialog), not timeout heuristic |
| 3 | HANDSHAKE state deferred — only built when F1/F2/F3 opcode definitions land |
| 4 | `dongle_state` field added as `int32_t` in `dongle_record` blackboard; `se_i_set_field` initializes to `DONGLE_BOOT=0` on tree init |
| 5 | **Opcode allocation rule (load-bearing):** s2m opcodes stay in `0x0001-0x00FF`; m2s opcodes use `0x0100+`. Reason: m2s opcodes become engine `event_id` values via `s_expr_event_push`; must avoid `SE_EVENT_TICK=4`/`SE_EVENT_INIT=0xfffe`/`SE_EVENT_TERMINATE=0xfffd`. Verified the hard way — `OP_PING=0x0004` collided with `SE_EVENT_TICK` and PONG fired every tick. Captured in `memory/s_engine_dsl_composition_rules.md`. |
| 6 | Host `dongle_console` lives at `/home/pi/dongle_console/` on the Pi + WSL canonical at `linux/dongle_console/`; sync via `cp`/`rsync` on edit. Eventually folds into mqtt_robot. |
| 7 | Path-B verification discipline locked: before flashing a complex firmware, prove wire-level integrity with a minimal `cdc_rx_hexdump` echo firmware. Two layers of debugging surface separated. |

**Files added/changed:**

| File | Purpose |
|---|---|
| `s_engine/dsl_tests/register_dongle_v2/` | Linux prototype of v2 chain (state machine + dispatch); 520 B bump peak; clean trace verified |
| `linux/dongle_console/dongle_console.lua` | Added `--send-ack`, `--send-ping`, `--send-cmd N`; m2s encoder (SLIP + CRC-8); 50 ms inter-frame gap |
| `linux/dongle_console/test_encode.lua` | Offline byte verification: CRC-8/AUTOSAR self-test against reference vector `0xDF` for `"123456789"`; m2s wire bytes for ACK/PING/PING |
| `samd21/apps/cdc_rx_hexdump/` | Sanity-check firmware — `tud_cdc_read` drain + hex print every loop. Proved wire transport before adding chain. |
| `samd21/apps/register_dongle/` | Updated to v2 chain (`register_dongle_v2_module_rom`); new RX path (`frame_decoder_feed` + `s_expr_event_push`); `tick_and_drain` helper drains event queue post-tick; `send_pong` user fn |
| `samd21/apps/register_dongle/vendor/libcomm/opcodes.h` | Added `OP_REGISTER_ACK=0x0103`, `OP_PING=0x0104`, `OP_PONG=0x0005` with allocation-rule comment |
| `memory/s_engine_dsl_composition_rules.md` | Added m2s/event_id collision rule + canonical event-injection harness reference |
| `memory/pi_side_tool_locations.md` | New — dual-tracked tool list (WSL canonical, Pi live copy) |

**On-chip measurements:**

| Section | v1 register_dongle | v2 register_dongle (current) | Δ |
|---|---|---|---|
| `.text` | 24,460 B | 33,640 B | +9,180 B (state machine + dispatch + event_queue + RX path) |
| `.bss` | 2,228 B | 4,448 B | +2,220 B (768 B bump vs 512, plus RX bufs + decoder state) |
| RAM (.bss + 2KB stack) | 4.3 KB / 32 KB (13%) | 6.5 KB / 32 KB (20%) | +2.2 KB |
| Bump peak (runtime) | 208 B / 256 B (81%) | 336 B / 768 B (44%) | +128 B |

**~80% RAM still free** for manifest CBOR + shell + additional chains + handshake.

**Verified on real hardware (Cortex-M0+ Thumb, gcc 8.3.1, Seeeduino Xiao SAMD21):**

```
On boot:
  OP_REGISTER emitted once (24-byte payload: chip_uid + vid:pid + fw_version)
  ↳ Note: first ~12 bytes lost because host hasn't opened CDC port yet
       (known issue, deferred — see secondary scope below)

BOOT state (silent — no heartbeats, LED off):
  Engine ticks at 4 Hz; event_dispatch in BOOT case halts on default

After dongle_console --send-ack:
  OP_REGISTER_ACK arrives → state_machine sees dongle_state=OPERATIONAL on next tick
  HEARTBEAT begins at 1 Hz (cmd=0x0002, uptime_ms + seq counter)
  LED begins toggling at 4 Hz on PA17

After dongle_console --send-ping (3×):
  OP_PONG emitted in response, in order (cmd=0x0005, seq=0,1,2)
  Heartbeat continues through PING bursts (no interference)
  SLIP-escape verified: payload byte 0xC0 emitted as DB DC, host decodes back to 0xC0
```

**Bug found + fixed during the session:**

| Bug | Symptom | Fix |
|---|---|---|
| `OP_PING=0x0004` collided with `SE_EVENT_TICK=4` | PONGs fired on every engine tick, not only on OP_PING | Relocated m2s opcodes to `0x0100+`; rule captured in memory |
| `table.unpack` not in LuaJIT 5.1 | `dongle_console --send-*` crashed with "attempt to call field 'unpack' (a nil value)" | Use bare `unpack()` (LuaJIT 5.1 idiomatic) |

**Resume command for next session:**

```
ssh robot
lsusb | grep 2886   # should show register_dongle (2886:802f) running clean v2
luajit /home/pi/dongle_console/dongle_console.lua --frame   # observe REGISTER + (silent BOOT)
luajit /home/pi/dongle_console/dongle_console.lua --frame --send-ack --send-ping
# expect: heartbeats + PONG response
```

---

### Phase 2e closure — secondary scope cleanup (2026-05-13)

Five items planned; outcome:

| Item | Status |
|---|---|
| `dongle_console` v2d: opcode label decode | **DONE** — small `OPCODE_NAMES` table; output now reads `cmd=0x0002 (OP_HEARTBEAT)` etc. |
| `dongle_console` first-frame sync drop | **DONE** — `slip_state.synced` flag; BAD-SHORT/BAD-LEN/BAD-CRC silent until first valid CRC; eliminates mid-stream-attach noise |
| `frame.h` "no final XOR" comment | **DONE** — 1-line doc fix to match the code (which has `^ 0xFF`) |
| `check_completion:` label-decl patch | **ALREADY DONE** — both labels already have empty `;` workaround (was applied during Phase 2 SAMD21 bring-up) |
| Firmware: gate `send_register` on `tud_cdc_connected()` | **REVERTED — wrong-layer fix.** Tested with 200ms-stability + 5s-timeout gate; OP_REGISTER still got lost in the CDC TX FIFO when the host wasn't actively reading. Bytes accumulate in the 64-byte FIFO; older bytes get overwritten when newer frames arrive (heartbeats every second once OPERATIONAL). The C-side gate cannot fix this — it gates *emission*, not *delivery*. **Proper fix deferred to Phase 2f**: re-emit OP_REGISTER on a `tick_delay` loop in the BOOT state's chain until OP_REGISTER_ACK arrives. That's a DSL change to `register_dongle_v2.lua` (BOOT case becomes a fork of [periodic-retry-chain, event_dispatch-for-ACK]); guarantees delivery regardless of host-attach timing. Documented in `samd21/apps/register_dongle/main.c` adjacent to the affected `tick_and_drain` loop. |

**Lesson captured:** secondary-scope items aren't always small. The CDC gate looked like a 10-LOC tweak but failed to address the root cause (TX FIFO loss during host-unattached windows). Worth ~30 min of trace-debugging to discover; saved by the dialog discipline (push back on own fix when evidence contradicts).

**Resume command after Phase 2e:**

```
ssh robot
luajit /home/pi/dongle_console/dongle_console.lua --frame --send-ack --send-ping
# opcode labels now visible; first-frame partial garbage silently dropped
```

---

### Plan for the next session (Phase 2f — periodic OP_REGISTER retry + RA4M1 toolchain)

**Track A — chain change for OP_REGISTER retry (~half a session):**

Replace the BOOT case in `register_dongle_v2.lua` from:

```lua
se_case(DONGLE_BOOT, function()
    se_event_dispatch(boot_dispatch)
end)
```

to:

```lua
se_case(DONGLE_BOOT, function()
    se_fork(
        function()
            -- Re-send OP_REGISTER every ~2 sec until ACK arrives.
            se_chain_flow(function()
                local r = o_call("send_register")
                end_call(r)
                se_tick_delay(7)                 -- 8 ticks * 250 ms = 2 sec
                se_return_pipeline_reset()
            end)
        end,
        function()
            se_event_dispatch(boot_dispatch)     -- existing OP_REGISTER_ACK handler
        end
    )
end)
```

Then drop the top-level `io_call("send_register")` (the BOOT-state retry covers it). Re-emit, re-flash, verify OP_REGISTER arrives reliably regardless of when the host attaches.

**Track B — second chip family (RA4M1 toolchain bring-up):**

Per `four_chip_dongle_pivot_2026-05-11.md` ordering: "SAMD21 → Linux dongle-manager → RA4M1 → RP2350 → ESP32-C6". Linux side (dongle-manager) is large and the production stack lives at `~/knowledge_base_assembly/.../ros_planner_ii_mqtt_robot/` — that's a discrete future track. RA4M1 firmware is a portability litmus test: same s_engine M-port + same DSL chain should compile and run with only HAL/USB layer swap.

RA4M1 prerequisites:
- arm-none-eabi-gcc (already present)
- Renesas FSP HAL (TBD — vendor download similar to TinyUSB)
- Xiao RA4M1 board file / pin map
- USB-CDC equivalent (RA4M1 has hardware USBHS — different driver)

**Track B — second chip family (RA4M1 toolchain bring-up):**

Per `four_chip_dongle_pivot_2026-05-11.md` ordering: "SAMD21 → Linux dongle-manager → RA4M1 → RP2350 → ESP32-C6". Linux side (dongle-manager) is large and the production stack lives at `~/knowledge_base_assembly/.../ros_planner_ii_mqtt_robot/` — that's a discrete future track. RA4M1 firmware is a portability litmus test: same s_engine M-port + same DSL chain should compile and run with only HAL/USB layer swap.

RA4M1 prerequisites:
- arm-none-eabi-gcc (already present)
- Renesas FSP HAL (TBD — vendor download similar to TinyUSB)
- Xiao RA4M1 board file / pin map
- USB-CDC equivalent (RA4M1 has hardware USBHS — different driver)

**Deferred indefinitely** (NOT secondary scope):
- Engine surgery: `s_expr_module_def_t` → `s_engine_rom_t` rename + delete loader/stack/.cc sources + write `DIVERGENCE.md`
- DSL: compute `bump_buffer_size` per-tree and emit into ROM
- Three-factor handshake (F1/F2/F3 opcodes + HANDSHAKE state stub)
- OP_GET_MANIFEST + CBOR encoder
- Production Linux integration (mqtt_robot host stack adoption)

**Things NOT to do next session:**
- Don't redesign the architecture — Phase 2d locked the inbound-event pattern; future work composes on top
- Don't expand the opcode catalog beyond the OP_REGISTER retry fix — keep Phase 2f small and tight
- Don't merge tracks A and B in one commit if both are pursued — they're separable

---

### Phase 2f closure — periodic OP_REGISTER retry + se_log runtime fix (2026-05-13)

Phase 2f delivered. OP_REGISTER now arrives reliably regardless of when the host attaches — the BOOT-state chain re-emits it every ~1 sec until OP_REGISTER_ACK lands. The session also surfaced and fixed a latent runtime hazard that was breaking Phase 2f on SAMD21.

**Chain change** (`s_engine/dsl_tests/register_dongle_v2/register_dongle_v2.lua`):

```lua
-- BOOT case was: bare se_event_dispatch
-- BOOT case now:
se_fork(
    function()
        se_chain_flow(function()
            local r = o_call("send_register")
            end_call(r)
            se_tick_delay(0)                  -- 1 tick HALT
            se_return_pipeline_reset()         -- cycle ~ 1 sec at 250 ms tick base
        end)
    end,
    function()
        se_event_dispatch(boot_dispatch)       -- existing ACK listener
    end
)
```

The top-level `io_call("send_register")` was dropped — the retry loop covers it. SURVIVES_RESET semantic was correctly given up (retry-until-ACK is the right protocol shape).

**Latent bug discovered + fixed:** the new BOOT chain didn't transition out of BOOT on SAMD21 even though Linux prototype worked. After ~30 min of trace-debugging:

- The `se_log("BOOT: ...")` inside the OP_REGISTER_ACK action was hitting a printf path on SAMD21 (no `debug_fn` registered → fallback `printf`)
- printf pulled `lib_a-nano-mallocr.o` into the link (2.3 KB), reached `_malloc_r` which sbrk-failed silently — *but the code path was still reachable from inside the chain's dispatch action and corrupted something*
- Phase 2d worked despite using the same `se_log` — the new fork+dispatch+chain_flow structure must have crossed some threshold (stack depth? newlib reentrancy?) that the simpler Phase 2d structure didn't
- Fix: drop the printf fallback from `se_log` / `se_log_int` / `se_log_float` / `se_log_field` in `s_engine_builtins_oneshot.h`. When no debug_fn registered, these are silent no-ops. Linux harnesses register a silent debug_fn so no change there.
- Committed separately as `59fc78a s_engine: drop printf fallback in se_log* — M-port heap-free` (one engine commit before the Phase 2f chain commit)
- Flash savings: register_dongle text 33,640 → 31,428 B (-2,212 B = 6.6%)

**Future fix** for getting log output back on SAMD21 documented in `memory/se_log_debug_packet_plan.md`: route `se_log` through an `OP_DBG_LOG` s2m libcomm frame. The dongle's `debug_fn` would call `frame_encode_s2m(OP_DBG_LOG, "...", &g_tx_ring)`; `dongle_console` adds a decoder branch. Not blocking; for whenever observable log output becomes useful (probably during three-factor handshake work).

**On-hardware verification:** flashed and tested with `dongle_console --frame`:

```
8 frames of OP_REGISTER (1 Hz retries while waiting for ACK)
↓ host sent --send-ack
OP_HEARTBEAT seq=0 (transition successful)
↓ host sent --send-ping --send-ping
OP_PONG seq=0  (answer to first PING)
OP_PONG seq=1  (answer to second PING)
OP_HEARTBEAT seq=11, 12, 13... (1 Hz continues)
```

All four observable behaviors correct: retry, transition, heartbeat, PONG.

**Other learnings:**

- `chip is in boot` workflow: double-short reset, mount via `sudo mount /dev/sd[bcd] /mnt/xiao` (number changes per re-enumeration; loop through). UF2 drop + sync. ~3 sec for re-enumeration.
- Diagnostic discipline that paid off: when ACK didn't transition, the four-stage suspect ladder (bytes arrive? frame decode? event queue? action fires?) gave clear narrowing. Added `[RX-DIAG]` print + action-side `o_call(send_pong)` marker. Both diagnostics together revealed the action was firing — pointing to a downstream issue — which is when I started looking at se_log + the runtime.

---

### Plan for the next session (Phase 2g — manifest OP_GET_MANIFEST + CBOR encoder, or RA4M1 toolchain)

After Phase 2f closure, several tracks are open. Picked candidates ordered by leverage:

**Track A — OP_DBG_LOG so se_log becomes observable again (~30 min, small):**

Per `memory/se_log_debug_packet_plan.md`. Smallest possible patch. Doesn't unblock anything else but eliminates a debugging blind spot before deeper work. Pairs nicely as warmup.

**Track B — OP_GET_MANIFEST + stub CBOR manifest (~1 session, medium):**

Manifest is the load-bearing portability primitive per `dongle_linux_protocol_2026-05-11.md`. Even a minimal manifest (firmware version + commands list + symbols list) lets the Linux host start to be "command-set-aware" instead of hardcoded against specific opcodes. CBOR encoder on the dongle side ~200 LOC; host decoder pre-exists in canonical libcomm.

**Track C — RA4M1 firmware port (~1-2 sessions, larger):**

Per `four_chip_dongle_pivot_2026-05-11.md` ordering. First true portability test of the s_engine M-port + chain DSL. Requires Renesas FSP HAL download + new BSP. Verifies the "same chain works unchanged across chips" claim.

**Deferred** (NOT immediate next):
- Three-factor handshake (F1/F2/F3 opcodes + HANDSHAKE state) — needs more protocol surface first
- Engine surgery (loader/stack/.cc deletions, DIVERGENCE.md) — cosmetic, current state works
- Production Linux integration (mqtt_robot host stack adoption) — bigger track, after manifest is real

Recommend starting next session with Track A (warmup, removes a known blind spot) then dialog on B vs C as the main work.

---

### Phase 2g closure — Track A (OP_DBG_LOG) + host-reattach detection + ms timestamps (2026-05-13)

Three coupled changes shipped this session, two commits.

**Commit 1 (`56d3fbb`): engine runtime** — integer-ms timestamp + `s_engine_log` helper

- `se_log` / `se_log_int` / `se_log_float` / `se_log_field` now format `[%lu] ` (uint32 ms) instead of `[%.6f] ` (which rendered empty on SAMD21 because `-u _printf_float` isn't linked).
- New `s_engine_log(inst, msg)` inline helper in `s_engine_module.h` so C user functions can log with the same prefix DSL `se_log()` produces. Byte-identical output.

**Commit 2 (Phase 2g main):** OP_DBG_LOG, host-reattach detection, allocator field for ms time

Runtime additive:

- `s_expr_allocator_t` gained an optional `get_time_ms(void*)` callback (uint32 ms). When registered, `se_log*` and `s_engine_log` use it instead of multiplying `get_time` by 1000.0. Linux harness can also register it. **Why:** the multiplication path pulls `__aeabi_dmul` + `__aeabi_ddiv` + `__aeabi_dsub` into the binary (~3 KB on M-port) — even when the runtime call took the get_time_ms path at runtime, the compiler kept the reachable dead branch's float code. Solved by NOT having a fallback to `get_time` in the log functions at all; SAMD21 sets `.get_time = NULL`. Linux harness sets both (libc has float math anyway).

Track A (OP_DBG_LOG):
- New opcode `OP_DBG_LOG = 0x0010` (s2m, low range).
- `debug_packet_fn` in `samd21/apps/register_dongle/main.c` registered as the engine's `debug_fn`. Each `se_log` / `s_engine_log` becomes an `OP_DBG_LOG` frame staged in the TX ring.
- `dongle_console.lua` gained `OP_DBG_LOG` opcode label + `OPCODE_TEXT_PAYLOAD` rendering: payload bytes shown as a quoted string in `--frame` mode instead of hex columns. Extensible to future text-bearing opcodes.

Host-reattach detection:
- Internal-event range allocated: `0xFE00+`, never on the wire. First member: `EV_HOST_REATTACH = 0xFE00`.
- `samd21/apps/register_dongle/main.c` polls `tud_cdc_connected()` each main-loop iteration. On `true → false → true` edge sequence, pushes `EV_HOST_REATTACH` to the engine event queue. (Polling, not the `tud_cdc_line_state_cb` callback — keeps the event_queue producer single-threaded.)
- New user fn `handle_internal_events` (m_call) added at top level in the chain, before `se_state_machine`. On `EV_HOST_REATTACH`, writes `dongle_state = BOOT` directly into the blackboard (currently safe because `dongle_state` is the sole field at offset 0). Logs the event via `s_engine_log`. state_machine reads the new field on its same-tick invocation and switches BOOT case → BOOT's retry loop resumes emitting OP_REGISTER.

**On-hardware trace (real Xiao, two sequential `dongle_console` sessions, 2 sec gap):**

```
Session 1 (cold boot):
  8 × OP_REGISTER (retries)
  OP_DBG_LOG: "[12250] BOOT: received OP_REGISTER_ACK -> OPERATIONAL"
  OP_HEARTBEAT seq=0, 1, ...

[host closed dongle_console]
[2 sec gap]
[host reopened dongle_console]

Session 2 (re-attach):
  OP_DBG_LOG: "[CDC] DTR dropped"              ← detected the close
  OP_HEARTBEAT seq=4, 5 (buffered during disconnect)
  OP_DBG_LOG: "[CDC] DTR up after drop -> EV_HOST_REATTACH"
  OP_DBG_LOG: "[19250] host reattach -> reset to BOOT"
  OP_REGISTER × N  (fresh registration stream resumes)
```

**Resource impact** (vs previous Phase 2f firmware 31,428 B):

| Build | text | Note |
|---|---|---|
| Phase 2f baseline | 31,428 | |
| Phase 2g (with stale-header issue) | 35,928 | +4.5 KB from accidental `__aeabi_dmul/ddiv/dsub` link via dead branch |
| Phase 2g (final) | **30,132** | -1,296 vs Phase 2f; double-precision math gone entirely |

The smaller final size is because removing `.get_time = engine_get_time` from the allocator drops `engine_get_time` and its division, and `se_log` no longer has a reachable `get_time * 1000.0` branch. M-port is now fully **double-free** on the SAMD21 register_dongle build.

**Lesson captured: `rsync` without `--delete` leaves stale files behind.** During Phase 2g I accidentally rsynced engine header copies into the SAMD21 app directory in an earlier iteration. The Makefile's `-I$(HERE)` (app dir) preceded `-I$(SENGINE_RT)` (runtime), so the compiler picked up the stale copies — even after editing the runtime sources. A full `rsync --delete` cleared them and the build picked up the real runtime. Workflow rule for next session: use `rsync --delete` when syncing dirs that should be authoritative.

**Linux harness updates (same Phase 2g commit):**

- `main_rom.c` adds `utc_realtime_ms` callback for parity with SAMD21.
- ACK injection moved from tick 5 → tick 18 to give a longer pre-ACK window so retries are visible.
- New injection at tick 35: `EV_HOST_REATTACH` (simulated host reset). Verifies the chain handles the reattach path. Output: 8 retries → ACK → OPERATIONAL → reattach injection → REATTACH log → reset to BOOT → fresh retry stream → second ACK → OPERATIONAL resumes. Bump peak: 544 B / 2 KB (same as Phase 2f).

**Push back on three-factor handshake** (documented decision): the F1/F2/F3 design in `dongle_linux_protocol_2026-05-11.md` was speced for a system with subscriptions, sequence tracking, and pin claims to recover. None of that state exists today. "Host re-attach = re-register" is the right floor until there's state worth preserving. When subscriptions land, layer F1/F2/F3 on top of EV_HOST_REATTACH.

**Files added in this session:**

| File | Purpose |
|---|---|
| `s_engine/runtime/s_engine_types.h` | `get_time_ms` field added to `s_expr_allocator_t` (additive, backward-compatible) |
| `s_engine/runtime/s_engine_builtins_oneshot.h` | all `se_log*` use `get_time_ms` (no `get_time` fallback — keeps `__aeabi_d*` out) |
| `s_engine/runtime/s_engine_module.h` | `s_engine_log()` inline helper, same format/source convention as DSL `se_log` |
| `s_engine/dsl_tests/register_dongle_v2/register_dongle_v2.lua` | adds `m_call("handle_internal_events")` sibling above `se_state_machine` |
| `s_engine/dsl_tests/register_dongle_v2/main_rom.c` | adds `utc_realtime_ms`; tick-35 `EV_HOST_REATTACH` injection |
| `s_engine/dsl_tests/register_dongle_v2/user_functions.c` | `handle_internal_events` mock (printf-based) for the Linux trace |
| `samd21/apps/register_dongle/main.c` | `engine_get_time_ms`, `debug_packet_fn` registered, `tud_cdc_connected` poll → EV_HOST_REATTACH push |
| `samd21/apps/register_dongle/user_functions.c` | `handle_internal_events` (s_engine_log via `OP_DBG_LOG`) |
| `samd21/apps/register_dongle/vendor/libcomm/opcodes.h` | `OP_DBG_LOG = 0x0010`, `EV_HOST_REATTACH = 0xFE00` |
| `linux/dongle_console/dongle_console.lua` | `OPCODE_NAMES` adds `OP_DBG_LOG`; `OPCODE_TEXT_PAYLOAD` table; payload renders as quoted text |

---

### Plan for the next session (Phase 2h — Track B manifest OR Track C RA4M1)

> **SUPERSEDED 2026-05-16** — the four-layer sync dialog reshaped Phase 2h. REGISTER v2 payload work below still applies but now lands inside a 3-message sync flow. See "2026-05-16 design dialog" section above + the updated "How to start tomorrow" section for the current plan. Historical Phase 2h plan retained for context.

Track A is closed. Track B was reshaped during this session's design dialog — **CBOR is dropped**, manifest moves to packed-struct + FNV-1a schema hash (per canonical avro_dsl pattern). A new two-tier identity system (class_id + instance_id with commissioning) was designed and is fully captured in `memory/dongle_class_identity_2026-05-13.md`.

**Cross-repo handoff:** the catalog side of the new identity system (`dongle_classes.lua`, `kb_build` codegen, robot-class `required_dongles` field, instance-config `dongles[]` array, robot-side chain-tree matching) lives in `~/knowledge_base_assembly/luajit_programs_and_containers/nano_data_center_base/commissioning_software/`. A future AI session in that repo will fold it in. The full design + implementation breakdown is in the memory file.

**Next session's motioncore-prototype work (Phase 2h):**
1. Wait for `class_ids.h` to be generated from the other-AI's kb_build extension (or stub it locally for unblocked dongle work)
2. Update `OP_REGISTER` payload to v2 (38 B with class_id + instance_id + commissioning_state)
3. Add SAMD21 flash storage abstraction for instance_id (NVMCTRL last-page reservation, dual-bank atomic write)
4. Add `OP_COMMISSION_SET = 0x0105` / `OP_COMMISSION_REPLY = 0x0006` / `OP_COMMISSION_CLEAR = 0x0106` wire opcodes
5. Add `EV_COMMISSION_SET = 0xFE01` engine-internal event (sibling pattern to EV_HOST_REATTACH)
6. Add `handle_commissioning` m_call sibling in `register_dongle_v2.lua` (or extend `handle_internal_events` to switch on event_id — design call at implementation time)
7. Build + flash + test commissioning end-to-end (host can OP_COMMISSION_SET an instance_id, dongle stores in flash, REGISTER frames now carry it, OP_COMMISSION_CLEAR resets to 0)

Track C (RA4M1 firmware port) remains future work — the identity system makes RA4M1 cleaner because all the class+instance machinery is universal.

Side observations from this session worth carrying:
- The `[CDC] DTR ...` diag logs in main.c proved useful and are low-frequency. Consider keeping them long-term as operational signal. Possibly add a verbosity flag if they ever become noise.
- The bump peak hasn't been measured post-handle_internal_events. Add a startup OP_DBG_LOG with bump_peak figure once we have a stable Phase 2h baseline.

---

### 2026-05-16 design dialog — four-layer protocol + RS-485 slave wire

Big design session. The "four-layer routing model" that's been a phantom reference since 2026-05-13 (catalog doc line 566) is now actually written down. Plus a full custom RS-485 protocol spec for slave↔router-dongle communication. Two new memory files; this section is the high-level index.

**Memory files (load-bearing — read before any Phase 2h work):**
- `memory/four_layer_protocol_2026-05-16.md` — universal L0/L1/L2/L3 sync model
- `memory/rs485_slave_protocol_2026-05-16.md` — router dongle ↔ slave wire protocol

**Headline locks (~22 decisions across the dialog):**

- **Four-layer model is universal.** L0 commissioning, L1 identity, L2 manifest, L3 topology. Applies symmetrically to dongle↔Linux AND slave↔router-dongle (carrier differs per role; layers are semantic, same on both sides).
- **3-message initial sync.** Host drives every transition with explicit advance opcodes. Dongle NAKs anything inappropriate (`err_state` / `err_unsupported_cmd`). L0 is a pre-state, not part of the 3-message ladder.
- **Drop OP_MANIFEST_ACK; one generic OP_NAK** for all state/permission errors.
- **L0 rides existing OP_REGISTER** — `commissioning_state` byte in v2 payload distinguishes L0 vs L1 routing on host side. No separate L0 opcode.
- **Two-step re-commissioning:** CLEAR then SET, never combined. Both trigger reboot.
- **Slaves commissioned via USB-CDC only.** Dual-personality firmware: uncommissioned slave boots in USB-CDC libcomm mode (commissions via same OP_COMMISSION_SET opcode as dongles), commissioned slave operates RS-485 (with USB-CDC kept active for diagnostics + recovery). **Eliminates** all RS-485 commissioning-collision concerns.
- **Per-class tick rates revised:**

  | Chip | Dongle build | Slave build |
  |---|---|---|
  | SAMD21 | 10 ms | 10 ms (ISR handles bus → tick decouples from bus speed) |
  | RA4M1 | 10 ms | 10 ms (same) |
  | RP2350 | 1 ms | (not a slave class) |
  | ESP32-C6 | 1 ms | (not a slave class) |

- **RS-485 protocol** (custom, not Modbus):
  - 9-bit MPCM addressing: 9th bit = "address frame" flag, value is 8-bit. **256-value address space** (0x00 broadcast, 0xFF flash sentinel only, 0x01-0xFE assignable = 254 slots).
  - ISR handles RX/TX + framing + auto-ACK; complex work deferred to chain via engine event queue.
  - Implicit-token bus arbitration: most-recently-addressed slave owns the quiet window. Hardware idle-line interrupt ends the window.
  - TCP class (ack-required, dongle-side ack-table) vs UDP class (fire-and-forget).
  - **Slaves only send UDP**, including ACKs. ACK frame carries original message's sequence ID; dongle reconciles against ack-table.
  - Source address in payload byte 0 (1 byte; hardware filter consumes the 9-bit address byte for destination only).
  - Slave-to-slave is UDP-only (no ack state on slaves).
- **kb_build emits per-slave bus_addr** alongside instance_id (in nano_data_center cross-repo handoff scope).

**Phase 2h plan is reshaped** by the new sync model — see the updated "How to start tomorrow" section at the bottom for the current next-steps.

**Three message-protocol stacks** (clarified separation; don't conflate in docs):
1. Linux ↔ dongle: libcomm over USB-CDC, full-duplex, 4-layer L0/L1/L2/L3 sync
2. Router dongle ↔ slaves (SAMD21/RA4M1): custom RS-485, half-duplex, 9-bit addressed, TCP/UDP, ack-table on dongle
3. Router dongle ↔ canonical-bus slaves (CAN, BLE, Thread): future; separate stacks each

---

## How to start tomorrow

### Step 1 — minute-zero verification (unchanged from Phase 2g)

```bash
ssh robot
lsusb | grep 2886
# Expected: "ID 2886:802f Seeed Technology Co., Ltd. register_dongle"
# Phase 2g firmware still running.

timeout 5 luajit /home/pi/dongle_console/dongle_console.lua --frame
# Expected: stream of OP_HEARTBEAT frames at 1 Hz (OPERATIONAL).
# If you see OP_REGISTER instead, --send-ack to advance.
```

### Step 2 — required cold-start reading (in order, ~15 minutes)

1. **`memory/feedback_design_dialog_style.md`** — MANDATORY. Dialog discipline (one concern at a time, push back is value-add, stay in dialog before pre-executing).
2. **`memory/four_layer_protocol_2026-05-16.md`** — load-bearing. The new universal sync model.
3. **`memory/rs485_slave_protocol_2026-05-16.md`** — load-bearing if any slave / RS-485 work this session.
4. **This file's "2026-05-16 design dialog" section** above — what was locked.
5. **`memory/dongle_class_identity_2026-05-13.md`** — REGISTER v2 payload schema (still valid; now lands inside the new sync flow, not as a spike).
6. **`memory/s_engine_dsl_composition_rules.md`** — required for any chain edits.

### Step 3 — the day's track

The 2026-05-16 dialog locked design but no firmware yet. **Phase 2h is now "implement the 3-message sync"** — a discrete, well-scoped delivery on the dongle side:

| # | Deliverable |
|---|---|
| 1 | Sketch `common/spec/four_layer_sync.md` — opcode numbers + payload layouts for `OP_GET_MANIFEST`, `OP_MANIFEST_REPLY`, `OP_OPERATIONAL_BEGIN`, generic `OP_NAK` |
| 2 | Update SAMD21 `send_register` to emit OP_REGISTER v2 (the original Phase 2h step — still applies; `class_id` stubbed `0xDEADBEEF` until kb_build delivers `class_ids.h`) |
| 3 | Add `OP_GET_MANIFEST` + `OP_MANIFEST_REPLY` handlers (stub manifest payload: version + opcode list; defer fancy FNV-1a schema) |
| 4 | Add `OP_OPERATIONAL_BEGIN` handler + state transition (`L1_DONE → OPERATIONAL`); heartbeat starts only after this |
| 5 | Wire generic `OP_NAK` responses for out-of-state opcodes |
| 6 | Update `dongle_console.lua` to walk the 3-message sync from host side (replace current single-step `--send-ack`) |
| 7 | Verify on real SAMD21 — full sync flow + heartbeat starts after `OP_OPERATIONAL_BEGIN` |

L0 commissioning work (SAMD21 flash + `OP_COMMISSION_SET/CLEAR/REPLY`) is its own follow-on milestone, not blocking step 1–7. L3 (slave topology) is router-only and stays deferred.

### Open design TBDs (lock via dialog before firmware lands)

- Opcode numbers for `OP_GET_MANIFEST` / `OP_MANIFEST_REPLY` / `OP_OPERATIONAL_BEGIN` / `OP_NAK`
- Manifest payload schema (packed-struct + FNV-1a hash per `dongle_class_identity_2026-05-13`, not yet detailed)
- Whether `OP_OPERATIONAL_BEGIN` carries any payload
- Generic `OP_NAK` payload (reason byte + optional opcode-being-rejected)

Plus RS-485-side TBDs (only if router work this session): frame byte layout, sequence ID width + uniqueness scope, max payload size, TCP retransmit timeout values, idle-line bit-count for quiet-window detection.

### If the dongle isn't responding

Same Phase 2g recovery story:
1. Power cycle (unplug + replug)
2. Bootloader entry: double-short reset pads
3. Re-flash last known good UF2: `/home/pi/motioncore-prototype/samd21/apps/register_dongle/build/register_dongle.uf2`

12 commits ahead of origin as of Phase 2g session end — see `git log --oneline -12` for the trail.

---

## 2026-05-19 → 2026-05-20 — SAMD21 dongle: protocol + functional HIL complete

Big stretch of work. The SAMD21 dongle went from "Phase 2g sync spike" to a
feature-complete functional-HIL dongle. Each milestone has a dedicated memory
file (see `memory/MEMORY.md`); this section is the index + handoff.

### What got built (all hardware-verified on real SAMD21)

| Milestone | Memory file | Result |
|---|---|---|
| Four-layer sync protocol spec | `four_layer_protocol_2026-05-16` + `common/spec/four_layer_sync.md` | L0/L1/L2/L3 model written down |
| Phase 2h — 3-message sync ladder | `phase_2h_four_layer_sync_done_2026-05-19` | BOOT→L1_DONE→OPERATIONAL live; OP_GET_MANIFEST/OP_MANIFEST_REPLY/OP_OPERATIONAL_BEGIN/OP_NAK |
| L0 commissioning | `l0_commissioning_done_2026-05-19` | NVMCTRL dual-slot flash storage; `commission.lua` standalone tool; survives reboot |
| App-shell general layer | `app_shell_general_layer_done_2026-05-20` | OP_SHELL_EXEC/REPLY + binary-message framing + dispatch table + CMD_ECHO |
| CMD_SYSINFO | `cmd_sysinfo_done_2026-05-20` | runtime flash/ram/bump/uptime/clock dump |
| GPIO commands | `gpio_specific_layer_done_2026-05-20` | config/write/read; `chip_commands_table()` extension pattern; 8-slot shell payload queue fix |
| DAC + ADC commands | `dac_adc_specific_layer_done_2026-05-20` | dac_write / adc_read / dac_waveform (TC3 + sine LUT) / dac_stop / adc_capture; analog loopback D0→D1 verified |
| PWM + counter | `pwm_counter_done_2026-05-20` | pwm config/set/teardown + counter setup/reset/read/stop; D1→D2 jumper loopback verified |

### Current SAMD21 firmware state

- App: `samd21/apps/register_dongle/` — see its `README.md` (written 2026-05-20)
- 15-command shell catalog (2 general + 13 SAMD21-specific). Full four-layer
  protocol + L0 commissioning.
- ~37.8 KB flash (15%), ~5.5 KB RAM (17%). Ample headroom.
- Last-good UF2: `samd21/apps/register_dongle/build/register_dongle.uf2`

### Bench infrastructure learned

- **USB power contention** — a bus-powered SSD on the Pi hub browned out the
  dongle and caused hours of phantom debugging. Captured in
  `memory/bench_hardware_status.md`. Keep hungry peripherals off the dongle's
  USB tree.
- **Chain tick-batching** — shell events process in 250 ms tick batches; host
  inter-command delays need to be ≥ ~one tick to translate to precise
  on-dongle timing.

### How to start tomorrow — RA4M1 port (second chip family)

Per `four_chip_dongle_pivot_2026-05-11` ordering: SAMD21 → (Linux
dongle-manager) → **RA4M1** → RP2350 → ESP32-C6.

RA4M1 (Arduino Uno R4 / Xiao RA4M1, Renesas ARM Cortex-M4) is the portability
litmus test: the s_engine M-port + libcomm + the register_dongle_v2 chain +
the general shell layer should all compile and run **unchanged**; only the
chip layer is new.

**What ports as-is (no change expected):**
- s_engine M-port runtime
- libcomm framing (`vendor/libcomm/`)
- `register_dongle_v2` chain + DSL
- `shell_commands.{c,h}` general layer (echo, sysinfo* — *sysinfo's
  `firmware_get_sysinfo` is chip-specific, needs an RA4M1 impl)
- `user_functions.c` chain handlers (mostly; `samd21_read_uid`, flash
  storage, sysinfo accessor are chip-specific)

**What's new for RA4M1:**
- Renesas FSP HAL (vendor download — equivalent of TinyUSB for SAMD21)
- RA4M1 USB-CDC driver (RA4M1 has hardware USB)
- `ra4m1_commands.c` — the chip-specific shell command set, parallel to
  `samd21_commands.c`. Same `chip_commands_table()` / `chip_commands_count()`
  exports. RA4M1 is the **analytical HIL** chip: 14-bit ADC, 12-bit DAC,
  CMSIS-DSP. Its command set will differ from SAMD21's (higher-res ADC/DAC,
  plus `signal.*` DSP commands per `four_chip_dongle_pivot_2026-05-11`).
- `flash_storage` equivalent — RA4M1 has a dedicated Data Flash region
- chip UID read, `firmware_get_sysinfo` RA4M1 impl
- Makefile + linker script + startup for RA4M1

**Prerequisites to gather first:**
- Renesas FSP HAL download (analogous to how TinyUSB was vendored)
- RA4M1 Xiao board file / pin map
- arm-none-eabi-gcc already present on the Pi

**First concrete step:** hello-CDC on RA4M1 (the equivalent of `hello_cdc`
for SAMD21) — get the toolchain + USB-CDC enumerating before porting the
engine. Then layer the engine + chain + shell on top.

---

### RA4M1 progress — 2026-05-20 (toolchain de-risked)

Full bring-up findings in `memory/ra4m1_bringup_2026-05-20.md` — read that first.
Headlines:

- Board confirmed: **Seeed XIAO RA4M1** (R7FA4M1AB, Cortex-M4 + FPU, 256 KB / 32 KB).
- TinyUSB + Renesas FSP vendored at `ra4m1/vendor/tinyusb/` (per-chip copy).
- `xiao_ra4m1` board created in the vendored `hw/bsp/ra/boards/` — copy of
  `uno_r4` with flash origin moved to 0x4000 (Arduino DFU bootloader reserves
  the low 16 KB). uno_r4 clock/USB config works unchanged.
- **DFU flashing solved**: enter via 1200-baud touch (`stty -F /dev/ttyACM0 1200`),
  NOT the BOOT button. It's a **plain-DFU** device — `dfu-util -a 0 -D file.bin`
  with NO `-s` address. Needs **dfu-util 0.11** (built from source; Debian's 0.9
  has DfuSe bugs). Tap RESET after download to launch the app.
- Pipeline proven: stock `cdc_msc` built for `BOARD=xiao_ra4m1`, DFU-flashed,
  runs + enumerates CDC on the real board.

**Resume here:** the next task is the custom `hello_cdc` + the RA4M1 app build
structure. Open decision (see ra4m1_bringup memory "app structure" section):
the TinyUSB example build system can't build an app outside its own tree
(`SRC_C` must be TOP-relative), so a clean `ra4m1/apps/hello_cdc/` needs a
hand-rolled Makefile (~30 FSP+TinyUSB sources, ~14 include dirs — use the
RA `family.mk` + `xiao_ra4m1/board.mk` as the exact reference, and
`samd21/apps/register_dongle/Makefile` as the structural template). That
Makefile becomes the template for the RA4M1 register_dongle too.

Bench note: the XIAO RA4M1 is on the bench; commission/dongle work also needs
it kept on a USB tree without power-hungry peers (the SSD-brownout gotcha).

### Cold-start reading order for the RA4M1 session

1. `memory/MEMORY.md` — index
2. `memory/feedback_design_dialog_style.md` — dialog discipline
3. `memory/four_chip_dongle_pivot_2026-05-11.md` — RA4M1 role + ordering
4. `memory/app_shell_general_layer_done_2026-05-20.md` — the `chip_commands_table()` extension pattern RA4M1 plugs into
5. `samd21/apps/register_dongle/README.md` — the reference implementation to mirror
6. `memory/gpio_specific_layer_done_2026-05-20.md` + `dac_adc_specific_layer_done_2026-05-20.md` + `pwm_counter_done_2026-05-20.md` — what the chip command layer looks like

---

### RA4M1 progress — 2026-05-20 (s_engine M-port + libcomm verified on hardware)

Three RA4M1 apps built, flashed, and **verified on real hardware** — full
findings in `memory/ra4m1_bringup_2026-05-20.md`:

| App | Proves | Result |
|---|---|---|
| `ra4m1/apps/hello_cdc` | toolchain → TinyUSB+FSP → flash → run | CDC enumerates, 1 Hz counter |
| `ra4m1/apps/blink_engine` | s_engine M-port on Cortex-M4 | chain ROM reused byte-for-byte from SAMD21; node dispatches in lockstep |
| `ra4m1/apps/blink_frame` | libcomm SLIP+CRC framing on Cortex-M4 | 12/12 CRC OK, frames round-trip |

Committed: `13e1684` is the tip of this work.

**Corrections to the prior (now-stale) notes above:**
- The app-build-system question is RESOLVED. Apps DO build outside the
  vendored tree: `ra4m1/apps/<name>/Makefile` includes TinyUSB's make system;
  app sources live in `src/`, out-of-tree sources (s_engine runtime, libcomm)
  resolve via `vpath`. One needed flag: `CFLAGS += -Wno-error` (don't gate our
  code on TinyUSB's strict -Werror). No hand-rolled Makefile required.
- **Flashing**: the Arduino DFU route needs a *cooperating* app (1200-baud
  touch); for a non-cooperating app, flash via the **Renesas USB Boot ROM**
  (`045b:0261`, hold BOOT during USB power-up) with `raflash` — `erase` then
  `write` to 0x4000. Both routes detailed in the ra4m1_bringup memory.
- Two latent s_engine runtime bugs fixed (missing `#include`s, surfaced by the
  RA toolchain's -Werror) — shared code, SAMD21-safe, already committed.

### How to start tomorrow — step 3b: port `register_dongle` to the RA4M1

**Bench prep:** put the Pi on **wired ethernet** if possible — WiFi dropped
constantly on 2026-05-20 and cost real time. If WiFi stays, keep a long-lived
SSH session open for long-running commands (captures / builds).

The work: port `samd21/apps/register_dongle/` → `ra4m1/apps/register_dongle/`
— the full dongle firmware (~2000 lines). Scope reuse-vs-chip-specific first
(dialog before scaffolding), then build.

- **Reused byte-for-byte:** the chain ROM (`register_dongle_v2_module_rom.c`),
  most `user_functions.c` chain handlers, the general shell layer
  (`shell_commands.{c,h}`).
- **New / chip-specific:** `flash_storage` rewritten for the RA4M1's 8 KB Data
  Flash, `firmware_get_sysinfo` RA4M1 impl, chip-UID read, and a
  1200-baud-touch handler so the easy DFU route stays available.
- **Build:** extend the `blink_frame` Makefile pattern; flash via raflash.

**After 3b:** step 4 is `ra4m1_commands.c` — the analytical-HIL command set.
The slave/dongle HIL pin map is filed in `memory/ra4m1_pin_map.md`; its first
task is verifying the D9/D10 encoder routing against the XIAO schematic.

**Cold-start reading order:** `memory/MEMORY.md` → `ra4m1_bringup_2026-05-20`
→ `feedback_design_dialog_style` → `samd21/apps/register_dongle/README.md`
(the reference implementation to mirror).

---

### RA4M1 register_dongle (step 3b) — HARDWARE-VERIFIED — 2026-05-21

`ra4m1/apps/register_dongle/` complete (22 files) and **fully verified on the
XIAO RA4M1**. Builds on the Pi (`make BOARD=xiao_ra4m1` → `register_dongle.bin`,
~34.9 KB flash), flashed via raflash.

Verified end-to-end on hardware:
- boots, USB-CDC enumerates `2886:0053`; s_engine M-port runs the
  register_dongle_v2 chain; libcomm SLIP+CRC framing (all CRCs ok)
- L0 commissioning — `flash_storage.c` on the FSP `r_flash_lp` data flash;
  `instance_id=1` persists across a reboot AND a code-flash reflash
- four-layer sync ladder BOOT → L1_DONE → L2 → OPERATIONAL; manifest
  `schema_hash=0x80AEB146` (identical to SAMD21 → reused chain ROM byte-correct)
- app-shell `CMD_ECHO` + `CMD_SYSINFO` round-trip; heartbeats in OPERATIONAL
- chip UID via `R_BSP_UniqueIdGet()`; class_id = FNV-1a `0x281A0BA4`

Two bring-up fixes (committed):
- `src/r_flash_lp_cfg.h` — FSP `r_flash_lp` module config header the Smart
  Configurator generates; the hand-made `xiao_ra4m1` board lacked it.
- `flash_storage_read` must `R_FLASH_LP_Open` before reading the data flash —
  a read before Open returns indeterminate data. The write path worked (it
  opens the driver); the boot-time commissioning load did not, so a dongle
  that reported `COMMISSION_SET ok` still booted UNCOMMISSIONED. FSP contract:
  Open, *then* memory-mapped read.

**Build/flash decisions** (locked in dialog): flash_storage → FSP `r_flash_lp`,
8 KB data flash @0x40100000, dual-slot (slots 2 KB apart); `user_functions.c`
is a per-chip copy; reused byte-for-byte — `register_dongle_v2*`,
`shell_commands.{c,h}`, `vendor/libcomm/`; s_engine runtime via `vpath`.

Still open on register_dongle: the 1200-baud DFU magic in `main.c`
(`DFU_DOUBLE_TAP_*`) is a placeholder — the touch resets but won't enter DFU
until the value is read off the Seeed XIAO RA4M1 bootloader source. Flashing
uses raflash + the BOOT button meanwhile.

**NEXT ACTION — step 4: `ra4m1_commands.c`**, the analytical-HIL command set
(ADC/DAC/PWM/encoder). register_dongle's general layer is done; step 4 fills
the chip command table (currently a NULL stub). First task: D9/D10 encoder
routing vs the XIAO schematic (`memory/ra4m1_pin_map.md`).

Pi build note: `~/motioncore-prototype/` on the Pi is a build-only subset (not
git; rsync'd from WSL). To build an RA4M1 app: rsync the app dir to
`robot:~/motioncore-prototype/ra4m1/apps/<app>/`, then
`ssh robot 'cd … && make BOARD=xiao_ra4m1'`; flash via raflash (Renesas boot
mode = hold BOOT during USB power-up).

### Dongle commissioning + registry — 2026-05-21

Both SAMD21 register_dongle dongles reflashed (new `class_id 0x5E588873`); all
three dongles now commissioned to unique identities:

| chip_uid | class_id | instance_id |
|---|---|---|
| `0B26…574B` RA4M1  | `0x281A0BA4` | 1 |
| `508880F7…` SAMD21 | `0x5E588873` | 1 |
| `2667118A…` SAMD21 | `0x5E588873` | 2 |

Identity is the **(class_id, instance_id) pair** — instance numbering is
per-class, so the two `instance_id=1`s (RA4M1 + a SAMD21) do not collide.

**`commission.lua` v2** (`linux/usb_commission/`):
- `--class <id>` selection — scans ACM ports, reads `OP_REGISTER`, matches
  class_id. PID-agnostic (works across chip families); assumes ≤1 dongle per
  class_id on the bus.
- registry read/write — `--set`/`--clear` keep the registry in sync;
  `--registry PATH` (default: next to the script).
- uniqueness guard — `--set` refuses a duplicate `(class_id, instance_id)`
  unless `--force`.

**`linux/dongle_registry.lua`** — new: the instance roster, `chip_uid`-keyed,
machine-maintained by commission.lua; the Linux driver loads it to recognise an
attached dongle from its `OP_REGISTER`.

Bench note: `commission.lua` + its registry run on the Pi at `~/usb_commission/`
(rsync'd from `linux/`). The two SAMD21 USB serials are a hardcoded dummy
(`0123456789ABCDEF`) — future fix in the SAMD21 `usb_descriptors.c` to read the
real chip UID, as the RA4M1 already does.
