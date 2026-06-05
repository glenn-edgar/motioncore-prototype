// ============================================================================
// hc4067.h — 74HC4067 16-channel analog multiplexer / demultiplexer.
//
// The 74HC4067 is NOT an I2C part: it's a combinational analog switch. One
// common pin (SIG / "Z") is connected to any one of 16 channel pins (Y0..Y15)
// selected by four digital address lines S0..S3, gated by an active-low enable
// (E̅). There are no internal registers — "driving the chip" means driving
// those five GPIOs on the SAMD21.
//
// Typical wiring on this board:
//   * S0..S3, E̅  -> five SAMD21 GPIOs (this driver drives them via PORT regs)
//   * SIG        -> a SAMD21 ADC channel (read by the caller / scan callback)
//   * Y0..Y15    -> the 16 analog inputs being multiplexed
//
// Because the switch is bidirectional, the same part also works as a 1->16
// demux (drive SIG from a DAC, fan out to Y0..Y15) — the channel-select API is
// identical.
//
// Pin coordinates use the repo's compact phys_id (matches samd21_pin_table.h):
//     phys_id = (port << 5) | (pin & 0x1F)     port 0 = PORTA, 1 = PORTB
// Use the HC4067_PA()/HC4067_PB() helpers below.
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define HC4067_PA(pin)   ((uint8_t)((0u << 5) | ((pin) & 0x1Fu)))
#define HC4067_PB(pin)   ((uint8_t)((1u << 5) | ((pin) & 0x1Fu)))
#define HC4067_NO_PIN    0xFFu   // E̅ hardwired to GND (always enabled)

#define HC4067_NUM_CHANNELS  16u

typedef struct {
    uint8_t s[4];          // phys_id of S0,S1,S2,S3 (S0 = LSB of channel)
    uint8_t en;            // phys_id of the enable line, or HC4067_NO_PIN
    bool    en_active_low; // true for the 74HC4067 (E̅): GPIO low = enabled
    int8_t  current;       // last channel selected, -1 = unknown
    bool    enabled;       // tracks the enable-line state
} hc4067_t;

// Configure the select lines (and enable line, if present) as GPIO outputs and
// leave the mux DISABLED with channel 0 selected. For a 74HC4067 pass
// en_active_low = true. If E̅ is hardwired to GND, pass en = HC4067_NO_PIN and
// the device is treated as always-enabled.
void hc4067_init(hc4067_t *d,
                 uint8_t s0, uint8_t s1, uint8_t s2, uint8_t s3,
                 uint8_t en, bool en_active_low);

// Drive the four address lines to select channel 0..15. Does not touch the
// enable line. Out-of-range channel is ignored. The analog path needs a short
// settling time before SIG is read — see hc4067_settle()/hc4067_scan().
void hc4067_select(hc4067_t *d, uint8_t channel);

// Enable / disable the mux via E̅ (no-op if E̅ is hardwired).
void hc4067_enable(hc4067_t *d, bool on);

// Convenience: select a channel and enable in one call.
void hc4067_select_enable(hc4067_t *d, uint8_t channel);

// Busy-wait roughly `cycles` CPU cycles — a cheap settle between selecting a
// channel and sampling SIG. At 48 MHz, ~48 cycles ≈ 1 µs. Pass 0 to skip.
void hc4067_settle(uint32_t cycles);

// Read-back of the last selected channel (-1 if never selected).
int8_t hc4067_current_channel(const hc4067_t *d);

// --- scan helper (decoupled from any particular ADC) -----------------------
// Caller supplies a function that samples the SIG pin and returns a raw code;
// hc4067_scan walks channels [first..first+count), selecting + settling +
// reading each into out[]. The mux is enabled for the scan and left in its
// prior enable state afterwards. settle_cycles is passed to hc4067_settle()
// after each channel select. Returns the number of channels written.
typedef uint16_t (*hc4067_adc_read_fn)(void *ctx);

uint8_t hc4067_scan(hc4067_t *d, uint8_t first, uint8_t count,
                    uint16_t *out, hc4067_adc_read_fn read, void *ctx,
                    uint32_t settle_cycles);
