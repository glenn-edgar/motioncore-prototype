# Provenance and license notice

This directory contains a vendored copy of Waveshare's STM32F103 SDK for
the Bus Servo Adapter (A) — a clean C port of the SCServo / SMS_STS
protocol library. This is the starting point for the Phase 1 / Phase 3
Zephyr port targeting the Raspberry Pi Pico 2 W.

## Source

- Download URL: https://files.waveshare.com/wiki/Bus-Servo-Adapter-(A)/STServoBCL_keil_f103.rar
- Date imported: 2026-05-02
- Original archive preserved as `_original.rar` in this directory
- Upstream wiki: https://www.waveshare.com/wiki/Bus_Servo_Adapter_(A)
- License: not stated in the archive. Assumed MIT, matching Waveshare's
  Arduino-side SCServo library license (`firmware/arduino/waveshare_servo_driver/LICENSE`).
  **TODO: confirm with Waveshare before publishing derivative work.**

## What this is

A Keil µVision project for an STM32F103 (BluePill-class MCU) that drives
ST/SC-series serial bus servos through one of two USART peripherals
(USART1 or USART2, selected by `#define`). Includes the full STM32
StdPeriph driver tree and Cortex-M3 startup files alongside the library
proper.

The interesting subtree for our port is **`SCSLib/`**, which contains 9
files (~1100 LOC of C) implementing:

- `INST.h` — protocol opcode constants
- `SC.c` / `SC.h` — wire-protocol layer (Ping, Read/Write, SyncRead/Write)
- `ST.c` / `ST.h` — ST-series application layer (WritePosEx, FeedBack, etc.)
- `SCSerail.c` — buffered TX/RX bridge (note: vendor typo, "Serail" not "Serial")
- `SCServo.h` — umbrella include
- `uart.c` / `uart.h` — STM32-specific USART driver (4-function abstraction)

The `examples/` directory contains 13 setup/loop-style sketches mirroring
the SC/STSCL examples shipped with the Arduino library (Ping, WritePos,
SyncWritePos, SyncRead, FeedBack, RegWritePos, CalibrationOfs, Broadcast,
ProgramEprom, WriteSpe, etc.).

## Modification status

**This vendor directory is FROZEN.** Do not modify these files in place —
they are the upstream reference baseline that the working port in
`../scservo/` is derived from. Updates to upstream Waveshare SDK should
be re-imported here as fresh snapshots, not patched in.

## Why vendored, not a download script

The download URL is on `files.waveshare.com` under a hash-bucketed path
that is not discoverable via search engines (we only located it through
the Wayback Machine). Waveshare URLs are not stable. A vendored copy
guarantees the source is available regardless of upstream changes.

## Encoding note

Several files (`SC.c`, `SC.h`, `SCSerail.c`, `uart.c`, `SCServo.h`) have
GBK-encoded Chinese comments rather than UTF-8. The code is
ASCII-only — only comments are affected — but text editors not configured
for GBK will display them as garbled characters. Optional cleanup item;
not load-bearing for the port.
