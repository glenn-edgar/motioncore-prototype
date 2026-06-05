// ============================================================================
// tca9555.c — TI TCA9555 driver. See tca9555.h.
//
// Word layout: port-0 register in the low byte, port-1 in the high byte. The
// command-byte pointer auto-increments, so each 16-bit op is one [reg][lo][hi]
// write or a [reg]+read-2.
// ============================================================================

#include "tca9555.h"
#include "../common/i2c_bus.h"

static bool wr16(tca9555_t *d, uint8_t reg0, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v & 0xFFu), (uint8_t)(v >> 8) };
    return i2c_bus_write_block(d->addr, reg0, b, 2);
}

static bool rd16(tca9555_t *d, uint8_t reg0, uint16_t *v) {
    uint8_t b[2];
    if (!i2c_bus_read_block(d->addr, reg0, b, 2)) return false;
    *v = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
    return true;
}

bool tca9555_init(tca9555_t *d, uint8_t addr7) {
    i2c_bus_init();
    d->addr = addr7;
    d->out_shadow = 0x0000u;

    if (!i2c_bus_write(d->addr, 0, 0)) return false;   // presence probe

    if (!wr16(d, TCA9555_REG_OUTPUT0, 0x0000u)) return false;
    if (!wr16(d, TCA9555_REG_POLINV0, 0x0000u)) return false;
    if (!wr16(d, TCA9555_REG_CONFIG0, 0xFFFFu)) return false;  // all inputs
    return true;
}

bool tca9555_set_dir(tca9555_t *d, uint16_t dir_mask) {
    return wr16(d, TCA9555_REG_CONFIG0, dir_mask);
}

bool tca9555_set_polarity(tca9555_t *d, uint16_t inv_mask) {
    return wr16(d, TCA9555_REG_POLINV0, inv_mask);
}

bool tca9555_write(tca9555_t *d, uint16_t value) {
    if (!wr16(d, TCA9555_REG_OUTPUT0, value)) return false;
    d->out_shadow = value;
    return true;
}

bool tca9555_read(tca9555_t *d, uint16_t *value) {
    return rd16(d, TCA9555_REG_INPUT0, value);
}

bool tca9555_write_pin(tca9555_t *d, uint8_t pin, bool level) {
    if (pin > 15u) return false;
    uint16_t v = d->out_shadow;
    if (level) v |=  (uint16_t)(1u << pin);
    else       v &= ~(uint16_t)(1u << pin);
    return tca9555_write(d, v);
}

bool tca9555_read_pin(tca9555_t *d, uint8_t pin, bool *level) {
    if (pin > 15u) return false;
    uint16_t v;
    if (!tca9555_read(d, &v)) return false;
    *level = ((v >> pin) & 1u) != 0u;
    return true;
}
