// ============================================================================
// samd21_hal_pin.c — HAL pin-claim + configuration for the SAMD21G18A.
// See header for design notes.
// ============================================================================

#include "samd21_hal_pin.h"
#include "samd21.h"
#include <string.h>

// Pin-ownership table indexed by physical pin id (port<<5 | pin). 64 entries
// covers PA0..PA31 + PB0..PB31. ~128 B in .bss.
typedef struct {
    uint8_t owner_slot;   // slot index 0..N-1, or HAL_PIN_UNCLAIMED (0xFF)
    uint8_t mode;         // hal_pin_mode_t
} hal_pin_claim_record_t;

static hal_pin_claim_record_t g_claims[HAL_PIN_TABLE_SIZE];
static bool                   g_claims_initialised = false;

static void ensure_initialised(void) {
    if (g_claims_initialised) return;
    for (uint8_t i = 0; i < HAL_PIN_TABLE_SIZE; i++) {
        g_claims[i].owner_slot = HAL_PIN_UNCLAIMED;
        g_claims[i].mode       = HAL_PIN_MODE_UNCLAIMED;
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

// ---- public API ---------------------------------------------------------

hal_pin_claim_status_t hal_pin_claim(uint8_t phys_id, uint8_t slot, hal_pin_mode_t mode) {
    ensure_initialised();
    if (phys_id >= HAL_PIN_TABLE_SIZE) return HAL_PIN_CLAIM_NO_SUCH_PIN;
    if (mode == HAL_PIN_MODE_UNCLAIMED || mode > HAL_PIN_MODE_ADC_SCAN) {
        return HAL_PIN_CLAIM_BAD_MODE;
    }

    // Find the pin record in the board table (validates that phys_id corresponds
    // to a real Xiao pin and gives us capability bits).
    const board_pin_t* bp = 0;
    for (uint8_t i = 0; i < g_board_pin_count; i++) {
        if (board_pin_phys_id(&g_board_pins[i]) == phys_id) {
            bp = &g_board_pins[i];
            break;
        }
    }
    if (bp == 0) return HAL_PIN_CLAIM_NO_SUCH_PIN;
    if (board_pin_is_reserved(bp)) return HAL_PIN_CLAIM_RESERVED;

    // Capability check: every GPIO mode requires CAP_GPIO; ADC requires CAP_ADC.
    bool needs_gpio = (mode == HAL_PIN_MODE_GPIO_IN
                    || mode == HAL_PIN_MODE_GPIO_IN_PU
                    || mode == HAL_PIN_MODE_GPIO_IN_PD
                    || mode == HAL_PIN_MODE_GPIO_OUT);
    bool needs_adc  = (mode == HAL_PIN_MODE_ADC_SCAN);
    if (needs_gpio && (bp->caps & BOARD_PIN_CAP_GPIO) == 0u) return HAL_PIN_CLAIM_CAP_MISSING;
    if (needs_adc  && (bp->caps & BOARD_PIN_CAP_ADC)  == 0u) return HAL_PIN_CLAIM_CAP_MISSING;

    // Ownership check. Slice 2 semantic: at most one slot per pin. Re-claim
    // by the same slot is allowed (idempotent) — re-configures.
    if (g_claims[phys_id].owner_slot != HAL_PIN_UNCLAIMED
        && g_claims[phys_id].owner_slot != slot) {
        return HAL_PIN_CLAIM_TAKEN;
    }

    // Apply PORT configuration.
    if (mode == HAL_PIN_MODE_GPIO_OUT) {
        port_configure_output(bp->port, bp->pin);
    } else if (needs_gpio) {
        port_configure_input(bp->port, bp->pin, mode);
    }
    // ADC mode: future slice 4 will set up the ADC mux here.

    g_claims[phys_id].owner_slot = slot;
    g_claims[phys_id].mode       = (uint8_t)mode;
    return HAL_PIN_CLAIM_OK;
}

void hal_pin_release_slot(uint8_t slot) {
    ensure_initialised();
    for (uint8_t id = 0; id < HAL_PIN_TABLE_SIZE; id++) {
        if (g_claims[id].owner_slot != slot) continue;
        uint8_t port = BOARD_PHYS_PIN_PORT(id);
        uint8_t pin  = BOARD_PHYS_PIN_PIN(id);
        port_reset_to_safe(port, pin);
        g_claims[id].owner_slot = HAL_PIN_UNCLAIMED;
        g_claims[id].mode       = HAL_PIN_MODE_UNCLAIMED;
    }
}

uint8_t hal_pin_get_owner(uint8_t phys_id) {
    ensure_initialised();
    if (phys_id >= HAL_PIN_TABLE_SIZE) return HAL_PIN_UNCLAIMED;
    return g_claims[phys_id].owner_slot;
}

hal_pin_mode_t hal_pin_get_mode(uint8_t phys_id) {
    ensure_initialised();
    if (phys_id >= HAL_PIN_TABLE_SIZE) return HAL_PIN_MODE_UNCLAIMED;
    return (hal_pin_mode_t)g_claims[phys_id].mode;
}

uint8_t hal_pin_read(uint8_t phys_id) {
    ensure_initialised();
    if (phys_id >= HAL_PIN_TABLE_SIZE) return 0;
    uint8_t port = BOARD_PHYS_PIN_PORT(phys_id);
    uint8_t pin  = BOARD_PHYS_PIN_PIN(phys_id);
    return (PORT->Group[port].IN.reg & (1u << pin)) ? 1u : 0u;
}

void hal_pin_write(uint8_t phys_id, uint8_t value) {
    ensure_initialised();
    if (phys_id >= HAL_PIN_TABLE_SIZE) return;
    // Only act if claimed as output — silently ignore otherwise so a
    // misconfigured interlock can't drive a pin into trouble.
    if (g_claims[phys_id].mode != HAL_PIN_MODE_GPIO_OUT) return;
    uint8_t port = BOARD_PHYS_PIN_PORT(phys_id);
    uint8_t pin  = BOARD_PHYS_PIN_PIN(phys_id);
    const uint32_t mask = (1u << pin);
    if (value) PORT->Group[port].OUTSET.reg = mask;
    else       PORT->Group[port].OUTCLR.reg = mask;
}
