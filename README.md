# motioncore-prototype

ST3215 serial-bus-servo motion subsystem prototype targeting a **Raspberry Pi
Pico 2 W** (RP2350) as the standalone motion dongle, with a Linux host (Pi
Zero / Pi 4 / Pi 5) as the upstream planner. The MCU owns the
millisecond-scale wheel control loop and presents a register-mapped
interface upward — mirroring the FIRST Motioncore / Systemcore split.

This repo is the bring-up substrate that derisks the Linux integration and
the firmware abstraction layers before committing to production silicon.

## Hardware target

- **MCU board:** Raspberry Pi Pico 2 W (RP2350 + CYW43439 — Cortex-M33×2
  with FPU, 520 KB SRAM, 4 MB flash, native USB-CDC, Wi-Fi/BLE).
- **Bus driver:** Waveshare Bus Servo Adapter (A) — standalone driver
  board (no MCU), 1 Mbaud UART input, 9–12.6 V DC-jack power, 3.3 V
  tolerant.
- **Servos:** 2× ST3215 (SMS_STS-family) serial bus servos as diff-drive
  wheels.
- **Host:** Pi 4 (current dev/build host @ 192.168.1.66) and a Pi
  Zero–class deployment host downstream.
- **Future:** quadrature encoders (PIO-decoded), optional CAN transceiver
  (PIO can2040 pattern), IMU + I²C sensors.

## Firmware stack

- **SDK:** Raspberry Pi Pico SDK (master, RP2350 support).
- **RTOS:** FreeRTOS-Kernel SMP — Raspberry Pi fork (mainline FreeRTOS
  doesn't have RP2350 ports yet).
- **Loader:** `picotool` over USB BOOTSEL; UF2 drag-n-drop also works.
- **Servo library:** vendored from Waveshare's STM32F103 SDK
  (`STServoBCL_keil_f103.rar`) — clean C port of the SCServo / SMS_STS /
  ST3215 protocol stack with a 4-function UART abstraction. Glue layers
  in `*_pico.c` files; protocol code stays platform-agnostic C99.

## Phase plan (current revision — see `continue.md` for the full state)

1. **Block A (done) — Pi-as-dev-host toolchain.** Pico SDK, RPi
   FreeRTOS-Kernel fork, picotool, CMake, all installed on the Pi 4.
2. **Block B (done, not yet flashed)** — `firmware/pico/apps/00_smp_hello/`:
   two FreeRTOS tasks pinned one-per-core, both `printf` core ID over
   USB-CDC. `.uf2` builds; flash gated on Pico 2 W arrival.
3. **Block C (next) — SCServo library port.** Drop in the vendored
   Waveshare SDK + write `uart_pico.c` + `timing_pico.c` + CMakeLists.
4. **Block D — STSCL example apps** (Ping, WritePos, SyncWritePos,
   SyncRead, FeedBack, RegWritePos, CalibrationOfs) as FreeRTOS apps
   under `firmware/pico/apps/0N_*`.
5. **Blocks E + F — diff-drive physics in C99 + 200 Hz control task.**
   Telemetry tasks float on core 1.
6. **PIO blocks** as their hardware lands — half-duplex UART (replaces
   plain UART backing if needed), quadrature decoder, CAN.

## Layout

| Path | Purpose |
|---|---|
| `continue.md` | Current design state, decisions, next-session entry point |
| `docs/dev-host-setup.md` | How to set up SSH alias + SSHFS + Pi toolchain |
| `docs/continue.md` | Original (partly stale) full design doc — read for protocol/architecture only |
| `docs/toolchain-wsl.md` | Historical WSL+arduino-cli toolchain doc; superseded |
| `firmware/pico/` | Active firmware tree (RP2350 / Pico SDK / FreeRTOS) |
| `firmware/pico/scservo/` | Working port of the SCServo library |
| `firmware/pico/waveshare_stm32_sdk/` | Frozen vendored Waveshare STM32 SDK (do not edit) |
| `firmware/pico/apps/` | One CMake project per app |
| `firmware/arduino/` | Reference-only Arduino-ESP32 vendor copy (frozen) |

## Dev workflow

This repo is built **on a Pi 4 dev host** (`pi@192.168.1.66`); other
machines edit it via SSHFS. See `docs/dev-host-setup.md` for the
end-to-end setup, including the SSH alias, SSHFS mount, and Pi-side
toolchain install (with the gotchas that cost time the first round —
Bullseye apt's CMake/arm-gcc both too old, RPi FreeRTOS-Kernel fork
required, etc.).

To build any app:

```bash
ssh robot 'cd ~/work/motioncore-prototype/firmware/pico/apps/00_smp_hello && \
           mkdir -p build && cd build && cmake -G Ninja .. && ninja'
```

To flash a Pico 2 W: hold BOOTSEL while plugging USB, copy the `.uf2`
onto the resulting `RPI-RP2`/`RP2350` drive.

## License

MIT — see `LICENSE`. The vendored Waveshare STM32 SDK is included on the
assumption it inherits the MIT terms of Waveshare's Arduino-side library;
to be confirmed before any public release. See `firmware/pico/waveshare_stm32_sdk/NOTICE.md`.
