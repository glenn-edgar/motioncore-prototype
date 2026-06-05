// ============================================================================
// sx1509.c — Semtech SX1509 driver. See sx1509.h.
//
// 16-bit register pairs are accessed as a 2-byte block starting at the B
// register: byte[0] -> B (IO8..IO15, the high byte of our word), byte[1] -> A
// (IO0..IO7, the low byte). Auto-increment is on by default (RegMisc bit3 = 0).
// ============================================================================

#include "sx1509.h"
#include "../common/i2c_bus.h"

// RegClock 0x1E: OscSource [6:5] = 0b10 -> internal 2 MHz oscillator.
#define SX1509_CLOCK_INTERNAL_2MHZ   (0x2u << 5)

// Write a 16-bit word to a B/A register pair (high byte -> B, low byte -> A).
static bool wr16(sx1509_t *d, uint8_t reg_b, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)(v & 0xFFu) };
    return i2c_bus_write_block(d->addr, reg_b, b, 2);
}

static bool rd16(sx1509_t *d, uint8_t reg_b, uint16_t *v) {
    uint8_t b[2];
    if (!i2c_bus_read_block(d->addr, reg_b, b, 2)) return false;
    *v = ((uint16_t)b[0] << 8) | b[1];
    return true;
}

// RegIOn (intensity) address for a pin. Pins 0-3 / 8-11 have 3-register blocks
// (TOn, IOn, Off); pins 4-7 / 12-15 have 5-register blocks (TOn, IOn, Off,
// TRise, TFall). IOn is always the 2nd register of the block.
static uint8_t led_ion_reg(uint8_t pin) {
    if (pin <= 3u)  return (uint8_t)(0x29u + pin * 3u + 1u);
    if (pin <= 7u)  return (uint8_t)(0x35u + (pin - 4u) * 5u + 1u);
    if (pin <= 11u) return (uint8_t)(0x49u + (pin - 8u) * 3u + 1u);
    return            (uint8_t)(0x55u + (pin - 12u) * 5u + 1u);
}

// RegTOn (on-time) is the 1st register of the block; RegOff is the 3rd.
static uint8_t led_ton_reg(uint8_t pin) {
    if (pin <= 3u)  return (uint8_t)(0x29u + pin * 3u);
    if (pin <= 7u)  return (uint8_t)(0x35u + (pin - 4u) * 5u);
    if (pin <= 11u) return (uint8_t)(0x49u + (pin - 8u) * 3u);
    return            (uint8_t)(0x55u + (pin - 12u) * 5u);
}

bool sx1509_init(sx1509_t *d, uint8_t addr7) {
    i2c_bus_init();
    d->addr = addr7;

    // Software reset: 0x12 then 0x34 to RegReset.
    if (!i2c_bus_write_reg8(d->addr, SX1509_REG_RESET, 0x12u)) return false;
    if (!i2c_bus_write_reg8(d->addr, SX1509_REG_RESET, 0x34u)) return false;

    // Post-reset defaults: all inputs, data high.
    d->dir_shadow  = 0xFFFFu;
    d->data_shadow = 0xFFFFu;
    if (!rd16(d, SX1509_REG_DIR_B,  &d->dir_shadow))  return false;
    if (!rd16(d, SX1509_REG_DATA_B, &d->data_shadow)) return false;
    return true;
}

bool sx1509_set_dir(sx1509_t *d, uint16_t dir_mask) {
    if (!wr16(d, SX1509_REG_DIR_B, dir_mask)) return false;
    d->dir_shadow = dir_mask;
    return true;
}

bool sx1509_set_pullups(sx1509_t *d, uint16_t mask) {
    return wr16(d, SX1509_REG_PULLUP_B, mask);
}

bool sx1509_set_pulldowns(sx1509_t *d, uint16_t mask) {
    return wr16(d, SX1509_REG_PULLDOWN_B, mask);
}

bool sx1509_set_open_drain(sx1509_t *d, uint16_t mask) {
    return wr16(d, SX1509_REG_OPEN_DRAIN_B, mask);
}

bool sx1509_set_input_disable(sx1509_t *d, uint16_t mask) {
    return wr16(d, SX1509_REG_INPUT_DISABLE_B, mask);
}

bool sx1509_write(sx1509_t *d, uint16_t value) {
    if (!wr16(d, SX1509_REG_DATA_B, value)) return false;
    d->data_shadow = value;
    return true;
}

bool sx1509_read(sx1509_t *d, uint16_t *value) {
    return rd16(d, SX1509_REG_DATA_B, value);
}

bool sx1509_write_pin(sx1509_t *d, uint8_t pin, bool level) {
    if (pin > 15u) return false;
    uint16_t v = d->data_shadow;
    if (level) v |=  (uint16_t)(1u << pin);
    else       v &= ~(uint16_t)(1u << pin);
    return sx1509_write(d, v);
}

bool sx1509_read_pin(sx1509_t *d, uint8_t pin, bool *level) {
    if (pin > 15u) return false;
    uint16_t v;
    if (!sx1509_read(d, &v)) return false;
    *level = ((v >> pin) & 1u) != 0u;
    return true;
}

bool sx1509_led_driver_init(sx1509_t *d, uint8_t led_clk_div,
                            bool log_b, bool log_a) {
    if (led_clk_div < 1u || led_clk_div > 7u) return false;

    // RegClock: enable the internal 2 MHz oscillator as the clock source.
    if (!i2c_bus_write_reg8(d->addr, SX1509_REG_CLOCK,
                            SX1509_CLOCK_INTERNAL_2MHZ)) return false;

    // RegMisc: ClkX freq [6:4] = led_clk_div (LED clock = fOSC / 2^(div-1)),
    // log/linear per port (bit7 = port B, bit3 = port A). AutoInc stays on
    // because RegMisc bit3 here is the port-A log flag... no: bit3 in RegMisc
    // is "LED driver mode A (log)". Auto-increment-disable is a separate bit
    // and we never set it. Build the byte explicitly:
    uint8_t misc = (uint8_t)((led_clk_div & 0x7u) << 4);
    if (log_b) misc |= (1u << 7);
    if (log_a) misc |= (1u << 3);
    if (!i2c_bus_write_reg8(d->addr, SX1509_REG_MISC, misc)) return false;

    return true;
}

bool sx1509_led_setup_pin(sx1509_t *d, uint8_t pin) {
    if (pin > 15u) return false;
    uint16_t bit = (uint16_t)(1u << pin);

    // Disable the input buffer (recommended for LED-driver / open-drain pins).
    uint16_t indis;
    if (!rd16(d, SX1509_REG_INPUT_DISABLE_B, &indis)) return false;
    if (!sx1509_set_input_disable(d, indis | bit)) return false;

    // Pin must be an output.
    if (!sx1509_set_dir(d, d->dir_shadow & (uint16_t)~bit)) return false;

    // Enable the LED driver on this pin.
    uint16_t en;
    if (!rd16(d, SX1509_REG_LED_DRV_EN_B, &en)) return false;
    if (!wr16(d, SX1509_REG_LED_DRV_EN_B, en | bit)) return false;

    // RegData bit must be 0 for the LED driver to source the PWM (data=1 forces
    // the output high / LED off in the sink wiring).
    if (!sx1509_write_pin(d, pin, false)) return false;

    return true;
}

bool sx1509_led_set_intensity(sx1509_t *d, uint8_t pin, uint8_t intensity) {
    if (pin > 15u) return false;
    return i2c_bus_write_reg8(d->addr, led_ion_reg(pin), intensity);
}

bool sx1509_led_blink(sx1509_t *d, uint8_t pin,
                      uint8_t on_time, uint8_t off_time, uint8_t on_intensity) {
    if (pin > 15u) return false;
    uint8_t ton = led_ton_reg(pin);
    // RegTOn (block+0): on-time, 5 bits.
    if (!i2c_bus_write_reg8(d->addr, ton, (uint8_t)(on_time & 0x1Fu)))  return false;
    // RegIOn (block+1): on intensity.
    if (!i2c_bus_write_reg8(d->addr, (uint8_t)(ton + 1u), on_intensity)) return false;
    // RegOff (block+2): [7:3] off-time, [2:0] off-intensity (leave 0).
    if (!i2c_bus_write_reg8(d->addr, (uint8_t)(ton + 2u),
                            (uint8_t)((off_time & 0x1Fu) << 3))) return false;
    return true;
}
