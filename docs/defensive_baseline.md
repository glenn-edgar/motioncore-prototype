# Defensive baseline for bare-metal firmware

This document records the safety techniques we apply across every chip
port in this project (currently SAMD21; coming RA4M1, RP2350, ESP32-C6).
The recipe is distilled from WITTENSTEIN High-Integrity Systems' SAFERTOS
— the commercial safety-certified derivative of FreeRTOS (IEC 61508 SIL 3
/ ISO 26262 ASIL D / IEC 62304 / DO-178B). We don't use SAFERTOS itself,
but the engineering principles it codifies transfer cleanly to bare-metal
main-loop firmware.

The motivating incident: a multi-day debug cycle on the SAMD21 interlock
framework's slice 4 was lost to silent stack overflow corrupting a noinit
persistence struct. Every one of the techniques below would have caught
that bug at iteration 0 instead of iteration 15. We adopted them so we
don't pay that cost again.

## Why this matters across the project

Each new chip port (SAMD21 → RA4M1 → RP2350 → ESP32-C6) re-implements
the same primitives: noinit persistence, HAL pin claims, an interlock
state machine, USB CDC shell. Without a shared defensive baseline, each
port would independently discover the same classes of bugs (stack
overflow into noinit, silent data corruption, half-initialised state at
boot). Applying this recipe from day 1 on a new port makes the
debugging cost predictable: when something fails, the failure is
**loud** and **early**.

## The ten techniques

Ordered roughly by leverage (most impactful first).

### 1. Pre-overflow stack-pointer check, not just post-mortem canary

**Rule:** at every supervisor tick, read SP and compare against
`_sstack + margin`. Panic *before* the corrupting write happens.

```c
extern char _sstack[];
#define STACK_PRE_OVERFLOW_MARGIN  256u   // size for deepest IRQ chain × safety factor

static inline void check_sp_or_panic(void) {
    uint32_t sp;
    __asm__ volatile ("mov %0, sp" : "=r"(sp));
    if (sp < ((uintptr_t)_sstack + STACK_PRE_OVERFLOW_MARGIN)) {
        panic(PANIC_STACK_NEAR_OVERFLOW, sp);
    }
}
```

Cortex-M4 (RA4M1) and M33 (RP2350) have a hardware `MSPLIM` / `PSPLIM`
register that does this for free — set it once at boot, no per-tick
overhead. Cortex-M0+ (SAMD21) and RISC-V (ESP32-C6) use the software
version above.

Keep painted canaries as a second-layer defense (catches overflows that
happen between SP-checks, e.g., during a deep IRQ chain).

### 2. Magic + version + size header on every persistent or cross-boundary struct

**Rule:** any struct kept in `.noinit`, read from flash, or exchanged
across a module API begins with:

```c
typedef struct {
    uint32_t magic;        // chip+module+struct-specific constant
    uint16_t version;      // bumped when layout changes
    uint16_t self_size;    // sizeof(this struct), checked against expected
    /* ... payload ... */
} foo_persistent_t;
```

Every reader (not just `boot_init`) validates all three before touching
the payload. Mismatch → panic with discriminated code.

### 3. Linker guard regions between adjacent memory zones

**Rule:** put a named guard region filled with a fixed pattern between
zones that have no business sharing memory (e.g., `.bss` ↔ `.stack`,
`.stack` ↔ `.noinit`). Check the pattern at boot and at every
supervisor tick.

```
.bss > ram
.guard_bss_stack (NOLOAD) : {
    . = ALIGN(4);
    _sguard_bs = .;
    . = . + GUARD_SIZE;
    _eguard_bs = .;
} > ram
.stack (NOLOAD) : { ... } > ram
.guard_stack_noinit (NOLOAD) : { ... } > ram
.noinit (NOLOAD) : { ... } > ram
```

Fill with `0xC1A551C1` (`CLASSIC`) at boot, check periodically. Tripped
pattern → adjacent zone overflowed into the guard. Log the suspected
zone and panic.

### 4. Single `panic()` function with discriminated codes

**Rule:** one non-returning function for every "this should never
happen" failure. Records SP, PC, LR, fault code, uptime into a reserved
crash-record slot, then `NVIC_SystemReset()`.

```c
typedef enum {
    PANIC_NONE                  = 0,
    PANIC_STACK_NEAR_OVERFLOW   = 1,
    PANIC_PERSIST_MAGIC_BAD     = 2,
    PANIC_PERSIST_VERSION_BAD   = 3,
    PANIC_PERSIST_SIZE_BAD      = 4,
    PANIC_GUARD_REGION_BAD      = 5,
    PANIC_HAL_CLAIM_TABLE_BAD   = 6,
    PANIC_INIT_CANARY_BAD       = 7,
    PANIC_PERIPHERAL_BAD        = 8,
    /* extend per project */
} panic_code_t;

__attribute__((noreturn)) void panic(panic_code_t code, uint32_t arg);
```

Next boot, the boot path checks the crash record and emits
`[PANIC] code=N arg=0x... pc=0x... sp=0x... uptime=N ms`. This is the
de-facto post-mortem.

This is separate from hardware-fault recording (HardFault_Handler), which
already exists. `panic()` is for software-detected invariant violations.

### 5. `system_self_check()` at end of init, before the main loop

**Rule:** a single function that validates every invariant the main
loop will rely on. If anything fails, panic before entering the loop.

```c
void system_self_check(void) {
    if (*(volatile uint32_t*)(uintptr_t)_sstack != STACK_CANARY_VALUE)
        panic(PANIC_INIT_CANARY_BAD, 0);
    if (!guard_region_intact(_sguard_bs, _eguard_bs))
        panic(PANIC_GUARD_REGION_BAD, 1);
    if (g_persistent_state.magic != EXPECTED_MAGIC)
        panic(PANIC_PERSIST_MAGIC_BAD, g_persistent_state.magic);
    if (!pin_table_unique())
        panic(PANIC_PIN_TABLE_DUPLICATE, 0);
    if (!hal_peripherals_initialised())
        panic(PANIC_PERIPHERAL_BAD, 0);
    /* add invariants as the firmware grows */
}

int main(void) {
    /* all init */
    system_self_check();
    /* for (;;) main loop */
}
```

Refuses to enter the main loop on any assertion failure. Catches half-
initialised state on day 1 of any new chip port.

### 6. Status-return convention at every module boundary

**Rule:** every function at a module API boundary returns
`<module>_status_t`, not `bool` or `void`. Output values pass through
reference parameters. Apply `__attribute__((warn_unused_result))` so the
compiler complains about ignored failure paths.

```c
// Bad:
uint16_t hal_pin_read_adc(uint8_t phys_id);  // 0 is also a valid reading

// Good:
__attribute__((warn_unused_result))
hal_status_t hal_pin_read_adc(uint8_t phys_id, uint16_t* out_value);
```

Apply to all NEW boundaries from this point forward. Migrate older
surface only when touching it for another reason.

### 7. Bounded waits — no infinite spin loops

**Rule:** every wait has a tick-counted upper bound and returns a
`TIMEOUT` status when exceeded. No `while (!flag);`.

```c
// Bad:
while (!ADC->INTFLAG.bit.RESRDY) {}

// Good:
hal_status_t wait_until_or_timeout(volatile uint8_t* flag, uint32_t timeout_us);
if (wait_until_or_timeout(&ADC->INTFLAG.bit.RESRDY, 1000) != HAL_OK) {
    return HAL_ERR_TIMEOUT;
}
```

### 8. No dynamic allocation after init

**Rule:** all allocation happens in a startup bump allocator that runs
exactly once at boot. After `system_self_check()`, no further `malloc`.
If the bump runs out at startup → panic (config error: bump too small).

### 9. Reduce surface area — delete features with sharp edges

**Rule:** if a code path is dangerous and an alternative exists, delete
the dangerous one. SAFERTOS removed `xTaskResumeFromISR`,
`vQueueDelete`, co-routines, stream buffers, all `#ifdef` build matrices.
Our equivalents:

- No build-time feature flags. One config per firmware image. Separate
  build targets (e.g., `ROLE=bus_controller|dongle|slave` produces three
  separate firmwares).
- Don't add API knobs we don't currently need. Resist configurability
  for its own sake.

### 10. Compile-time validation over runtime checks where possible

**Rule:** anything knowable at compile time becomes a `_Static_assert`
or preprocessor constant, not a runtime check.

```c
_Static_assert(sizeof(il_inst_t) == 58, "il_inst_t layout drift");
_Static_assert(IL_MAX_INPUTS <= 8, "input mask only 8 bits");
_Static_assert(IL_NAME_MAX % 4 == 0, "name[] alignment");
```

Runtime budget is finite; spend it on things you can't catch at compile
time.

## Per-chip adaptation

| Technique | SAMD21 (M0+) | RA4M1 (M4) | RP2350 (M33) | ESP32-C6 (RISC-V) |
|---|---|---|---|---|
| 1. Pre-overflow SP | Software, inline asm | MSPLIM register (cheap) | MSPLIM / PSPLIM | Software, RISC-V mscratch |
| 2. Magic+version+size | Identical, portable C | Identical | Identical | Identical |
| 3. Linker guard zones | Linker change | Linker change | Linker change | Linker change |
| 4. `panic()` + crash slot | Identical | Identical | Identical | Identical |
| 5. `system_self_check` | Identical | Identical | Identical | Identical |
| 6. Status-return | Identical | Identical | Identical | Identical |
| 7. Bounded waits | Identical | Identical | Identical | Identical |
| 8. No malloc after init | Identical | Identical | Identical | Identical |
| 9. Surface reduction | Identical | Identical | Identical | Identical |
| 10. Compile-time checks | Identical | Identical | Identical | Identical |

## Inheriting from this baseline (new chip port)

When starting a new chip port:

1. Set up the linker script with `.noinit` placed ABOVE `.stack`,
   plus guard regions between adjacent zones (technique #3).
2. Implement `panic()` + crash record + boot-line emitter (technique #4).
3. Implement `stack_paint_and_canary()` plus a `stack_check_periodic()`
   that runs the pre-overflow SP check (technique #1).
4. Define a `system_self_check()` even if it starts mostly empty —
   technique #5 lets you fill it in incrementally as invariants
   accumulate.
5. Adopt the status-return convention (technique #6) for ALL new APIs
   you write for the chip.
6. The remaining techniques (#7-#10) are coding discipline applied as
   you write the rest of the firmware.

The SAMD21 register_dongle build at commit `3d8c5be` implements
techniques 1 (partial — paint+canary; no pre-overflow SP yet) and a
weaker version of 4. The full recipe is being rolled in over slices 5
and 6.

## Reference

The deep-dive memory at
`memory/defensive_baseline_recipe.md` (project-private) carries the same
techniques plus links to the SAFERTOS source material.

## Sources

- [SAFERTOS v9 User Manual (sample)](https://www.highintegritysystems.com/downloads/manuals_and_datasheets/Sample_SafeRTOS_User_Manual.pdf)
- [SAFERTOS App Note: Upgrading from FreeRTOS to SAFERTOS](https://www.highintegritysystems.com/downloads/manuals_and_datasheets/Upgrading_from_FreeRTOS_to_SafeRTOS.PDF)
- [SAFERTOS Technical Overview](https://www.highintegritysystems.com/downloads/manuals_and_datasheets/SafeRTOS_Technical_Overview.pdf)
- [IAR — Detecting Stack Overflows in RTOS Designs](https://www.iar.com/knowledge/learn/rtos-detecting-stack-overflows-part-2)
