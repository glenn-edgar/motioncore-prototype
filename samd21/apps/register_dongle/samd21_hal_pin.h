// ============================================================================
// samd21_hal_pin.h — HAL pin-claim + configuration API used by the interlock
// framework. Single source of truth for which slot owns which physical pin
// and in what mode. Configures PORT registers as a side effect of claim().
//
// Slice 2 (single-slot) semantics:
//   * Each physical pin is owned by at most one slot at any time.
//   * Reserved pins (DAC/I2C/UART) refuse all claims.
//   * Slice 3 will relax to typed sharing (READ stackable, WRITE shareable
//     with matching values) — the API surface won't change.
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
    HAL_PIN_CLAIM_OK            = 0,
    HAL_PIN_CLAIM_NO_SUCH_PIN   = 1,
    HAL_PIN_CLAIM_RESERVED      = 2,    // statically owned (DAC/I2C/UART)
    HAL_PIN_CLAIM_TAKEN         = 3,    // currently claimed by another slot
    HAL_PIN_CLAIM_CAP_MISSING   = 4,    // pin doesn't support requested mode
    HAL_PIN_CLAIM_BAD_MODE      = 5,    // mode value out of range
} hal_pin_claim_status_t;

// ---- claim / release -----------------------------------------------------

// Atomically: validate pin/cap/availability, record slot ownership, configure
// the PORT registers per mode. Caller does NOT need to call any other init.
//
// On failure NO side effects (no register writes, no ownership recorded).
hal_pin_claim_status_t hal_pin_claim(uint8_t phys_id, uint8_t slot, hal_pin_mode_t mode);

// Release every claim owned by `slot`. Resets affected pins to safe default
// (INEN=0, DIR=0, no pull). Idempotent.
void                   hal_pin_release_slot(uint8_t slot);

// Inspection helpers (debug / status emission).
uint8_t                hal_pin_get_owner(uint8_t phys_id);  // slot id or 0xFF
hal_pin_mode_t         hal_pin_get_mode (uint8_t phys_id);

// ---- runtime I/O ---------------------------------------------------------

// Read current logic level of an input pin. Returns 0/1; returns 0 if the
// pin isn't claimed or isn't configured as an input.
uint8_t                hal_pin_read    (uint8_t phys_id);

// Drive an output pin. No-op if the pin isn't claimed as GPIO_OUT.
void                   hal_pin_write   (uint8_t phys_id, uint8_t value);
