// ============================================================================
// samd21_hal.h — chip-level HAL for register_dongle bring-up bits that don't
// fit cleanly in samd21_commands.c (WDT, reset cause). Patterned on
// ra4m1_hal.h but adapted for SAMD21G18A.
// ============================================================================

#pragma once

#include <stdint.h>

// ---- Watchdog Timer (WDT) --------------------------------------------------
// Layer-2 WDT per wdt-layer2-pet-from-s-engine: pet site lives inside the
// s_engine chain pump (s_expr_node_tick), NOT the C main loop. If chains
// stop progressing, chip resets in ~2 s and recovers itself.
//
// WDT clock is OSCULP32K (32.768 kHz, always-on, survives main-clock
// failures — SAMD21 analogue of RA4M1 IWDTLOCO). Routed via a private GCLK
// generator (GCLK5) so we don't disturb GCLK0..GCLK4 set up by the BSP.
// GCLK5 prescaled /32 → 1024 Hz. WDT PER=0x9 (4096 cycles) → 4 s timeout.
// 16× margin over 250 ms pet cadence.
//
// On SAMD21, unlike RA4M1, PM->RCAUSE is preserved across all warm resets
// and only cleared on power-on, so peripheral SFRs are also fully reset
// (PM/RSTC route — the architectural reason the WDT recovery path that
// failed on RA4M1 is expected to work here).

void hal_wdt_init(void);   // arm + first refresh; safe to call exactly once
void hal_wdt_pet(void);    // refresh: write 0xA5 to WDT.CLEAR

// ---- Reset-cause capture ---------------------------------------------------
// Snapshot of PM->RCAUSE, latched at boot. Bit map (per SAMD21 datasheet
// DS40001882 §16.8.16):
//   bit 0 = POR     (power-on)
//   bit 1 = BOD12
//   bit 2 = BOD33
//   bit 4 = EXT     (external reset pin)
//   bit 5 = WDT     (** the layer-2 WDT bite signal **)
//   bit 6 = SYST    (software system reset: NVIC_SystemReset / DFU touch)
//
// SAMD21 RCAUSE is not auto-cleared by reads; it persists until the next
// reset. Still snapshot-once for symmetry with the RA4M1 pattern and so
// later code can't accidentally re-read after a state change.

void    hal_capture_reset_cause(void);   // call ONCE, very early in main()
uint8_t hal_get_reset_cause(void);       // returns the captured snapshot

// ---- Static peripheral initialisation --------------------------------------
// Brings up always-on peripherals (DAC, ADC, eventually I2C + RS-485) at boot,
// before the s_engine starts. Their pins are statically reserved — GPIO
// commands refuse them via pin_is_reserved() in samd21_commands.c. Call once
// after hal_wdt_init(); idempotent thanks to per-peripheral g_*_initialized
// guards.

void samd21_peripherals_init(void);

#ifdef I2C_CLIENT
void i2c_store_service(void);   // M2b: service a pending config-store flash commit
#endif
