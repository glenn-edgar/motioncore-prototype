// ============================================================================
// ads1115.c — TI ADS1115 driver. See ads1115.h.
//
// All registers are 16-bit, MSB-first, so the i2c_bus_*_reg16 helpers do the
// byte ordering. Single-shot flow: write Config with OS=1, poll OS=1 (ready),
// read Conversion.
// ============================================================================

#include "ads1115.h"
#include "../common/i2c_bus.h"

// Config register field positions.
#define ADS1115_CFG_OS_SINGLE   (1u << 15)  // write 1 = start single conversion
#define ADS1115_CFG_OS_READY    (1u << 15)  // read 1 = not currently converting
#define ADS1115_CFG_MUX_POS     12
#define ADS1115_CFG_PGA_POS     9
#define ADS1115_CFG_MODE_SINGLE (1u << 8)   // single-shot (default)
#define ADS1115_CFG_DR_POS      5
#define ADS1115_CFG_COMP_QUE_DISABLE 0x3u   // [1:0] = 11 -> comparator off

// Bounded poll for conversion-complete. The slowest data rate (8 SPS) takes
// ~125 ms; this many read-Config round-trips comfortably covers it.
#define ADS1115_POLL_MAX  2000u

float ads1115_pga_fullscale(ads1115_pga_t pga) {
    switch (pga) {
        case ADS1115_PGA_6_144V: return 6.144f;
        case ADS1115_PGA_4_096V: return 4.096f;
        case ADS1115_PGA_2_048V: return 2.048f;
        case ADS1115_PGA_1_024V: return 1.024f;
        case ADS1115_PGA_0_512V: return 0.512f;
        case ADS1115_PGA_0_256V: return 0.256f;
        default:                 return 2.048f;
    }
}

bool ads1115_init(ads1115_t *d, uint8_t addr7) {
    i2c_bus_init();
    d->addr = addr7;
    d->pga  = ADS1115_PGA_2_048V;
    d->dr   = ADS1115_DR_128;

    // Presence probe: read the Config register (always responds).
    uint16_t cfg;
    return i2c_bus_read_reg16(d->addr, ADS1115_REG_CONFIG, &cfg);
}

void ads1115_set_pga(ads1115_t *d, ads1115_pga_t pga) { d->pga = pga; }
void ads1115_set_data_rate(ads1115_t *d, ads1115_dr_t dr) { d->dr = dr; }

bool ads1115_read_raw(ads1115_t *d, ads1115_mux_t mux, int16_t *raw) {
    uint16_t cfg =
        ADS1115_CFG_OS_SINGLE
      | ((uint16_t)(mux   & 0x7u) << ADS1115_CFG_MUX_POS)
      | ((uint16_t)(d->pga & 0x7u) << ADS1115_CFG_PGA_POS)
      | ADS1115_CFG_MODE_SINGLE
      | ((uint16_t)(d->dr & 0x7u) << ADS1115_CFG_DR_POS)
      | ADS1115_CFG_COMP_QUE_DISABLE;

    if (!i2c_bus_write_reg16(d->addr, ADS1115_REG_CONFIG, cfg)) return false;

    // Poll OS until the conversion is done (OS reads 1 when idle/ready).
    uint16_t status = 0;
    uint32_t tries  = ADS1115_POLL_MAX;
    do {
        if (--tries == 0u) return false;
        if (!i2c_bus_read_reg16(d->addr, ADS1115_REG_CONFIG, &status)) return false;
    } while ((status & ADS1115_CFG_OS_READY) == 0u);

    uint16_t conv;
    if (!i2c_bus_read_reg16(d->addr, ADS1115_REG_CONVERSION, &conv)) return false;
    *raw = (int16_t)conv;   // two's-complement 16-bit code
    return true;
}

bool ads1115_read_volts(ads1115_t *d, ads1115_mux_t mux, float *volts) {
    int16_t raw;
    if (!ads1115_read_raw(d, mux, &raw)) return false;
    *volts = ((float)raw / 32768.0f) * ads1115_pga_fullscale(d->pga);
    return true;
}
