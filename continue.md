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
