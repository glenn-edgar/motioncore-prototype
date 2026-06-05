// ============================================================================
// spectral.h — RA4M1 offline averaged power spectrum (Welch-style).
//
// Rewired (task #15): spectral is NO LONGER its own ADC mode. It is a Tier-0
// consumer of the control core's decimated streams. spectral_start() brings the
// 20 kHz sample ISR + decimators online (motor idle/coast), and the control ISR
// pushes the selected (channel × rate) decimated sample into spectral_feed().
// When each N=1024 buffer fills, the foreground pump windows it (Hamming via a
// recursive oscillator — no table), runs arm_rfft_fast_f32 in place, and
// accumulates |X[k]|² into a per-bin float accumulator. After target_frames the
// state becomes DONE and the chip goes offline (control_analysis_stop). The host
// pages the accumulator out via spectral_read and divides by frames_done.
//
// This pairs with the DAC test-signal generator (control.c): jumper A0→A1, run
// dac_square, then spectral_start on a stream — the harmonics fold/decimate into
// the chosen band so you see the comb + alias sidebands + window leakage.
//
// CMSIS-DSP packed real-FFT output (length N reals):
//   out[0]=DC, out[1]=Nyquist(bin N/2), out[2k],out[2k+1]=re/im of bin k.
//   bin count = N/2 + 1 = SPECTRAL_BINS.
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define SPECTRAL_N           1024u
#define SPECTRAL_BINS        (SPECTRAL_N / 2u + 1u)   // 513 (also quefrency bins)
#define SPECTRAL_MAX_FRAMES  100u
#define SPECTRAL_CHANNEL_MAX 2u    // 0..2 = A1/A2/A3 (control decimated channels)

// `compute` selects the post-processing done when the Welch average completes.
// PSD (power_sum) is always accumulated; CEPSTRUM/BOTH additionally compute the
// real cepstrum c[n] = IDFT(log|X|) into work[] (read via cepstrum_read_bins).
#define SPECTRAL_COMPUTE_PSD      0u   // PSD only (default)
#define SPECTRAL_COMPUTE_CEPSTRUM 1u   // + real cepstrum
#define SPECTRAL_COMPUTE_BOTH     2u   // + real cepstrum (PSD always available too)

typedef enum {
    SPECTRAL_STATE_IDLE    = 0,
    SPECTRAL_STATE_RUNNING = 1,
    SPECTRAL_STATE_DONE    = 2,    // "complete" — N captures averaged, chip offline
    SPECTRAL_STATE_ERROR   = 3,
} spectral_state_t;

// ---- API -------------------------------------------------------------------

// Offline averaged PSD over a control decimated stream:
//   source  = CONTROL_RATE_20K / _1K / _100 (see control.h)
//   channel = 0..2 (A1/A2/A3)
//   target_frames = 1..SPECTRAL_MAX_FRAMES (Welch averages)
// Brings sampling online, captures + averages N windowed buffers, then DONE +
// chip offline. `compute` = SPECTRAL_COMPUTE_* (cepstrum computed on DONE).
// Returns false on bad args or motor busy.
bool spectral_start(uint8_t source, uint8_t channel, uint16_t target_frames,
                    uint8_t compute);

// Abort: disarm the feed, take the chip offline, state → IDLE. Idempotent.
void spectral_stop(void);

// Query (for SPECTRAL_STATUS). Valid any time (state lives in resident RAM).
spectral_state_t spectral_state(void);
uint32_t         spectral_frames_done(void);
uint16_t         spectral_target_frames(void);
uint8_t          spectral_source(void);

// Copy a slice of the power accumulator (host divides by frames_done). Returns
// floats written; 0 if IDLE/ERROR or offset past the end.
uint16_t spectral_read_bins(uint16_t offset, uint16_t count, float* out_f32);

// Copy a slice of the real cepstrum (quefrency bins 0..N/2). Valid only after
// DONE with compute != PSD. Returns floats written; 0 otherwise. Quefrency bin n
// corresponds to a period of n / fs_source seconds (the source stream's rate).
uint16_t cepstrum_read_bins(uint16_t offset, uint16_t count, float* out_f32);

// Called from the control 20 kHz sample ISR (when armed) with the selected
// decimated-stream sample. Appends to the capture buffer; flags frame-ready on
// a full N-sample frame. Drops while a frame awaits the pump (Welch-safe).
void spectral_feed(uint16_t sample);

// Tier-0 processing — call from main()'s superloop. FFTs each full frame.
void spectral_pump(void);
