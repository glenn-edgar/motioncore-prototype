// ============================================================================
// samd21_hal_pin.h — HAL pin-claim + configuration API used by the interlock
// framework. Single source of truth for which slot owns which physical pin
// and in what mode. Configures PORT registers as a side effect of claim().
//
// Sharing semantics (slice 3):
//   * Inputs: single-owner. A second slot's claim is refused with TAKEN.
//   * Outputs: shareable IFF the new claim declares the same (ok,err) pair
//     as the existing claim — otherwise VALUE_MISMATCH. Sharing is the
//     mechanism for OR-of-vetoes voting in the tick loop.
//   * Reserved pins (DAC/I2C/UART) refuse all claims.
//
// The HAL itself does NOT compute votes — interlock_tick_all() determines
// which slots are vetoing and passes a veto_mask into hal_pin_drive_outputs().
//
// Pin coordinates are the compact id from samd21_pin_table.h:
//   phys_id = (port << 5) | (pin & 0x1F)
// ============================================================================

#pragma once

#include <stdint.h>
#include "samd21_pin_table.h"

#define HAL_PIN_TABLE_SIZE   64u
#define HAL_PIN_UNCLAIMED    0xFFu

typedef enum {
    HAL_PIN_MODE_UNCLAIMED      = 0,
    HAL_PIN_MODE_GPIO_IN        = 1,
    HAL_PIN_MODE_GPIO_IN_PU     = 2,    // input + pullup
    HAL_PIN_MODE_GPIO_IN_PD     = 3,    // input + pulldown
    HAL_PIN_MODE_GPIO_OUT       = 4,
    HAL_PIN_MODE_ADC_SCAN       = 5,    // reserved for slice 4
} hal_pin_mode_t;

typedef enum {
    HAL_PIN_CLAIM_OK             = 0,
    HAL_PIN_CLAIM_NO_SUCH_PIN    = 1,
    HAL_PIN_CLAIM_RESERVED       = 2,    // statically owned (DAC/I2C/UART)
    HAL_PIN_CLAIM_TAKEN          = 3,    // currently claimed (input single-owner; or output by different mode)
    HAL_PIN_CLAIM_CAP_MISSING    = 4,    // pin doesn't support requested mode
    HAL_PIN_CLAIM_BAD_MODE       = 5,    // mode value out of range / wrong API
    HAL_PIN_CLAIM_VALUE_MISMATCH = 6,    // shared output declared with different ok/err values
} hal_pin_claim_status_t;

// ---- claim / release -----------------------------------------------------

// Claim an INPUT pin for `slot`. Output mode is refused with BAD_MODE — use
// hal_pin_claim_output() for output pins.
//
// On failure NO side effects (no register writes, no ownership recorded).
hal_pin_claim_status_t hal_pin_claim(uint8_t phys_id, uint8_t slot, hal_pin_mode_t mode);

// Claim an OUTPUT pin for `slot` with explicit ok/err values. Sharing rules:
//   * unclaimed → claim, record values
//   * already claimed by `slot` → idempotent re-config
//   * claimed by other slot as output with matching ok/err → adds this slot
//     to the share mask (returns OK)
//   * claimed by other slot as output with different ok/err → VALUE_MISMATCH
//   * claimed by other slot as input → TAKEN
//
// `ok_value` and `err_value` must be 0 or 1 and must differ; the DSL parser
// already enforces this but the HAL re-checks for defence-in-depth.
hal_pin_claim_status_t hal_pin_claim_output(uint8_t phys_id, uint8_t slot,
                                            uint8_t ok_value, uint8_t err_value);

// Release every claim held by `slot`. For shared output pins, clears just
// this slot's bit from the mask; the pin is reset to safe only when the
// last sharer releases it. Idempotent.
void                   hal_pin_release_slot(uint8_t slot);

// Inspection helpers (debug / status emission). Returns the slot bitmap
// (bit i = slot i is sharing this pin), or 0 if unclaimed.
uint8_t                hal_pin_get_owners(uint8_t phys_id);
hal_pin_mode_t         hal_pin_get_mode  (uint8_t phys_id);

// ---- runtime I/O ---------------------------------------------------------

// Read current logic level of an input pin. Returns 0/1; returns 0 if the
// pin isn't claimed or isn't configured as an input.
uint8_t                hal_pin_read    (uint8_t phys_id);

// Drive every claimed OUTPUT pin per OR-of-vetoes:
//   - if (claim.slot_mask & veto_mask) != 0 → drive err_value
//   - else                                  → drive ok_value
// `veto_mask` bit i = "slot i is currently vetoing" (TF=F). Slots that are
// EMPTY/POISONED/non-DSL have no claims and don't participate. Always
// re-asserts (idempotent), so transient PORT corruption self-heals.
void                   hal_pin_drive_outputs(uint8_t veto_mask);
