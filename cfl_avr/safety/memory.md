# memory.md — AVR32SD32 (AVR® SD) Curiosity Nano bring-up

Working notes for bringing up the **EV75S16A AVR32SD32 Curiosity Nano** with a
bare-metal **avr-gcc** toolchain on a **3.3 V** target rail. Sources: AVR SD
data sheet **DS40002629** (read directly), Curiosity Nano User Guide
**DS50003842**, and board schematic **SCH 02-01254** (rev A1). XC8 / MPLAB are
not used anywhere here — this is the open-toolchain path.

Goal of the project: use the SD hardware safety blocks (lockstep, ECC, error
controller, monitors) as **reliability hardening for non-certified** use. We are
NOT chasing ISO 26262 / IEC 61508 certification, so the gated FMEDA / Safety
Manual are reference-only and not on the critical path. Everything load-bearing
is in the public data sheet (hardware) or in public standards (IEC 60730 Annex H
Class B diagnostics).

---

## 0. TL;DR for the next session

- 20 MHz runs at 3.3 V with **no derating** (flat AVR-Dx-style speed/voltage).
- After reset the clock is **OSCHF @ 4 MHz**; bump to 20 MHz via CCP-protected
  `CLKCTRL.OSCHFCTRLA`.
- **LED0 = PD2** (active-LOW, current sink), **SW0 = PF2** (needs internal pull-up).
- Program/debug over the on-board **nEDBG using UPDI**; no external programmer.
  Flash with `pymcuprog` or `avrdude -c jtag3updi`. Set Vtarget = 3.3 V.
- **ECC forces 16-bit Flash writes** — the programmer must honor this.
- `ERRCTRL` is **always on and cannot be disabled**; it is the central fault hub.
- The §6 startup code already blinks + arms the supply VLM. Confirm the blink on
  hardware before moving to ECC / ERRCTRL.

---

## 1. Board facts (Curiosity Nano EV75S16A)

Mounted MCU: **AVR32SD32** (32-pin, rev A1 production silicon).

On-board **nEDBG** debugger — no external tools needed:
- **UPDI** program + debug (single-wire) on debugger pin **DBG0**.
- **USB-C**, mass-storage **drag-and-drop** programming, **CDC** virtual serial,
  and DGI (Data Gateway Interface).
- **Adjustable target regulator 2.7–5.1 V** (set this to 3.3 V for our work).
- **VDDIO2 / MVIO** level-shifter rail fed from the debugger regulator at 3.3 V.
- USB-powered; `VOFF` / `VBUS` / `VTG` brought to the edge.

Debugger ↔ target connections (UG Table 3-2) — useful so we don't accidentally
reuse these pins:

| Debugger line | AVR32SD32 pin | Note |
|---|---|---|
| DBG0 | UPDI | program/debug |
| DBG1 | PF5 | also drives the green **heartbeat LED** (debugger side) |
| DBG2 | PF2 | shared with **SW0** sense |
| DBG3 | RESET | |
| CDC TX → | PD5 | target **USART0 RX** (serial console in) |
| CDC RX ← | PD4 | target **USART0 TX** (serial console out) |

On-board user peripherals (from schematic SCH 02-01254):
- **User LED0 = PD2** — wired **active-LOW / current-sink**: drive **LOW = ON**,
  HIGH = off.
- **User switch SW0 = PF2** — active-low, **no external pull-up**, enable the
  internal pull-up (`PORTF.PIN2CTRL = PORT_PULLUPEN_bm`). Shared with debugger DBG2.
- Green power/status LED + heartbeat LED are debugger-driven (PF5), not LED0.

Crystals / alt clock sources on the board:
- 20 MHz HF crystal footprint on **PA0/PA1** (XTALHF1/2) — optional XOSCHF.
- 32.768 kHz crystal on **PF0/PF1** (XTAL32K) — for RTC / OSCHF auto-tune.

Serial console for debug: target **USART0 on PD4 (TX) / PD5 (RX)** is wired to
the debugger CDC port → shows up as a host COM port. Good first instrument.

> Pin confidence: LED0=PD2 / SW0=PF2 are from the board schematic and are
> consistent with the UG Table 3-2 debugger map. If anything misbehaves, glance
> at UG **DS50003842 Figure 1-3** to re-confirm.

---

## 2. Data sheet essentials (DS40002629)

### 2.1 Part / memory
8-bit AVR, **dual-core lockstep**, 20 MHz max. 32 KB Flash (ECC), 4 KB SRAM
(ECC), 256 B EEPROM (ECC). UPDI programming. SECDED ECC on all memories.

### 2.2 Supply & frequency — the 3.3 V verdict
- Operating: **VDD 2.7–5.5 V** (flat, Table 46-2). VDDIO2/MVIO **1.71–5.5 V**.
- **fCLK_MAIN max = 20 MHz** (Table 46-9), constrained *only* by the flat
  operating-voltage window — **no frequency-vs-VDD derating table**. The whole
  data sheet is characterized at **VDD = 3.0 V with OSCHF = 20 MHz**.
- **=> Full 20 MHz is in spec at 3.3 V.** No need to slow the clock for 3.3 V.

### 2.3 Clock controller (CLKCTRL, ch.18) — all writes are CCP-protected
- After **any reset**: `CLK_MAIN = OSCHF @ 4 MHz` (the default FRQSEL = 0x3).
- `CLKCTRL.OSCHFCTRLA.FRQSEL[3:0]` selects OSCHF output frequency → set to 20 MHz.
- `CLKCTRL.MCLKCTRLB` — main prescaler: `PEN` enables, `PDIV` divides /1…/64.
  Leave `PEN = 0` so `CLK_PER = CLK_MAIN = 20 MHz`.
- `CLKCTRL.MCLKCTRLA.CLKSEL` — main source; already OSCHF after reset.
- `CLKCTRL.MCLKSTATUS.OSCHFS` — set when OSCHF is stable; spin on it.
- Use avr-libc **`_PROTECTED_WRITE(reg, val)`** for every CLKCTRL write (emits the
  Configuration Change Protection key).

### 2.4 Supply monitoring — BOD + VLM (ch.27)
Two layers:
- **Brown-out RESET floor** = `FUSE.BODCFG` (LEVEL + ACTIVE). **Fuse-locked**:
  `BOD.CTRLA.ACTIVE` is loaded from the fuse and can't be raised at runtime.
  Levels seen: `0x0 = 1.9 V`, `0x1 = 2.45 V`, `0x2 = 2.70 V` (more above). For a
  3.3 V rail set **BODLEVEL2 = 2.70 V** as the reset floor.
- **Voltage Level Monitor (VLM)** = runtime early-warning, sits a % *above* the
  BOD floor:
  - `BOD.VLMCTRLA.VLMLVL[1:0]` → ~5 % / 15 % / 25 % above VBOD.
  - `BOD.INTCTRL` → `VLMCFG[1:0]` (direction: below / above / both) + `VLMIE`.
  - `BOD.INTFLAGS.VLMIF` (write-1-clear); `BOD.STATUS.VLMS` (live); ISR `BOD_VLM_vect`.
  - For 3.3 V: **VLMLVL = 15 %** → warns at ~3.11 V (margin under nominal, above
    the 2.70 V floor). 25 % (~3.38 V) sits above 3.3 V → would trip constantly.

### 2.5 Functional Safety Concept (ch.1) — Core-Independent Safety (CIS)
All of these are **autonomous hardware**; most report into the **Error Controller**.
Target fault-detection-time down to **1 ms**.

- **Error Controller (ERRCTRL, ch.22)** — *always enabled, cannot be disabled*.
  Central front-end: every module's HW error maps to an **Error Channel**; a fault
  sets that channel's **Error Status Flag (ESF)**; the channel's **Error Source
  Control `ESCx.ERRLVL`** sets the **severity**, which decides the effect:

  | ERRLVL severity | Effect |
  |---|---|
  | CRITICAL | autonomous **safe state** via ERRCTRL reset (`STATUSA.ECRF` set) |
  | NONCRITICAL | **NMI / interrupt** request |
  | IOSAFE / FLOAT | **float I/O** to safe state |
  | (CPU lockstep mismatch) | **machine-check reset** (not user-routable) |

  Config flow: `CTRLA.STATE = CONFIG` to write `ESCx`, then `STATE = NORMAL` to
  arm; optional timeout via `TIMECNT`. Validate with **error injection**
  (`ESFTEST`). This is the next major bring-up step.

- **Dual-core lockstep (DCLS)** — two identical CPUs run the same program; a DCLS
  comparator continuously checks them. **Any mismatch → machine-check reset.**
  Plus built-in illegal-opcode and illegal-address detection. **Transparent to
  software** — you write a single program image, no "lockstep code."
- **ECC** — SECDED on Flash/EEPROM/SRAM with **redundant checkers** (latent-fault
  detection). Any ECC error → ERRCTRL. **Constraint: Flash must be written in
  16-bit words** (programmer implication — see §5).
- **Data bus parity** (redundant control signals).
- **Fuse CRC + ECC** protection of config/calibration fuses.
- **Clock frequency monitor + failure detection** (independent clock) → ERRCTRL.
- **Voltage Regulator Monitor (VMON)** — over/under-voltage on the *regulated
  internal domain* → ERRCTRL. (Distinct from BOD/VLM, which watch external VDD.)
- **Dual watchdogs**: **WDT** (async, independent clock, **direct reset**) +
  **SWDT** (sync, main-clock, counts cycles/instructions, → ERRCTRL).
- **Stack monitor** (overflow/underflow) → ERRCTRL.
- **Peripheral redundancy** (duplicated instances), incl. separate ADC vrefs.
- **Integrity**: fault injection, sleep-entry protection, WDT clock-failure
  detect, OCD/DFT-disabled monitors.

### 2.6 Chapter map (PDF ≈ printed page; offset drifts 0…+2, grep by title)

| Ch | Title | ~Page |
|---|---|---|
| 1 | Functional Safety Concept | 14 |
| 10 | AVR CPU | 38 |
| 11 | BUSMATRIX | 55 |
| 12 | Memories | 60 |
| 16 | NVMCTRL (Flash + ECC) | 96 |
| 17 | RAMCTRL (SRAM ECC) | 126 |
| 18 | CLKCTRL | 136 |
| 20 | RSTCTRL (reset flags) | 186 |
| 21 | CPUINT (interrupt ctrl) | 201 |
| 22 | ERRCTRL | 213 |
| 26 | MVIO (multi-voltage I/O) | 289 |
| 27 | BOD / VLM | 297 |
| 29 / 30 | WDT / SWDT | 312 / 324 |
| 38 | CRCSCAN | 569 |
| 46 | Electrical Characteristics | 705 |

---

## 3. Toolchain

- Compiler: **avr-gcc** with `-mmcu=avr32sd32` (a mainline target in recent
  avr-gcc; otherwise supplied by the atpack).
- Device headers/startup: **Microchip AVR-Sx_DFP** atpack via
  `-B $PACK/gcc/dev/avr32sd32 -I $PACK/include`.
- CCP register writes: avr-libc `_PROTECTED_WRITE()`.
- `_gc` enum macro spellings (e.g. `CLKCTRL_FRQSEL_20M_gc`, `BOD_VLMLVL_15ABOVE_gc`)
  come from `iom_avr32sd32.h` in the atpack. The register/field *names* in this
  doc are from the data sheet and are correct; if a `_gc` token doesn't compile,
  grep the header for the field (FRQSEL, VLMLVL, …) and substitute.

### Build
```sh
PACK=/path/to/Microchip.AVR-Sx_DFP.<ver>
avr-gcc -mmcu=avr32sd32 -O2 -DF_CPU=20000000UL \
    -B "$PACK/gcc/dev/avr32sd32" -I "$PACK/include" \
    avr32sd_3v3_bringup.c -o bringup.elf
avr-objcopy -O ihex bringup.elf bringup.hex
avr-size bringup.elf
```

---

## 4. Fuses (set once at programming time)

The brown-out RESET behavior lives in `FUSE.BODCFG` and is **not** runtime-raisable
(see §2.4). For a 3.3 V rail:
- `BODCFG.LEVEL  = BODLEVEL2` (2.70 V reset floor)
- `BODCFG.ACTIVE = enabled` (continuous, or SAMPLED to save power)

Everything else (the VLM warning) is configured in software at runtime.

---

## 5. Flashing over the on-board nEDBG (no MPLAB)

The nEDBG presents UPDI to open tools. Use whichever your environment has:
```sh
# Microchip's open Python tool (needs a build that knows avr32sd32):
pymcuprog write -d avr32sd32 -t uart -u <serial-port> -f bringup.hex --erase --verify

# or avrdude with the on-board nano debugger over UPDI:
avrdude -c jtag3updi -p avr32sd32 -U flash:w:bringup.hex:i
```
Notes:
- **ECC ⇒ 16-bit Flash writes.** The data sheet (FuSa §1.3) requires any
  program/debug interface to write Flash in 16-bit words. A current pymcuprog /
  avrdude that knows the part handles this; an out-of-date tool may corrupt ECC —
  update the tool or point it at the atpack device file if `avr32sd32` is unknown.
- **Target voltage** is set by the debugger, not a fuse. Set Vtarget = **3.3 V**
  (MPLAB project property, or your pymcuprog/pyedbglib build's voltage option) and
  **confirm it** before trusting the VLM math (the 15 % warning is computed
  relative to a 3.3 V rail).
- Serial console: open the CDC COM port (target USART0 on PD4/PD5) for printf-style
  debug once USART0 is configured.

---

## 6. Startup code — `avr32sd_3v3_bringup.c`

3.3 V first-boot: 20 MHz clock + supply VLM early-warning + LED0 heartbeat.
LED0/SW0 pins filled in from the board schematic (PD2 / PF2). This is step 1; ECC
+ ERRCTRL are the next layers (see §7).

```c
/*
 * avr32sd_3v3_bringup.c
 * ---------------------------------------------------------------------------
 * AVR32SD32 first-boot bring-up for the EV75S16A Curiosity Nano on a 3.3 V rail.
 * Toolchain: avr-gcc + avr-libc + AVR-Sx_DFP atpack.
 *
 * Does (all grounded in DS40002629):
 *   1. CLK_MAIN -> 20 MHz from internal OSCHF.
 *      Table 46-9: fCLK_MAIN max = 20 MHz, gated only by the flat 2.7-5.5 V
 *      operating window -> full 20 MHz is in spec at 3.3 V, no derating.
 *   2. Supply Voltage Level Monitor (VLM) as an early under-voltage warning
 *      sized for a 3.3 V rail (~15% above the 2.70 V BOD floor => ~3.11 V).
 *   3. LED0 heartbeat; LED0 latches solid ON if the VLM trips.
 *
 * Board pins (schematic SCH 02-01254):
 *   LED0 = PD2, active-LOW (current sink): drive LOW = on.
 *   SW0  = PF2, active-low, needs internal pull-up (not used in this file).
 *
 * Build / fuse / flash: see memory.md sections 3-5.
 *
 * _gc macro spellings come from iom_avr32sd32.h; register/field NAMES are from
 * the data sheet and are correct. If a *_gc token below doesn't compile, grep
 * the atpack header for the field and substitute.
 * ---------------------------------------------------------------------------
 */

#define F_CPU 20000000UL   /* CLK_PER after clock_20mhz(): OSCHF 20 MHz, no prescale */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <stdint.h>

/* ---- Board pins (EV75S16A, schematic SCH 02-01254) ---- */
#define LED0_VPORT   VPORTD     /* LED0 is on PORT D ... */
#define LED0_BIT     2          /* ... pin PD2, active-LOW (drive LOW = on)   */
/* SW0 = PF2 (active-low, enable internal pull-up) -- not used here yet. */

static volatile uint8_t supply_fault = 0;

/* Switch CLK_MAIN to 20 MHz from OSCHF. CLKCTRL regs are CCP-protected, so
 * every write goes through _PROTECTED_WRITE (emits the CCP key). */
static void clock_20mhz(void)
{
    /* After reset CLK_MAIN = OSCHF @ 4 MHz (datasheet 18.3.2).
     * Select 20 MHz; keep the main prescaler disabled so CLK_PER = 20 MHz. */
    _PROTECTED_WRITE(CLKCTRL.OSCHFCTRLA, CLKCTRL_FRQSEL_20M_gc);
    _PROTECTED_WRITE(CLKCTRL.MCLKCTRLB, 0x00);          /* PEN = 0 -> divide by 1 */

    /* CLKSEL already = OSCHF out of reset; just wait for OSCHF stable. */
    while (!(CLKCTRL.MCLKSTATUS & CLKCTRL_OSCHFS_bm)) { /* spin */ }
}

/* Voltage Level Monitor: early under-voltage warning ABOVE the fuse-set BOD
 * reset floor (BODLEVEL2 = 2.70 V). VLMLVL picks how far above:
 *    5% -> ~2.84 V   15% -> ~3.11 V   25% -> ~3.38 V
 * On a 3.3 V rail 15% (~3.11 V) warns on a sagging rail without false-tripping
 * at nominal. 25% sits above 3.3 V -> do NOT use it here. */
static void supply_vlm_init(void)
{
    BOD.VLMCTRLA = BOD_VLMLVL_15ABOVE_gc;     /* verify _gc spelling in iom hdr */
    BOD.INTCTRL  = BOD_VLMCFG_BELOW_gc        /* IRQ when VDD crosses DOWN thru it */
                 | BOD_VLMIE_bm;
    sei();
}

/* VDD sagged below ~3.11 V. In a real node this is where you'd notify the
 * Error Controller / drive outputs to a safe state. Bring-up: latch a flag. */
ISR(BOD_VLM_vect)
{
    BOD.INTFLAGS = BOD_VLMIF_bm;   /* write 1 to clear */
    supply_fault = 1;
}

int main(void)
{
    clock_20mhz();

    /* LED0 output, start OFF (active-low -> drive HIGH = off). */
    LED0_VPORT.DIR |= (uint8_t)(1u << LED0_BIT);
    LED0_VPORT.OUT |= (uint8_t)(1u << LED0_BIT);

    supply_vlm_init();

    for (;;) {
        if (supply_fault) {
            LED0_VPORT.OUT &= (uint8_t)~(1u << LED0_BIT);   /* solid ON = warning latched */
        } else {
            /* Writing 1 to VPORTx.IN toggles the matching OUT bit on modern AVR
             * -- single-instruction toggle, no read-modify-write. */
            LED0_VPORT.IN = (uint8_t)(1u << LED0_BIT);       /* heartbeat */
            _delay_ms(250);
        }
    }
}
```

---

## 7. Next steps (roadmap for the Claude Code window)

1. **[DONE] 3.3 V clock + VLM + blink** — §6. Confirm heartbeat on hardware first.
2. **ECC bring-up** (NVMCTRL ch.16 / RAMCTRL ch.17): confirm ECC is active, read
   ECC status/error registers, optionally inject a correctable/uncorrectable
   error and observe the ERRCTRL signal. Remember the 16-bit Flash write rule.
3. **ERRCTRL bring-up** (ch.22): enter `CTRLA.STATE = CONFIG`, set per-channel
   `ESCx.ERRLVL` severities, arm with `STATE = NORMAL`. Add an **NMI handler** for
   NONCRITICAL faults; verify CRITICAL → autonomous reset; use `ESFTEST` error
   injection to validate each channel. Read `RSTCTRL` reset flags + `STATUSA.ECRF`
   on boot to detect what reset us (machine-check vs ERRCTRL vs BOD vs WDT).
4. **Class B software diagnostics** on a TCB tick: CRCSCAN flash check, RAM march
   (C-/X), CPU register test, stack-monitor check. Implement straight from
   **IEC 60730 Annex H** — no gated Microchip library needed for non-certified use.
   Set the run cadence by engineering judgment (the gated Safety Manual's only
   unique content is the certified FDTI scheduling, which we don't need).
5. **Watchdogs**: WDT (async, direct reset) + SWDT (sync, windowed) → ERRCTRL.
6. **Lockstep**: automatic; nothing to enable. Just document the machine-check
   reset path and detect it via the reset flags on boot.

## 8. Open items / cautions
- `_gc` macro spellings unverified against the atpack header (FRQSEL 20M, VLMLVL
  15%, VLMCFG below) — confirm on first compile.
- Confirm `pymcuprog` / `avrdude` build recognizes `avr32sd32` and honors 16-bit
  Flash writes (ECC).
- Confirm Vtarget = 3.3 V at the board before trusting VLM thresholds.
- Pin map (LED0=PD2, SW0=PF2) from schematic; re-check UG Fig 1-3 if GPIO misbehaves.
- The SD-specific FMEDA / Safety Manual are **request-only via Microchip sales**,
  not public downloads. Not needed for this non-certified effort.


