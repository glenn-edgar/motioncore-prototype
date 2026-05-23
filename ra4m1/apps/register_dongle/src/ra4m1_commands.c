// ============================================================================
// ra4m1_commands.c — RA4M1 workbench (mode 1) app-shell command table.
//
// Step 4: the analytical-HIL command set. Mirrors the SAMD21 chip command IDs
// 1:1 (0x0100..0x010E) so dongle_console.lua stays uniform across chips, plus
// CMD_SET_MODE / CMD_GET_MODE for the multi-mode foundation. Every handler is
// a thin wrapper over ra4m1_hal — the drivers are shared with the future
// mode 2-4 ISRs.
//
// shell_find_cmd() searches the general table (CMD_ECHO / CMD_SYSINFO) first,
// then falls through to chip_commands_table() exported here.
//
// Differences from samd21_commands.c (the RA4M1 is the higher-res HIL chip):
//   * DAC is 12-bit (0..4095), ADC is 14-bit (0..16383).
//   * The counter slot is a real x4 quadrature decoder (GPT1), not a
//     unidirectional pulse counter — COUNTER_READ returns signed position.
//   * The RA4M1 has internal pull-UP only: GPIO_MODE_INPUT_PULLDOWN is
//     rejected.
//   * The DAC waveform generator runs on the mode periodic timer (see mode.c)
//     via the workbench mode's on_periodic callback.
// ============================================================================

#include <string.h>                   // memcpy (float -> wire)

#include "shell_commands.h"
#include "vendor/libcomm/opcodes.h"   // SHELL_STATUS_*
#include "ra4m1_hal.h"
#include "mode.h"
#include "spectral.h"                 // mode 2: averaged power spectrum
#include "bsp/board_api.h"            // board_millis()

// ---- RA4M1-specific command IDs (0x0110+: multi-mode control) --------------
// 0x0100..0x010E are the shared chip commands defined in shell_commands.h.

#define CMD_SET_MODE        ((uint16_t)0x0110)
#define CMD_GET_MODE        ((uint16_t)0x0111)
#define CMD_ANALOG_START    ((uint16_t)0x0112)
#define CMD_ANALOG_READ     ((uint16_t)0x0113)
#define CMD_ANALOG_STOP     ((uint16_t)0x0114)

// 0x0115..0x0118: mode-2 spectral (averaged power spectrum) — see spectral.h.
#define CMD_SPECTRAL_START  ((uint16_t)0x0115)
#define CMD_SPECTRAL_STATUS ((uint16_t)0x0116)
#define CMD_SPECTRAL_READ   ((uint16_t)0x0117)
#define CMD_SPECTRAL_STOP   ((uint16_t)0x0118)

// ---- DAC waveform-generator state ------------------------------------------
// Lives in the shared mode arena — only the workbench mode owns it. Written by
// the command handlers, consumed by the periodic ISR (hence volatile).

#define DAC_WF_PHASE_STEPS  64u

typedef struct {
    volatile bool     active;
    uint8_t           waveform_type;
    uint16_t          amplitude;        // peak-to-peak, 0..4095
    uint16_t          offset;           // DC center, 0..4095
    uint32_t          isrs_remaining;   // 0 = infinite
    uint8_t           phase;            // 0..63
} wf_state_t;

// g_mode_arena is aligned(8); the void* hop tells the compiler the cast is
// alignment-safe (silences -Wcast-align on the uint8_t[] -> struct cast).
#define WF  (*(volatile wf_state_t*)(void*)g_mode_arena)

// ============================================================================
// GPIO — raw (port,pin); host resolves board labels. port 0..9, pin 0..15.
// ============================================================================

static bool validate_pin(uint8_t port, uint8_t pin)
{
    return port <= 9u && pin <= 15u;
}

// CMD_GPIO_CONFIG — args: port:u8 pin:u8 mode:u8 ; result: empty
static uint8_t cmd_gpio_config(shell_reader_t* args, shell_writer_t* result)
{
    (void)result;
    uint8_t port = sr_u8(args);
    uint8_t pin  = sr_u8(args);
    uint8_t mode = sr_u8(args);
    if (args->overflow)            return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;
    if (!validate_pin(port, pin))  return SHELL_STATUS_BAD_ARGS;
    // RA4M1 has internal pull-up only — no pull-down.
    if (mode > GPIO_MODE_INPUT_PULLUP) return SHELL_STATUS_BAD_ARGS;

    hal_gpio_config(port, pin,
                    mode == GPIO_MODE_OUTPUT,
                    mode == GPIO_MODE_INPUT_PULLUP);
    return SHELL_STATUS_OK;
}

// CMD_GPIO_WRITE — args: port:u8 pin:u8 level:u8 ; result: empty
static uint8_t cmd_gpio_write(shell_reader_t* args, shell_writer_t* result)
{
    (void)result;
    uint8_t port  = sr_u8(args);
    uint8_t pin   = sr_u8(args);
    uint8_t level = sr_u8(args);
    if (args->overflow)            return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;
    if (!validate_pin(port, pin))  return SHELL_STATUS_BAD_ARGS;
    if (level > 1u)                return SHELL_STATUS_BAD_ARGS;

    hal_gpio_write(port, pin, level != 0u);
    return SHELL_STATUS_OK;
}

// CMD_GPIO_READ — args: port:u8 pin:u8 ; result: level:u8
static uint8_t cmd_gpio_read(shell_reader_t* args, shell_writer_t* result)
{
    uint8_t port = sr_u8(args);
    uint8_t pin  = sr_u8(args);
    if (args->overflow)            return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;
    if (!validate_pin(port, pin))  return SHELL_STATUS_BAD_ARGS;

    sw_u8(result, hal_gpio_read(port, pin));
    return result->overflow ? SHELL_STATUS_RESULT_TOO_BIG : SHELL_STATUS_OK;
}

// ============================================================================
// DAC — 12-bit, fixed pin D0/P014.
// ============================================================================

// CMD_DAC_WRITE — args: value:u16 (0..4095) ; result: empty
static uint8_t cmd_dac_write(shell_reader_t* args, shell_writer_t* result)
{
    (void)result;
    uint16_t value = sr_u16(args);
    if (args->overflow)            return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;
    if (value > 4095u)             return SHELL_STATUS_BAD_ARGS;

    // A static write wins over any running waveform.
    if (WF.active) {
        WF.active = false;
        mode_periodic_stop();
    }
    hal_dac_write(value);
    return SHELL_STATUS_OK;
}

// ============================================================================
// DAC waveform generator — runs on the mode periodic timer (mode.c). The
// workbench mode's on_periodic callback (workbench_periodic_isr, below) steps
// one sample per ISR; 64 phase steps per output cycle.
// ============================================================================

#define DAC_WF_FREQ_MIN     50u
#define DAC_WF_FREQ_MAX     500u

#define DAC_WF_TYPE_SINE       0u
#define DAC_WF_TYPE_RAMP_UP    1u
#define DAC_WF_TYPE_RAMP_DOWN  2u
#define DAC_WF_TYPE_SQUARE     3u

// 64-point 12-bit sine LUT, centered 2048, peak amplitude 2047.
static const uint16_t g_sine_lut12[DAC_WF_PHASE_STEPS] = {
    2048, 2249, 2447, 2642, 2831, 3013, 3185, 3347,
    3495, 3630, 3750, 3853, 3939, 4007, 4056, 4085,
    4095, 4085, 4056, 4007, 3939, 3853, 3750, 3630,
    3495, 3347, 3185, 3013, 2831, 2642, 2447, 2249,
    2048, 1847, 1649, 1454, 1265, 1083,  911,  749,
     601,  466,  346,  243,  157,   89,   40,   11,
       1,   11,   40,   89,  157,  243,  346,  466,
     601,  749,  911, 1083, 1265, 1454, 1649, 1847,
};

// workbench mode on_periodic callback — see g_modes[] in mode.c.
void workbench_periodic_isr(void)
{
    if (!WF.active) {
        return;
    }

    int32_t amp    = (int32_t)WF.amplitude;
    int32_t offset = (int32_t)WF.offset;
    int32_t sample;

    switch (WF.waveform_type) {
        case DAC_WF_TYPE_SINE: {
            int32_t lut    = (int32_t)g_sine_lut12[WF.phase] - 2048;  // -2048..+2047
            int32_t scaled = lut * amp / 4095;
            sample = offset + scaled;
            break;
        }
        case DAC_WF_TYPE_RAMP_UP: {
            int32_t v = (int32_t)WF.phase * amp / (int32_t)(DAC_WF_PHASE_STEPS - 1u);
            sample = offset - amp / 2 + v;
            break;
        }
        case DAC_WF_TYPE_RAMP_DOWN: {
            int32_t v = (int32_t)(DAC_WF_PHASE_STEPS - 1u - WF.phase) * amp
                      / (int32_t)(DAC_WF_PHASE_STEPS - 1u);
            sample = offset - amp / 2 + v;
            break;
        }
        case DAC_WF_TYPE_SQUARE:
        default:
            sample = (WF.phase < (DAC_WF_PHASE_STEPS / 2u))
                   ? offset + amp / 2
                   : offset - amp / 2;
            break;
    }

    if (sample < 0)       sample = 0;
    if (sample > 4095)    sample = 4095;
    hal_dac_write((uint16_t)sample);

    WF.phase = (uint8_t)((WF.phase + 1u) % DAC_WF_PHASE_STEPS);

    // Duration: 0 = infinite; otherwise stop when the ISR budget runs out.
    if (WF.isrs_remaining != 0u) {
        if (--WF.isrs_remaining == 0u) {
            WF.active = false;
            mode_periodic_stop();
        }
    }
}

// CMD_DAC_WAVEFORM_WRITE — args: waveform:u8 amplitude:u16 offset:u16
//                                frequency_hz:u32 duration_ms:u32 ; result: empty
static uint8_t cmd_dac_waveform_write(shell_reader_t* args, shell_writer_t* result)
{
    (void)result;
    uint8_t  waveform    = sr_u8(args);
    uint16_t amplitude   = sr_u16(args);
    uint16_t offset      = sr_u16(args);
    uint32_t frequency   = sr_u32(args);
    uint32_t duration_ms = sr_u32(args);
    if (args->overflow)               return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)      return SHELL_STATUS_BAD_ARGS;
    if (waveform > DAC_WF_TYPE_SQUARE) return SHELL_STATUS_BAD_ARGS;
    if (amplitude > 4095u || offset > 4095u) return SHELL_STATUS_BAD_ARGS;
    if (frequency < DAC_WF_FREQ_MIN || frequency > DAC_WF_FREQ_MAX)
                                      return SHELL_STATUS_BAD_ARGS;

    uint32_t isr_rate = frequency * DAC_WF_PHASE_STEPS;
    uint32_t isrs = (duration_ms == 0u)
                  ? 0u
                  : (duration_ms * isr_rate / 1000u);

    // Stop any running waveform, install the new state, then start the timer.
    mode_periodic_stop();
    hal_dac_write(offset);                 // force DAC init + park at offset
    WF.waveform_type  = waveform;
    WF.amplitude      = amplitude;
    WF.offset         = offset;
    WF.isrs_remaining = isrs;
    WF.phase          = 0u;
    WF.active         = true;
    mode_periodic_start(isr_rate);
    return SHELL_STATUS_OK;
}

// CMD_DAC_STOP — args: empty ; result: empty. DAC holds the last sample.
static uint8_t cmd_dac_stop(shell_reader_t* args, shell_writer_t* result)
{
    (void)result;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    WF.active = false;
    mode_periodic_stop();
    return SHELL_STATUS_OK;
}

// ============================================================================
// ADC — 14-bit. Channels are RA4M1 AN numbers (D1=0 D2=1 D3=2 D0=9 D5=22).
// ============================================================================

// CMD_ADC_READ — args: channel:u8 ; result: value:u16 (0..16383)
static uint8_t cmd_adc_read(shell_reader_t* args, shell_writer_t* result)
{
    uint8_t channel = sr_u8(args);
    if (args->overflow)              return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)     return SHELL_STATUS_BAD_ARGS;
    if (!hal_adc_channel_valid(channel)) return SHELL_STATUS_BAD_ARGS;

    sw_u16(result, hal_adc_read(channel));
    return result->overflow ? SHELL_STATUS_RESULT_TOO_BIG : SHELL_STATUS_OK;
}

// CMD_ADC_CAPTURE — multi-channel buffered capture.
// args:   num_channels:u8 (1..8) channels:u8[num_channels]
//         num_samples:u16 delta_time_us:u32 (>= 1000)
// result: num_channels:u8 num_samples:u16 samples:u16[num_channels*num_samples]
// v1 cap: total samples <= 60 to fit one OP_SHELL_REPLY frame.
#define ADC_CAPTURE_MAX_CHANNELS  8u
#define ADC_CAPTURE_MAX_SAMPLES   60u
#define ADC_CAPTURE_MIN_DELTA_US  1000u

static uint8_t cmd_adc_capture(shell_reader_t* args, shell_writer_t* result)
{
    uint8_t num_channels = sr_u8(args);
    if (args->overflow) return SHELL_STATUS_BAD_ARGS;
    if (num_channels == 0u || num_channels > ADC_CAPTURE_MAX_CHANNELS)
        return SHELL_STATUS_BAD_ARGS;

    uint8_t channels[ADC_CAPTURE_MAX_CHANNELS];
    for (uint8_t i = 0; i < num_channels; i++) {
        channels[i] = sr_u8(args);
        if (args->overflow)                       return SHELL_STATUS_BAD_ARGS;
        if (!hal_adc_channel_valid(channels[i]))  return SHELL_STATUS_BAD_ARGS;
    }

    uint16_t num_samples   = sr_u16(args);
    uint32_t delta_time_us = sr_u32(args);
    if (args->overflow)            return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;
    if (num_samples == 0u)         return SHELL_STATUS_BAD_ARGS;
    if (delta_time_us < ADC_CAPTURE_MIN_DELTA_US) return SHELL_STATUS_BAD_ARGS;

    uint32_t total = (uint32_t)num_channels * (uint32_t)num_samples;
    if (total > ADC_CAPTURE_MAX_SAMPLES) return SHELL_STATUS_BAD_ARGS;

    uint32_t delta_ms = delta_time_us / 1000u;

    sw_u8 (result, num_channels);
    sw_u16(result, num_samples);

    for (uint16_t s = 0; s < num_samples; s++) {
        uint32_t slot_start_ms = board_millis();

        for (uint8_t c = 0; c < num_channels; c++) {
            sw_u16(result, hal_adc_read(channels[c]));
            if (result->overflow) return SHELL_STATUS_RESULT_TOO_BIG;
        }

        if (s < num_samples - 1u) {
            while ((board_millis() - slot_start_ms) < delta_ms) {
                // hold the sample slot
            }
        }
    }
    return SHELL_STATUS_OK;
}

// ============================================================================
// PWM — GPT3 / GTIOC3A on D8/P111. v1: only D8 is wired.
// ============================================================================

// CMD_PWM_CONFIG — args: port:u8 pin:u8 freq_hz:u32 resolution:u8 ; result: empty
// The RA4M1 PWM is a fixed 12-bit interface (duty 0..4095) — the duty-dither
// ISR resolves 12-bit at any supported frequency, so `resolution` is accepted
// for wire-compatibility with the SAMD21 but not otherwise used.
static uint8_t cmd_pwm_config(shell_reader_t* args, shell_writer_t* result)
{
    (void)result;
    uint8_t  port       = sr_u8 (args);
    uint8_t  pin        = sr_u8 (args);
    uint32_t freq_hz    = sr_u32(args);
    uint8_t  resolution = sr_u8 (args);
    if (args->overflow)            return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;
    if (port != 1u || pin != 11u)  return SHELL_STATUS_BAD_ARGS;   // D8/P111 only
    if (resolution != 8u && resolution != 10u && resolution != 11u
        && resolution != 12u && resolution != 16u) return SHELL_STATUS_BAD_ARGS;

    if (!hal_pwm_config(freq_hz)) return SHELL_STATUS_BAD_ARGS;
    return SHELL_STATUS_OK;
}

// CMD_PWM_SET — args: duty:u16 (0..4095, 12-bit) ; result: empty
static uint8_t cmd_pwm_set(shell_reader_t* args, shell_writer_t* result)
{
    (void)result;
    uint16_t duty = sr_u16(args);
    if (args->overflow)            return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;
    if (!hal_pwm_active())         return SHELL_STATUS_BAD_ARGS;
    if (duty > 4095u)              return SHELL_STATUS_BAD_ARGS;

    hal_pwm_set(duty);
    return SHELL_STATUS_OK;
}

// CMD_PWM_TEARDOWN — args: empty ; result: empty
static uint8_t cmd_pwm_teardown(shell_reader_t* args, shell_writer_t* result)
{
    (void)result;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    hal_pwm_teardown();
    return SHELL_STATUS_OK;
}

// ============================================================================
// Counter — the COUNTER_* commands drive one of two backends, selected by the
// COUNTER_SETUP pin:
//   D9/D10 (P110/P109) -> GPT1 x4 quadrature encoder — COUNTER_READ returns a
//                         signed 32-bit position.
//   D7    (P301)       -> GPT4 plain 16-bit rising-edge counter — COUNTER_READ
//                         returns an unsigned count (e.g. for the D8 PWM-count
//                         loopback).
// Both report through the same u32 result; the host knows which by the pin.
// ============================================================================

#define CTR_NONE     0u
#define CTR_ENCODER  1u   // GPT1 quadrature, D9/D10
#define CTR_COUNTER  2u   // GPT4 edge counter, D7

static uint8_t g_counter_backend = CTR_NONE;

// CMD_COUNTER_SETUP — args: port:u8 pin:u8 ; result: empty.
// Pin selects the backend: D9/D10 (1,9)/(1,10) -> quadrature encoder;
// D7 (3,1) -> GPT4 edge counter.
static uint8_t cmd_counter_setup(shell_reader_t* args, shell_writer_t* result)
{
    (void)result;
    uint8_t port = sr_u8(args);
    uint8_t pin  = sr_u8(args);
    if (args->overflow)            return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;

    if (port == 1u && (pin == 9u || pin == 10u)) {
        hal_encoder_setup();                        // D9/D10 -> GPT1 quadrature
        g_counter_backend = CTR_ENCODER;
    } else if (port == 3u && pin == 1u) {
        hal_counter_setup();                        // D7 -> GPT4 edge counter
        g_counter_backend = CTR_COUNTER;
    } else {
        return SHELL_STATUS_BAD_ARGS;
    }
    return SHELL_STATUS_OK;
}

// CMD_COUNTER_RESET — args: empty ; result: empty
static uint8_t cmd_counter_reset(shell_reader_t* args, shell_writer_t* result)
{
    (void)result;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;

    if (g_counter_backend == CTR_ENCODER)      hal_encoder_reset();
    else if (g_counter_backend == CTR_COUNTER) hal_counter_reset();
    else return SHELL_STATUS_BAD_ARGS;
    return SHELL_STATUS_OK;
}

// CMD_COUNTER_READ — args: reset_flag:u8 ; result: count:u32
// Encoder backend: signed position (host reads as int32). Counter backend:
// unsigned edge count.
static uint8_t cmd_counter_read(shell_reader_t* args, shell_writer_t* result)
{
    uint8_t reset_flag = sr_u8(args);
    if (args->overflow)            return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;

    uint32_t value;
    if (g_counter_backend == CTR_ENCODER) {
        value = (uint32_t)hal_encoder_read();
        if (reset_flag != 0u) hal_encoder_reset();
    } else if (g_counter_backend == CTR_COUNTER) {
        value = hal_counter_read();
        if (reset_flag != 0u) hal_counter_reset();
    } else {
        return SHELL_STATUS_BAD_ARGS;
    }
    sw_u32(result, value);
    return result->overflow ? SHELL_STATUS_RESULT_TOO_BIG : SHELL_STATUS_OK;
}

// CMD_COUNTER_STOP — args: empty ; result: empty
static uint8_t cmd_counter_stop(shell_reader_t* args, shell_writer_t* result)
{
    (void)result;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    if (g_counter_backend == CTR_ENCODER)      hal_encoder_stop();
    else if (g_counter_backend == CTR_COUNTER) hal_counter_stop();
    g_counter_backend = CTR_NONE;
    return SHELL_STATUS_OK;
}

// ============================================================================
// Analog collection — background statistics on the 4 ADC channels.
//
// ANALOG_START begins a ~1 kHz main-loop sampler over D1/D2/D3/D5
// (AN0/AN1/AN2/AN22); per channel it keeps Welford running mean + M2 plus
// min/max. ANALOG_READ snapshots {n, mean, stddev, min, max} for the interval
// since the last READ, then zeros the accumulators (reset-on-read) — sampling
// continues. ANALOG_STOP halts sampling, leaving the last interval for a final
// READ. D0 (the DAC) is not sampled.
//
// The sampler runs in the main loop (workbench_analog_poll), so it can't delay
// the DAC-waveform ISR and shares the main-loop context with the command
// handlers — no race on the accumulators, no IRQ masking needed.
// ============================================================================

#define ANALOG_CH_COUNT  4u

// The 4 analog-capable XIAO pins, as ADC AN channels (D1, D2, D3, D5).
static const uint8_t g_analog_channels[ANALOG_CH_COUNT] = { 0u, 1u, 2u, 22u };

static struct {
    volatile bool active;
    uint32_t      last_ms;                  // 1 kHz rate-limit anchor
    uint32_t      n;                        // samples since the last READ
    float         mean[ANALOG_CH_COUNT];    // Welford running mean
    float         m2[ANALOG_CH_COUNT];      // Welford sum of squared deltas
    uint16_t      min[ANALOG_CH_COUNT];
    uint16_t      max[ANALOG_CH_COUNT];
} g_analog;

static void analog_reset_accumulators(void)
{
    g_analog.n = 0u;
    for (uint32_t i = 0; i < ANALOG_CH_COUNT; i++) {
        g_analog.mean[i] = 0.0f;
        g_analog.m2[i]   = 0.0f;
        g_analog.min[i]  = 0xFFFFu;
        g_analog.max[i]  = 0u;
    }
}

// Called every main-loop iteration (main.c). Samples the 4 channels at ~1 kHz
// and folds each into the per-channel Welford accumulators + min/max.
void workbench_analog_poll(void)
{
    if (!g_analog.active) {
        return;
    }
    uint32_t now = board_millis();
    if ((now - g_analog.last_ms) < 1u) {            // ~1 kHz cap
        return;
    }
    g_analog.last_ms = now;
    g_analog.n++;

    for (uint32_t i = 0; i < ANALOG_CH_COUNT; i++) {
        uint16_t v = hal_adc_read(g_analog_channels[i]);
        // Welford online mean + M2.
        float delta = (float)v - g_analog.mean[i];
        g_analog.mean[i] += delta / (float)g_analog.n;
        float delta2 = (float)v - g_analog.mean[i];
        g_analog.m2[i] += delta * delta2;
        // min / max.
        if (v < g_analog.min[i]) g_analog.min[i] = v;
        if (v > g_analog.max[i]) g_analog.max[i] = v;
    }
}

// Write a little-endian float32 to the shell result.
static void sw_f32(shell_writer_t* w, float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    sw_u32(w, bits);
}

// CMD_ANALOG_START — args: empty ; result: empty
static uint8_t cmd_analog_start(shell_reader_t* args, shell_writer_t* result)
{
    (void)result;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    analog_reset_accumulators();
    g_analog.last_ms = board_millis();
    g_analog.active  = true;
    return SHELL_STATUS_OK;
}

// CMD_ANALOG_READ — args: empty
// result: n:u32  then 4x { mean:f32  stddev:f32  min:u16  max:u16 }  (52 B)
// Stats for the interval since the last READ; resets the accumulators after.
static uint8_t cmd_analog_read(shell_reader_t* args, shell_writer_t* result)
{
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;

    uint32_t n = g_analog.n;
    sw_u32(result, n);
    for (uint32_t i = 0; i < ANALOG_CH_COUNT; i++) {
        float    mean   = (n > 0u) ? g_analog.mean[i] : 0.0f;
        float    stddev = (n > 1u)
                        ? __builtin_sqrtf(g_analog.m2[i] / (float)(n - 1u))
                        : 0.0f;
        uint16_t mn     = (n > 0u) ? g_analog.min[i] : 0u;
        uint16_t mx     = (n > 0u) ? g_analog.max[i] : 0u;
        sw_f32(result, mean);
        sw_f32(result, stddev);
        sw_u16(result, mn);
        sw_u16(result, mx);
    }
    if (result->overflow) return SHELL_STATUS_RESULT_TOO_BIG;

    // Reset-on-read: the next READ reports a fresh interval; sampling continues.
    analog_reset_accumulators();
    return SHELL_STATUS_OK;
}

// CMD_ANALOG_STOP — args: empty ; result: empty.
// Halts sampling; the last interval stays for one final ANALOG_READ.
static uint8_t cmd_analog_stop(shell_reader_t* args, shell_writer_t* result)
{
    (void)result;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    g_analog.active = false;
    return SHELL_STATUS_OK;
}

// ============================================================================
// Spectral — mode 2: averaged power spectrum. See spectral.h for the model.
// SPECTRAL_START switches mode 1 → mode 2 + begins capture; STOP returns to
// mode 1. STATUS / READ are read-only and tolerate the wrong mode (returning
// IDLE state / 0 floats) so the host can poll safely after a STOP.
// ============================================================================

// CMD_SPECTRAL_START — args: fs_code:u8 channel:u8 target_frames:u16
//                     result: empty
static uint8_t cmd_spectral_start(shell_reader_t* args, shell_writer_t* result)
{
    (void)result;
    uint8_t  fs_code        = sr_u8 (args);
    uint8_t  channel        = sr_u8 (args);
    uint16_t target_frames  = sr_u16(args);
    if (args->overflow)          return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;

    // Switch into MODE_SPECTRAL (mode_set runs the old mode's on_exit + new
    // mode's on_enter). If we're already there, mode_set is a no-op.
    if (mode_set(MODE_SPECTRAL) != 0u) return SHELL_STATUS_CMD_FAILED;

    if (!spectral_start(fs_code, channel, target_frames)) {
        return SHELL_STATUS_BAD_ARGS;
    }
    return SHELL_STATUS_OK;
}

// CMD_SPECTRAL_STATUS — args: empty
// result: state:u8 frames_done:u32 frames_target:u16 fs_code:u8 channel:u8
//         (9 bytes total)
static uint8_t cmd_spectral_status(shell_reader_t* args, shell_writer_t* result)
{
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;

    // Read state from the spectral module. If we're not in MODE_SPECTRAL the
    // arena bytes belong to another mode — report IDLE/zero so the host's
    // poll path doesn't misinterpret garbage.
    uint8_t  state         = SPECTRAL_STATE_IDLE;
    uint32_t frames_done   = 0u;
    uint16_t frames_target = 0u;
    uint8_t  fs_code       = 0u;
    if (mode_get() == MODE_SPECTRAL) {
        state         = (uint8_t)spectral_state();
        frames_done   = spectral_frames_done();
        frames_target = spectral_target_frames();
        fs_code       = spectral_fs_code();
    }
    sw_u8 (result, state);
    sw_u32(result, frames_done);
    sw_u16(result, frames_target);
    sw_u8 (result, fs_code);
    // channel is only meaningful while we own the arena; emit 0 outside.
    sw_u8 (result, 0u);
    return result->overflow ? SHELL_STATUS_RESULT_TOO_BIG : SHELL_STATUS_OK;
}

// CMD_SPECTRAL_READ — args: offset:u16 count:u16
// result: count:u16  bins:f32[count]
// count caps at 30 (SHELL_RESULT_MAX=125 → 2 header + 30*4 = 122 bytes).
static uint8_t cmd_spectral_read(shell_reader_t* args, shell_writer_t* result)
{
    uint16_t offset = sr_u16(args);
    uint16_t count  = sr_u16(args);
    if (args->overflow)          return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;

    if (count > 30u) count = 30u;

    float bins[30];
    uint16_t n = 0u;
    if (mode_get() == MODE_SPECTRAL) {
        n = spectral_read_bins(offset, count, bins);
    }
    sw_u16(result, n);
    for (uint32_t i = 0; i < n; i++) {
        sw_f32(result, bins[i]);
    }
    return result->overflow ? SHELL_STATUS_RESULT_TOO_BIG : SHELL_STATUS_OK;
}

// CMD_SPECTRAL_STOP — args: empty ; result: empty.
// Aborts the capture and drops back to MODE_WORKBENCH.
static uint8_t cmd_spectral_stop(shell_reader_t* args, shell_writer_t* result)
{
    (void)result;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;

    if (mode_get() == MODE_SPECTRAL) {
        spectral_stop();
        mode_set(MODE_WORKBENCH);
    }
    return SHELL_STATUS_OK;
}

// ============================================================================
// Mode control — the multi-mode foundation (see mode.h).
// ============================================================================

// CMD_SET_MODE — args: mode:u8 ; result: empty
static uint8_t cmd_set_mode(shell_reader_t* args, shell_writer_t* result)
{
    (void)result;
    uint8_t mode = sr_u8(args);
    if (args->overflow)            return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;

    // mode_set rejects out-of-range / not-yet-implemented modes.
    if (mode_set((device_mode_t)mode) != 0u) return SHELL_STATUS_BAD_ARGS;
    return SHELL_STATUS_OK;
}

// CMD_GET_MODE — args: empty ; result: mode:u8
static uint8_t cmd_get_mode(shell_reader_t* args, shell_writer_t* result)
{
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    sw_u8(result, (uint8_t)mode_get());
    return result->overflow ? SHELL_STATUS_RESULT_TOO_BIG : SHELL_STATUS_OK;
}

// ---- chip-specific dispatch table ------------------------------------------

static const shell_cmd_entry_t g_chip_commands[] = {
    { CMD_GPIO_CONFIG,        "gpio_config",        cmd_gpio_config        },
    { CMD_GPIO_WRITE,         "gpio_write",         cmd_gpio_write         },
    { CMD_GPIO_READ,          "gpio_read",          cmd_gpio_read          },
    { CMD_DAC_WRITE,          "dac_write",          cmd_dac_write          },
    { CMD_ADC_READ,           "adc_read",           cmd_adc_read           },
    { CMD_DAC_WAVEFORM_WRITE, "dac_waveform_write", cmd_dac_waveform_write },
    { CMD_DAC_STOP,           "dac_stop",           cmd_dac_stop           },
    { CMD_ADC_CAPTURE,        "adc_capture",        cmd_adc_capture        },
    { CMD_PWM_CONFIG,         "pwm_config",         cmd_pwm_config         },
    { CMD_PWM_SET,            "pwm_set",            cmd_pwm_set            },
    { CMD_PWM_TEARDOWN,       "pwm_teardown",       cmd_pwm_teardown       },
    { CMD_COUNTER_SETUP,      "counter_setup",      cmd_counter_setup      },
    { CMD_COUNTER_RESET,      "counter_reset",      cmd_counter_reset      },
    { CMD_COUNTER_READ,       "counter_read",       cmd_counter_read       },
    { CMD_COUNTER_STOP,       "counter_stop",       cmd_counter_stop       },
    { CMD_SET_MODE,           "set_mode",           cmd_set_mode           },
    { CMD_GET_MODE,           "get_mode",           cmd_get_mode           },
    { CMD_ANALOG_START,       "analog_start",       cmd_analog_start       },
    { CMD_ANALOG_READ,        "analog_read",        cmd_analog_read        },
    { CMD_ANALOG_STOP,        "analog_stop",        cmd_analog_stop        },
    { CMD_SPECTRAL_START,     "spectral_start",     cmd_spectral_start     },
    { CMD_SPECTRAL_STATUS,    "spectral_status",    cmd_spectral_status    },
    { CMD_SPECTRAL_READ,      "spectral_read",      cmd_spectral_read      },
    { CMD_SPECTRAL_STOP,      "spectral_stop",      cmd_spectral_stop      },
};

const shell_cmd_entry_t* chip_commands_table(void)
{
    return g_chip_commands;
}

uint8_t chip_commands_count(void)
{
    return (uint8_t)(sizeof(g_chip_commands) / sizeof(g_chip_commands[0]));
}
