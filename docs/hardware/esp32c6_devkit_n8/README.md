# ESP32-C6-DEV-KIT-N8 (Waveshare) — Pinout Reference

**Board:** Waveshare ESP32-C6-DEV-KIT-N8 with pre-soldered headers. Pin-compatible with Espressif's reference ESP32-C6-DevKitC-1.
**Module:** ESP32-C6-WROOM-1-N8 — RISC-V single-core @ 160 MHz, 512 KB SRAM, 8 MB flash, WiFi 6 + BLE 5 + IEEE 802.15.4 (Zigbee/Thread).
**Onboard USB:** CH343 USB-UART bridge + CH334 USB hub, alongside native ESP32 USB-Serial-JTAG. One USB-C cable presents two virtual COM ports to the host.

---

## ⚠ 38-pin terminal-board mismatch warning

This board has **32 pins** (16 per side). Generic 38-pin screw-terminal carriers sold for the original ESP32-WROOM-32 DevKit **do not match this board's pin layout or silk-screen labels**:

- Different header-row spacing: this board is **22.86 mm (0.9")**; ESP32-WROOM-32 38-pin carriers are typically 25.4 mm (1.0").
- Different GPIO numbering: ESP32-C6 ≠ ESP32 classic. The labels on the carrier (e.g., "GPIO2", "GPIO15") refer to the *original ESP32's* pin map, not the C6.
- Six terminal positions will be unused.

**Use this document — not the silk-screen — as the authoritative mapping at the bench.**

---

## Full pinout

Looking down at the board with the USB-C connector facing you:

### J1 — left header (16 pins)

| Header pin | Label | GPIO | Notes |
|:---:|:---:|:---:|---|
| 1 | 3V3 | — | 3.3 V regulated power out |
| 2 | RST | — | active-low reset |
| 3 | 4 | GPIO4 | ⚠ strapping (MTMS); ADC1_CH4 |
| 4 | 5 | GPIO5 | ⚠ strapping (MTDI); ADC1_CH5 |
| 5 | 6 | GPIO6 | JTAG MTCK; ADC1_CH6 — free if not using SWD/JTAG |
| 6 | 7 | GPIO7 | JTAG MTDO — free if not using SWD/JTAG |
| 7 | 0 | GPIO0 | free; ADC1_CH0 |
| 8 | 1 | GPIO1 | free; ADC1_CH1 |
| 9 | 8 | GPIO8 | ⚠ strapping; drives onboard RGB LED |
| 10 | 10 | GPIO10 | free |
| 11 | 11 | GPIO11 | free |
| 12 | 2 | GPIO2 | free; ADC1_CH2 |
| 13 | 3 | GPIO3 | free; ADC1_CH3 |
| 14 | 5V | — | 5 V in/out (USB-C) |
| 15 | GND | — | ground |
| 16 | NC | — | no connection |

### J3 — right header (16 pins)

| Header pin | Label | GPIO | Notes |
|:---:|:---:|:---:|---|
| 1 | GND | — | ground |
| 2 | TX | GPIO16 | 🚫 reserved — wired to CH343 USB-UART bridge |
| 3 | RX | GPIO17 | 🚫 reserved — wired to CH343 USB-UART bridge |
| 4 | 15 | GPIO15 | ⚠ strapping (boot log enable) |
| 5 | 23 | GPIO23 | free |
| 6 | 22 | GPIO22 | free |
| 7 | 21 | GPIO21 | free |
| 8 | 20 | GPIO20 | free |
| 9 | 19 | GPIO19 | free |
| 10 | 18 | GPIO18 | free |
| 11 | 9 | GPIO9 | ⚠ strapping (boot mode select) |
| 12 | GND | — | ground |
| 13 | 13 | GPIO13 | 🚫 reserved — native USB D+ |
| 14 | 12 | GPIO12 | 🚫 reserved — native USB D− |
| 15 | GND | — | ground |
| 16 | NC | — | no connection |

### Legend

- **free** — fully available for application use
- **⚠ strapping** — usable but the chip samples this pin at reset; do not drive externally during power-up; any pull-up/down must match the chip's boot-time default
- **🚫 reserved** — hardwired on this board to USB peripherals; not available unless you cut traces

---

## Pin-budget summary

| Category | Count | GPIOs |
|---|:---:|---|
| Fully free | 12 | GPIO0, GPIO1, GPIO2, GPIO3, GPIO10, GPIO11, GPIO18, GPIO19, GPIO20, GPIO21, GPIO22, GPIO23 |
| JTAG (free if no SWD) | 2 | GPIO6, GPIO7 |
| Strapping (use with care) | 5 | GPIO4, GPIO5, GPIO8, GPIO9, GPIO15 |
| Reserved (USB) | 4 | GPIO12, GPIO13, GPIO16, GPIO17 |
| **Total application-usable** | **~19** | (12 free + 2 JTAG-free + 5 strapping-cautious) |

---

## Two USB endpoints, one USB-C cable

The CH334 hub routes the USB-C connection to **both**:

1. **Native ESP32 USB-Serial-JTAG** (chip's internal USB peripheral on GPIO12/GPIO13). Appears on the Linux host as `/dev/ttyACMx`. Supports CDC-ACM data, firmware flashing, and JTAG debugging — simultaneously, in one device.
2. **CH343 USB-UART bridge** (wired to GPIO16/GPIO17 on the chip). Appears as `/dev/ttyUSBx`. Pure UART pass-through up to ~3 Mbps.

**Recommended split for the dongle role:**

| Stream | Endpoint | Purpose |
|---|---|---|
| libcomm uplink to Linux | native USB (`/dev/ttyACMx`) | binary `bus_msg_t` framing, packet-oriented |
| `dbg_shell` text + telemetry | CH343 UART (`/dev/ttyUSBx`) | interactive shell, log lines, traces |

Two physically separate endpoints, no firmware-side multiplexing.

---

## Recommended pin assignments for router-dongle role

| Function | TX / signal A | RX / signal B | Notes |
|---|:---:|:---:|---|
| Native USB (libcomm uplink) | GPIO13 (D+) | GPIO12 (D−) | hardwired |
| CH343 UART (debug shell) | GPIO16 | GPIO17 | hardwired |
| TWAI (CAN, hardware) | GPIO10 | GPIO11 | both fully free and adjacent; TWAI pins are matrix-routable |
| RS-485 (UART, auto-direction) | GPIO18 | GPIO19 | both fully free and adjacent |
| Status LEDs | GPIO22, GPIO23 | — | free, adjacent, near GND |
| ADC capability (4 channels) | GPIO0–GPIO3 | — | ADC1_CH0..3 |
| Spare | GPIO20, GPIO21 | — | reserve |

Avoid the 5 strapping pins (GPIO4, GPIO5, GPIO8, GPIO9, GPIO15) for primary bus signals. They are usable for outputs once the chip is past reset, but external pulls during boot will affect strap state.

---

## ADC

- ESP32-C6 has **one 12-bit ADC** (ADC1). No ADC2.
- 7 channels: ADC1_CH0..6 → GPIO0, GPIO1, GPIO2, GPIO3, GPIO4, GPIO5, GPIO6
- Channels CH4–CH6 land on strapping/JTAG pins; the four clean channels for application use are **CH0–CH3 on GPIO0–GPIO3**.

---

## Physical dimensions

| Dimension | Value |
|---|---|
| Pin pitch | 2.54 mm (0.1") |
| Header-row separation | **22.86 mm (0.9")** |
| Total board length (incl. USB-C overhang) | ~52 mm |
| Header pin-row length | ~38 mm (15 × 2.54 mm) |
| Board width | ~25 mm |

**Footprint vs Pico 2 W:** header-row separation differs (22.86 mm vs Pico's 17 mm). Carrier PCBs are not interchangeable between RP2350 and ESP32-C6 boards.

---

## Boot-mode strapping behavior

ESP32-C6 samples the following pins at reset:

| GPIO | Function at boot | Default state |
|---|---|---|
| GPIO8 | (informational) | pulled-up |
| GPIO9 | boot mode select (low = download, high = SPI flash) | pulled-up → normal boot |
| GPIO15 | ROM serial log enable (low = silent, high = print) | pulled-up → log enabled |
| GPIO4, GPIO5 | JTAG signal source select | sampled at boot |

If you wire a transceiver or peripheral to a strapping pin, ensure it does not actively drive that pin during the chip's reset window (~20 ms after EN goes high). Open-drain outputs from devices that come up high-Z are safe; push-pull outputs from devices in unknown reset state are risky.

---

## Sources

- [Espressif ESP32-C6-DevKitC-1 user guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32c6/esp32-c6-devkitc-1/user_guide.html) — authoritative pinout; Waveshare board is pin-compatible with this reference.
- [Waveshare ESP32-C6-DEV-KIT-N8 product page](https://www.waveshare.com/esp32-c6-dev-kit-n8.htm) — board overview, confirms CH343 + CH334 USB design and pin compatibility.
- [Waveshare wiki — ESP32-C6-DEV-KIT-N8](https://www.waveshare.com/wiki/ESP32-C6-DEV-KIT-N8) — full board docs (accessible in a browser).
- [ESP32-C6 Technical Reference Manual](https://www.espressif.com/sites/default/files/documentation/esp32-c6_technical_reference_manual_en.pdf) — chip-level peripheral details including TWAI (CAN), USB-Serial-JTAG, and strapping pin behavior.

---

*Document date: 2026-05-11. Module: ESP32-C6-WROOM-1-N8 (8 MB flash).*
