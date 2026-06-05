// ============================================================================
// spectral.c — RA4M1 offline averaged power spectrum (Welch-style).
//
// Rewired (task #15): spectral is a Tier-0 consumer of the control core's
// decimated streams, NOT its own ADC mode. See spectral.h for the contract.
//
// Lifecycle (offline op):
//   spectral_start(source, channel, N)
//      → control_analysis_start()  (20 kHz ISR + decimators online, motor coast)
//      → control_spectral_arm(rate, channel)  (ISR now calls spectral_feed)
//   ... the control ISR pushes the selected stream sample into spectral_feed()
//       at the stream rate; on a full N-frame the foreground spectral_pump()
//       windows (Hamming via recurrence) + arm_rfft_fast_f32 + accumulates |X|² ...
//   frames_done == N → DONE + control_spectral_disarm() + control_analysis_stop()
//                      ("complete", chip offline)
//
// Memory: the small control/status (g_sp) is a RESIDENT static so spectral_status
// is valid any time; only the big buffers (cap/work/power_sum/rfft) overlay
// g_mode_arena (~8.2 KB), reused by goertzel/workbench when spectral is idle.
//
// Single capture buffer: spectral_feed drops samples while frame_ready is set
// (pump owns cap_buf) — clean SPSC handoff, immaterial for Welch averaging.
//
// CMSIS-DSP packed real-FFT output (length N reals):
//   out[0]=DC, out[1]=Nyquist(bin N/2), out[2k],out[2k+1]=re/im of bin k.
// ============================================================================

#include "spectral.h"

#include <string.h>   // memset, memcpy
#include <math.h>     // cosf (window recurrence seed)

#include "mode.h"      // g_mode_arena, MODE_ARENA_SIZE
#include "control.h"   // control_analysis_start/stop, control_spectral_arm/disarm, CONTROL_RATE_*
#include "arm_math.h"  // arm_rfft_fast_instance_f32, arm_rfft_fast_f32

// ---- resident control/status (NOT in g_mode_arena — valid any time) --------
static struct {
    volatile spectral_state_t state;
    uint8_t           source;        // CONTROL_RATE_* (20k/1k/100)
    uint8_t           channel;       // 0..2 = A1/A2/A3
    uint8_t           compute;       // SPECTRAL_COMPUTE_*
    uint16_t          target_frames;
    volatile uint32_t frames_done;
    volatile uint16_t cap_idx;       // 0..N-1 fill index
    volatile uint8_t  frame_ready;   // set by spectral_feed on full; cleared by pump
} g_sp = { SPECTRAL_STATE_IDLE, 0, 0, 0, 0, 0, 0, 0 };

// ---- big buffers overlay g_mode_arena (valid only while running) -----------
typedef struct {
    uint16_t                   cap_buf[SPECTRAL_N];     // 2 KB
    float                      work[SPECTRAL_N];        // 4 KB  rfft in/out
    float                      power_sum[SPECTRAL_BINS];// 2 KB  accumulator
    arm_rfft_fast_instance_f32 rfft;
} spec_buf_t;

_Static_assert(sizeof(spec_buf_t) <= MODE_ARENA_SIZE,
               "spec_buf_t exceeds MODE_ARENA_SIZE — bump MODE_ARENA_SIZE");

// g_mode_arena is aligned(8); the void* hop silences -Wcast-align.
#define SB  (*(spec_buf_t*)(void*)g_mode_arena)

// ---- stream feed (called from the control 20 kHz sample ISR) ----------------

void spectral_feed(uint16_t sample)
{
    if (g_sp.state != SPECTRAL_STATE_RUNNING) return;
    if (g_sp.frame_ready) return;        // pump owns cap_buf; drop (Welch-safe)
    SB.cap_buf[g_sp.cap_idx++] = sample;
    if (g_sp.cap_idx >= SPECTRAL_N) {
        g_sp.cap_idx     = 0u;
        g_sp.frame_ready = 1u;
    }
}

// ---- cepstrum post-process (once, on DONE) ---------------------------------
// Real cepstrum c[n] = IDFT( log|X(f)| ). We have power_sum[k] = Σ|X[k]|², so
// log|X[k]| = 0.5·ln(power_sum[k]). The log-power of a real signal is real+even,
// so we build the packed real-spectrum input (imag = 0) and inverse-rFFT it in
// place into work[] → the real cepstrum (quefrency bins 0..N/2). Reuses work[]
// (free after the last Welch frame); power_sum is preserved (PSD stays readable).
// A floor on the power avoids ln(0); its constant offset is harmless for peaks.
static void compute_cepstrum(void)
{
    const float floor_p = 1.0f;
    for (uint32_t k = 1u; k < SPECTRAL_N / 2u; k++) {
        const float p = SB.power_sum[k];
        SB.work[2u * k]      = 0.5f * logf(p > floor_p ? p : floor_p);  // real
        SB.work[2u * k + 1u] = 0.0f;                                    // imag
    }
    const float p0 = SB.power_sum[0];
    const float pN = SB.power_sum[SPECTRAL_N / 2u];
    SB.work[0] = 0.5f * logf(p0 > floor_p ? p0 : floor_p);              // DC
    SB.work[1] = 0.5f * logf(pN > floor_p ? pN : floor_p);              // Nyquist
    arm_rfft_fast_f32(&SB.rfft, SB.work, SB.work, 1);                   // inverse → cepstrum
}

// ---- foreground processing -------------------------------------------------

void spectral_pump(void)
{
    if (g_sp.state != SPECTRAL_STATE_RUNNING) return;
    if (!g_sp.frame_ready) return;

    const uint16_t* cap = SB.cap_buf;    // feed paused (frame_ready) — we own it

    // DC-blank: subtract the mean so the large bias doesn't smear low bins.
    uint32_t sum = 0u;
    for (uint32_t i = 0; i < SPECTRAL_N; i++) sum += cap[i];
    const float mean = (float)sum * (1.0f / (float)SPECTRAL_N);

    // Hamming window w[n] = 0.54 - 0.46·cos(2πn/(N-1)) via recursive cosine
    // oscillator (no 4 KB table): cos(θ(n+1)) = 2cos(θ)·cos(θn) − cos(θ(n−1)).
    {
        const float theta = 6.28318530717958647692f / (float)(SPECTRAL_N - 1u);
        const float k = 2.0f * cosf(theta);
        float c_prev = 1.0f;             // cos(0)
        float c_cur  = cosf(theta);      // cos(θ)
        SB.work[0] = ((float)cap[0] - mean) * (0.54f - 0.46f * c_prev);
        SB.work[1] = ((float)cap[1] - mean) * (0.54f - 0.46f * c_cur);
        for (uint32_t i = 2u; i < SPECTRAL_N; i++) {
            const float c_next = k * c_cur - c_prev;
            SB.work[i] = ((float)cap[i] - mean) * (0.54f - 0.46f * c_next);
            c_prev = c_cur;
            c_cur  = c_next;
        }
    }

    // Real FFT in place; accumulate |X[k]|².
    arm_rfft_fast_f32(&SB.rfft, SB.work, SB.work, 0);
    SB.power_sum[0]              += SB.work[0] * SB.work[0];   // DC
    SB.power_sum[SPECTRAL_N / 2] += SB.work[1] * SB.work[1];   // Nyquist
    for (uint32_t kk = 1u; kk < SPECTRAL_N / 2u; kk++) {
        const float re = SB.work[2u * kk];
        const float im = SB.work[2u * kk + 1u];
        SB.power_sum[kk] += re * re + im * im;
    }

    g_sp.frame_ready = 0u;
    g_sp.frames_done++;

    if (g_sp.frames_done >= (uint32_t)g_sp.target_frames) {
        control_spectral_disarm();
        control_analysis_stop();              // N captures done → chip offline
        if (g_sp.compute != SPECTRAL_COMPUTE_PSD) {
            compute_cepstrum();               // post-process the averaged PSD → work[]
        }
        g_sp.state = SPECTRAL_STATE_DONE;     // "complete" (set last)
    }
}

// ---- API ------------------------------------------------------------------

bool spectral_start(uint8_t source, uint8_t channel, uint16_t target_frames,
                    uint8_t compute)
{
    if (source > CONTROL_RATE_100)                                  return false;
    if (channel > SPECTRAL_CHANNEL_MAX)                            return false;
    if (target_frames == 0u || target_frames > SPECTRAL_MAX_FRAMES) return false;
    if (compute > SPECTRAL_COMPUTE_BOTH)                           return false;

    // Clear the accumulator + buffers; preserve nothing from any prior run.
    memset(g_mode_arena, 0, MODE_ARENA_SIZE);

    g_sp.source        = source;
    g_sp.channel       = channel;
    g_sp.compute       = compute;
    g_sp.target_frames = target_frames;
    g_sp.frames_done   = 0u;
    g_sp.cap_idx       = 0u;
    g_sp.frame_ready   = 0u;

    if (arm_rfft_fast_init_f32(&SB.rfft, SPECTRAL_N) != ARM_MATH_SUCCESS) {
        g_sp.state = SPECTRAL_STATE_ERROR;
        return false;
    }

    g_sp.state = SPECTRAL_STATE_RUNNING;
    control_spectral_arm(source, channel);       // arm feed (state already RUNNING)
    if (control_analysis_start() != 0u) {        // bring sampling online
        control_spectral_disarm();
        g_sp.state = SPECTRAL_STATE_IDLE;
        return false;                            // motor busy
    }
    return true;
}

void spectral_stop(void)
{
    control_spectral_disarm();
    control_analysis_stop();
    g_sp.state = SPECTRAL_STATE_IDLE;
}

spectral_state_t spectral_state(void)         { return g_sp.state; }
uint32_t         spectral_frames_done(void)   { return g_sp.frames_done; }
uint16_t         spectral_target_frames(void) { return g_sp.target_frames; }
uint8_t          spectral_source(void)        { return g_sp.source; }

uint16_t spectral_read_bins(uint16_t offset, uint16_t count, float* out_f32)
{
    if (g_sp.state == SPECTRAL_STATE_IDLE || g_sp.state == SPECTRAL_STATE_ERROR) {
        return 0u;
    }
    if (offset >= SPECTRAL_BINS) {
        return 0u;
    }
    uint16_t avail = (uint16_t)(SPECTRAL_BINS - offset);
    uint16_t n     = (count < avail) ? count : avail;
    memcpy(out_f32, &SB.power_sum[offset], (size_t)n * sizeof(float));
    return n;
}

uint16_t cepstrum_read_bins(uint16_t offset, uint16_t count, float* out_f32)
{
    // Valid only after DONE with a cepstrum computed (it lives in work[]).
    if (g_sp.state != SPECTRAL_STATE_DONE)            return 0u;
    if (g_sp.compute == SPECTRAL_COMPUTE_PSD)         return 0u;
    if (offset >= SPECTRAL_BINS)                      return 0u;
    uint16_t avail = (uint16_t)(SPECTRAL_BINS - offset);
    uint16_t n     = (count < avail) ? count : avail;
    memcpy(out_f32, &SB.work[offset], (size_t)n * sizeof(float));
    return n;
}
