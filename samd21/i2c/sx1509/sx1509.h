// ============================================================================
// sx1509.h — Semtech SX1509 16-channel I2C I/O expander + LED driver.
//
// The Semtech SX1509 (as used on the SparkFun breakout). Beyond plain GPIO
// this chip has an internal
// 2 MHz oscillator, per-pin pull-up/pull-down/open-drain, debounce, a keypad
// scanner, and a 256-step PWM "LED driver" with hardware breathe ramps. This
// driver covers GPIO + the LED driver (intensity + blink/breathe); the keypad
// engine is left out (add later if needed).
//
// Register pairs are <name>B (port B = IO8..IO15) at the lower/even address and
// <name>A (port A = IO0..IO7) at the next/odd address. With auto-increment
// (default) a 16-bit access is a 2-byte block starting at the B register, byte
// order [B][A]. Word layout in this driver: bit 0 = IO0 ... bit 15 = IO15.
//
// Direction convention follows the chip: RegDir bit = 1 -> INPUT, 0 -> OUTPUT
// (power-on default = all inputs). The LED driver SINKS current: an LED wired
// from V+ through a resistor to the pin lights when the pin is driven low /
// PWM-on.
//
// 7-bit address from ADDR1:ADDR0 straps — note these are NOT contiguous:
//   00 = 0x3E   01 = 0x3F   10 = 0x70   11 = 0x71
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define SX1509_ADDR_00   0x3Eu
#define SX1509_ADDR_01   0x3Fu
#define SX1509_ADDR_10   0x70u
#define SX1509_ADDR_11   0x71u

// Register map (B = lower address of each pair).
enum {
    SX1509_REG_INPUT_DISABLE_B = 0x00, SX1509_REG_INPUT_DISABLE_A = 0x01,
    SX1509_REG_LONG_SLEW_B     = 0x02, SX1509_REG_LONG_SLEW_A     = 0x03,
    SX1509_REG_LOW_DRIVE_B     = 0x04, SX1509_REG_LOW_DRIVE_A     = 0x05,
    SX1509_REG_PULLUP_B        = 0x06, SX1509_REG_PULLUP_A        = 0x07,
    SX1509_REG_PULLDOWN_B      = 0x08, SX1509_REG_PULLDOWN_A      = 0x09,
    SX1509_REG_OPEN_DRAIN_B    = 0x0A, SX1509_REG_OPEN_DRAIN_A    = 0x0B,
    SX1509_REG_POLARITY_B      = 0x0C, SX1509_REG_POLARITY_A      = 0x0D,
    SX1509_REG_DIR_B           = 0x0E, SX1509_REG_DIR_A           = 0x0F,
    SX1509_REG_DATA_B          = 0x10, SX1509_REG_DATA_A          = 0x11,
    SX1509_REG_INT_MASK_B      = 0x12, SX1509_REG_INT_MASK_A      = 0x13,
    SX1509_REG_CLOCK           = 0x1E,
    SX1509_REG_MISC            = 0x1F,
    SX1509_REG_LED_DRV_EN_B    = 0x20, SX1509_REG_LED_DRV_EN_A    = 0x21,
    SX1509_REG_RESET           = 0x7D,
};

typedef struct {
    uint8_t  addr;        // 7-bit, one of SX1509_ADDR_*
    uint16_t dir_shadow;  // mirrors RegDir (1=input)
    uint16_t data_shadow; // mirrors RegData
} sx1509_t;

// Software-reset (0x12,0x34 to RegReset), probe, capture default shadows.
// Returns false if the chip does not ACK. Leaves all pins as inputs.
bool sx1509_init(sx1509_t *d, uint8_t addr7);

// --- plain GPIO -------------------------------------------------------------
bool sx1509_set_dir(sx1509_t *d, uint16_t dir_mask);      // 1=input, 0=output
bool sx1509_set_pullups(sx1509_t *d, uint16_t mask);      // 1=enable pull-up
bool sx1509_set_pulldowns(sx1509_t *d, uint16_t mask);    // 1=enable pull-down
bool sx1509_set_open_drain(sx1509_t *d, uint16_t mask);   // 1=open-drain output
bool sx1509_set_input_disable(sx1509_t *d, uint16_t mask);// 1=disable in-buffer

bool sx1509_write(sx1509_t *d, uint16_t value);           // drive output regs
bool sx1509_read(sx1509_t *d, uint16_t *value);           // read live pins

bool sx1509_write_pin(sx1509_t *d, uint8_t pin, bool level);
bool sx1509_read_pin(sx1509_t *d, uint8_t pin, bool *level);

// --- LED driver -------------------------------------------------------------
// Bring up the internal 2 MHz oscillator and the LED-driver clock. Must be
// called once before any sx1509_led_* call. led_clk_div sets the PWM/breathe
// time base: ClkX = 2 MHz / 2^(led_clk_div-1); pass 1 for the fastest. log_b/
// log_a select logarithmic (true) vs linear (false) intensity ramps per port.
bool sx1509_led_driver_init(sx1509_t *d, uint8_t led_clk_div,
                            bool log_b, bool log_a);

// Configure one pin as an LED-driver output (output dir, input buffer off,
// LED-driver-enable set). Call once per LED pin after sx1509_led_driver_init.
bool sx1509_led_setup_pin(sx1509_t *d, uint8_t pin);

// Set static PWM intensity 0..255 (0 = off/full-bright depends on wiring; with
// the typical V+->LED->pin sink wiring, 255 = brightest).
bool sx1509_led_set_intensity(sx1509_t *d, uint8_t pin, uint8_t intensity);

// Hardware blink. on_time / off_time are 0..31 (0 = steady on). The dt for
// each step is set by led_clk_div in sx1509_led_driver_init. Only pins that
// have RegTOn/RegOff registers (all 16 do) participate. Returns false for an
// out-of-range pin.
bool sx1509_led_blink(sx1509_t *d, uint8_t pin,
                      uint8_t on_time, uint8_t off_time, uint8_t on_intensity);
