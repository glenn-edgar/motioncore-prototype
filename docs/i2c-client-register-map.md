# I2C client — register map (draft)

Status: **draft / proposed** (2026-05-29). A standalone reduced firmware build
(`ROLE=i2c_client`) that turns a SAMD21 / RA4M1 into a commodity I2C target:
GPIO + ADC + DAC + PWM + one local safety interlock, behind a fixed 7-bit
address. Identity is the **mux channel** (TCA9548A-style), so every unit ships
the *same* address and identical firmware — no commissioning.

This register map **is the contract**: any I2C master (Arduino `Wire`, RPi
`smbus`, a PLC, another vendor's MCU) drives it with plain register reads/writes
and needs no knowledge of libcomm / s_engine / the DSL. Treat it like an
off-the-shelf chip datasheet.

**Per-chip maps.** SAMD21 and RA4M1 expose *different* capabilities, so each has
its own map. They share one **address layout** and feature blocks live at the
same offsets on both — a block is simply absent (reserved, `CAPABILITIES` bit 0)
on a chip that lacks it. So master code stays portable; it reads `CAPABILITIES`
and `VERSION` to know what's present. The table below is the **SAMD21** map; the
RA4M1 deltas are at the end.

## Conventions

- **Address**: fixed 7-bit, compile-time default `0x28` (mux channel = unit identity).
- **Access**: write the 1-byte register pointer, then read or write data. The
  pointer **auto-increments** across multi-byte reads/writes (block transfers OK).
- **Endianness**: multi-byte values are **little-endian** (matches the MCU + the
  rest of the stack). Documented, not assumed.
- **Reserved/unused bits** read 0; write 0. Reserved registers read 0.
- **Clock stretching**: the client stretches SCL only when a write needs work
  (DAC update, `SAVE_CFG`, re-arming the interlock). Plain reads never stretch —
  ADC results free-run into their registers.
- **Config persistence**: writing `CONTROL.SAVE_CFG=1` snapshots the writable
  config (GPIO dir/pull, DAC, PWM, the interlock block) to flash; it reloads on
  boot and after a WDT recovery, so the interlock re-arms itself unattended.

## Register map

| Addr | Name | Acc | Width | Notes |
|---|---|---|---|---|
| **System** ||||
| 0x00 | `WHO_AM_I`     | ro | u8  | fixed device id `0xC1` |
| 0x01 | `VERSION`      | ro | u8  | fw rev: hi nibble major, lo nibble minor |
| 0x02 | `CAPABILITIES` | ro | u8  | b0 GPIO, b1 ADC, b2 DAC, b3 PWM, b4 INTERLOCK. **SAMD21 = 0x17 (no PWM)** |
| 0x03 | `STATUS`       | ro | u8  | b0 WDT_RESET (since boot), b1 IL_TRIPPED (mirror of IL_STATUS latch), b2 CFG_DIRTY, b3 FAULT |
| 0x04 | `CONTROL`      | rw | u8  | b0 SOFT_RESET, b1 SAVE_CFG (self-clear), b2 LOAD_DEFAULTS |
| 0x05–0x0F | reserved  | -  | -   | |
| **GPIO (8-bit port)** ||||
| 0x10 | `GPIO_DIR`  | rw | u8 | 1 = output, 0 = input (per pin) |
| 0x11 | `GPIO_OUT`  | rw | u8 | output latch (effective on output pins) |
| 0x12 | `GPIO_IN`   | ro | u8 | live pin levels |
| 0x13 | `GPIO_PULL` | rw | u8 | pull-up enable on input pins |
| 0x14–0x1F | reserved | - | - | |
| **ADC (free-running, 12-bit right-justified)** ||||
| 0x20 | `ADC_CH0` | ro | u16 | last conversion, channel 0 |
| 0x22 | `ADC_CH1` | ro | u16 | channel 1 |
| 0x24 | `ADC_CH2` | ro | u16 | channel 2 |
| 0x26 | `ADC_CH3` | ro | u16 | channel 3 |
| 0x28 | `ADC_ENABLE` | rw | u8 | bitmask: which channels the loop samples (round-robin) |
| 0x29 | `ADC_OVERSAMPLE` | rw | u8 | 0..4 → 2^N hardware averages |
| 0x2A–0x2F | reserved | - | - | |
| **DAC (10-bit)** ||||
| 0x30 | `DAC_VALUE` | rw | u16 | 0..1023; write updates output (brief stretch) |
| 0x32 | `DAC_CTRL`  | rw | u8  | b0 ENABLE |
| 0x33–0x37 | reserved | - | - | |
| **PWM** ||||
| 0x38–0x3F | reserved | - | - | **No PWM on SAMD21** (`CAPABILITIES` b3 = 0). Range held for the RA4M1 PWM block so the layouts align. |
| **Interlock (single — analog OR GPIO, mutually exclusive)** ||||
| 0x40 | `IL_CTRL`     | rw | u8  | b0 ARM, b1 CLEAR_LATCH (self-clear) |
| 0x41 | `IL_SOURCE`   | rw | u8  | b7 TYPE (0 = GPIO, 1 = ANALOG); b3..0 = pin (GPIO 0–7) or ADC channel (0–3) |
| 0x42 | `IL_COMPARE`  | rw | u8  | op: 0 LT, 1 GT, 2 EQ, 3 NE — the **pass** (safe) condition |
| 0x43 | `IL_THRESH`   | rw | u16 | ANALOG: 12-bit level. GPIO: 0/1 in the low bit |
| 0x45 | `IL_OUT_PIN`  | rw | u8  | GPIO pin driven on trip (0xFF = none, status-only) |
| 0x46 | `IL_OUT_SAFE` | rw | u8  | level driven onto IL_OUT_PIN while tripped (0/1) |
| 0x47 | `IL_STATUS`   | ro | u8  | b0 ARMED, b1 LIVE_PASS, b2 LATCHED_TRIP, b3 OUT_DRIVEN |
| 0x48–0x4F | reserved  | -  | -   | |

The SAMD21 interlock watches exactly **one** source — either an analog (ADC)
channel **or** a GPIO pin, chosen by `IL_SOURCE.TYPE`. Analog compares the 12-bit
reading against `IL_THRESH`; GPIO compares the pin level against `IL_THRESH` bit0.
Not both at once (it is a single slot).

## Interlock semantics

- Evaluated by a **hardware-timer ISR (default 1 kHz / 1 ms)**, NOT a slow
  software tick — there is no s_engine in this build, so the 250 ms chain cadence
  does not apply. Worst-case response = one timer period (bounded, deterministic;
  unaffected by I2C/clock-stretch jitter). Optional µs-class fast path: ADC
  **window-comparator** interrupt (analog) / EIC edge interrupt (GPIO), with
  EVSYS able to drive the safe-output pin CPU-independently.
- **The eval rate is fixed-fast, not a writable register** — a master-settable
  period could be set slow and defeat the safety. (Settable only within a bounded
  fast range, if at all.)
- **Enforcement is local and immediate**: when the compare is *not* satisfied
  (unsafe), the interlock drives `IL_OUT_PIN` to `IL_OUT_SAFE` that tick,
  independent of whether the master ever polls.
- **`LIVE_PASS`** = the safe condition currently holds. **`LATCHED_TRIP`** =
  sticky; set on any pass→fail transition, stays set until `IL_CTRL.CLEAR_LATCH`.
  This is the polled-status guarantee: a momentary trip that self-clears between
  two master reads is preserved in the latch so the master can't miss it.
- `ARM` gates evaluation. On boot / WDT recovery the saved config reloads and the
  interlock re-arms itself if `ARM` was set when `SAVE_CFG` ran.
- Master workflow: set `IL_SOURCE`/`IL_COMPARE`/`IL_THRESH`/`IL_OUT_PIN`/
  `IL_OUT_SAFE` → `IL_CTRL.ARM=1` → optionally `CONTROL.SAVE_CFG=1`. Then poll
  `IL_STATUS` whenever; `CLEAR_LATCH` after handling a trip.

## Per-chip maps

Shared address layout; each chip differs in which blocks exist + value widths,
reflected in `CAPABILITIES` and `VERSION`. Physical pin → channel binding lives
in each chip's section.

### SAMD21 (XIAO) — `CAPABILITIES = 0x17`
- **No PWM** (0x38 block reserved).
- 12-bit ADC (right-justified in u16), 10-bit DAC (0..1023).
- Two pins consumed by the I2C-slave SERCOM; the rest map to the 8-bit GPIO port
  + ADC channels + the DAC pin.
- Interlock: **one source, analog OR GPIO** (`IL_SOURCE.TYPE`).
- This is the authoritative SAMD21 map above.

### RA4M1 (XIAO) — separate map (TBD)
- **Adds PWM** at the reserved 0x38 block (GPT).
- 14-bit ADC (right-justified in u16), 12-bit DAC.
- `CAPABILITIES = 0x1F`. Channel counts per the RA4M1 pin budget.
- Same interlock model. Full RA4M1 table to be drafted when that build starts.

## Open / to confirm

- Address default `0x28` and `WHO_AM_I` magic `0xC1` are placeholders.
- GPIO width fixed at 8 (one port); widen to 16 (two ports `_A`/`_B`) if a unit
  needs it — would add a mirrored 0x10-block.
- Whether ADC is purely free-running (current draft) or also offers a
  convert-on-read trigger for low-power units.
- SAMD21 ADC channel count (draft shows 4 — confirm against the free pins left
  after the I2C slave SERCOM + DAC pin are reserved).
