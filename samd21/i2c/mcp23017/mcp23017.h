// ============================================================================
// mcp23017.h — Microchip MCP23017 16-bit I2C GPIO expander.
//
// Two 8-bit ports A (pins 0..7) and B (pins 8..15). This driver treats them as
// a single 16-bit word: bit 0 = GPA0 ... bit 7 = GPA7, bit 8 = GPB0 ...
// bit 15 = GPB7.
//
// IOCON.BANK is left at its power-on default 0 (registers interleaved A/B in
// pairs, pointer auto-increments) so a 16-bit access is a 2-byte block write/
// read starting at the A register. We also clear IOCON.SEQOP=0 (auto-increment
// enabled) explicitly in mcp23017_init().
//
// Direction convention follows the chip: IODIR bit = 1 -> INPUT, 0 -> OUTPUT
// (power-on default is all inputs). Pull-ups are 100k, input-only, via GPPU.
//
// 7-bit address: 0x20 | (A2 A1 A0). Up to 8 on one bus.
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MCP23017_ADDR_BASE   0x20u
// Compose the 7-bit address from the A2/A1/A0 strap pins (0..7).
#define MCP23017_ADDR(a)     ((uint8_t)(MCP23017_ADDR_BASE | ((a) & 0x07u)))

// Register map (IOCON.BANK = 0). A reg even, B reg odd.
enum {
    MCP23017_REG_IODIRA   = 0x00, MCP23017_REG_IODIRB   = 0x01,
    MCP23017_REG_IPOLA    = 0x02, MCP23017_REG_IPOLB    = 0x03,
    MCP23017_REG_GPINTENA = 0x04, MCP23017_REG_GPINTENB = 0x05,
    MCP23017_REG_DEFVALA  = 0x06, MCP23017_REG_DEFVALB  = 0x07,
    MCP23017_REG_INTCONA  = 0x08, MCP23017_REG_INTCONB  = 0x09,
    MCP23017_REG_IOCON    = 0x0A, // (also mirrored at 0x0B)
    MCP23017_REG_GPPUA    = 0x0C, MCP23017_REG_GPPUB    = 0x0D,
    MCP23017_REG_INTFA    = 0x0E, MCP23017_REG_INTFB    = 0x0F,
    MCP23017_REG_INTCAPA  = 0x10, MCP23017_REG_INTCAPB  = 0x11,
    MCP23017_REG_GPIOA    = 0x12, MCP23017_REG_GPIOB    = 0x13,
    MCP23017_REG_OLATA    = 0x14, MCP23017_REG_OLATB    = 0x15,
};

// IOCON bits.
#define MCP23017_IOCON_BANK   (1u << 7)
#define MCP23017_IOCON_MIRROR (1u << 6)  // OR the two INT pins together
#define MCP23017_IOCON_SEQOP  (1u << 5)  // 1 = disable address auto-increment
#define MCP23017_IOCON_DISSLW (1u << 4)
#define MCP23017_IOCON_HAEN   (1u << 3)  // (MCP23S17 only)
#define MCP23017_IOCON_ODR    (1u << 2)  // INT pin open-drain
#define MCP23017_IOCON_INTPOL (1u << 1)  // INT pin active-high when set

typedef struct {
    uint8_t addr;   // 7-bit, e.g. MCP23017_ADDR(0)
} mcp23017_t;

// Initialise: confirm presence, force BANK=0 / SEQOP=0, all pins input, no
// pull-ups, output latch cleared. Returns false if the chip does not ACK.
bool mcp23017_init(mcp23017_t *d, uint8_t addr7);

// Direction: 1 bit = INPUT, 0 bit = OUTPUT (matches the chip's IODIR).
bool mcp23017_set_dir(mcp23017_t *d, uint16_t dir_mask);
// 1 bit = enable the 100k pull-up on that (input) pin.
bool mcp23017_set_pullups(mcp23017_t *d, uint16_t pu_mask);
// 1 bit = invert the polarity reported by mcp23017_read().
bool mcp23017_set_polarity(mcp23017_t *d, uint16_t inv_mask);

// Drive all 16 output latches at once (OLATA/OLATB). Bits mapped to input
// pins are stored but have no effect until the pin becomes an output.
bool mcp23017_write(mcp23017_t *d, uint16_t value);
// Read the live pin state (GPIOA/GPIOB).
bool mcp23017_read(mcp23017_t *d, uint16_t *value);

// Read-modify-write a single output pin (0..15) against the shadow latch.
bool mcp23017_write_pin(mcp23017_t *d, uint8_t pin, bool level);
// Read a single live pin level.
bool mcp23017_read_pin(mcp23017_t *d, uint8_t pin, bool *level);

// --- interrupt-on-change ----------------------------------------------------
// en_mask:  1 = enable IOC on that pin.
// compare_default: 1 = compare against DEFVAL, 0 = compare against previous
//                  value. defval supplies the DEFVAL bits used where
//                  compare_default is set.
bool mcp23017_config_interrupt(mcp23017_t *d, uint16_t en_mask,
                               uint16_t compare_default, uint16_t defval);
// INTF: which pins triggered. INTCAP: captured port value at trigger time
// (reading INTCAP clears the interrupt). Either out pointer may be NULL.
bool mcp23017_read_interrupt(mcp23017_t *d, uint16_t *intf, uint16_t *intcap);
