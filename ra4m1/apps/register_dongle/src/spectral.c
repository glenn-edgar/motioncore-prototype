// ============================================================================
// spectral.c — RA4M1 mode 2: averaged power spectrum (Welch-style).
//
// Architecture overview (see spectral.h for the contract):
//
//   ┌──────────────────────────┐
//   │  GPT0 overflow @ fs      │   mode periodic timer (slot 4)
//   └──────────┬───────────────┘
//              ▼
//   spectral_periodic_isr():
//     1. read ADDR[channel] from the previous tick's conversion
//     2. store in capture buffer at cap_idx
//     3. kick next conversion (ADCSR.ADST = 1) so it overlaps the next 50 µs
//     4. on buffer full: swap to the other capture, set frame_ready
//
//   spectral_pump() (called from main's superloop, no ISR):
//     5. if frame_ready: window × capture → work[], arm_rfft_fast_f32 in place,
//        accumulate |X[k]|² into power_sum[], frames_done++
//     6. when frames_done == target_frames: state = DONE, stop the timer.
//
// All working state lives in g_mode_arena, cast to spec_arena_t. The arena is
// memset to 0 on entry so the volatile control flags (frame_ready, cap_idx,
// frames_done) start clean even after a workbench→spectral→workbench→spectral
// cycle.
//
// CMSIS-DSP rfft_fast f32 packed output convention (length N reals):
//   out[0]   = DC (real)
//   out[1]   = Nyquist (real, bin N/2)
//   out[2k], out[2k+1]  = re/im of bin k, for k = 1..N/2-1
// So the bin count is N/2 + 1 = 513.
// ============================================================================

#include "spectral.h"

#include <string.h>   // memset
#include <math.h>     // cosf, M_PI (window precompute, called once)

#include "bsp_api.h"          // CMSIS core + R_* peripherals
#include "mode.h"             // g_mode_arena, mode_periodic_start/stop
#include "arm_math.h"         // arm_rfft_fast_instance_f32, arm_rfft_fast_f32

// ---- ADC channel mapping (matches CMD_ANALOG_START's channel 0..3) --------
// channel 0 → D1/P000/AN000   channel 1 → D2/P001/AN001
// channel 2 → D3/P002/AN002   channel 3 → D5/P100/AN022

typedef struct { uint8_t an; uint8_t port; uint8_t pin; } spec_pin_t;

static const spec_pin_t SPECTRAL_PINS[4] = {
    {  0u, 0u, 0u },   // D1 - AN0
    {  1u, 0u, 1u },   // D2 - AN1
    {  2u, 0u, 2u },   // D3 - AN2
    { 22u, 1u, 0u },   // D5 - AN22
};

// ---- PFS / MSTP / ADC registers (subset; full set is in ra4m1_hal.c) ------

#define PFS_ASEL       (1u << 15)
#define PFS_PWPR_UNLK  (1u << 6)
#define PFS_PWPR_B0WI  (1u << 7)
#define MSTPD_ADC140   (1u << 16)
#define ADCSR_ADST     (1u << 15)
#define ADCER_14BIT    (3u << 1)        // ADPRC=11b: 14-bit, right-aligned

static inline void pfs_unlock(void)
{
    R_PMISC->PWPR    = 0u;               // clear B0WI
    R_PMISC->PWPR    = PFS_PWPR_UNLK;    // PFSWE=1
}
static inline void pfs_lock(void)
{
    R_PMISC->PWPR    = 0u;
    R_PMISC->PWPR    = PFS_PWPR_B0WI;
}

// ---- arena layout ---------------------------------------------------------
// One overlay struct stored at the start of g_mode_arena (8-aligned). ~10.3 KB
// at N=1024 (after dropping the 4 KB window table for on-the-fly generation);
// the static_assert below guards against arena-size regressions.

typedef struct {
    // --- control / status (volatile fields touched by ISR + foreground) ---
    volatile spectral_state_t state;
    uint8_t           fs_code;
    uint8_t           channel;
    uint16_t          target_frames;
    volatile uint32_t frames_done;

    // --- capture ping-pong (ISR fills one, pump reads the other) ----------
    volatile uint16_t cap_idx;          // 0..N-1 within current capture
    volatile uint8_t  cap_writer;       // 0 or 1: which cap_buf the ISR fills
    volatile uint8_t  frame_ready;      // set by ISR when a capture completes
    // _pad to keep arrays 4-aligned (cap arrays are u16, frame_ready is u8)
    uint8_t           _pad0;
    uint16_t          cap_buf[2][SPECTRAL_N];

    // --- processing buffers (f32) -----------------------------------------
    // (no window[] table — the Hamming window is generated on the fly by a
    //  recursive cosine oscillator in spectral_pump, saving 4 KB of arena.)
    float             work[SPECTRAL_N];          // rfft in/out (in-place)
    float             power_sum[SPECTRAL_BINS];  // accumulator

    // --- CMSIS-DSP instance -----------------------------------------------
    arm_rfft_fast_instance_f32 rfft;
} spec_arena_t;

// Compile-time guard: must fit in MODE_ARENA_SIZE.
_Static_assert(sizeof(spec_arena_t) <= MODE_ARENA_SIZE,
               "spec_arena_t exceeds MODE_ARENA_SIZE — bump MODE_ARENA_SIZE");

// g_mode_arena is aligned(8); the void* hop tells the compiler the cast is
// alignment-safe (silences -Wcast-align).
#define SP  (*(spec_arena_t*)(void*)g_mode_arena)

// ---- ADC setup (one channel, software trigger, single scan) ---------------

static void spectral_adc_setup(uint8_t channel)
{
    const spec_pin_t* p = &SPECTRAL_PINS[channel];

    R_MSTP->MSTPCRD &= ~MSTPD_ADC140;
    (void)R_MSTP->MSTPCRD;

    pfs_unlock();
    R_PFS->PORT[p->port].PIN[p->pin].PmnPFS = PFS_ASEL;
    pfs_lock();

    R_ADC0->ADCSR   = 0u;
    R_ADC0->ADCER   = ADCER_14BIT;
    R_ADC0->ADADC   = 0u;
    R_ADC0->ADSTRGR = 0u;

    if (p->an < 16u) {
        R_ADC0->ADANSA[0] = (uint16_t)(1u << p->an);
        R_ADC0->ADANSA[1] = 0u;
    } else {
        R_ADC0->ADANSA[0] = 0u;
        R_ADC0->ADANSA[1] = (uint16_t)(1u << (p->an - 16u));
    }
    // Leave ADSSTR at reset (0x0B sampling cycles) — adequate.

    // Prime the pipeline: kick one conversion now so the first ISR tick reads
    // a real sample instead of stale ADDR contents.
    R_ADC0->ADCSR |= ADCSR_ADST;
}

// Read the current ADC result (assumed complete by the time the next ISR
// tick fires — ADC conversion ≈ 1 µs at 14-bit, ISR period ≥ 50 µs at
// fs_code=1). Trigger the next conversion in the same call.
static inline uint16_t spectral_adc_sample_and_kick(uint8_t an)
{
    // Belt-and-suspenders: spin-wait for ADST to clear in case the ADC took
    // longer than expected (debugging / clock-config errors). At fs_code=1
    // this loop should fall through immediately.
    while ((R_ADC0->ADCSR & ADCSR_ADST) != 0u) { }
    uint16_t s = (uint16_t)(R_ADC0->ADDR[an] & 0x3FFFu);
    R_ADC0->ADCSR |= ADCSR_ADST;          // kick next
    return s;
}

// ---- mode descriptor hooks ------------------------------------------------

void spectral_on_enter(void)
{
    memset(g_mode_arena, 0, MODE_ARENA_SIZE);
    SP.state = SPECTRAL_STATE_IDLE;
    // The actual rfft init / ADC setup / timer start happens in spectral_start
    // — entering the mode is just "I'm ready to receive a START".
}

void spectral_on_exit(void)
{
    // Stop the timer if a capture was in flight; ADC is left ungated (cheap).
    mode_periodic_stop();
    R_ADC0->ADCSR = 0u;
    SP.state = SPECTRAL_STATE_IDLE;
}

void spectral_periodic_isr(void)
{
    if (SP.state != SPECTRAL_STATE_RUNNING) {
        return;
    }

    const uint8_t an = SPECTRAL_PINS[SP.channel].an;
    uint16_t s = spectral_adc_sample_and_kick(an);

    SP.cap_buf[SP.cap_writer][SP.cap_idx++] = s;
    if (SP.cap_idx >= SPECTRAL_N) {
        // Buffer full — flip; foreground pump will process the one we just
        // finished. If the previous frame_ready is still set, foreground is
        // behind and we'd overrun — that should not happen at our FFT-vs-tick
        // budgets, but flag the condition by going to ERROR rather than
        // silently corrupting the data.
        if (SP.frame_ready) {
            SP.state = SPECTRAL_STATE_ERROR;
            mode_periodic_stop();
            return;
        }
        SP.cap_idx    = 0u;
        SP.cap_writer ^= 1u;
        SP.frame_ready = 1u;
    }
}

// ---- foreground processing -------------------------------------------------

void spectral_pump(void)
{
    if (SP.state != SPECTRAL_STATE_RUNNING) {
        return;
    }
    if (!SP.frame_ready) {
        return;
    }

    // The "reader" buffer is the one the ISR is NOT currently writing.
    const uint8_t reader = SP.cap_writer ^ 1u;
    const uint16_t* cap = SP.cap_buf[reader];

    // Two-pass DC-blank + window. cap is u16 ADC counts (0..16383, midscale
    // ~8192 for a VREF/2-biased input). Without mean-subtraction the large DC
    // term smears into bins adjacent to DC via the Hamming main lobe (and
    // sidelobes at −43 dB), polluting the low-frequency bins that matter most
    // for motor work. Cost: ~50 µs total at N=1024 on M4F — negligible vs
    // the ~1 ms FFT.
    uint32_t sum = 0u;
    for (uint32_t i = 0; i < SPECTRAL_N; i++) {
        sum += cap[i];                                  // max ~16.7M, fits u32
    }
    const float mean = (float)sum * (1.0f / (float)SPECTRAL_N);
    // Hamming window w[n] = 0.54 - 0.46·cos(2πn/(N-1)), generated on the fly by
    // a recursive cosine oscillator instead of a 4 KB table:
    //   cos(θ(n+1)) = 2cos(θ)·cos(θn) − cos(θ(n−1)),  θ = 2π/(N-1).
    // Two cosf at frame start, then 1 mul + 1 sub per sample. Recurrence drift
    // over N=1024 is ~1e-4 — negligible for a window shape.
    {
        const float theta  = 6.28318530717958647692f / (float)(SPECTRAL_N - 1u);
        const float k      = 2.0f * cosf(theta);
        float c_prev = 1.0f;            // cos(0)
        float c_cur  = cosf(theta);     // cos(θ)
        SP.work[0] = ((float)cap[0] - mean) * (0.54f - 0.46f * c_prev);
        SP.work[1] = ((float)cap[1] - mean) * (0.54f - 0.46f * c_cur);
        for (uint32_t i = 2u; i < SPECTRAL_N; i++) {
            const float c_next = k * c_cur - c_prev;
            SP.work[i] = ((float)cap[i] - mean) * (0.54f - 0.46f * c_next);
            c_prev = c_cur;
            c_cur  = c_next;
        }
    }

    // Real FFT in place. CMSIS-DSP packed output: see header comment.
    arm_rfft_fast_f32(&SP.rfft, SP.work, SP.work, 0);

    // Accumulate |X[k]|² into power_sum.
    SP.power_sum[0]              += SP.work[0] * SP.work[0];          // DC
    SP.power_sum[SPECTRAL_N / 2] += SP.work[1] * SP.work[1];          // Nyquist
    for (uint32_t k = 1u; k < SPECTRAL_N / 2u; k++) {
        const float re = SP.work[2u * k];
        const float im = SP.work[2u * k + 1u];
        SP.power_sum[k] += re * re + im * im;
    }

    SP.frame_ready = 0u;
    SP.frames_done++;

    if (SP.frames_done >= (uint32_t)SP.target_frames) {
        SP.state = SPECTRAL_STATE_DONE;
        mode_periodic_stop();
        R_ADC0->ADCSR = 0u;
    }
}

// ---- API (called from ra4m1_commands.c handlers) ---------------------------

bool spectral_start(uint8_t fs_code, uint8_t channel, uint16_t target_frames)
{
    if (fs_code < SPECTRAL_MIN_FS_CODE || fs_code > SPECTRAL_MAX_FS_CODE) return false;
    if (channel > SPECTRAL_CHANNEL_MAX)                                   return false;
    if (target_frames == 0u || target_frames > SPECTRAL_MAX_FRAMES)       return false;

    // Re-zero working buffers; preserve nothing from any prior capture.
    memset(g_mode_arena, 0, MODE_ARENA_SIZE);

    SP.fs_code        = fs_code;
    SP.channel        = channel;
    SP.target_frames  = target_frames;
    SP.frames_done    = 0u;
    SP.cap_idx        = 0u;
    SP.cap_writer     = 0u;
    SP.frame_ready    = 0u;

    // arm_rfft_fast_init_f32 returns ARM_MATH_ARGUMENT_ERROR for unsupported
    // lengths; SPECTRAL_N=1024 is supported in every CMSIS-DSP we'd ship.
    if (arm_rfft_fast_init_f32(&SP.rfft, SPECTRAL_N) != ARM_MATH_SUCCESS) {
        SP.state = SPECTRAL_STATE_ERROR;
        return false;
    }

    spectral_adc_setup(channel);

    SP.state = SPECTRAL_STATE_RUNNING;

    const uint32_t fs_hz = SPECTRAL_BASE_FS_HZ / fs_code;
    mode_periodic_start(fs_hz);
    return true;
}

void spectral_stop(void)
{
    mode_periodic_stop();
    R_ADC0->ADCSR = 0u;
    SP.state = SPECTRAL_STATE_IDLE;
}

spectral_state_t spectral_state(void)         { return SP.state; }
uint32_t         spectral_frames_done(void)   { return SP.frames_done; }
uint16_t         spectral_target_frames(void) { return SP.target_frames; }
uint8_t          spectral_fs_code(void)       { return SP.fs_code; }

uint16_t spectral_read_bins(uint16_t offset, uint16_t count, float* out_f32)
{
    if (SP.state == SPECTRAL_STATE_IDLE || SP.state == SPECTRAL_STATE_ERROR) {
        return 0u;
    }
    if (offset >= SPECTRAL_BINS) {
        return 0u;
    }
    uint16_t avail = (uint16_t)(SPECTRAL_BINS - offset);
    uint16_t n     = (count < avail) ? count : avail;
    memcpy(out_f32, &SP.power_sum[offset], (size_t)n * sizeof(float));
    return n;
}
