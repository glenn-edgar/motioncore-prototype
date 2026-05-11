# continue.md — motioncore-prototype

**Status:** plan revised 2026-05-04 (evening). **Major scope pivot today** — from "Block A–G port the SCServo library to a Pico" to "build a libcomm-based dongle on FreeRTOS-SMP that fronts an existing chain_tree planner stack." The pre-pivot Block A–G content is superseded; pre-pivot continue.md is in git history (`git log -- continue.md`).

The vendored Waveshare STM32 SDK at `firmware/pico/waveshare_stm32_sdk/` and the working dir `firmware/pico/scservo/` remain on disk as reference. They may feed back in at Phase 6 (RS-485 bring-up — deferred). They are not on the current critical path.

---

## 1. What this device is

The Pico 2 W is a **dongle adaptor** that bridges:
- USB-CDC ↔ a Pi running chain_tree planner (master in libcomm parlance, address `0x00`).
- RS-485 (PIO half-duplex) → bus servo slaves (addresses `0x01..0xFC`).
- CAN (PIO soft-CAN, can2040-style) → CAN peripherals.

It implements the existing libcomm protocol (SLIP framing + CRC-8/AUTOSAR + 40-byte `bus_msg_t` envelope, addressing `0x00..0xFF`, dongle self at `0xFE`, commissioning at `0xFF`). Identity `(dongle_type, dongle_instance)` is written to internal flash via a commissioning protocol; pre-commission the unit responds only at `0xFF` and refuses to start its real-time threads.

---

## 2. Today's deliverables (exist on the Pi at `/home/pi/work/motioncore-prototype/`, not yet on WSL)

- **Three Pico 2 W's flashed and validated** with `00_smp_hello.uf2`. SMP works on both cores. USB-CDC under FreeRTOS works (three-part recipe: `pico_async_context_freertos` linked, `async_context_freertos_init` called from a FreeRTOS task, `configSUPPORT_PICO_SYNC_INTEROP=1`).
- **Two known-good USB cables identified.** USB enumeration gotcha: avoid bad xhci hub ports (kernel logs `error -32` / `error -71`); see `bench_hardware_status.md`.
- **`firmware/pico/common/reboot_cause.{h,c}`** — captures scratch[0..3] on boot, stamps `WATCHDOG_TIMEOUT` as the default, exposes `reboot_with_cause(cause, d0, d1, d2)` for controlled fail-stops. Scratch[4..7] reserved by pico-sdk and not touched. Cause-code enum extends to user-defined codes from 64.
- **`firmware/pico/apps/01_ram_stats/`** — prints RAM map at boot (FreeRTOS heap free/min, newlib heap, per-task stack hwm). 7 tasks at steady state.
- **`firmware/pico/apps/02_watchdog/`** — hardware watchdog (500 ms) + idle-hook liveness flags + guardian feeder on core 0. Auto-wedge demo proves end-to-end fail-stop with diagnostic captured to scratch and surfaced on next boot.
- **The SMP gotcha that bit us today and the cure:** `vApplicationGetPassiveIdleTaskMemory` indexed `tcb[configNUMBER_OF_CORES - 1]` with `xCoreID` (out of bounds), silently corrupted core 1's passive idle. Today's fix: `configSUPPORT_STATIC_ALLOCATION=0`. For the libcomm port, will re-enable static allocation with **`configKERNEL_PROVIDED_STATIC_MEMORY=1`** alongside it — that flag makes FreeRTOS provide the idle/timer-task memory hooks itself, bypassing the booby trap.

These three apps are **diagnostic test scaffolding, not production threads.** They prove capabilities; the real dongle thread layout starts at `firmware/pico/dongle/<class>/` (created in Phase 4).

---

## 3. Architecture (locked at end of 2026-05-04)

Full spec in memory `dongle_libcomm_pico_port_plan.md`. Read it first thing next session — it's the primary architectural reference. Headlines:

- **Hard real-time end-to-end EXCEPT the USB-CDC edge to the Pi.** Internal queues use REBOOT-on-overflow policy. USB-CDC slices (`mgr_in_q`, `ext_tx_q` slice → `ext_bus_pi`) use NAK because USB is async by construction. No soft-fallback / "tier-1 hold" / "tier-2 ramp" behaviors — deadline miss → reboot with cause → next-boot init handles recovery in the class.
- **Core 1 is RT-only.** Hosts `ext_bus_pio_rs485`, `ext_bus_can`, and the inner control closure of every RT class (`drive_base_inner`). Everything else (manager, internal_bus, ext_bus_pi, drive_base_outer, guardian, async_context, FreeRTOS internals) on core 0.
- **Inner+outer split per RT class.** `*_outer` = LOW priority on core 0, planner / mission state. `*_inner` = HIGH priority on core 1, fast control closure. Communicate via depth-2 queues with REBOOT policy.
- **Class registration model.** Dongle `main()` is generic. Each class provides one entry point: `class_register(internal_bus, manager, class_config_cbor, instance_id)`. Adding a new class is one new file + one entry in `class_table[]`. Internal robots are swappable.
- **Persistent commission record.** LittleFS in last 64 KB of flash. `/commission.cbor` holds `(schema_version, dongle_type, dongle_instance, chip_id, firmware_compat, class_config, bus_topology, provenance)`. Schema is additive (CBOR fields can be added without breaking older firmware).
- **Commissioning is a separate Pi-side program** triggered via MQTT request from elsewhere. Robot doesn't care who initiated — sees authenticated CBOR-over-USB-CDC at address `0xFF`. Once commissioned, reboot to operational mode at `0xFE`.
- **Deadline supervisor** extends the 02_watchdog guardian. Each RT thread stamps `last_completed_tick` per cycle; guardian on core 0 checks ages every 50 ms; any miss → `reboot_with_cause(REBOOT_CAUSE_DEADLINE_MISS, thread_id, age_ms, expected_period_ms)`.
- **Health snapshot telemetry at 1 Hz** (opcode `OP_GET_HEALTH_SNAPSHOT`). Per-queue hwm, per-task stack hwm + run_time_pct, per-core idle %, heap, per-bus error counts. Idle % via FreeRTOS `configGENERATE_RUN_TIME_STATS=1` — no DIY accounting.
- **Debug serial port + shell (infra-provided, conditionally compiled).** A dedicated debug UART (default: UART1 on a reserved GP pair) plus a general-purpose shell core (`firmware/pico/common/dbg_shell.{c,h}`) is part of every slave build by default. Shell provides line parsing, command registration, and generic commands (`help`, `list`, `get`/`set` against the shared symbol namespace, `tasks`, `queues`, `reboot`, `unlock`, `estop`); each class registers its own commands at boot via the same dispatch table that backs ChainTree action special-forms and host `OP_*_SET_PARAM` opcodes — one source of truth, four consumers. Output flows through a shared SRAM ring buffer (default 32 KB) drained to UART by a single LOW-priority task via DMA. Policy: **drop-on-full** (telemetry loss is acceptable; this is deliberately the opposite of the REBOOT-on-overflow rule for control queues). Each layer is gated by a literal `#if 1 ... #endif` block at the top of its source file — `firmware/pico/common/dbg_uart.c` wraps the UART + ring + drainer; `firmware/pico/common/dbg_shell.c` wraps the parser. Toggle by editing the source from `#if 1` to `#if 0` (or vice versa). No CMake option, no `-D` flag, no Kconfig — the toggle lives in the file it controls. Default `#if 1` for both in dev builds. Flip the shell layer to `#if 0` in production builds that need telemetry capture without exposing an interactive command surface; flip both for security-hardened or pin-constrained production builds. Stripping both saves ~10 KB code + 32 KB SRAM and releases two GPIOs.

**Reboot-cause enum extensions (additions to `reboot_cause.h` for the dongle):**

```
REBOOT_CAUSE_QUEUE_OVERFLOW   = 10
REBOOT_CAUSE_DEADLINE_MISS    = 11
REBOOT_CAUSE_HANDLER_BUDGET   = 12
REBOOT_CAUSE_BUS_ERROR_BURST  = 13
REBOOT_CAUSE_COMMISSION_APPLIED = 14
REBOOT_CAUSE_COMMISSION_INVALID = 15
REBOOT_CAUSE_FLASH_FS_CORRUPT   = 16
```

---

## 4. Plan of action (agreed 2026-05-04)

| Phase | Output | Effort | Status |
|---|---|---|---|
| **P0** | **Migrate the existing chain_tree container stack from WSL to the Pi.** Pull existing images via the Docker Hub credentials already set up on both hosts; point the planner stack at a local mosquitto; validate `test_random_paths.lua` + `test_mock_planner.lua` against the Pi-side stack. | ~1 day | **Open. Awaiting (a)/(b) decision below.** |
| P1 | `bus_kernel_pico.c` port + FreeRTOSConfig retune. Three contract extensions (`bus_thread_set_affinity`, `bus_msgq_high_water[/_reset]`, `bus_msgq_set_overflow_policy`) plus likely `BUS_MUTEX_STORAGE_BYTES` 64→96. Committed canonically to the WSL libcomm repo. | ~3 days | not started |
| P2 | LittleFS at last 64 KB of flash. Single `flash_worker` task on core 0 serializes writes via `flash_safe_execute()`. Mount/read/write/erase + power-loss tests. | ~2 days | not started |
| P3 | Commission protocol (`OP_COMMISSION_*` opcodes). CBOR record schema + validator on Pico. Pi-side commission tool inside the container, MQTT-request triggered. Pre/post-commission state machine on the Pico. | ~3 days | not started |
| P4 | Generic dongle skeleton + class registration framework. `mock_robot` class for end-to-end smoke test. Dongle `main()` is class-agnostic. | ~4 days | not started |
| P5 | drive_base inner+outer split + deadline supervisor. Mock-bus stub for `ext_bus_pio_rs485`. Wedge test for inner deadline miss. | ~5 days | not started |
| P6 | RS-485 + servo bring-up. **Detailed planning deferred** — when bench hardware is staged we plan: master against USB-RS485 probe, slave-base test partner, then real servo bring-up. | TBD | not started |
| P7 | CAN bring-up. **Detailed planning deferred** — same shape as P6. | TBD | not started |
| P8 | Health snapshot telemetry + Pi-side dashboard inside the container. | ~3 days | not started |

Critical path: **P0 → P1 → P2 → P3 → P4 → P5.** Phases 6–8 follow once 1–5 are green and bench hardware (jumpers, transceivers, partners) is staged.

---

## 5. Where source lives

- **WSL repo** `/home/gedgar/motioncore-prototype/`: continue.md (this file), older `firmware/pico/scservo/`, `firmware/pico/waveshare_stm32_sdk/`. **Does NOT have today's `apps/` and `common/` work.**
- **Pi repo** `/home/pi/work/motioncore-prototype/` (SSHFS-mounted at `/home/gedgar/robot-fs/work/motioncore-prototype/`): same content **plus** today's `firmware/pico/apps/{00_smp_hello, 01_ram_stats, 02_watchdog}/` and `firmware/pico/common/{reboot_cause.{h,c}, CMakeLists.txt}`.
- **The two are not git-synced.** Pi has the deliverables. After Pi restart and SSHFS reconnect, decide whether to rsync today's files back to WSL or keep the Pi as canonical going forward.
- **libcomm canonical tree:** `/home/gedgar/knowledge_base_assembly/luajit_programs_and_containers/building_blocks/ros_planner_ii_mqtt_robot/libcomm/` (WSL host). P1 will clone/sync this to the Pi.
- **Toolchain on the Pi** (validated today): arm-none-eabi-gcc 14.2.Rel1 at `~/pico/arm-gnu-toolchain-...`, pico-sdk master at `~/pico/pico-sdk/`, FreeRTOS-Kernel (RPi fork, single-commit clone) at `~/pico/FreeRTOS-Kernel/`, picotool 2.2.0-a4 at `/usr/local/bin/picotool`, CMake 4.3.2 via pip user install.
- **Pi USB SSD** mounted at `/home/pi/mountpoint`. Will need to remount cleanly after reboot before Docker / build steps that depend on it.

---

## 6. Open decisions before P0 starts

1. **Containerized firmware toolchain (option a) vs native Pi toolchain (option b)?**
   - (a) Pico firmware build moves into a `mycorp/dongle_firmware_dev` container image (arm-none-eabi-gcc + pico-sdk + FreeRTOS-Kernel + libcomm headers). Reproducible across machines, but adds an image and needs `/dev/ttyACM0` pass-through plumbing for picotool.
   - (b) Keep the existing Pi-native toolchain at `~/pico/...` for the firmware build; only the Linux-side runtime is containerized.
   - **One-word answer needed.** Lean: (b) for speed, (a) if you want full container discipline from the start.

2. **bus_kernel.h contract changes go upstream to WSL libcomm repo, yes?** (Three additive functions + likely one storage-budget bump.) Recommended; awaiting final OK.

3. **Branch strategy for `motioncore-prototype`:** feature branch per phase, merge to `main` on gate-pass. Recommended; awaiting OK.

---

## 7. Where to start next session

1. **Verify the Pi survived the reboot:**
   ```
   ssh robot 'mount | grep mountpoint; ls ~/work/motioncore-prototype/firmware/pico/; ls ~/pico/'
   ```
   Expect `/home/pi/mountpoint` remounted, the `apps/` + `common/` dirs intact, the toolchain present.

2. **Re-establish the WSL→Pi SSHFS mount:**
   ```
   sshfs robot:/home/pi /home/gedgar/robot-fs   # or whatever the existing fstab/script does
   ```

3. **Read `dongle_libcomm_pico_port_plan.md`** in the memory dir. That's the architectural spec; everything in §3 of this file is a summary.

4. **Pick up at the (a)/(b) firmware-toolchain decision in §6.** One word unblocks P0.

5. **Start P0 — container migration to the Pi:**
   - On the Pi: pull the existing chain_tree container stack via Docker Hub creds.
   - Bring up the planner + mosquitto + mock_robot harness on the Pi.
   - Run `test_random_paths.lua` + `test_mock_planner.lua` against the Pi-side stack until green-pass identical to WSL.
   - Deliverable: same test suite green on the Pi as currently green on WSL. ~1 day.

6. **After P0 green, start P1 — `bus_kernel_pico.c` port.** Plan in `dongle_libcomm_pico_port_plan.md` §"Order of work."

---

## 8. Cross-references

- **Memory** at `/home/gedgar/.claude/projects/-home-gedgar-motioncore-prototype/memory/`:
  - `dongle_libcomm_pico_port_plan.md` — **primary architectural reference** for the dongle. Read first.
  - `dongle_protocol_reboot_cause.md` — first post-boot packet must carry reboot_cause; scratch[4..7] reserved by pico-sdk.
  - `bench_hardware_status.md` — 3 Picos flashed, USB enumeration gotcha (xhci hub-port).
  - `pico_sdk_freertos_setup.md` — toolchain on the Pi 4 dev host, USB-CDC three-part recipe, the SMP static-alloc passive-idle bug we hit and fixed today.
  - `project_architecture.md`, `file_locations.md`, `scservo_protocol_notes.md`, `development_plan_2026-05-03.md` — older notes, partially superseded by today's pivot but kept for context.
- **libcomm canonical tree:** `~/knowledge_base_assembly/luajit_programs_and_containers/building_blocks/ros_planner_ii_mqtt_robot/libcomm/` on WSL.
- **Pi dev host:** SSH alias `robot` (192.168.1.66, user `pi`). Repo at `~/work/motioncore-prototype/`. WSL editor mount at `~/robot-fs`.
- **Upstream sources of record:** Raspberry Pi pico-sdk, RPi FreeRTOS-Kernel fork, can2040 (PIO CAN reference), LittleFS (when P2 lands).
