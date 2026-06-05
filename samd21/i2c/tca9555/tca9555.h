// ============================================================================
// tca9555.h — Texas Instruments TCA9555 16-bit I2C GPIO expander.
//
// Register-compatible with the NXP PCA9555. Eight registers, command byte =
// register index, pointer auto-increments (and wraps within 0..7), so a
// 16-bit access is a 2-byte block starting at the port-0 register.
//
//   0  Input port 0      (read-only, live pin state, pins 0..7)
//   1  Input port 1      (read-only, pins 8..15)
//   2  Output port 0     (drive value when configured as output)
//   3  Output port 1
//   4  Polarity inv 0    (1 = invert the value returned by a read)
//   5  Polarity inv 1
//   6  Configuration 0   (1 = INPUT, 0 = OUTPUT;  power-on = all inputs)
//   7  Configuration 1
//
// Word layout: bit 0 = P0_0 ... bit 7 = P0_7, bit 8 = P1_0 ... bit 15 = P1_7.
//
// Direction convention follows the chip: config bit = 1 -> INPUT, 0 -> OUTPUT.
// (Note this is the OPPOSITE of the TCA9555 having no internal pull-ups — pins
// float when configured as inputs, so add external pulls.)
//
// 7-bit address: 0x20 | (A2 A1 A0). Up to 8 on one bus. Shares the 0x20..0x27
// range with MCP23017/PCF8575 — don't mix the same strap on one bus.
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define TCA9555_ADDR_BASE   0x20u
#define TCA9555_ADDR(a)     ((uint8_t)(TCA9555_ADDR_BASE | ((a) & 0x07u)))

enum {
    TCA9555_REG_INPUT0  = 0x00,
    TCA9555_REG_INPUT1  = 0x01,
    TCA9555_REG_OUTPUT0 = 0x02,
    TCA9555_REG_OUTPUT1 = 0x03,
    TCA9555_REG_POLINV0 = 0x04,
    TCA9555_REG_POLINV1 = 0x05,
    TCA9555_REG_CONFIG0 = 0x06,
    TCA9555_REG_CONFIG1 = 0x07,
};

typedef struct {
    uint8_t  addr;     // 7-bit
    uint16_t out_shadow;  // mirrors the Output port registers for RMW pin ops
} tca9555_t;

// Probe + set a known state: all inputs, no polarity inversion, output
// register cleared. Returns false if the chip does not ACK.
bool tca9555_init(tca9555_t *d, uint8_t addr7);

// 1 bit = INPUT, 0 bit = OUTPUT (matches the chip config register).
bool tca9555_set_dir(tca9555_t *d, uint16_t dir_mask);
// 1 bit = invert that pin in subsequent reads.
bool tca9555_set_polarity(tca9555_t *d, uint16_t inv_mask);

// Drive all 16 output registers. Stored for input-configured pins; takes
// effect when the pin is switched to output.
bool tca9555_write(tca9555_t *d, uint16_t value);
// Read the live input registers (after polarity inversion).
bool tca9555_read(tca9555_t *d, uint16_t *value);

// Single-pin RMW against the output shadow / single-pin read.
bool tca9555_write_pin(tca9555_t *d, uint8_t pin, bool level);
bool tca9555_read_pin(tca9555_t *d, uint8_t pin, bool *level);
