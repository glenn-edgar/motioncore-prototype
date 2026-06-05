// ============================================================================
// ads1115.h — TI ADS1115 16-bit I2C delta-sigma ADC.
//
// The TI ADS1115 — a 16-bit, 4-channel single-ended / 2-channel differential
// ADC with a programmable gain amp and a built-in comparator. Not a GPIO
// expander, but it shares the bus.
//
// Four 16-bit, MSB-first registers selected by an address pointer byte:
//   0x00 Conversion  (read-only, signed 16-bit result)
//   0x01 Config      (mux, PGA, mode, data rate, comparator)
//   0x02 Lo_thresh
//   0x03 Hi_thresh
//
// This driver does blocking single-shot reads: write Config with OS=1 to start,
// poll OS in Config until the conversion completes, read Conversion. Convert to
// volts with the PGA full-scale range.
//
// 7-bit address from the ADDR pin: GND=0x48, VDD=0x49, SDA=0x4A, SCL=0x4B.
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define ADS1115_ADDR_GND   0x48u
#define ADS1115_ADDR_VDD   0x49u
#define ADS1115_ADDR_SDA   0x4Au
#define ADS1115_ADDR_SCL   0x4Bu

enum {
    ADS1115_REG_CONVERSION = 0x00,
    ADS1115_REG_CONFIG     = 0x01,
    ADS1115_REG_LO_THRESH  = 0x02,
    ADS1115_REG_HI_THRESH  = 0x03,
};

// MUX [14:12] — input multiplexer.
typedef enum {
    ADS1115_MUX_DIFF_0_1 = 0x0,  // AIN0 - AIN1 (default)
    ADS1115_MUX_DIFF_0_3 = 0x1,  // AIN0 - AIN3
    ADS1115_MUX_DIFF_1_3 = 0x2,  // AIN1 - AIN3
    ADS1115_MUX_DIFF_2_3 = 0x3,  // AIN2 - AIN3
    ADS1115_MUX_SINGLE_0 = 0x4,  // AIN0 - GND
    ADS1115_MUX_SINGLE_1 = 0x5,  // AIN1 - GND
    ADS1115_MUX_SINGLE_2 = 0x6,  // AIN2 - GND
    ADS1115_MUX_SINGLE_3 = 0x7,  // AIN3 - GND
} ads1115_mux_t;

// PGA [11:9] — full-scale range. LSB size = FSR / 32768.
typedef enum {
    ADS1115_PGA_6_144V = 0x0,
    ADS1115_PGA_4_096V = 0x1,
    ADS1115_PGA_2_048V = 0x2,   // default
    ADS1115_PGA_1_024V = 0x3,
    ADS1115_PGA_0_512V = 0x4,
    ADS1115_PGA_0_256V = 0x5,
} ads1115_pga_t;

// DR [7:5] — data rate (samples/s). Higher = faster convert, more noise.
typedef enum {
    ADS1115_DR_8   = 0x0,
    ADS1115_DR_16  = 0x1,
    ADS1115_DR_32  = 0x2,
    ADS1115_DR_64  = 0x3,
    ADS1115_DR_128 = 0x4,   // default
    ADS1115_DR_250 = 0x5,
    ADS1115_DR_475 = 0x6,
    ADS1115_DR_860 = 0x7,
} ads1115_dr_t;

typedef struct {
    uint8_t       addr;   // 7-bit
    ads1115_pga_t pga;    // remembered so reads can convert to volts
    ads1115_dr_t  dr;
} ads1115_t;

// Probe the device and store default PGA (±2.048 V) and data rate (128 SPS).
// Returns false if it does not ACK. Leaves the chip in power-down single-shot.
bool ads1115_init(ads1115_t *d, uint8_t addr7);

// Pick the gain / data rate used by subsequent single-shot reads.
void ads1115_set_pga(ads1115_t *d, ads1115_pga_t pga);
void ads1115_set_data_rate(ads1115_t *d, ads1115_dr_t dr);

// Blocking single-shot read of the given mux. Returns the signed raw code in
// *raw (-32768..32767). Polls the OS bit until conversion completes (bounded;
// returns false on bus error / timeout).
bool ads1115_read_raw(ads1115_t *d, ads1115_mux_t mux, int16_t *raw);

// Same, converted to volts using the configured PGA full-scale.
bool ads1115_read_volts(ads1115_t *d, ads1115_mux_t mux, float *volts);

// Full-scale (volts) for a PGA setting — handy for callers doing their own
// scaling. e.g. ADS1115_PGA_2_048V -> 2.048f.
float ads1115_pga_fullscale(ads1115_pga_t pga);
