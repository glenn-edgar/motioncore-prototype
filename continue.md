# continue.md — motioncore-prototype

**Status:** plan revised 2026-05-03. Firmware path changed from Zephyr →
**Raspberry Pi Pico SDK + FreeRTOS-Kernel SMP**. Driver: PIO-first for
half-duplex UART, quadrature decoders, and (likely) CAN. Hardware target
unchanged from the previous revision: Raspberry Pi Pico 2 W + Waveshare
Bus Servo Adapter (A). Working port directory has been renamed
`firmware/zephyr/` → `firmware/pico/`. No board has been flashed yet.

This file supersedes the 2026-05-02 evening plan that targeted Zephyr
direct. The Waveshare STM32 SDK find (§5) is unchanged and ports just as
cleanly to Pico SDK as it did to Zephyr — possibly more so, since Pico
SDK's `uart_*` API is closer in shape to the vendored `uart.h` than
Zephyr's device-tree-driven UART API.

The Arduino ESP32 vendor copy at `firmware/arduino/waveshare_servo_driver/`
remains **reference-only** — kept for register-init recipes, protocol
cross-checks, and as a "USB-to-bus dongle" via its `SERIAL_FORWARDING` mode.

---

## 1. Where things stand

- Repo live at https://github.com/glenn-edgar/motioncore-prototype (public, MIT).
- **Active firmware tree:** `firmware/pico/` (renamed from `firmware/zephyr/`).
  - `firmware/pico/waveshare_stm32_sdk/` — frozen vendored copy of
    Waveshare's STM32F103 SDK (`STServoBCL_keil_f103.rar`), a clean C
    port of the SCServo / SMS_STS / ST3215 protocol library with a
    4-function UART abstraction. **Do not edit in place.**
  - `firmware/pico/scservo/` — working port directory. Holds the 7
    platform-agnostic source files copied from `waveshare_stm32_sdk/SCSLib/`
    (~960 LOC of vendored C, untouched). Will gain `uart_pico.c`,
    `timing_pico.c`, `CMakeLists.txt` during Block C. See
    `firmware/pico/scservo/PORT.md` for the per-file inventory and known
    issues. (PORT.md still references "Zephyr" in places — to be edited
    with Block C.)
- **Reference-only firmware tree:** `firmware/arduino/waveshare_servo_driver/`.
  Frozen vendored copy of `waveshare/Servo-Driver-with-ESP32 @ ac24be32`.
- **Design doc:** `docs/continue.md` — original ESP32 motion-subsystem
  design. Stale on hardware, RTOS, and Welford. **Read for the
  Architectural Recap (§1) and protocol-level reasoning only.**
- **Toolchain doc:** `docs/toolchain-wsl.md` — historical (arduino-cli +
  ESP32). To be superseded by `docs/toolchain-pico-sdk.md`.
- **Memory** under `~/.claude/projects/-home-gedgar-motioncore-prototype/memory/`
  has machine-readable notes: project_architecture.md, file_locations.md,
  scservo_protocol_notes.md.

## 2. Hardware on hand

| Item | Role | Status |
|---|---|---|
| Raspberry Pi Pico 2 W | Target MCU (RP2350 + CYW43439) | **TO ORDER** |
| Waveshare Bus Servo Adapter (A) | Standalone bus driver, UART input, 9–12.6 V power, drives single-wire TTL bus | **TO ORDER** |
| 2× ST3215 serial bus servos | Wheels | on hand |
| Waveshare Servo Driver with ESP32 | Demoted to reference / `SERIAL_FORWARDING` dongle | on hand |
| External auto-sense RS-485 breakouts | For long-cable runs if ever needed | on hand (likely unused) |
| Pi Zero–class Linux host | Future top-level controller | on hand |
| Quadrature encoders (qty TBD) | Wheel feedback / external axes — drives the PIO decision | not yet specified |
| CAN transceiver (e.g. SN65HVD230, MCP2542) | Optional bus to peripherals/other MCUs | not yet specified |

## 3. Hardware stack — with rationale

```
Host (Pi 5 / Pi Zero, future ChainTree planning layer)
       ↑
       │  USB-CDC (debug + commands during dev)
       │  Wi-Fi or BLE via CYW43439 (optional, telemetry)
       │  Optional SLIP-over-UART1 (PIO framer)
       ↓
Raspberry Pi Pico 2 W (RP2350, dual Cortex-M33 @ 150 MHz, FPU, 520 KB SRAM)
   ├── UART0 / PIO   ──── Waveshare Bus Servo Adapter (A) ──── ST3215 wheels
   │                       (PIO half-duplex w/ tristate is the likely route)
   ├── PIO SM        ──── Quadrature decoder, 1 SM per encoder
   ├── PIO SM        ──── CAN frame TX/RX (can2040-style soft-CAN) → transceiver
   ├── I²C0          ──── primary IMU
   ├── I²C1          ──── other I²C sensors
   └── USB-CDC       ──── debug console + flashing via picotool
```

**Why Pico SDK + FreeRTOS-Kernel SMP, not Zephyr:**

| Concern | Zephyr-direct (prior plan) | Pico SDK + FreeRTOS SMP (this plan) |
|---|---|---|
| PIO ergonomics | Zephyr has no first-class PIO abstraction — write your own glue against the chip headers | `pioasm` + `hardware/pio.h` are the canonical path; existing PIO programs (quadrature, can2040 CAN, half-duplex UART) drop in cleanly |
| Both M33 cores usable | Only one core supported under Zephyr today | FreeRTOS-Kernel SMP runs tasks across both cores natively |
| Toolchain maturity on RP2350 | Board port is "actively maintained" but young | Pico SDK is the reference platform — `picotool`, OpenOCD-rp2350 fork, and CYW43439 driver all first-class |
| Flashing / loader | `picotool` (works) | `picotool` (same — drag-n-drop UF2 also works) |
| Half-duplex UART tristate | Open question — would need Zephyr RS-485 mode or manual GPIO | PIO half-duplex with tristate built into the PIO program is well-trodden |
| Servo C library port | 4-function UART glue against Zephyr `uart_poll_*` | 4-function UART glue against `uart_read_blocking` / `uart_write_blocking` (or PIO equivalents) — even simpler |
| Portability story for the physics layer | "Drop the C module onto NuttX/etc." was a real benefit | Lost. Mitigation: keep physics/protocol code C99-only, glue stays in `*_pico.c` files |
| OTA / MCUboot | Not yet supported on RP2350 | Same gap. Not Phase 1. |

**What Pico SDK + FreeRTOS SMP costs us vs. Zephyr (acknowledged):**

- **Less RTOS-y device-tree / driver model.** No DTS overlays, no
  `device_get_binding`. Acceptable for a single-board prototype.
- **Less peripheral abstraction across vendors.** Code becomes
  RP2xxx-specific. Mitigation above.
- **No Kconfig.** Use `pico_set_*` CMake helpers and `#define`s.

**Why FreeRTOS-Kernel SMP (specifically the Raspberry Pi pico-sdk
integration):**

- Maintained by RPi alongside the SDK; CMake integration with
  `pico_cyw43_arch` is already wired.
- SMP scheduler routes tasks to either M33 core; can also pin a task to
  a core for the 200 Hz control loop.
- Same FreeRTOS API everyone knows — no new mental model.

**Hardware constraints to verify before ordering (unchanged from prior rev):**

1. Bus Servo Adapter (A) silkscreen direction — Waveshare's "TX-TX
   RX-RX" wording is ambiguous. Confirm against silkscreen photos.
2. DC-jack polarity (center-positive standard, but check).
3. Pico 2 W's `VBUS` / `VSYS` pin routing if running off the same
   battery that powers the servo bus.
4. Common ground between Pico 2 W and the adapter board.

## 4. Firmware stack — with rationale

**SDK:** Raspberry Pi `pico-sdk` (master, RP2350 support).
**RTOS:** `FreeRTOS-Kernel` SMP port, via the RPi-maintained pico-sdk
integration (sets up tick on SysTick, pins ISRs sensibly, plays nice
with `pico_cyw43_arch`).
**Loader:** `picotool` (USB BOOTSEL or `picotool reboot -f`). Drag-n-drop
UF2 also works.
**Debugger (optional):** OpenOCD RP2350 fork + a second Pico as a
picoprobe. Not required for Phase 1 — `printf` over USB-CDC is fine.

**Servo library:** Waveshare STM32F103 SDK (`STServoBCL_keil_f103.rar`),
imported as the seed for `firmware/pico/scservo/`. The 4-function UART
hook interface in `uart.h` maps directly to either Pico SDK `uart_*` or
to a PIO half-duplex UART program — see §5.

**Application code discipline (unchanged):**

> Any new physics-model code goes in its own `.c`/`.h` pair, uses only
> C99 + `<stdint.h>` / `<stdbool.h>`, and does not include any
> platform-specific header. Platform-specific code lives ONLY in the
> Pico-suffixed glue files (`*_pico.c`).

**PIO programs in scope (in priority order):**

1. **Half-duplex UART with tristate.** Replaces the open question from
   the prior plan about whether `uart_poll_out` tristates correctly.
   Owns one PIO state machine; presents the same byte interface the
   Waveshare `uart.h` expects.
2. **Quadrature decoder.** One state machine per encoder. Common pattern
   on RP2xxx; reference programs exist. Frees both M33 cores from
   counting edges.
3. **CAN bus (can2040-style soft-CAN).** Two PIO programs (TX/RX) plus
   a transceiver IC. Frees us from picking a CAN-capable SoC.
4. **SLIP framer (deferred unless host link needs it).** Could live in
   software on UART1 — only worth a PIO SM if we hit framing-CPU pain.

PIO programs are RP2xxx-portable, so investment carries forward across
the RP2040 / RP2350 family.

## 5. The Waveshare STM32 SDK find — what it is, why it matters

Unchanged from prior revision. Summary:

Waveshare's wiki for the Bus Servo Adapter (A) advertises a
multi-platform SDK; one of the platform demos
(`STServoBCL_keil_f103.rar`) is a clean C port of the SCServo / SMS_STS
protocol with a 4-function UART abstraction. Download URL (not
discoverable via search; Wayback Machine):

```
https://files.waveshare.com/wiki/Bus-Servo-Adapter-(A)/STServoBCL_keil_f103.rar
```

Archive layout (relevant parts only):

```
STServoBCL_keil_f103/
└── SCSLib/             ← what we care about (~1100 LOC of C)
    ├── INST.h           protocol opcode constants
    ├── SC.c / SC.h      wire-protocol layer
    ├── ST.c / ST.h      ST3215 (SMS_STS) application layer — 25 functions
    ├── SCSerail.c       buffered TX/RX bridge ("Serail" is upstream typo)
    ├── SCServo.h        umbrella include
    └── uart.c / uart.h  STM32-specific USART driver + 4-function abstraction
```

**The win — UART abstraction is exactly the right shape:**

```c
// uart.h
void    Uart_Init(uint32_t baudRate);
void    Uart_Flush(void);
int16_t Uart_Read(void);                       // -1 if RX empty
void    Uart_Send(uint8_t *buf, uint8_t len);  // blocking TX
```

Two viable backings on the Pico:

- **Plain Pico SDK UART:** ~50 LOC of glue against `uart_init`,
  `uart_read_blocking` (with non-blocking RX via `uart_is_readable`),
  `uart_write_blocking`. Half-duplex tristate handled by toggling the
  TX pin function between UART and SIO between transmits, OR by relying
  on the Bus Servo Adapter (A)'s auto-sense driver if the bench test
  in §7.1 confirms it.
- **PIO half-duplex UART:** ~80 LOC of glue + a small PIO program.
  Tristate is owned by the PIO and provably correct. Recommended path
  if §7.1 reveals any tristate issue with the plain-UART path.

**License caveat:** No LICENSE in the archive. Assumed MIT to match
Waveshare's Arduino-side library terms. Confirm before publishing
derivative work.

**Encoding caveat:** Some files (SC.*, SCSerail.c, uart.*) have
GBK-encoded Chinese comments. Code is ASCII. Optional one-shot `iconv`
cleanup — recommended before edits begin.

## 6. Phase 1 plan (revised 2026-05-03)

| Block | Output | Status |
|---|---|---|
| A | Pico SDK + FreeRTOS-Kernel SMP toolchain on **Pi 4 (raspberrypi @ 192.168.1.66)**: arm-gcc 14.2.Rel1, `pico-sdk`, `pico-extras`, `FreeRTOS-Kernel` (RPi fork — see §A.1), `picotool` 2.2.0-a4, CMake 4.3.2 via pip. Repo at `~/work/motioncore-prototype`. WSL is editor-only over SSHFS (`~/robot-fs`). | **done 2026-05-03** |
| B | SMP smoke test at `firmware/pico/apps/00_smp_hello/` — two FreeRTOS tasks pinned one-per-core, both `printf` to USB-CDC. **Verified on hardware 2026-05-03: USB-CDC streams `core0 tick=N core=0` at 1 Hz. Outstanding: core1 task does not produce output — second core launch / affinity to debug next session.** | **flashed, single-core only** |
| **C** | **`firmware/pico/scservo/` builds and links — vendored library + `uart_pico.c` + `timing_pico.c` + `CMakeLists.txt`.** Plain-UART backing first; swap to PIO half-duplex if §7.1 demands it. | **vendored sources copied; glue files pending** |
| D | Port main.c + 7 STSCL examples (Ping, WritePos, SyncWritePos, SyncRead, FeedBack, RegWritePos, CalibrationOfs) as FreeRTOS apps under `firmware/pico/apps/`. Verify each on hardware. | not started |
| E | Diff-drive physics in C99, own .c/.h, no platform headers. | not started |
| F | 200 Hz FreeRTOS task (pinned to core 0) driving E + ST.c. Telemetry/comms tasks float on core 1. | not started |
| **PIO-1** | **PIO half-duplex UART program** (only if §7.1 fails for plain UART, or if we want guaranteed-correct tristate from day one). | not started |
| **PIO-2** | **PIO quadrature decoder** + C wrapper, one SM per encoder. Pulled in once encoder hardware is on the bench. | not started |
| **PIO-3** | **CAN via can2040-style PIO** + transceiver. Pulled in once the CAN use case is concrete. | not started |
| → | **Deliverable: portable C module (E+F + scservo) + reusable PIO building blocks** | — |

Welford anomaly detection: **out of scope** (carried over).

**Effort estimate:** ~5 days for Blocks A–F. PIO-2/PIO-3 add ~1 day
each when their hardware arrives.

## 7. Prerequisites before starting Block C

### 7.1 Half-duplex tristate verification (~30 minutes)

Wire Pico 2 W's UART0 TX to the Bus Servo Adapter (A)'s RX pin and to a
logic analyzer probe. Capture the line during a `uart_write_blocking` of
a known byte sequence at 1 Mbaud, then verify:

1. Clean rising/falling edges, ~1 µs bit width, no double-driving.
2. **TX line tristates between transmits** (idle high-Z, not held low).
   On the Pico SDK plain-UART path this depends on the adapter's
   auto-sense driver doing the right thing — Pico's UART itself drives
   the line continuously. If tristate is needed at the MCU, switch to
   the PIO half-duplex path (PIO-1 above).
3. Servo replies arrive on the same wire after a ~50–80 µs turnaround.

**If (2) fails:** promote PIO-1 from "only if needed" to "required for
Block C", and document in `firmware/pico/scservo/PORT.md`.

### 7.2 Bus Servo Adapter (A) wiring and silkscreen check

Before powering on:

1. Confirm DC-jack polarity (center-positive expected) with a meter.
2. Confirm UART label direction on silkscreen.
3. Confirm common ground between Pico 2 W and adapter is wired.

### 7.3 Servo ID assignment

Out of the box, both ST3215s ship with ID = 1 (collision). Before any
two-servo test:

1. Connect ONE servo at a time to the bus.
2. Use the upstream Waveshare ESP32 board's `SERIAL_FORWARDING` mode (or
   a one-shot `ProgramEprom.c` sketch) to assign IDs 1 and 2.
3. Label the IDs on the physical servos.

This is once-per-servo. No reason to roll our own — the Waveshare ESP32
board handles it via web UI.

## 8. Phase 1 → Phase 3 path

Under the new plan, **Phase 3 = Phase 1 plus wireless/control-loop
features turned on, plus PIO-driven peripherals as they're needed**.
Same MCU, same SDK, same RTOS. There is no longer any RTOS or framework
hop in Phase 3.

What Phase 3 actually adds:

- Wi-Fi/BLE telemetry via the CYW43439 (Pico SDK `pico_cyw43_arch_lwip_*`).
- Pi-host command link over UART1 (or USB-CDC channel 2).
- ChainTree Level-1 reactive layer on the Pico, talking up to ChainTree
  Level-2 planning on the host.
- Quadrature + CAN PIO programs land here at the latest (sooner if their
  hardware shows up during Phase 1).
- Optional: pin the control loop to core 0 and move all telemetry / Wi-Fi
  work to core 1 explicitly (FreeRTOS SMP `vTaskCoreAffinitySet`).

Carry-forward risks from prior revision:

- **No MCUboot for RP2350 yet.** No OTA from either Zephyr or Pico SDK.
  For prototype, use `picotool` over USB.
- **OpenOCD for RP2350** needs the rpi fork (released 0.12.0 predates
  the chip). `picotool` remains the easier path for Phase 1.
- **CYW43439 + FreeRTOS SMP integration** has occasional rough edges —
  some Pico SDK CYW43 calls assume single-core. Pin the cyw43 task to
  one core to sidestep.

## 9. Open decisions

1. **Plain-UART backing vs PIO half-duplex from day one for Block C.**
   Plain UART is faster to write (~50 LOC) but takes on tristate risk;
   PIO half-duplex is ~80 LOC + a tiny PIO program but provably correct.
   **Recommendation: write plain-UART path first, run §7.1 on the bench,
   switch to PIO if needed. Both paths leave the SCServo library
   untouched.**

2. **GBK → UTF-8 SDK cleanup.** Same as before. Files affected: SC.c,
   SC.h, SCSerail.c, SCServo.h, uart.c, uart.h.
   **Recommend a one-shot `iconv` commit before any code edits.**

3. **When to bring in PIO-2 (quadrature) and PIO-3 (CAN).** Both are
   hardware-driven — pulled in when the encoders / CAN transceiver land
   on the bench. Don't build either speculatively.

4. **`SyncWriteSpe` / `SyncReadPosSpeed` helpers.** Same as before —
   defer until Block F profiling shows we need them.

5. **Old Waveshare ESP32 board role.** Reference + servo-ID-assignment
   dongle. No active firmware work.

6. **License confirmation with Waveshare** for `STServoBCL_keil_f103.rar`.
   Low priority for prototype, must-do before any public release.

7. **Whether to vendor `FreeRTOS-Kernel` and `pico-sdk` as submodules
   or fetch via CMake `FetchContent`.** Submodules give reproducible
   builds; FetchContent is less repo clutter. **Recommend submodules
   pinned to specific tags** so the build is deterministic on a fresh
   WSL.

## 10. Dev environment as actually built (2026-05-03)

Block A and Block B both completed this session. The build host is the
**Pi 4 at 192.168.1.66** (Debian Bullseye, aarch64, 8 GB RAM, ~190 GB
free). WSL is editor-only via SSHFS at `~/robot-fs` mapping
`pi@192.168.1.66:/home/pi`. SSH alias `robot` in `~/.ssh/config` uses
key `~/.ssh/id_pi4_66`.

### A.0 What's installed on the Pi

| Component | Version | Path |
|---|---|---|
| Arm GNU Toolchain | 14.2.Rel1 (Nov 2024) | `~/pico/arm-gnu-toolchain-14.2.rel1-aarch64-arm-none-eabi/` |
| pico-sdk | master | `~/pico/pico-sdk/` |
| pico-extras | master | `~/pico/pico-extras/` |
| FreeRTOS-Kernel (RPi fork) | master | `~/pico/FreeRTOS-Kernel/` |
| pico-examples | master | `~/pico/pico-examples/` |
| picotool | 2.2.0-a4 | `/usr/local/bin/picotool` |
| CMake | 4.3.2 (pip user) | `~/.local/bin/cmake` |
| ninja-build | 1.10.1 (apt) | `/usr/bin/ninja` |
| Project repo | clone of glenn-edgar/motioncore-prototype | `~/work/motioncore-prototype/` |

`~/.bashrc` exports `PICO_SDK_PATH`, `PICO_EXTRAS_PATH`,
`FREERTOS_KERNEL_PATH` and prepends both `~/.local/bin` and the arm
toolchain `bin/` to `PATH`. **Non-interactive ssh skips bashrc** — pass
env vars inline when scripting builds via ssh.

### A.1 Toolchain gotchas hit during Block A → save these

These cost time the first pass; **do not** re-discover them next session:

1. **Bullseye apt arm-gcc is gcc 8 (2019)** — too old for RP2350. Always
   install Arm's binary toolchain to `~/pico/`. Apt's
   `gcc-arm-none-eabi` package is unusable.
2. **Bullseye apt CMake is 3.18** — too old for current pico-sdk
   (`pico_status_led` target needs CMake 3.19+). `pip3 install --user
   --upgrade cmake` gets 4.x via wheel; no sudo, no system change.
3. **Mainline `FreeRTOS/FreeRTOS-Kernel` does NOT contain the RP2350
   port.** Use the Raspberry Pi fork at
   `https://github.com/raspberrypi/FreeRTOS-Kernel.git`. The RP2350
   ports live at `portable/ThirdParty/GCC/RP2350_ARM_NTZ` and
   `RP2350_RISC-V`.
4. **`pico-examples` ships its own `FreeRTOS_Kernel_import.cmake`
   files** that hard-code the mainline FreeRTOS-Kernel layout
   (`portable/ThirdParty/Community-Supported-Ports/...`) which the RPi
   fork does not have. Workaround for smoke-testing pico-examples:
   configure with `FREERTOS_KERNEL_PATH` unset so the freertos examples
   are skipped. **Our own apps include the FreeRTOS_Kernel_import.cmake
   that ships *inside the RPi fork*** at `portable/ThirdParty/GCC/
   RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake` — see
   `firmware/pico/apps/00_smp_hello/CMakeLists.txt`.
5. **The Pi's outbound internet is ~50 KB/s** (cause unclear — wired
   eth0, healthy LAN). WSL's outbound is ~14 MB/s. **For any download
   over ~10 MB, fetch on WSL and rsync to the Pi over LAN** (~350 KB/s
   in practice; the LAN itself is also slow but still 7× faster than
   the Pi's WAN). The Arm toolchain (~145 MB) takes ~7 min via this
   path, vs ~50 min direct.
6. **FreeRTOSConfig.h for the RP2350 ARM-NTZ SMP port requires more
   than the typical FreeRTOS configs.** See
   `firmware/pico/apps/00_smp_hello/FreeRTOSConfig.h` — in addition to
   the usual settings, the NTZ port needs:
   `configUSE_PASSIVE_IDLE_HOOK`, `configENABLE_TRUSTZONE`,
   `configRUN_FREERTOS_SECURE_ONLY`, `configENABLE_MPU`,
   `configENABLE_FPU`, `secureconfigMAX_SECURE_CONTEXTS`,
   `configMAX_API_CALL_INTERRUPT_PRIORITY`,
   `configMAX_SYSCALL_INTERRUPT_PRIORITY`,
   `configKERNEL_INTERRUPT_PRIORITY`, plus a `configASSERT(x)` macro.
   `configASSERT` must use `portDISABLE_INTERRUPTS()` (not
   `taskDISABLE_INTERRUPTS()`) because portmacro.h calls it before
   task.h is included.
7. **With `configSUPPORT_STATIC_ALLOCATION=1` and SMP, the app must
   provide four callbacks**: `vApplicationGetIdleTaskMemory`,
   `vApplicationGetPassiveIdleTaskMemory` (SMP-specific, takes
   `xCoreID`), `vApplicationGetTimerTaskMemory`, and (because
   `configUSE_MALLOC_FAILED_HOOK=1`) `vApplicationMallocFailedHook`.
   With `configCHECK_FOR_STACK_OVERFLOW=2`, also need
   `vApplicationStackOverflowHook`. The minimal versions that work are
   in `00_smp_hello/main.c`.
8. **`pico_stdio_usb` produces no output under FreeRTOS until you
   add three things together:**
   (a) link `pico_async_context_freertos` in CMakeLists,
   (b) call `async_context_freertos_init()` from a FreeRTOS task
       (we use a brief `setup_task` that inits and self-deletes),
   (c) set `configSUPPORT_PICO_SYNC_INTEROP=1` and
       `configSUPPORT_PICO_TIME_INTEROP=1` in FreeRTOSConfig.h —
       these make pico-sdk's mutex/sleep_ms yield to FreeRTOS instead
       of deadlocking. Without (c) the printf path inside stdio_usb
       hangs on its internal mutex.
9. **`configMAX_SYSCALL_INTERRUPT_PRIORITY` should be 16, not 0**, on
   Cortex-M33 NTZ. Setting to 0 prevents API-callable interrupts from
   ever running. The pico-examples canonical value is 16.
10. **The pico-examples `hello_world/serial` is the UART variant** —
    will not produce `/dev/ttyACM*`. Use `hello_world/usb` for the
    USB-CDC smoke test.
11. **picotool's `-x` flag executes after load** — much better than
    a separate reboot dance. `picotool reboot -u -f` to put a running
    Pico back into BOOTSEL is needed before the next flash.

### A.3 Block B verified on hardware (2026-05-03)

`firmware/pico/apps/00_smp_hello/build/00_smp_hello.uf2` flashed to a
Pico 2 W (BOOTSEL → picotool load -x). Pico re-enumerated as
`2e8a:0009 Raspberry Pi Pico` with `/dev/ttyACM0`. Output stream:

```
core0 tick=N core=0
core0 tick=N+1 core=0
...
```

at 1 Hz, read with pyserial (DTR asserted by default). **The `core1`
task does NOT produce output** — only the `core0` task is active. Two
candidate causes for next session:

- The second M33 core isn't actually being launched by
  `vTaskStartScheduler()` despite `configNUMBER_OF_CORES=2`.
- `vTaskCoreAffinitySet(t1, 1u << 1)` fails or the mask convention is
  inverted on this port.

Debug plan: drop affinity entirely and let both tasks float; if both
tasks then print but `core=0` for both, second core isn't running. If
both print and `core=` differs, affinity logic was wrong. Compare
against `pico-examples/freertos/hello_freertos.c` directly.

### A.4 Hardware-day gotchas (USB enumeration on this Pi 4)

- **The Pi 4 chronic undervoltage** (hwmon hwmon1 cycles every
  ~30–60s) was a red herring — once we plugged through a known-good
  port directly, USB-CDC worked. PSU swap deferred to user.
- **Plain Picos do NOT enumerate USB out-of-the-box.** The bootrom
  only enables USB when BOOTSEL is held during plug-in. Without
  BOOTSEL or running firmware, the USB bus is silent (LED still on
  from VBUS). XIAO boards behave differently — they ship with a
  bootloader that exposes USB-CDC unconditionally.
- **No driver install needed.** The Linux `cdc_acm` kernel module
  handles all USB-CDC class devices including Picos. Same with
  mass-storage in BOOTSEL mode.

### A.5 Next-session entry point

1. Skim §1 + §3 + §4 + §6 + §10 of this file to recover context.
2. Decide open #1 (plain UART vs PIO from day one) and open #2 (GBK
   cleanup). Recommendations: plain UART first, GBK fix yes.
3. **Address the Pi 4 PSU.** Use the official 5V/3A USB-C; the chronic
   undervoltage warnings need to go away before USB hubs become
   reliable.
4. **Debug single-core SMP** in `00_smp_hello` (see §A.3). Quick
   tests in order: drop affinity → check both tasks run → if both
   land on core=0, second core not booting. Diff against
   `pico-examples/freertos/hello_freertos.c` for the missing pattern.
5. Run §7.1 (half-duplex tristate test) — needs a logic analyzer. If
   unavailable, defer and pre-bias toward PIO-1.
6. **Block C:** write `uart_pico.c` (~50 LOC plain UART, or ~80 LOC
   PIO), `timing_pico.c` (~10 LOC against
   `to_ms_since_boot(get_absolute_time())` / `sleep_ms`), and
   `firmware/pico/scservo/CMakeLists.txt`. Build-only first.
7. Update `firmware/pico/scservo/PORT.md` to drop "Zephyr" wording and
   reflect the Pico SDK glue file names (`uart_pico.c`,
   `timing_pico.c`).
8. **Block D:** port the 7 STSCL examples as `firmware/pico/apps/0N_*`
   apps. Flash one at a time. First-flash target is `01_ping` against
   a single ID-1 servo on the bus.

### Build-and-flash one-liner

```bash
ssh robot 'export PICO_SDK_PATH=$HOME/pico/pico-sdk; \
           export FREERTOS_KERNEL_PATH=$HOME/pico/FreeRTOS-Kernel; \
           export PATH=$HOME/.local/bin:$HOME/pico/arm-gnu-toolchain-14.2.rel1-aarch64-arm-none-eabi/bin:$PATH; \
           set -e; cd ~/work/motioncore-prototype/firmware/pico/apps/<APP>/build && \
           ninja && \
           sudo picotool reboot -u -f; sleep 3; \
           sudo picotool load -x <APP>.uf2; sleep 3; \
           python3 -c "import serial,time; s=serial.Serial(\"/dev/ttyACM0\",115200,timeout=8); s.dtr=True; time.sleep(0.5); print(s.read(800).decode(errors=\"replace\"))"'
```

Use `set -e` — the `ninja | tail` pattern silently masks build failures
because `tail` exits 0 even if `ninja` exited non-zero.

## 11. Cross-references

- **Active port directory:** `firmware/pico/scservo/` (with `PORT.md`
  documenting per-file status — note: PORT.md still references Zephyr
  pending Block C cleanup).
- **Vendored Waveshare STM32 SDK (frozen):**
  `firmware/pico/waveshare_stm32_sdk/` (with `NOTICE.md`).
- **Reference-only Arduino tree (frozen):**
  `firmware/arduino/waveshare_servo_driver/` (with `NOTICE.md`).
- **Original full design doc (partially stale):** `docs/continue.md`.
- **Toolchain doc (now historical):** `docs/toolchain-wsl.md`. To be
  superseded by `docs/toolchain-pico-sdk.md`.
- **Persistent project memory:**
  `~/.claude/projects/-home-gedgar-motioncore-prototype/memory/`
  - `project_architecture.md` — being updated this session for the
    Pico-SDK + FreeRTOS-SMP pivot
  - `file_locations.md` — Waveshare SDK download URL, related upstream refs
  - `scservo_protocol_notes.md` — sign-magnitude quirks, library API gaps,
    mode setup cookbook
- **Upstream sources of record:**
  - Waveshare Bus Servo Adapter (A) wiki: https://www.waveshare.com/wiki/Bus_Servo_Adapter_(A)
  - Raspberry Pi Pico SDK: https://github.com/raspberrypi/pico-sdk
  - Pico SDK docs: https://www.raspberrypi.com/documentation/pico-sdk/
  - FreeRTOS-Kernel: https://github.com/FreeRTOS/FreeRTOS-Kernel
  - pico-examples FreeRTOS / SMP samples: https://github.com/raspberrypi/pico-examples
  - can2040 (PIO CAN reference): https://github.com/KevinOConnor/can2040
  - FEETECH SDK enhanced fork: https://github.com/adityakamath/SCServo_Linux
