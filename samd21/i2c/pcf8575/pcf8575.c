// ============================================================================
// pcf8575.c — NXP/TI PCF8575 driver. See pcf8575.h.
//
// No register pointer: writes are 2 raw data bytes (low port first), reads are
// 2 raw data bytes. The shadow word doubles as the input-enable mask because
// on a quasi-bidirectional pin a written 1 is exactly "released for input".
// ============================================================================

#include "pcf8575.h"
#include "../common/i2c_bus.h"

bool pcf8575_init(pcf8575_t *d, uint8_t addr7) {
    i2c_bus_init();
    d->addr = addr7;
    // Drive all pins high (released). This is also the chip's power-on state.
    return pcf8575_write(d, 0xFFFFu);
}

bool pcf8575_write(pcf8575_t *d, uint16_t value) {
    uint8_t b[2] = { (uint8_t)(value & 0xFFu), (uint8_t)(value >> 8) };
    if (!i2c_bus_write(d->addr, b, 2)) return false;
    d->shadow = value;
    return true;
}

bool pcf8575_read(pcf8575_t *d, uint16_t *value) {
    uint8_t b[2];
    if (!i2c_bus_read(d->addr, b, 2)) return false;
    *value = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
    return true;
}

bool pcf8575_set_input_mask(pcf8575_t *d, uint16_t mask) {
    return pcf8575_write(d, d->shadow | mask);
}

bool pcf8575_write_pin(pcf8575_t *d, uint8_t pin, bool level) {
    if (pin > 15u) return false;
    uint16_t v = d->shadow;
    if (level) v |=  (uint16_t)(1u << pin);
    else       v &= ~(uint16_t)(1u << pin);
    return pcf8575_write(d, v);
}

bool pcf8575_read_pin(pcf8575_t *d, uint8_t pin, bool *level) {
    if (pin > 15u) return false;
    uint16_t v;
    if (!pcf8575_read(d, &v)) return false;
    *level = ((v >> pin) & 1u) != 0u;
    return true;
}
