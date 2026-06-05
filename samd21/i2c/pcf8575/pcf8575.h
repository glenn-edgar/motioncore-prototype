// ============================================================================
// pcf8575.h — NXP/TI PCF8575 16-bit quasi-bidirectional I2C I/O expander.
//
// Unlike the MCP23017/TCA9555 there are NO internal registers and no direction
// register. The chip presents 16 "quasi-bidirectional" pins:
//
//   * Write a 1 to a pin  -> weak pull-up active; the pin can be driven LOW by
//                            an external source and then READ as an input.
//   * Write a 0 to a pin  -> strong pull-down; the pin is held LOW (output).
//
// So "configure as input" == "write 1 to that bit". There is no separate
// config register: the last value written IS the configuration. This driver
// keeps a shadow of the last written word so single-pin operations and the
// input mask compose correctly.
//
// Bus protocol is dead simple and pointer-less:
//   * Write transaction:  [P0..P7][P8..P15]   (2 data bytes, low port first)
//   * Read  transaction:  read 2 bytes        (P0..P7 then P8..P15)
//
// Word layout: bit 0 = P00 ... bit 7 = P07, bit 8 = P10 ... bit 15 = P17.
//
// 7-bit address: 0x20 | (A2 A1 A0). (The PCF8575 — note: the PCF8575C and the
// 8-bit PCF8574 share this range; verify your strap.)
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define PCF8575_ADDR_BASE   0x20u
#define PCF8575_ADDR(a)     ((uint8_t)(PCF8575_ADDR_BASE | ((a) & 0x07u)))

typedef struct {
    uint8_t  addr;       // 7-bit
    uint16_t shadow;     // last word written (also the input-enable mask)
} pcf8575_t;

// Probe the device and drive all pins HIGH (0xFFFF) — the safe "all inputs /
// released" power-on-equivalent state. Returns false if it does not ACK.
bool pcf8575_init(pcf8575_t *d, uint8_t addr7);

// Write the full 16-bit port. Bits set to 1 act as inputs/weak-high; bits set
// to 0 are driven low.
bool pcf8575_write(pcf8575_t *d, uint16_t value);

// Read the live state of all 16 pins. Pins you intend to read must currently
// have a 1 in the shadow (call pcf8575_set_input_mask first) so they aren't
// held low by the chip.
bool pcf8575_read(pcf8575_t *d, uint16_t *value);

// Convenience: write 1s to the masked pins (releasing them so they can be
// read or pulled low externally) without disturbing the others. Equivalent to
// shadow |= mask, then write.
bool pcf8575_set_input_mask(pcf8575_t *d, uint16_t mask);

// Single-pin output against the shadow. level=1 releases/raises the pin,
// level=0 drives it low.
bool pcf8575_write_pin(pcf8575_t *d, uint8_t pin, bool level);
// Single-pin read (pin should be released — shadow bit = 1 — to read external).
bool pcf8575_read_pin(pcf8575_t *d, uint8_t pin, bool *level);
