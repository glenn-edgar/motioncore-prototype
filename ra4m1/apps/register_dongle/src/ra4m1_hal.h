// ============================================================================
// ra4m1_hal.h — RA4M1 analytical-HIL peripheral driver layer.
//
// Bare-CMSIS-register drivers for the XIAO RA4M1's HIL peripherals. Each is a
// thin init + read/write surface; the workbench command handlers in
// ra4m1_commands.c are thin wrappers over these, and the future mode 2-4 ISRs
// call the same functions directly. Keeping the drivers separate from the
// command handlers is what lets modes drop in without rework.
//
// Pin allocation (verified vs XIAO RA4M1 schematic v1.0 + RA4M1 manual):
//   D0/P014  — DAC (DA0, 12-bit) + ADC AN009
//   D1/P000  — ADC AN000      D2/P001 — ADC AN001      D3/P002 — ADC AN002
//   D5/P100  — ADC AN022
//   D8/P111  — PWM, GPT3 / GTIOC3A
//   D9/P110  — encoder phase B, GPT1 / GTIOC1B
//   D10/P109 — encoder phase A, GPT1 / GTIOC1A
// GPIO commands take raw (port,pin); the host resolves board labels.
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

// ---- GPIO ------------------------------------------------------------------
// port 0..9, pin 0..15. The RA4M1 has internal pull-UP only (no pull-down).

void    hal_gpio_config(uint8_t port, uint8_t pin, bool output, bool pullup);
void    hal_gpio_write (uint8_t port, uint8_t pin, bool level);
uint8_t hal_gpio_read  (uint8_t port, uint8_t pin);

// ---- ADC (ADC140, 14-bit, single-scan software-triggered) ------------------
// Channels are RA4M1 AN numbers. hal_adc_channel_valid() filters to the five
// analog pins actually bonded to the XIAO header.

bool     hal_adc_channel_valid(uint8_t channel);
uint16_t hal_adc_read(uint8_t channel);          // 14-bit result, 0..16383

// ---- DAC (DA0, 12-bit) — fixed pin D0/P014 ---------------------------------

void hal_dac_write(uint16_t value);              // 0..4095

// ---- PWM (GPT3 / GTIOC3A) — fixed pin D8/P111 ------------------------------
// 12-bit duty interface (0..4095). A GPT3-overflow duty-dither ISR (error-
// feedback) maps the 12-bit setpoint onto the hardware period each cycle, so a
// frequency too high for a native 4096-step period (e.g. 20 kHz, period 2400)
// still resolves 12-bit on time-average. hal_pwm_config returns false for an
// out-of-range frequency (period must fit the 16-bit GPT3 counter).

bool hal_pwm_config(uint32_t freq_hz);
void hal_pwm_set(uint16_t duty12);               // 0..4095
void hal_pwm_teardown(void);
bool hal_pwm_active(void);

// ---- Quadrature encoder (GPT1 / GTIOC1A+B) — fixed pins D10/P109 + D9/P110 -
// GPT1 is a 32-bit channel doing x4 hardware quadrature decode. hal_encoder_read
// returns signed position relative to the last hal_encoder_reset().

void    hal_encoder_setup(void);
int32_t hal_encoder_read(void);
void    hal_encoder_reset(void);
void    hal_encoder_stop(void);
bool    hal_encoder_active(void);

// ---- Pulse counter (GPT4 / GTIOC4B) — fixed pin D7/P301 --------------------
// A plain 16-bit rising-edge counter, separate from the GPT1 quadrature
// encoder — COUNTER_SETUP routes by pin (D7 → here, D9/D10 → encoder). Count
// is relative to the last hal_counter_reset() (software zero offset); wraps
// modulo 65536.

void     hal_counter_setup(void);
uint32_t hal_counter_read(void);
void     hal_counter_reset(void);
void     hal_counter_stop(void);
bool     hal_counter_active(void);
