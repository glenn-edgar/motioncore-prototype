// ============================================================================
// mcp23017.c — Microchip MCP23017 driver. See mcp23017.h.
//
// All 16-bit accesses use the A register as the base and rely on BANK=0
// auto-increment: writing/reading two bytes lands A then B. Word layout is
// A in the low byte, B in the high byte.
// ============================================================================

#include "mcp23017.h"
#include "../common/i2c_bus.h"

// Write a 16-bit value to an A/B register pair (low byte -> A, high -> B).
static bool wr16(mcp23017_t *d, uint8_t reg_a, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v & 0xFFu), (uint8_t)(v >> 8) };
    return i2c_bus_write_block(d->addr, reg_a, b, 2);
}

// Read a 16-bit value from an A/B register pair.
static bool rd16(mcp23017_t *d, uint8_t reg_a, uint16_t *v) {
    uint8_t b[2];
    if (!i2c_bus_read_block(d->addr, reg_a, b, 2)) return false;
    *v = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
    return true;
}

// Shadow of the output latch so single-pin writes don't need a read-back of
// OLAT (which would also be fine, but this keeps each write to one transfer).
static uint16_t s_olat[8];   // indexed by A2A1A0 strap (addr & 7)

bool mcp23017_init(mcp23017_t *d, uint8_t addr7) {
    i2c_bus_init();
    d->addr = addr7;

    // Presence probe (address-only write).
    if (!i2c_bus_write(d->addr, 0, 0)) return false;

    // Force a known IOCON: BANK=0, sequential operation enabled (SEQOP=0),
    // INT push-pull active-low. Writing the single-byte register at 0x0A is
    // valid in both BANK modes, so this recovers a chip left in BANK=1.
    if (!i2c_bus_write_reg8(d->addr, MCP23017_REG_IOCON, 0x00)) return false;

    // All inputs, no inversion, no pull-ups, latches cleared, no IOC.
    if (!wr16(d, MCP23017_REG_IODIRA,   0xFFFFu)) return false;
    if (!wr16(d, MCP23017_REG_IPOLA,    0x0000u)) return false;
    if (!wr16(d, MCP23017_REG_GPPUA,    0x0000u)) return false;
    if (!wr16(d, MCP23017_REG_GPINTENA, 0x0000u)) return false;
    if (!wr16(d, MCP23017_REG_OLATA,    0x0000u)) return false;

    s_olat[d->addr & 0x07u] = 0x0000u;
    return true;
}

bool mcp23017_set_dir(mcp23017_t *d, uint16_t dir_mask) {
    return wr16(d, MCP23017_REG_IODIRA, dir_mask);
}

bool mcp23017_set_pullups(mcp23017_t *d, uint16_t pu_mask) {
    return wr16(d, MCP23017_REG_GPPUA, pu_mask);
}

bool mcp23017_set_polarity(mcp23017_t *d, uint16_t inv_mask) {
    return wr16(d, MCP23017_REG_IPOLA, inv_mask);
}

bool mcp23017_write(mcp23017_t *d, uint16_t value) {
    if (!wr16(d, MCP23017_REG_OLATA, value)) return false;
    s_olat[d->addr & 0x07u] = value;
    return true;
}

bool mcp23017_read(mcp23017_t *d, uint16_t *value) {
    return rd16(d, MCP23017_REG_GPIOA, value);
}

bool mcp23017_write_pin(mcp23017_t *d, uint8_t pin, bool level) {
    if (pin > 15u) return false;
    uint16_t v = s_olat[d->addr & 0x07u];
    if (level) v |=  (uint16_t)(1u << pin);
    else       v &= ~(uint16_t)(1u << pin);
    return mcp23017_write(d, v);
}

bool mcp23017_read_pin(mcp23017_t *d, uint8_t pin, bool *level) {
    if (pin > 15u) return false;
    uint16_t v;
    if (!mcp23017_read(d, &v)) return false;
    *level = ((v >> pin) & 1u) != 0u;
    return true;
}

bool mcp23017_config_interrupt(mcp23017_t *d, uint16_t en_mask,
                               uint16_t compare_default, uint16_t defval) {
    if (!wr16(d, MCP23017_REG_DEFVALA,  defval))          return false;
    if (!wr16(d, MCP23017_REG_INTCONA,  compare_default)) return false;
    if (!wr16(d, MCP23017_REG_GPINTENA, en_mask))         return false;
    return true;
}

bool mcp23017_read_interrupt(mcp23017_t *d, uint16_t *intf, uint16_t *intcap) {
    if (intf) {
        if (!rd16(d, MCP23017_REG_INTFA, intf)) return false;
    }
    if (intcap) {
        // Reading INTCAP also clears the interrupt condition.
        if (!rd16(d, MCP23017_REG_INTCAPA, intcap)) return false;
    }
    return true;
}
