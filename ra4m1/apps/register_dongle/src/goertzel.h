// ============================================================================
// goertzel.h — RA4M1 mode 3: order-tracked Goertzel bank (motor diagnostics).
//
// Tracks K bins where each bin is locked to a *shaft order* (multiple of the
// mechanical fundamental). The chip reads the encoder once per block, computes
// f_k = order_k × RPM / 60, refreshes the Goertzel coefficient table, then
// runs the next block. Bin centers follow RPM automatically. The host owns
// bearing geometry (BPFO/BPFI/BSF/FTF computations); the chip just runs the
// orders it's told.
//
// Sub-modes:
//   A (Hz-bin, gated):   block-mode Goertzel with RPM-stability gate. Output
//                        accumulates only when RPM is stable (constant-velocity
//                        plateaus). The only sub-mode implemented in v1.
//   B (order tracking):  reserved — bin meaning becomes "samples per shaft
//                        revolution"; the angular resampler would feed
//                        non-uniform samples to the same Goertzel engine.
//                        Not built — needs higher-CPR encoder.
//
// Gate state machine (sub-mode A):
//   open  when  rpm_now > min_rpm  AND  |dRPM/dt| < accel_thresh
//   else closed: finalized magnitudes are computed but NOT accumulated,
//                Goertzel state is reset for the next block.
//
// Per-block lifecycle (foreground pump):
//   1. sample_count_in_block reaches block_n → ISR sets block_ready
//   2. pump reads encoder, derives new RPM
//   3. finalize magnitude^2 per bin from (s1, s2, coef)
//   4. gate check: accumulate into mag2_sum[] iff open, n_blocks_accumulated++
//   5. recompute coef[k] = 2·cos(2π · order_k · RPM/60 / fs)  for next block
//   6. reset s1[k] = s2[k] = 0
//
// Host reads via CMD_GOERTZEL_READ: returns (n_blocks_accumulated, mag2_sum[K])
// and optionally resets accumulators. Magnitude RMS per bin = sqrt(mag2_sum[k]
// / n_blocks) / N_per_block; host applies LSB→V scaling.
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define GOERTZEL_MAX_BINS         32u
#define GOERTZEL_MIN_BLOCK_N      256u
#define GOERTZEL_MAX_BLOCK_N      16384u
#define GOERTZEL_BASE_FS_HZ       20000u
#define GOERTZEL_MIN_FS_CODE      1u
#define GOERTZEL_MAX_FS_CODE      10u
#define GOERTZEL_CHANNEL_MAX      3u
#define GOERTZEL_ENC_TICKS_PER_REV 4u   // motor encoder spec — see continue.md

#define GOERTZEL_SUB_MODE_HZ      0u    // sub-mode A
#define GOERTZEL_SUB_MODE_ORDER   1u    // sub-mode B (reserved)

// Wire state.
typedef enum {
    GOERTZEL_STATE_IDLE    = 0,
    GOERTZEL_STATE_RUNNING = 1,
    GOERTZEL_STATE_ERROR   = 2,
} goertzel_state_t;

// ---- API surface ----------------------------------------------------------

// CMD_GOERTZEL_CONFIG — scalar parameters; orders set separately. Validates
// ranges. State must be IDLE (call STOP first if running). Stores config in
// the arena.
bool goertzel_config(uint8_t  sub_mode,
                     uint8_t  fs_code,
                     uint16_t block_n,
                     uint8_t  channel,
                     uint8_t  gate_enabled,
                     float    accel_thresh_rpm_per_sec,
                     float    min_rpm,
                     uint8_t  order_count);

// CMD_GOERTZEL_SET_ORDERS — write count orders starting at offset. Paged so
// the K=32 max fits the 124-byte SHELL_EXEC payload across two calls.
// Each "order" is a f32 multiplier of mechanical shaft frequency.
bool goertzel_set_orders(uint8_t offset, uint8_t count, const float* orders);

// CMD_GOERTZEL_START — kick the timer + ADC, reset Goertzel + accumulators.
// Requires a prior successful CONFIG (with all orders set).
bool goertzel_start(void);

// CMD_GOERTZEL_STOP — halt timer, drop to IDLE. Idempotent.
void goertzel_stop(void);

// CMD_GOERTZEL_INJECT_RPM — debug path for bench testing without a motor.
// rpm = NaN means "use the encoder reading"; any other finite value overrides
// the encoder for the next block's coefficient table and gate check.
void goertzel_inject_rpm(float rpm);

// CMD_GOERTZEL_STATUS — returns state, blocks-accumulated, last computed RPM,
// gate-open flag. n_blocks_total counts every finalized block (including
// gate-rejected ones), useful for diagnosing low yield.
goertzel_state_t goertzel_state(void);
uint32_t         goertzel_n_blocks_accumulated(void);
uint32_t         goertzel_n_blocks_total(void);
float            goertzel_last_rpm(void);
uint8_t          goertzel_gate_open(void);

// CMD_GOERTZEL_READ — copy mag2_sum[offset .. offset+n) into out_f32. Returns
// the number of floats actually written (clipped to order_count - offset).
// If reset != 0, zero mag2_sum + n_blocks_accumulated after the read (atomic
// w.r.t. the foreground pump).
uint16_t goertzel_read(uint16_t offset, uint16_t count, uint8_t reset,
                       float* out_f32, uint32_t* out_n_blocks);

// ---- mode descriptor hooks ------------------------------------------------

void goertzel_on_enter(void);
void goertzel_on_exit(void);
void goertzel_periodic_isr(void);   // ADC sample tick + per-bin recursion

// ---- foreground pump ------------------------------------------------------
// Called from main()'s superloop. No-op until block_ready is set by the ISR.
// Does the per-block work: read encoder → RPM → finalize → gate → accumulate
// → recompute coefs → reset Goertzel state.

void goertzel_pump(void);
