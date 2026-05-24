// ============================================================================
// samd21_hal.c — WDT + reset-cause for register_dongle.
// See samd21_hal.h header for design rationale.
// ============================================================================

#include "samd21_hal.h"
#include "samd21.h"

// ---------------------------------------------------------------------------
// Watchdog Timer
//
// GCLK5 = OSCULP32K /32 = 1024 Hz (DIVSEL=1 + DIV=4 → 2^(4+1) = 32).
// GCLK_CLKCTRL_ID_WDT = 0x03 per datasheet §15.8.3.
// PER = 0x9 → 4096 cycles / 1024 Hz = 4 s. 16× margin over 250 ms tick.
// ---------------------------------------------------------------------------

void hal_wdt_init(void)
{
    // 1. Enable WDT bus clock.
    PM->APBAMASK.reg |= PM_APBAMASK_WDT;

    // 2. GCLK5 generator: OSCULP32K, exponential divide /32.
    GCLK->GENDIV.reg = GCLK_GENDIV_ID(5) | GCLK_GENDIV_DIV(4);
    while (GCLK->STATUS.bit.SYNCBUSY) { /* spin */ }

    GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(5)
                      | GCLK_GENCTRL_SRC_OSCULP32K
                      | GCLK_GENCTRL_DIVSEL
                      | GCLK_GENCTRL_GENEN;
    while (GCLK->STATUS.bit.SYNCBUSY) { /* spin */ }

    // 3. Route GCLK5 to WDT clock channel.
    GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID_WDT
                      | GCLK_CLKCTRL_GEN_GCLK5
                      | GCLK_CLKCTRL_CLKEN;
    while (GCLK->STATUS.bit.SYNCBUSY) { /* spin */ }

    // 4. Configure WDT in normal (non-windowed) mode, PER=4096 cycles.
    WDT->CTRL.reg = 0;  // disable so CONFIG is writable
    while (WDT->STATUS.bit.SYNCBUSY) { /* spin */ }

    WDT->CONFIG.reg = WDT_CONFIG_PER_4K;
    WDT->EWCTRL.reg = 0;  // disable early-warning interrupt

    WDT->CTRL.reg = WDT_CTRL_ENABLE;
    while (WDT->STATUS.bit.SYNCBUSY) { /* spin */ }

    // 5. First refresh.
    hal_wdt_pet();
}

void hal_wdt_pet(void)
{
    // Writing the 0xA5 key clears the WDT count. Any other value triggers
    // immediate system reset, so do NOT write any other constant here.
    WDT->CLEAR.reg = WDT_CLEAR_CLEAR_KEY;
}

// ---------------------------------------------------------------------------
// Reset-cause capture
// ---------------------------------------------------------------------------

static uint8_t g_reset_cause_snapshot = 0u;

void hal_capture_reset_cause(void)
{
    g_reset_cause_snapshot = PM->RCAUSE.reg;
    // RCAUSE is RO + sticky; don't try to clear it.
}

uint8_t hal_get_reset_cause(void)
{
    return g_reset_cause_snapshot;
}
