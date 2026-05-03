# scservo — working port directory

This is the **working** port of the Waveshare STM32 SCServo C library to
Raspberry Pi Pico 2 W under Zephyr. Files here may be modified.

The frozen vendor source lives in `../waveshare_stm32_sdk/` — diff
against `../waveshare_stm32_sdk/SCSLib/` to see what has changed from
upstream.

## What's here today (2026-05-02 import)

| File | Status | Source | Notes |
|---|---|---|---|
| `INST.h` | vendored verbatim | `SCSLib/INST.h` | 7 protocol opcodes |
| `SC.h` | vendored verbatim | `SCSLib/SC.h` | wire-protocol API |
| `SC.c` | vendored verbatim | `SCSLib/SC.c` | wire-protocol implementation |
| `ST.h` | vendored verbatim | `SCSLib/ST.h` | SMS_STS / ST3215 application API |
| `ST.c` | vendored verbatim | `SCSLib/ST.c` | application implementation |
| `SCSerial.c` | renamed | `SCSLib/SCSerail.c` | upstream typo "Serail" → "Serial" |
| `SCServo.h` | vendored verbatim | `SCSLib/SCServo.h` | umbrella include |
| `uart.h` | vendored verbatim | `SCSLib/uart.h` | 4-function UART abstraction |

**~960 LOC total**, all platform-agnostic C.

## What still needs to be written for Zephyr / Pico 2 W

| File | Purpose | Approx LOC |
|---|---|---:|
| `uart_zephyr.c` | implements `Uart_Init`/`Flush`/`Read`/`Send` from `uart.h` against Zephyr's UART API on the RP2350. Drives the 1 Mbaud bus to the Waveshare Bus Servo Adapter (A). | ~80 |
| `timing_zephyr.c` | implements `millis()` / `delay()` against `k_uptime_get_32()` / `k_msleep()` for the existing example sketches | ~10 |
| `CMakeLists.txt` | Zephyr module manifest (zephyr_library + sources) | ~15 |
| `Kconfig` | optional module options (ring-buffer size, default IOTimeOut) | ~10 |

## Known issues to address during the port

1. **`SCSerial.c::readSC` uses an iteration-counter timeout, not real time.**
   `IOTimeOut = 5000` increments per loop iteration — wall-clock duration
   depends on MCU speed. On RP2350 @ 150 MHz the loop runs ~2× faster than
   on STM32F103 @ 72 MHz. Replace with `k_uptime_get_32()` for real ms timeouts.
2. **`SC.c::syncReadBegin` uses `malloc`** — change to a static buffer
   (we know `IDN ≤ 2` for diff-drive).
3. **Half-duplex tristate** — verify the Bus Servo Adapter (A)'s
   auto-sense driver works with Pico 2 W's `uart_poll_out` on a logic
   analyzer before betting Block C on it.
4. **GBK-encoded Chinese comments** in some files — optional cleanup.
5. **`SCSerial.c` includes `<stm32f10x.h>`** at the top — strip this once
   we have `uart_zephyr.c` providing the equivalent; it's only there for
   the original `__IO` / `USART_*` types and is unused at this layer.

## Phase 3 portability discipline

Anything written here should compile against any C99 + `<stdint.h>` +
`<stdbool.h>` environment. The Zephyr-specific code lives ONLY in
`uart_zephyr.c` and `timing_zephyr.c` — swapping platforms means swapping
those two files, not editing the protocol layer.
