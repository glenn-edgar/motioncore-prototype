// ============================================================================
// spectral.h — RA4M1 mode 2: averaged power spectrum (Welch-style).
//
// At SPECTRAL_START: a sample timer runs at fs = 20 kHz / fs_code (codes 1..10
// → 20, 10, 6.667, ..., 2 kHz). The mode periodic ISR samples ONE ADC channel
// per tick into a ping-pong capture buffer; when each N=1024 sample buffer
// fills the foreground loop windows it (Hamming, precomputed once), runs an
// arm_rfft_fast_f32 in place, accumulates |X[k]|² into a per-bin float
// accumulator, and bumps a frame counter. After target_frames (1..100) the
// state becomes DONE; the host pages the accumulator out via SPECTRAL_READ
// and divides by frame_count to get the averaged power spectrum.
//
// CMSIS-DSP packed real-FFT output convention (length N reals):
//   out[0] = DC                   (real)
//   out[1] = Nyquist (bin N/2)    (real)
//   out[2k], out[2k+1] = re/im of bin k, for k = 1..N/2-1
// Hence the bin count returned by the device is N/2 + 1 = SPECTRAL_BINS.
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define SPECTRAL_N           1024u
#define SPECTRAL_BINS        (SPECTRAL_N / 2u + 1u)   // 513
#define SPECTRAL_MAX_FRAMES  100u

// fs_code 1..10 -> fs = 20_000 / fs_code  Hz (table in continue.md).
#define SPECTRAL_MIN_FS_CODE 1u
#define SPECTRAL_MAX_FS_CODE 10u
#define SPECTRAL_BASE_FS_HZ  20000u

// Channels 0..3 → D1/D2/D3/D5 on the XIAO header (matches CMD_ANALOG_START's
// channel order; see spectral.c for AN-number mapping).
#define SPECTRAL_CHANNEL_MAX 3u

// Wire state for SPECTRAL_STATUS. Matches the enum the host parses.
typedef enum {
    SPECTRAL_STATE_IDLE    = 0,
    SPECTRAL_STATE_RUNNING = 1,
    SPECTRAL_STATE_DONE    = 2,
    SPECTRAL_STATE_ERROR   = 3,
} spectral_state_t;

// ---- API surface (called by ra4m1_commands.c handlers + mode descriptor) ---

// Begin a capture: configure ADC for the channel, precompute the Hamming
// window, kick the mode periodic timer at fs. Returns false on bad args.
// MUST be called with g_device_mode == MODE_SPECTRAL (the command handler
// switches modes first via mode_set).
bool spectral_start(uint8_t fs_code, uint8_t channel, uint16_t target_frames);

// Abort: stop the timer, return state to IDLE. Idempotent.
void spectral_stop(void);

// Query (for SPECTRAL_STATUS).
spectral_state_t spectral_state(void);
uint32_t         spectral_frames_done(void);
uint16_t         spectral_target_frames(void);
uint8_t          spectral_fs_code(void);

// Copy a slice of the power accumulator into out_f32 (host divides by
// frame_count). Returns the actual number of floats written. Returns 0 if
// state is IDLE/ERROR or offset is past the end. Guards count <= bins
// remaining; cap a sane max at the call site (libcomm MTU).
uint16_t spectral_read_bins(uint16_t offset, uint16_t count, float* out_f32);

// ---- mode descriptor hooks (referenced by g_modes[MODE_SPECTRAL]) ----------

void spectral_on_enter(void);
void spectral_on_exit(void);
void spectral_periodic_isr(void);   // ADC sample tick

// ---- foreground processing -------------------------------------------------
// Called from main()'s superloop alongside workbench_analog_poll(). Picks up
// a filled capture buffer (set by the ISR), windows + rffts + accumulates.
// No-op if no frame is ready, so the cost is one volatile read per loop.

void spectral_pump(void);
