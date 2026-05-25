// ============================================================================
// samd21_pin_table.h — board-label ↔ physical-pin table for Seeeduino Xiao.
//
// The interlock-framework DSL refers to pins by their board label (D0..D10,
// A0..A10). This table is the chip-side translation layer that makes the
// SAME DSL text portable across SAMD21 / RA4M1 / ESP32-C6 Xiao-form-factor
// chips — each chip provides its own pin table; the parser is generic.
//
// "Physical pin id" packs port + pin into one byte:
//   id = (port << 5) | (pin & 0x1F)
// where port = 0 (PA) or 1 (PB) and pin = 0..31. This is the canonical
// "address" the HAL pin-claim API uses internally.
//
// Aliasing: D2 and A2 are the same physical pin in different modes (digital
// vs analog). Both labels resolve to the same physical_id, so the HAL claim
// table catches D2-as-GPIO and A2-as-ADC trying to coexist (mode collision).
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

// ---- Capability bits -----------------------------------------------------

#define BOARD_PIN_CAP_GPIO      0x01u   // can be configured as digital input/output
#define BOARD_PIN_CAP_ADC       0x02u   // has an AINx channel
#define BOARD_PIN_CAP_DAC       0x04u   // has DAC output (only D0/A0 on SAMD21)
#define BOARD_PIN_RESERVED      0x80u   // statically owned by always-on peripheral
                                        // (DAC, I2C, UART) — never claimable

// Sentinel for the adc_channel field when the pin has no ADC.
#define BOARD_PIN_ADC_NONE      0xFFu

// Compact physical pin identifier — fits in u8.
#define BOARD_PHYS_PIN(port, pin)   ((uint8_t)(((port) << 5) | ((pin) & 0x1F)))
#define BOARD_PHYS_PIN_PORT(id)     ((uint8_t)((id) >> 5))
#define BOARD_PHYS_PIN_PIN(id)      ((uint8_t)((id) & 0x1F))

// ---- Pin record ----------------------------------------------------------

typedef struct {
    char     label[4];        // "D0".."D10", "A0".."A10" — NUL-terminated
    uint8_t  port;            // 0=PA, 1=PB
    uint8_t  pin;             // 0..31
    uint8_t  adc_channel;     // AINx (0..19) or BOARD_PIN_ADC_NONE
    uint8_t  caps;            // BOARD_PIN_CAP_* | BOARD_PIN_RESERVED bits
} board_pin_t;

extern const board_pin_t g_board_pins[];
extern const uint8_t     g_board_pin_count;

// ---- Lookup helpers ------------------------------------------------------

// Resolve a label (with explicit length — caller's slice may not be
// NUL-terminated). Returns NULL if not in the table.
const board_pin_t* board_pin_lookup(const char* label, uint8_t label_len);

// Compact physical pin id from a record. Use this as the key for HAL claims;
// two different labels (D2 + A2) sharing the same physical pin must collide.
static inline uint8_t board_pin_phys_id(const board_pin_t* p) {
    return BOARD_PHYS_PIN(p->port, p->pin);
}

// Convenience: true if the record is statically reserved (DAC/I2C/UART).
static inline bool board_pin_is_reserved(const board_pin_t* p) {
    return (p->caps & BOARD_PIN_RESERVED) != 0u;
}
