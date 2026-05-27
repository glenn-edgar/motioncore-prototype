// ============================================================================
// samd21_hal_pin.c — HAL pin-claim + configuration for the SAMD21G18A.
// See header for design notes.
// ============================================================================

#include "samd21_hal_pin.h"
#include "samd21.h"
#include "samd21_adc.h"
#include <string.h>

// Pin-ownership table indexed by physical pin id (port<<5 | pin). 64 entries
// covers PA0..PA31 + PB0..PB31. ~512 B in .bss (8 B/record × 64).
//
// slot_mask: bit i set means slot i is sharing this pin. 0 = unclaimed.
// ok_value/err_value: meaningful for GPIO_OUT only; uniform across all
//   slots sharing the pin (enforced at claim time).
// oversample_exp/sh_cyc/adc_channel: meaningful for ADC_SCAN only; recorded
//   at claim time and reapplied on every hal_pin_read_adc.
typedef struct {
    uint8_t slot_mask;
    uint8_t mode;             // hal_pin_mode_t (uniform across sharers)
    uint8_t ok_value;         // output mode only
    uint8_t err_value;        // output mode only
    uint8_t oversample_exp;   // ADC mode only (0..7)
    uint8_t sh_cyc;           // ADC mode only (0..63)
    uint8_t adc_channel;      // ADC mode only (AIN index from pin table)
    uint8_t reserved;         // pad to 8 B
} hal_pin_claim_record_t;

static hal_pin_claim_record_t g_claims[HAL_PIN_TABLE_SIZE];
static bool                   g_claims_initialised = false;

static void ensure_initialised(void) {
    if (g_claims_initialised) return;
    for (uint8_t i = 0; i < HAL_PIN_TABLE_SIZE; i++) {
        g_claims[i].slot_mask      = 0;
        g_claims[i].mode           = HAL_PIN_MODE_UNCLAIMED;
        g_claims[i].ok_value       = 0;
        g_claims[i].err_value      = 0;
        g_claims[i].oversample_exp = 0;
        g_claims[i].sh_cyc         = 0;
        g_claims[i].adc_channel    = 0xFFu;
        g_claims[i].reserved       = 0;
    }
    g_claims_initialised = true;
}

// ---- low-level PORT helpers ---------------------------------------------

static void port_configure_input(uint8_t port, uint8_t pin, hal_pin_mode_t mode) {
    const uint32_t mask = (1u << pin);
    PORT->Group[port].DIRCLR.reg = mask;
    PORT->Group[port].PINCFG[pin].bit.PMUXEN = 0;
    PORT->Group[port].PINCFG[pin].bit.INEN   = 1;
    if (mode == HAL_PIN_MODE_GPIO_IN_PU) {
        PORT->Group[port].PINCFG[pin].bit.PULLEN = 1;
        PORT->Group[port].OUTSET.reg = mask;
    } else if (mode == HAL_PIN_MODE_GPIO_IN_PD) {
        PORT->Group[port].PINCFG[pin].bit.PULLEN = 1;
        PORT->Group[port].OUTCLR.reg = mask;
    } else {
        PORT->Group[port].PINCFG[pin].bit.PULLEN = 0;
    }
}

static void port_configure_output(uint8_t port, uint8_t pin) {
    const uint32_t mask = (1u << pin);
    PORT->Group[port].PINCFG[pin].bit.PMUXEN = 0;
    PORT->Group[port].PINCFG[pin].bit.INEN   = 0;
    PORT->Group[port].PINCFG[pin].bit.PULLEN = 0;
    PORT->Group[port].DIRSET.reg = mask;
}

static void port_reset_to_safe(uint8_t port, uint8_t pin) {
    const uint32_t mask = (1u << pin);
    PORT->Group[port].DIRCLR.reg                = mask;
    PORT->Group[port].PINCFG[pin].bit.INEN      = 0;
    PORT->Group[port].PINCFG[pin].bit.PULLEN    = 0;
    PORT->Group[port].PINCFG[pin].bit.PMUXEN    = 0;
    PORT->Group[port].OUTCLR.reg                = mask;
}

// Look up board pin record by phys_id. Returns NULL if id isn't a real Xiao pin.
static const board_pin_t* find_board_pin(uint8_t phys_id) {
    for (uint8_t i = 0; i < g_board_pin_count; i++) {
        if (board_pin_phys_id(&g_board_pins[i]) == phys_id) return &g_board_pins[i];
    }
    return 0;
}

// ---- public API ---------------------------------------------------------

hal_pin_claim_status_t hal_pin_claim(uint8_t phys_id, uint8_t slot, hal_pin_mode_t mode) {
    ensure_initialised();
    if (phys_id >= HAL_PIN_TABLE_SIZE) return HAL_PIN_CLAIM_NO_SUCH_PIN;
    if (mode == HAL_PIN_MODE_UNCLAIMED || mode > HAL_PIN_MODE_ADC_SCAN) {
        return HAL_PIN_CLAIM_BAD_MODE;
    }
    // Outputs MUST use hal_pin_claim_output for the (ok,err) declaration.
    if (mode == HAL_PIN_MODE_GPIO_OUT) return HAL_PIN_CLAIM_BAD_MODE;
    // ADC MUST use hal_pin_claim_adc for oversample/sh declaration.
    if (mode == HAL_PIN_MODE_ADC_SCAN) return HAL_PIN_CLAIM_BAD_MODE;

    const board_pin_t* bp = find_board_pin(phys_id);
    if (bp == 0) return HAL_PIN_CLAIM_NO_SUCH_PIN;
    if (board_pin_is_reserved(bp)) return HAL_PIN_CLAIM_RESERVED;

    bool needs_gpio = (mode == HAL_PIN_MODE_GPIO_IN
                    || mode == HAL_PIN_MODE_GPIO_IN_PU
                    || mode == HAL_PIN_MODE_GPIO_IN_PD);
    if (needs_gpio && (bp->caps & BOARD_PIN_CAP_GPIO) == 0u) return HAL_PIN_CLAIM_CAP_MISSING;

    const uint8_t slot_bit = (uint8_t)(1u << slot);

    // Inputs are single-owner. Allow re-claim by the same slot (idempotent).
    if (g_claims[phys_id].slot_mask != 0
        && g_claims[phys_id].slot_mask != slot_bit) {
        return HAL_PIN_CLAIM_TAKEN;
    }

    if (needs_gpio) {
        port_configure_input(bp->port, bp->pin, mode);
    }

    g_claims[phys_id].slot_mask = slot_bit;
    g_claims[phys_id].mode      = (uint8_t)mode;
    g_claims[phys_id].ok_value  = 0;
    g_claims[phys_id].err_value = 0;
    return HAL_PIN_CLAIM_OK;
}

hal_pin_claim_status_t hal_pin_claim_adc(uint8_t phys_id, uint8_t slot,
                                         uint8_t oversample_exp, uint8_t sh_cyc) {
    ensure_initialised();
    if (phys_id >= HAL_PIN_TABLE_SIZE)    return HAL_PIN_CLAIM_NO_SUCH_PIN;
    if (oversample_exp > 7u)              return HAL_PIN_CLAIM_BAD_MODE;
    if (sh_cyc > 63u)                     return HAL_PIN_CLAIM_BAD_MODE;

    const board_pin_t* bp = find_board_pin(phys_id);
    if (bp == 0)                          return HAL_PIN_CLAIM_NO_SUCH_PIN;
    if (board_pin_is_reserved(bp))        return HAL_PIN_CLAIM_RESERVED;
    if ((bp->caps & BOARD_PIN_CAP_ADC) == 0u) return HAL_PIN_CLAIM_CAP_MISSING;
    if (bp->adc_channel == BOARD_PIN_ADC_NONE) return HAL_PIN_CLAIM_CAP_MISSING;

    const uint8_t slot_bit = (uint8_t)(1u << slot);

    // ADC inputs are single-owner. Allow re-claim by the same slot (idempotent).
    if (g_claims[phys_id].slot_mask != 0
        && g_claims[phys_id].slot_mask != slot_bit) {
        return HAL_PIN_CLAIM_TAKEN;
    }
    // If reclaiming, must be ADC mode — refuse if a non-ADC claim is present.
    if (g_claims[phys_id].slot_mask == slot_bit
        && g_claims[phys_id].mode != (uint8_t)HAL_PIN_MODE_ADC_SCAN) {
        return HAL_PIN_CLAIM_TAKEN;
    }

    g_claims[phys_id].slot_mask      = slot_bit;
    g_claims[phys_id].mode           = (uint8_t)HAL_PIN_MODE_ADC_SCAN;
    g_claims[phys_id].ok_value       = 0;
    g_claims[phys_id].err_value      = 0;
    g_claims[phys_id].oversample_exp = oversample_exp;
    g_claims[phys_id].sh_cyc         = sh_cyc;
    g_claims[phys_id].adc_channel    = bp->adc_channel;
    return HAL_PIN_CLAIM_OK;
}

uint16_t hal_pin_read_adc(uint8_t phys_id) {
    ensure_initialised();
    if (phys_id >= HAL_PIN_TABLE_SIZE) return 0;
    const hal_pin_claim_record_t* c = &g_claims[phys_id];
    if (c->mode != (uint8_t)HAL_PIN_MODE_ADC_SCAN) return 0;
    if (c->adc_channel == 0xFFu) return 0;
    return samd21_adc_read_oneshot(c->adc_channel, c->oversample_exp, c->sh_cyc);
}

hal_pin_claim_status_t hal_pin_claim_output(uint8_t phys_id, uint8_t slot,
                                            uint8_t ok_value, uint8_t err_value) {
    ensure_initialised();
    if (phys_id >= HAL_PIN_TABLE_SIZE) return HAL_PIN_CLAIM_NO_SUCH_PIN;
    if (ok_value > 1u || err_value > 1u) return HAL_PIN_CLAIM_BAD_MODE;
    if (ok_value == err_value)           return HAL_PIN_CLAIM_VALUE_MISMATCH;

    const board_pin_t* bp = find_board_pin(phys_id);
    if (bp == 0) return HAL_PIN_CLAIM_NO_SUCH_PIN;
    if (board_pin_is_reserved(bp)) return HAL_PIN_CLAIM_RESERVED;
    if ((bp->caps & BOARD_PIN_CAP_GPIO) == 0u) return HAL_PIN_CLAIM_CAP_MISSING;

    const uint8_t slot_bit = (uint8_t)(1u << slot);

    if (g_claims[phys_id].slot_mask != 0) {
        // Pin currently claimed by some slot(s). Reject if not an output share.
        if (g_claims[phys_id].mode != HAL_PIN_MODE_GPIO_OUT) {
            return HAL_PIN_CLAIM_TAKEN;
        }
        // Output sharing: ok/err must match the existing declaration.
        if (g_claims[phys_id].ok_value  != ok_value
         || g_claims[phys_id].err_value != err_value) {
            return HAL_PIN_CLAIM_VALUE_MISMATCH;
        }
        // Add this slot to the share mask. Idempotent if bit already set.
        g_claims[phys_id].slot_mask |= slot_bit;
        return HAL_PIN_CLAIM_OK;
    }

    // First claim — configure pin and record values.
    port_configure_output(bp->port, bp->pin);
    g_claims[phys_id].slot_mask = slot_bit;
    g_claims[phys_id].mode      = (uint8_t)HAL_PIN_MODE_GPIO_OUT;
    g_claims[phys_id].ok_value  = ok_value;
    g_claims[phys_id].err_value = err_value;
    return HAL_PIN_CLAIM_OK;
}

void hal_pin_release_slot(uint8_t slot) {
    ensure_initialised();
    const uint8_t slot_bit = (uint8_t)(1u << slot);
    for (uint8_t id = 0; id < HAL_PIN_TABLE_SIZE; id++) {
        if ((g_claims[id].slot_mask & slot_bit) == 0u) continue;
        g_claims[id].slot_mask &= (uint8_t)~slot_bit;
        if (g_claims[id].slot_mask == 0u) {
            uint8_t port = BOARD_PHYS_PIN_PORT(id);
            uint8_t pin  = BOARD_PHYS_PIN_PIN(id);
            port_reset_to_safe(port, pin);
            g_claims[id].mode      = HAL_PIN_MODE_UNCLAIMED;
            g_claims[id].ok_value  = 0;
            g_claims[id].err_value = 0;
        }
        // Otherwise leave the pin configured for remaining sharers.
    }
}

uint8_t hal_pin_get_owners(uint8_t phys_id) {
    ensure_initialised();
    if (phys_id >= HAL_PIN_TABLE_SIZE) return 0;
    return g_claims[phys_id].slot_mask;
}

hal_pin_mode_t hal_pin_get_mode(uint8_t phys_id) {
    ensure_initialised();
    if (phys_id >= HAL_PIN_TABLE_SIZE) return HAL_PIN_MODE_UNCLAIMED;
    return (hal_pin_mode_t)g_claims[phys_id].mode;
}

bool hal_pin_check_consistency(void) {
    ensure_initialised();
    for (uint8_t id = 0; id < HAL_PIN_TABLE_SIZE; id++) {
        const hal_pin_claim_record_t* c = &g_claims[id];
        if (c->slot_mask == 0u) continue;
        // Output sharing is allowed; only input modes are single-owner.
        if (c->mode == (uint8_t)HAL_PIN_MODE_GPIO_OUT) continue;
        // popcount on an 8-bit value (Cortex-M0+ has no CLZ/POPCNT — use
        // the Kernighan loop).
        uint8_t m = c->slot_mask;
        uint8_t bits = 0;
        while (m) { m &= (uint8_t)(m - 1u); bits++; }
        if (bits > 1u) return false;
    }
    return true;
}

uint8_t hal_pin_read(uint8_t phys_id) {
    ensure_initialised();
    if (phys_id >= HAL_PIN_TABLE_SIZE) return 0;
    uint8_t port = BOARD_PHYS_PIN_PORT(phys_id);
    uint8_t pin  = BOARD_PHYS_PIN_PIN(phys_id);
    return (PORT->Group[port].IN.reg & (1u << pin)) ? 1u : 0u;
}

void hal_pin_drive_outputs(uint8_t veto_mask) {
    ensure_initialised();
    for (uint8_t id = 0; id < HAL_PIN_TABLE_SIZE; id++) {
        const hal_pin_claim_record_t* c = &g_claims[id];
        if (c->slot_mask == 0u) continue;
        if (c->mode != (uint8_t)HAL_PIN_MODE_GPIO_OUT) continue;
        uint8_t val = (c->slot_mask & veto_mask) ? c->err_value : c->ok_value;
        uint8_t port = BOARD_PHYS_PIN_PORT(id);
        uint8_t pin  = BOARD_PHYS_PIN_PIN(id);
        const uint32_t mask = (1u << pin);
        if (val) PORT->Group[port].OUTSET.reg = mask;
        else     PORT->Group[port].OUTCLR.reg = mask;
    }
}
