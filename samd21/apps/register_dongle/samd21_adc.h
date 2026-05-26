// ============================================================================
// samd21_adc.h — public single-shot ADC reader, shared between CMD_ADC_READ
// (samd21_commands.c) and the interlock framework's adc_int input source
// (samd21_hal_pin.c slice 4+).
//
// Implementation lives in samd21_commands.c — historically owned the SAMD21
// ADC bring-up + per-call config (adc_init, adc_apply_avg_hold, ain-to-pad
// table). This header exposes a single oneshot read so the HAL pin layer can
// reuse them without static-linkage gymnastics.
//
// Concurrency: NOT re-entrant; assumes single-threaded callers under the
// FreeRTOS chain pump. CMD_ANALOG_START (long-running multi-channel capture)
// must not overlap — see the design memo for the mutual-exclusion rule.
// ============================================================================

#pragma once

#include <stdint.h>

// Channel: AIN[0..19] index (per samd21_commands.c g_ain_to_pad table).
// oversample_exp: 0..7 → SAMPLENUM = 2^N (1, 2, 4, ..., 128 samples averaged).
// sh_cyc: 0..63 → SAMPCTRL.SAMPLEN (sample-hold cycles).
//
// Returns a 12-bit-equivalent result (0..4095) for any oversample value
// (ADJRES capped at 4 per the SAMD21 quirk — see adc_apply_avg_hold).
//
// Caller is responsible for input validation; out-of-range channel returns 0.
uint16_t samd21_adc_read_oneshot(uint8_t channel,
                                 uint8_t oversample_exp,
                                 uint8_t sh_cyc);
