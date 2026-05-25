# Interlock Framework — Prior Art Survey

**Date:** 2026-05-25
**Author:** Glenn Edgar (design dialog with Claude Code)
**Context:** Pre-implementation survey for the SAMD21 interlock framework
described in `samd21_interlock_framework_design.md`.

This document captures a prior-art search performed before committing to
implementation of a text-DSL-configured runtime interlock framework for
small microcontrollers (SAMD21G18A, R7FA4M1AB, ESP32-C6) in the
Xiao form factor. Recorded here for design provenance and to establish
public prior use under the repository's MIT license.

---

## Design summary

The framework being built has these properties:

1. **Programmable runtime safety interlocks** on a small microcontroller
   (Cortex-M0+ class), configurable from a host without firmware reflash.
2. **Text DSL on the wire** as the configuration format, e.g.:
   ```
   gpio_int;cfg[{(D1,D2):in,up},{(D3):out}];watch[D1:1,D2:1];out_ok[D3:0];out_err[D3:1]
   adc_int;cfg[(A1):oversample_16,sh_5];watch[A1:>2048,hyst50];out_ok[D3:0];out_err[D3:1]
   ```
   parsed once on the chip, persisted as text in `.noinit` RAM.
3. **Multiple interlocks running in parallel** (two slots in v1, expandable),
   votes merged per output pin using OR-of-vetoes (fail-safe AND of OKs).
4. **GPIO and ADC inputs unified under one interlock engine**; the
   interlock is a function returning T/F based on watch clauses; the
   side effect is the per-tick output vote.
5. **State survives a watchdog (WDT) reset** via `.noinit` RAM and a
   magic + slot ID + bootloop counter. WDT reset is *recovery*; power-on
   reset is *deliberate re-arm*.
6. **Cross-chip portability via board labels** (D1/A1) — the same DSL
   text runs unchanged on SAMD21, RA4M1, and ESP32-C6 because a chip-specific
   HAL library translates board labels to physical pin numbers.
7. **HAL is the pin-conflict gatekeeper** — typed claims (READ stackable,
   WRITE-output shareable with matching OK/ERR values, mode-mismatch
   refused).
8. **Status messages text-based, same DSL conventions, ≤64 B per slot**,
   pulled by the host (poll-on-change + explicit snapshot). Critical
   events (TRIP, TERM, ERROR, INIT) on a one-shot side queue.
9. **Identical opcode semantics over USB-CDC and RS-485** — master-driven
   polling on both transports so a single host driver speaks both.
10. **LuaJIT host builder operates on Lua dictionaries**, serializes to
    the wire DSL; same parser inverted to read status events.

---

## Prior art found

Searched in 2026-05 using multiple keyword combinations across patent
databases, GitHub, and embedded-trade publications. Findings grouped by
proximity to the design above.

### Group A — Industrial PLC safety controllers (Pilz / Rockwell / Banner)

These solve the same *problem* (configurable interlock logic) with very
different mechanisms:

| Product | Configuration mechanism |
|---|---|
| Pilz PNOZmulti | Graphical function-block editor (PNOZmulti Configurator PC tool) |
| Rockwell GuardLogix | IEC 61131-3 structured text + ladder + FBD in Studio 5000 |
| Banner safety controllers | Drag-drop function-block GUI |

Common traits in this group:
- **Graphical configuration**, not text DSL. Configuration is bound to a
  proprietary PC application.
- **Certified to IEC 61508 / ISO 13849** safety integrity levels.
  Certification cost and procedural overhead make these unsuitable for
  hobby / maker / small-OEM use.
- **Dedicated safety hardware** with redundant channels, diagnostic
  pulses, and certified failure modes.
- **State does not survive reset** by design — power-on reset and explicit
  re-commissioning is the safety pattern.
- **Single-transport** — each vendor uses its own safety bus (SafetyBus p,
  EtherCAT FSoE, etc.).

### Group B — Embedded watchdog multiplexers

Open-source libraries for managing multiple software watchdogs against
one hardware WDT.

- **`task-watchdog`** (GitHub: `piersfinlayson/task-watchdog`) — multiplexes
  multiple task watchdogs into a single hardware WDT, with runtime
  register / deregister. Supports `no_alloc` mode for resource-constrained
  MCUs. Closest *technical* analog in terms of registration pattern and
  no-alloc constraint.
- **`watchdogd`** (GitHub: `troglobit/watchdogd`) — Linux daemon, process
  supervisor. Not embedded-bare-metal; included for completeness.

Limitations vs the framework being built:
- **Timer-based only** — no GPIO or ADC composition.
- **No DSL** — registration is direct C function calls.
- **No output voting** — watchdog timeout triggers a hardware reset, not
  a composable per-pin "drive to safe state."
- **No cross-chip portability layer** — registration uses chip-specific
  resource handles.

### Group C — Patents

Multiple patents on safety relay configuration systems, almost all
describing graphical / function-block configuration:

- **US 9,971,330** "Safety relay configuration editor" — graphical editor.
- **US 9,361,073** "Development environment for a safety relay configuration system" — graphical.
- **US 9,977,407** "Safety relay configuration system for safety mat device using graphical interface" — graphical.
- **US 10,152,030** "Safety relay configuration system with safety monitoring and safety output function blocks" — graphical function-block config.
- **US 11,853,026** "Configurable distributed-interlock-system" — closest in spirit; describes a master device evaluating interlock conditions against a lookup table to prevent unsafe command sequences. Still graphical configuration; no text DSL; no microcontroller-class portability claim.

### Group D — Standards / frameworks

- **IEC 61131-3** — structured text + ladder + FBD + SFC + IL for PLCs.
  A *full programming language family* compiled to bytecode, not a small
  declarative DSL. Conceptually more general than what is needed for
  interlock supervision.
- **IEC 61508 / ISO 13849** — functional safety standards. Establish the
  fail-safe-default + diagnostic-emit + redundancy *concepts* that inform
  our design, without imposing the certification weight.
- **STM32Cube — Flexible Safety RTOS** (embedded-office.net) — RTOS
  scaffolding for IEC 61508-compliant systems on STM32. Heavyweight,
  STM32-specific, and oriented toward certification.

### Group E — Tangentially related

- **Microcontroller configuration via text/XML** (US 7,406,674,
  US 8,069,428, US 8,793,635, US 10,466,977) — patents on generating
  microcontroller firmware configuration from text descriptions. Different
  problem space — these are *build-time* configuration generators, not
  *runtime* interlock evaluators.

---

## What the design borrows from prior art

| From | Pattern adopted |
|---|---|
| PLC safety relays | Per-input expected-state composition → composite OK/ERR; named fail-safe output defaults |
| `task-watchdog` | Slot-based registration; no-alloc constraint; runtime register / deregister |
| IEC 61508 concepts | Separation of operational logic (HIL commands) from safety supervisor (interlocks); diagnostic emit on state change |

## What appears genuinely novel

In combination — none of these features individually is novel, but the
combination targeting hobby/maker-grade microcontrollers without
IEC certification overhead appears unoccupied in both open source and
commercial products as of May 2026:

1. **Text DSL on the wire** for cross-language tooling (Lua host → C chip)
   rather than graphical configuration or compiled IEC 61131-3.
2. **Cross-chip portability via board labels** (D1/A1) — same DSL text
   runs unchanged across SAMD21, RA4M1, ESP32-C6 because the HAL layer
   per chip provides the label-to-pin translation.
3. **State survives WDT reset by design** via `.noinit` RAM with magic +
   slot ID + bootloop counter. PLCs deliberately reset on power loss;
   this framework deliberately preserves state because WDT bite is
   *recovery* not *commissioning*.
4. **Identical wire semantics over USB-CDC AND RS-485** (master-driven
   polling on both transports) so a single host driver speaks both
   transports.
5. **Sub-1 KB flash footprint per chip** for the interlock engine + parser,
   targeting Cortex-M0+ class silicon at ~$2-5 per chip.

---

## Implications for design decisions

- **No certification pursued.** IEC 61508 / ISO 13849 routes are
  inappropriate for the target use case (hobby / maker / small-OEM
  bench-grade safety supervision). The framework borrows safety
  *concepts* without claiming certification.
- **No fork of `task-watchdog`.** The registration pattern is similar
  enough to inform our slot table; the I/O composition and DSL parser are
  too different to reuse.
- **No fork of IEC 61131-3 / OpenPLC.** Compilation pipeline overhead is
  not justified for the 2-slot interlock supervisor scope.
- **Public design.** Recording this survey + design memos in a
  MIT-licensed public repository establishes prior use, protecting the
  freedom of future implementers from later patent claims on the
  combination of features above.

---

## Sources

- [task-watchdog: multi-task watchdog library for embedded](https://github.com/piersfinlayson/task-watchdog)
- [watchdogd: Linux process watchdog supervisor](https://github.com/troglobit/watchdogd)
- [GuardLogix Safety Controllers overview](https://program-plc.blogspot.com/2015/08/guardlogix-safety-controllers-product.html)
- [Pilz safety relays product line](https://www.pilz.com/en-INT/products/relay-modules/safety-relays-protection-relays)
- [IEC 61131-3 languages for safety controllers](https://www.controldesign.com/safety/safety-controllers/article/55265226/which-iec-61131-3-languages-are-best-for-programmable-safety-controllers)
- [Configurable distributed-interlock-system patent (US 11,853,026)](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/11853026)
- [Safety relay configuration editor (US 9,971,330)](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/9971330)
- [Safety relay configuration system with safety monitoring and output function blocks (US 10,152,030)](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/10152030)
- [Door-Monitoring Interlock Switch with Configurable Safety (Rockwell AT133)](https://literature.rockwellautomation.com/idc/groups/literature/documents/at/safety-at133_-en-p.pdf)
- [STM32Cube — Flexible Safety RTOS documentation](https://www.embedded-office.net/eval/manual/latest/env/stm32cube.html)
- [Watchdog Timers in Embedded Systems: Preventing Silent Software Failures (Medium)](https://medium.com/embedworld/watchdog-timers-in-embedded-systems-preventing-silent-software-failures-bb17222f1107)
- [Implementing Robust Watchdog Timers for Embedded Systems (In Compliance Magazine)](https://incompliancemag.com/implementing-robust-watchdog-timers-for-embedded-systems/)
