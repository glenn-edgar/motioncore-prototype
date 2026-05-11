# CWC Hardware Notes — Pull Resistors

**Purpose:** specify the resistor pulls required on the CWC carrier board to guarantee a safe motor state during any condition where the RP2350 GPIOs are high-Z — power-up before firmware runs, MCU reset (RUN pin), hardware watchdog timeout, brown-out, or transition between resets and re-init.

The firmware-level defense (`cwc_emergency_shutdown` hook called from `reboot_with_cause()`) covers software-triggered reboots only. For hardware-triggered resets, the pulls below are the **only** thing keeping the H-bridge in a safe state.

---

## Critical (safety — required)

Without these, the H-bridge state during reset transitions is undefined. **Do not build the board without them.**

| GP# | Net | Pull | Value | Reason |
|---|---|---|---|---|
| GP11 | DRV8838 ENABLE | **down** | 10 kΩ | ENABLE=0 → brake state (both low-side FETs on, motor shorted). Floating ENABLE during reset could cross the DRV8838 logic threshold via leakage and drive the bridge. |
| GP12 | DRV8838 nSLEEP | **down** | 10 kΩ | nSLEEP=0 → coast (all FETs off, outputs Hi-Z). Overrides any ENABLE state inside the DRV8838. Belt-and-suspenders pairing with the GP11 pull-down: either resistor alone keeps the bridge safe. |
| GP10 | DRV8838 PHASE | **down** | 10 kΩ | PHASE direction is irrelevant when ENABLE=0, but a floating CMOS input wastes static power and is an EMI vulnerability. Pull-down (or pull-up — choice is arbitrary) gives it a defined level. |

Value rationale: 10 kΩ at 3.3 V is 330 µA sink current when the line is driven high — negligible vs. the DRV8838's >1 MΩ input impedance, and tight enough for solid noise immunity at the bridge input. Higher values (47 kΩ, 100 kΩ) save power but trade noise margin. Don't go above 47 kΩ on these three.

## Recommended (determinism / hygiene)

Not safety-critical but worth specifying so behavior is predictable and disconnect faults are detectable.

| GP# | Net | Pull | Value | Reason |
|---|---|---|---|---|
| GP14, GP15 | Encoder A, B | up | Pico **internal** (~50–80 kΩ) | If the encoder cable disconnects, the line floats and PIO would see noise as counts. Internal pull-up makes "no signal" the dominant value; the encoder-stall watchdog (§4.3) catches it cleanly. External resistor not needed unless the encoder is open-drain — confirm encoder output stage before omitting. |
| GP22 | Fault LED | matches LED polarity | 10 kΩ | LED off at reset (active-high LED → pull-down; active-low → pull-up). Cosmetic only. |
| GP28 | ADC2 (spare / temperature) | down | 100 kΩ | Prevents a floating ADC pin from producing garbage readings if the pad is unpopulated. Omit if a thermistor or temperature sensor is fitted — follow the sensor datasheet instead. |

## Not required

These pins are always actively driven or always at a defined voltage when the board is powered.

| GP# | Net | Reason |
|---|---|---|
| GP26 (ADC0) | INA240A4 V_OUT is push-pull; output is defined whenever V_S is powered. |
| GP27 (ADC1) | V_M divider is resistors only; always at a defined fraction of V_M. |
| (CAN / RS-485 pins) | Owned by slave infrastructure. Pull discipline lives in the slave-infra hardware notes, not this document. |

---

## Verification on bench

Run before any motor power-up on a freshly assembled board.

1. **MCU held in reset.** Ground the RUN pin via a jumper. Measure DC voltage on DRV8838 ENABLE, nSLEEP, PHASE pins. **All three must read < 0.5 V.** If any reads > 0.5 V, the corresponding pull is missing or wrong.
2. **BOOTSEL mode.** Press BOOTSEL while power-cycling. The Pico stalls in its boot ROM; firmware does not run. Repeat the DC measurement. All three pins must still be < 0.5 V — the SDK's default GPIO state at boot must not lift the bridge inputs.
3. **Power-up with motor connected.** Watch the motor output shaft during the 0 → 3.3 V rail rise on the MCU. There must be zero twitch — no inrush, no kick, no hum. If any motion: a pull is missing, or the pull value is too high to override leakage during the rail-rise transient.
4. **Software-triggered reboot.** From a running firmware, call `reboot_with_cause(REBOOT_CAUSE_USER_REQUEST, 0, 0, 0)`. Scope the ENABLE pin during the reboot. It must drop to 0 V (driven by `cwc_emergency_shutdown`) *before* the reset completes, and remain at 0 V (held by the pull-down) across the reset transition until firmware re-initializes.
5. **Watchdog reset.** Deliberately wedge a task to provoke a watchdog timeout (the `cwc_emergency_shutdown` hook does **not** run in this path — only the pulls protect). Motor must not move during the reset transition.

---

## Sequencing concern (PCB design)

When the firmware initializes the GPIOs at boot, the order must be:

1. Configure GP10/GP11/GP12 as outputs **driven low** *before* removing any pull-down (the firmware never removes them — they stay as pulls in parallel with the driven output, which is correct).
2. Configure the PWM slice for GP11 with duty 0.
3. Only after the bridge inputs are confirmed driven low: enable PWM on GP11 and set PHASE/nSLEEP per state-machine demand.

The pull-downs and the driven-low GPIO are not mutually exclusive — the GPIO drive overrides the resistor when the output is active. Keep the pull-downs populated for the reset-time protection they provide.

---

## Notes for the PCB designer

- Locate the three bridge-input pull-downs **physically close to the DRV8838**, not near the Pico, so they protect the bridge input pins even if the trace to the MCU is broken or unsoldered.
- Use ≥1 % tolerance on these three. Not because the exact value matters for safety, but because tolerance variations affect the ratio with the DRV8838's input leakage and can shift the worst-case threshold.
- Mark the three pull-downs as "DNI — DO NOT DEPOPULATE" on the BOM. Future cost-reduction efforts must not delete them.
