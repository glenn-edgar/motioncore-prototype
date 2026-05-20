# register_dongle — SAMD21 functional-HIL dongle firmware

Firmware for the Seeeduino XIAO SAMD21 acting as a **dongle** in the
motioncore four-chip dongle suite. It speaks the libcomm wire protocol to a
Linux host over USB-CDC and exposes a binary command shell for
hardware-in-the-loop (HIL) operations: GPIO, DAC, ADC, PWM, pulse counting.

This is the reference / first-chip implementation. RA4M1, RP2350 and ESP32-C6
ports reuse the engine + general-shell layers and supply their own
`<chip>_commands.c`.

---

## Architecture

```
  Linux host  ──USB-CDC──  SAMD21 dongle
                              │
       libcomm framing (SLIP + CRC-8/AUTOSAR)
                              │
       s_engine M-port chain  (register_dongle_v2)
         BOOT → L1_DONE → OPERATIONAL state machine
                              │
       app-shell (OP_SHELL_EXEC / OP_SHELL_REPLY)
         general commands  +  SAMD21-specific commands
```

Three building blocks merged into one binary:

| Block | Role |
|---|---|
| **s_engine M-port** | DSL-defined chain orchestrating the dongle lifecycle |
| **libcomm framer** | SLIP + CRC-8 wire encode/decode over USB-CDC |
| **register_dongle_v2 chain** | the four-layer-sync state machine + event dispatch |

See `common/spec/four_layer_sync.md` for the protocol spec.

---

## Four-layer protocol

| Layer | Purpose | Opcodes |
|---|---|---|
| L0 — Commissioning | persist class/instance identity to flash | `OP_COMMISSION_SET/CLEAR/REPLY` |
| L1 — Identity | declare who-I-am | `OP_REGISTER` (v2 payload) / `OP_REGISTER_ACK` |
| L2 — Manifest | declare runtime opcode catalog | `OP_GET_MANIFEST` / `OP_MANIFEST_REPLY` |
| (advance) | enter normal operation | `OP_OPERATIONAL_BEGIN` |

Dongle state machine: `UNCOMMISSIONED → BOOT → L1_DONE → OPERATIONAL`.
Host drives every transition; the dongle NAKs out-of-state opcodes.

A factory-fresh dongle boots `UNCOMMISSIONED` and accepts only
`OP_COMMISSION_SET`. After commissioning (+ reboot) it boots `BOOT` and runs
the L1→L2→OPERATIONAL sync ladder.

---

## App-shell command catalog

Commands are invoked via `OP_SHELL_EXEC` once the dongle is `OPERATIONAL`.
Wire: `[request_id u16][command_id u16][args_message]` → reply
`[request_id u16][status u8][result_message]`. Args/results are
command-specific little-endian binary messages.

### General layer (cross-chip)

| ID | Command | Args | Result |
|---|---|---|---|
| 0x0001 | echo | `len:u16, bytes[]` | same bytes |
| 0x0002 | sysinfo | — | flash/ram/bump/uptime/clock snapshot |

### SAMD21-specific layer

| ID | Command | Args | Result |
|---|---|---|---|
| 0x0100 | gpio_config | port, pin, mode (0=in/1=out/2=in_pullup/3=in_pulldown) | — |
| 0x0101 | gpio_write | port, pin, level | — |
| 0x0102 | gpio_read | port, pin | level:u8 |
| 0x0103 | dac_write | value:u16 (0..1023) | — |
| 0x0104 | adc_read | channel:u8 (AIN 0..19) | value:u16 (12-bit) |
| 0x0105 | dac_waveform_write | type, amp, offset, freq_hz, duration_ms | — |
| 0x0106 | dac_stop | — | — |
| 0x0107 | adc_capture | num_ch, channels[], num_samples, delta_us | samples:u16[] |
| 0x0108 | pwm_config | port, pin, freq_hz, resolution_bits | — |
| 0x0109 | pwm_set | duty:u16 | — |
| 0x010A | pwm_teardown | — | — |
| 0x010B | counter_setup | port, pin | — |
| 0x010C | counter_reset | — | — |
| 0x010D | counter_read | reset_flag:u8 | count:u32 |
| 0x010E | counter_stop | — | — |

**Peripheral notes (SAMD21G18A):**
- DAC: single 10-bit channel on **D0 / PA02**, AVCC reference (0–3.3 V).
- ADC: 12-bit, INTVCC1 ref + GAIN=DIV2 → full-scale 0–3.3 V. Channel arg is
  the `AIN[]` index, not the pin number.
- DAC waveform: TC3 timer-IRQ driven, 64-step phase, sine LUT; 50–500 Hz.
- PWM: TCC0/WO0 on **D1 / PA04**. `period = 48 MHz / freq − 1`. 25 kHz gives
  ~11-bit resolution (1920 levels). 16-bit PWM caps at ~732 Hz — the
  resolution × frequency product is bounded by the 48 MHz clock.
- Counter: EIC EXTINT[10] on **D2 / PA10** → EVSYS → TC4 (32-bit event count).

**Pin map (Xiao SAMD21 silkscreen → chip pin):**
```
D0=PA02  D1=PA04  D2=PA10  D3=PA11  D4=PA08  D5=PA09
D6=PB08  D7=PB09  D8=PA07  D9=PA05  D10=PA06   LED=PA17
```

---

## Build

Toolchain: `arm-none-eabi-gcc` (tested 8.3.1). Built on the Raspberry Pi
dev host (`ssh robot`); WSL lacks the cross toolchain.

```sh
cd samd21/apps/register_dongle
make            # → build/register_dongle.uf2
make clean
```

The DSL chain ROM (`register_dongle_v2_module_rom.c`) is pre-generated. To
regenerate after editing `register_dongle_v2.lua`:

```sh
cd s_engine/lua_dsl
luajit s_compile.lua ../dsl_tests/register_dongle_v2/register_dongle_v2.lua \
  --helpers=s_engine_helpers.lua \
  --emit-c=register_dongle_v2_module_rom.c \
  --outdir=../dsl_tests/register_dongle_v2/
# then copy register_dongle_v2{_module_rom.c,.h,_records.h} into this dir
```

Resource footprint (current build): ~37.8 KB flash (15%), ~5.5 KB RAM (17%).

---

## Flash (UF2 bootloader)

1. Double-short the Xiao's reset pads → bootloader mode. USB ID flips
   `2886:802f` → `2886:002f`; a mass-storage volume appears.
2. Mount it (the `/dev/sd?` letter drifts on every re-enumeration):
   ```sh
   sudo umount /mnt/xiao 2>/dev/null
   for d in /dev/sdb /dev/sdc /dev/sdd; do
     sudo mount -o uid=pi,gid=pi $d /mnt/xiao 2>/dev/null && break
   done
   ```
3. `cp build/register_dongle.uf2 /mnt/xiao/ && sync`
4. The bootloader flashes and reboots into the app (`2886:802f`).

Commissioning data lives in the top 2 flash rows (0x3FE00/0x3FF00) and
**survives a firmware reflash** — only the application region is overwritten.

---

## Host tools

| Tool | Purpose |
|---|---|
| `linux/dongle_console/dongle_console.lua` | wire observer + command sender (debug) |
| `linux/usb_commission/commission.lua` | standalone L0 commissioning tool (production) |

Commissioning a fresh dongle:
```sh
luajit commission.lua --set 42      # assign instance_id, reboots
luajit commission.lua --status      # verify
```

Walk the sync ladder + run a command:
```sh
luajit dongle_console.lua --frame --sync --send-shell-sysinfo
luajit dongle_console.lua --frame --sync --gpio-loopback-d2-d3
luajit dongle_console.lua --frame --sync --pwm-counter-test 1000
```

`dongle_console.lua --help` lists all `--send-shell-*` flags.

---

## Bench notes / gotchas

- **USB power contention.** A bus-powered SSD (or other hungry peripheral)
  sharing the Pi's USB hub can brown out a small MCU dongle — symptom is
  intermittent USB resets (enumerate → partial re-attach → repeat) and a
  phantom `/dev/sd?` node. **Remove power-hungry peripherals from the
  dongle's USB tree.** This caused significant flakiness during 2026-05-20
  bring-up until the SSD was unplugged.
- **UF2 bootloader I/O errors.** If the first `cp` to `/mnt/xiao` fails with
  "No space left", the FAT goes read-only. Recovery: unmount, loop through
  `/dev/sd[bcd]` (enumeration drifts), remount, retry.
- **`/dev/sd?` letter drifts** on every bootloader entry — always loop the
  mount across candidates, never hardcode.
- **Probe identification.** Xiao pads are small and adjacent; when an
  empirical pin reading contradicts the documented map, re-probe two
  candidate pads before concluding a board mismatch.
- **Chain tick-batching.** The dongle processes shell events in 250 ms tick
  batches, so host-side inter-command delays only translate to precise
  on-dongle timing when ≥ ~one tick. Use `--delay-ms ≥ 500` for any test
  where window timing matters.

---

## Source layout

```
register_dongle/
├── main.c                      USB-CDC loop, RX/TX, engine tick, reboot plumbing
├── user_functions.c            chain user fns (register, heartbeat, shell dispatch, ...)
├── shell_commands.{c,h}        general-layer shell: cursors, dispatch table, echo, sysinfo
├── samd21_commands.c           SAMD21-specific shell commands (GPIO/DAC/ADC/PWM/counter)
├── flash_storage.{c,h}         NVMCTRL dual-slot commissioning persistence
├── register_dongle_v2*.{c,h}   DSL-generated chain ROM (do not hand-edit)
├── usb_descriptors.c           USB-CDC descriptors (VID 0x2886 / PID 0x802F)
├── tusb_config.h               TinyUSB config
├── Makefile
└── vendor/libcomm/             vendored libcomm framing slice + opcodes.h
```
