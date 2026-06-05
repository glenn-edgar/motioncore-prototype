// ============================================================================
// i2c_bus.h — SAMD21 SERCOM2 I2C master transport (register level).
//
// This is the shared bus layer the per-chip drivers in ../<chip>/ sit on top
// of. It owns nothing chip-specific: it brings up SERCOM2 as an I2C master on
//
//     D4 = PA08  -> SDA  (SERCOM2 PAD[0], PMUX function D)
//     D5 = PA09  -> SCL  (SERCOM2 PAD[1], PMUX function D)
//
// 100 kHz, polling, smart-mode OFF so error paths (NACK / bus error / arb
// lost) can issue STOP cleanly. Lifted from the register_dongle app's I2C
// block (samd21_commands.c) and given a bounded busy-wait so a missing slave
// returns an error instead of wedging the CPU.
//
// All transfers are 7-bit-address. Returns are bool: true = ACKed/complete,
// false = NACK, bus error, arbitration lost, or timeout. On any failure the
// bus is left in a STOPped state, so the next call starts clean.
//
// f_GCLK = 48 MHz (GCLK0). BAUD = (f_GCLK / (2*f_SCL)) - 5 = 235 for 100 kHz.
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Bring up SERCOM2 + pin mux. Idempotent; safe to call more than once. If the
// register_dongle app already initialised SERCOM2 via samd21_peripherals_init(),
// you do NOT need to call this — the bus is shared.
void i2c_bus_init(void);

// START + addr(W) + len bytes + STOP.  len may be 0 (address-only probe).
bool i2c_bus_write(uint8_t addr7, const uint8_t *data, size_t len);

// START + addr(R) + read len bytes (NACK on last) + STOP. len >= 1.
bool i2c_bus_read(uint8_t addr7, uint8_t *data, size_t len);

// START + addr(W) + wlen bytes + repeated-START + addr(R) + rlen bytes + STOP.
// The canonical "set register pointer, then read" sensor pattern. wlen >= 1,
// rlen >= 1.
bool i2c_bus_write_read(uint8_t addr7,
                        const uint8_t *wbuf, size_t wlen,
                        uint8_t *rbuf, size_t rlen);

// --- register convenience helpers (8-bit register pointer) ----------------
// These cover the common "register-mapped" expanders (MCP23017, TCA9555,
// SX1509, ADS1115). PCF8575 has no register pointer and uses the raw
// read/write calls above.

// Write one 8-bit register: [reg][val].
bool i2c_bus_write_reg8(uint8_t addr7, uint8_t reg, uint8_t val);

// Read one 8-bit register: write [reg], repeated-start, read 1.
bool i2c_bus_read_reg8(uint8_t addr7, uint8_t reg, uint8_t *val);

// Write a 16-bit register MSB-first: [reg][hi][lo].
bool i2c_bus_write_reg16(uint8_t addr7, uint8_t reg, uint16_t val);

// Read a 16-bit register MSB-first: write [reg], repeated-start, read 2.
bool i2c_bus_read_reg16(uint8_t addr7, uint8_t reg, uint16_t *val);

// Write [reg] then a block of n bytes (auto-increment register pointer).
bool i2c_bus_write_block(uint8_t addr7, uint8_t reg, const uint8_t *data, size_t n);

// Write [reg], repeated-start, read n bytes (auto-increment register pointer).
bool i2c_bus_read_block(uint8_t addr7, uint8_t reg, uint8_t *data, size_t n);

// START + addr(W) + prefix + data[len] + STOP, all in one transaction and
// STREAMED (no scratch copy), so len may be large. This is the "control byte
// then payload" shape used by display controllers (e.g. SSD1306: prefix 0x40
// for a RAM data burst, 0x00 for a command burst). len may be 0.
bool i2c_bus_write_prefixed(uint8_t addr7, uint8_t prefix,
                            const uint8_t *data, size_t len);
