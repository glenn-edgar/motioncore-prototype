// ============================================================================
// goertzel.c — order-tracked Goertzel bank (sub-mode A: Hz-bin, gated).
//
// See goertzel.h for the architecture + per-block lifecycle. This file owns
// the arena layout, the ISR (per-sample bin recursion), the foreground pump
// (block finalize + RPM read + gate + coefficient refresh), and a private
// ADC setup. Encoder access goes through hal_encoder_setup/read in
// ra4m1_hal.c (GPT1 32-bit hardware x4 quadrature).
//
// ---- Goertzel recursion (bin k, per sample x[n]) ---------------------------
//   s_new   = x[n] + coef[k] * s1[k] - s2[k]
//   s2[k]   = s1[k]
//   s1[k]   = s_new
// At block end (N samples):
//   mag²[k] = s1[k]² + s2[k]² - coef[k] * s1[k] * s2[k]
//   s1 = s2 = 0
// coef[k] = 2·cos(2π·f_k/fs), where f_k = order_k * RPM/60.
//
// ---- ADC pipeline ----------------------------------------------------------
// Same one-channel single-scan software-triggered pattern as spectral: ISR
// reads ADDR[an] for the conversion the previous tick kicked off, then kicks
// the next. ADC conversion (≈1 µs) << ISR period (≥50 µs at fs_code=1), so
// the spin-wait guard is a sanity check that should fall through.
// ============================================================================

#include "goertzel.h"

#include <string.h>
#include <math.h>       // cosf, fabsf, isnan, isfinite

#include "bsp_api.h"
#include "mode.h"

// ra4m1_hal.c — encoder helpers.
extern void    hal_encoder_setup(void);
extern int32_t hal_encoder_read(void);
extern void    hal_encoder_reset(void);

// ---- ADC channel mapping (matches spectral / workbench analog) ------------
typedef struct { uint8_t an; uint8_t port; uint8_t pin; } gz_pin_t;

static const gz_pin_t GOERTZEL_PINS[4] = {
    {  0u, 0u, 0u },   // ch 0 → D1 / AN0
    {  1u, 0u, 1u },   // ch 1 → D2 / AN1
    {  2u, 0u, 2u },   // ch 2 → D3 / AN2
    { 22u, 1u, 0u },   // ch 3 → D5 / AN22
};

// PFS / MSTP / ADC bits (same as spectral.c — duplicated to keep modes
// self-contained; if we sprout a third user we'll factor them into ra4m1_hal.c).
#define PFS_ASEL       (1u << 15)
#define PFS_PWPR_UNLK  (1u << 6)
#define PFS_PWPR_B0WI  (1u << 7)
#define MSTPD_ADC140   (1u << 16)
#define ADCSR_ADST     (1u << 15)
#define ADCER_14BIT    (3u << 1)

static inline void pfs_unlock(void)
{
    R_PMISC->PWPR = 0u;
    R_PMISC->PWPR = PFS_PWPR_UNLK;
}
static inline void pfs_lock(void)
{
    R_PMISC->PWPR = 0u;
    R_PMISC->PWPR = PFS_PWPR_B0WI;
}

// ---- arena layout ----------------------------------------------------------

typedef struct {
    volatile goertzel_state_t state;

    // scalar config (CMD_GOERTZEL_CONFIG)
    uint8_t  sub_mode;          // GOERTZEL_SUB_MODE_HZ (v1)
    uint8_t  fs_code;           // 1..10
    uint16_t block_n;           // 256..16384
    uint8_t  channel;           // 0..3
    uint8_t  gate_enabled;      // 0/1
    uint8_t  order_count;       // 1..32
    uint8_t  _pad0;
    float    accel_thresh_rpm_per_sec;
    float    min_rpm;

    // RPM tracking
    int32_t  last_encoder_count;
    float    last_rpm;
    float    rpm_change_rate;
    float    inject_rpm;        // NaN = use encoder

    // gate state
    uint8_t  gate_open;
    uint8_t  _pad1[3];

    // per-bin state (parallel arrays)
    float    order[GOERTZEL_MAX_BINS];     // 4 × 32 = 128 B
    float    coef[GOERTZEL_MAX_BINS];      // 128 B
    float    s1[GOERTZEL_MAX_BINS];        // 128 B
    float    s2[GOERTZEL_MAX_BINS];        // 128 B
    float    mag2_sum[GOERTZEL_MAX_BINS];  // 128 B

    // block control (ISR ↔ pump)
    volatile uint32_t sample_count;
    volatile uint8_t  block_ready;
    uint8_t  _pad2[3];

    // accumulation counters
    volatile uint32_t n_blocks_accumulated;   // gate-open blocks contributing to mag2_sum
    uint32_t          n_blocks_total;         // every finalized block, gated or not
} gz_arena_t;

_Static_assert(sizeof(gz_arena_t) <= MODE_ARENA_SIZE,
               "gz_arena_t exceeds MODE_ARENA_SIZE — bump MODE_ARENA_SIZE");

#define GZ (*(gz_arena_t*)(void*)g_mode_arena)

// ---- ADC setup -------------------------------------------------------------

static void goertzel_adc_setup(uint8_t channel)
{
    const gz_pin_t* p = &GOERTZEL_PINS[channel];

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
    // Prime the pipeline so the first ISR tick reads a real sample.
    R_ADC0->ADCSR |= ADCSR_ADST;
}

static inline uint16_t goertzel_adc_sample_and_kick(uint8_t an)
{
    while ((R_ADC0->ADCSR & ADCSR_ADST) != 0u) { }
    uint16_t s = (uint16_t)(R_ADC0->ADDR[an] & 0x3FFFu);
    R_ADC0->ADCSR |= ADCSR_ADST;
    return s;
}

// ---- coefficient recompute -------------------------------------------------
// Called by the foreground pump at block boundaries. Bin frequency in Hz is
// derived from the current RPM and per-bin order multiplier. Clamps to
// (0, fs/2] so a malformed order never makes the recursion unstable.

static void goertzel_refresh_coefs(float rpm)
{
    const float fs_hz   = (float)(GOERTZEL_BASE_FS_HZ / (uint32_t)GZ.fs_code);
    const float mech_hz = rpm / 60.0f;
    const float two_pi  = 6.28318530717958647692f;
    const float nyq     = fs_hz * 0.5f;

    for (uint32_t k = 0; k < GZ.order_count; k++) {
        float f_k = GZ.order[k] * mech_hz;
        if (f_k <= 0.0f)  f_k = 0.0f;
        if (f_k >= nyq)   f_k = nyq;
        GZ.coef[k] = 2.0f * cosf(two_pi * f_k / fs_hz);
    }
}

// ---- mode descriptor hooks -------------------------------------------------

void goertzel_on_enter(void)
{
    memset(g_mode_arena, 0, MODE_ARENA_SIZE);
    GZ.state      = GOERTZEL_STATE_IDLE;
    GZ.inject_rpm = NAN;
    // Actual ADC + timer setup happens in goertzel_start.
}

void goertzel_on_exit(void)
{
    mode_periodic_stop();
    R_ADC0->ADCSR = 0u;
    GZ.state = GOERTZEL_STATE_IDLE;
}

// Per-sample ISR. Reads one ADC sample, runs the Goertzel recursion across all
// K bins, increments sample_count. At block end, sets block_ready and
// continues sampling (the pump will reset s1/s2 + sample_count when it
// finalizes — until then the new block accumulates into stale state, which
// the pump's reset overwrites before any output).
//
// Note: we do NOT pre-subtract a DC bias from the ADC sample. Goertzel for a
// non-DC bin naturally rejects DC over a block (the band-pass character of
// the recursion). Bin 0 / order 0 would track DC, which is rarely useful;
// users should pick orders > 0.
void goertzel_periodic_isr(void)
{
    if (GZ.state != GOERTZEL_STATE_RUNNING) {
        return;
    }

    const uint8_t an = GOERTZEL_PINS[GZ.channel].an;
    const float x = (float)goertzel_adc_sample_and_kick(an);

    const uint32_t K = GZ.order_count;
    for (uint32_t k = 0; k < K; k++) {
        float s_new = x + GZ.coef[k] * GZ.s1[k] - GZ.s2[k];
        GZ.s2[k] = GZ.s1[k];
        GZ.s1[k] = s_new;
    }

    GZ.sample_count++;
    if (GZ.sample_count >= (uint32_t)GZ.block_n) {
        GZ.block_ready = 1u;
        // Leave sample_count at block_n+ until pump resets — pump checks
        // block_ready, not sample_count, and we don't want to lose ISR ticks
        // while pump is mid-finalize.
    }
}

// ---- foreground pump -------------------------------------------------------

void goertzel_pump(void)
{
    if (GZ.state != GOERTZEL_STATE_RUNNING) return;
    if (!GZ.block_ready)                    return;

    // --- RPM ----------------------------------------------------------------
    int32_t  count_now   = hal_encoder_read();
    int32_t  delta_ticks = count_now - GZ.last_encoder_count;
    GZ.last_encoder_count = count_now;

    const float fs_hz       = (float)(GOERTZEL_BASE_FS_HZ / (uint32_t)GZ.fs_code);
    const float block_t_sec = (float)GZ.block_n / fs_hz;
    const float revs        = (float)delta_ticks / (float)GOERTZEL_ENC_TICKS_PER_REV;
    const float rpm_enc     = (revs / block_t_sec) * 60.0f;

    float rpm_now;
    if (isfinite(GZ.inject_rpm)) {
        rpm_now = GZ.inject_rpm;
    } else {
        rpm_now = rpm_enc;
    }

    GZ.rpm_change_rate = (rpm_now - GZ.last_rpm) / block_t_sec;

    // --- gate ---------------------------------------------------------------
    bool open = true;
    if (GZ.gate_enabled) {
        if (rpm_now < GZ.min_rpm)                                                open = false;
        else if (fabsf(GZ.rpm_change_rate) > GZ.accel_thresh_rpm_per_sec)        open = false;
    }
    GZ.gate_open = open ? 1u : 0u;

    // --- finalize magnitudes ------------------------------------------------
    // Block 1 is a warm-up: at goertzel_start() we seed coef[k] from RPM=0
    // (since we haven't read the encoder yet), which lands every bin on the
    // DC-pole edge (coef=2.0). At coef=2 the recursion is a double-integrator
    // for any DC input — s1, s2 grow ~N²·mean. With float32, mag² then sits
    // in the catastrophic-cancellation noise floor (10¹⁶-ish) and can flip
    // sign. Mathematically mag² ≥ 0, but the warm-up block's coefs don't
    // match our bin labels anyway, so the cleanest fix is to discard it.
    // Block 2+ runs with coefs refreshed from the actual RPM.
    const bool warm_up        = (GZ.n_blocks_total == 0u);
    const bool will_accumulate = open && !warm_up;

    const uint32_t K = GZ.order_count;
    for (uint32_t k = 0; k < K; k++) {
        const float s1 = GZ.s1[k];
        const float s2 = GZ.s2[k];
        const float c  = GZ.coef[k];
        const float mag2 = s1 * s1 + s2 * s2 - c * s1 * s2;
        if (will_accumulate) {
            GZ.mag2_sum[k] += mag2;
        }
    }

    if (will_accumulate) GZ.n_blocks_accumulated++;
    GZ.n_blocks_total++;
    GZ.last_rpm = rpm_now;

    // --- next-block prep ----------------------------------------------------
    goertzel_refresh_coefs(rpm_now);
    for (uint32_t k = 0; k < K; k++) {
        GZ.s1[k] = 0.0f;
        GZ.s2[k] = 0.0f;
    }
    GZ.sample_count = 0u;
    GZ.block_ready  = 0u;
}

// ---- API (called from ra4m1_commands.c handlers) ---------------------------

bool goertzel_config(uint8_t  sub_mode,
                     uint8_t  fs_code,
                     uint16_t block_n,
                     uint8_t  channel,
                     uint8_t  gate_enabled,
                     float    accel_thresh_rpm_per_sec,
                     float    min_rpm,
                     uint8_t  order_count)
{
    if (GZ.state == GOERTZEL_STATE_RUNNING)                          return false;
    if (sub_mode != GOERTZEL_SUB_MODE_HZ)                            return false;
    if (fs_code < GOERTZEL_MIN_FS_CODE || fs_code > GOERTZEL_MAX_FS_CODE)
                                                                     return false;
    if (block_n < GOERTZEL_MIN_BLOCK_N || block_n > GOERTZEL_MAX_BLOCK_N)
                                                                     return false;
    if (channel > GOERTZEL_CHANNEL_MAX)                              return false;
    if (order_count == 0u || order_count > GOERTZEL_MAX_BINS)        return false;
    if (!isfinite(accel_thresh_rpm_per_sec) || accel_thresh_rpm_per_sec < 0.0f)
                                                                     return false;
    if (!isfinite(min_rpm) || min_rpm < 0.0f)                        return false;

    GZ.sub_mode                 = sub_mode;
    GZ.fs_code                  = fs_code;
    GZ.block_n                  = block_n;
    GZ.channel                  = channel;
    GZ.gate_enabled             = gate_enabled ? 1u : 0u;
    GZ.order_count              = order_count;
    GZ.accel_thresh_rpm_per_sec = accel_thresh_rpm_per_sec;
    GZ.min_rpm                  = min_rpm;

    // Zero per-bin arrays (orders set separately).
    for (uint32_t k = 0; k < GOERTZEL_MAX_BINS; k++) {
        GZ.order[k]    = 0.0f;
        GZ.coef[k]     = 0.0f;
        GZ.s1[k]       = 0.0f;
        GZ.s2[k]       = 0.0f;
        GZ.mag2_sum[k] = 0.0f;
    }
    GZ.n_blocks_accumulated = 0u;
    GZ.n_blocks_total       = 0u;
    return true;
}

bool goertzel_set_orders(uint8_t offset, uint8_t count, const float* orders)
{
    if (count == 0u)                                  return false;
    if ((uint32_t)offset + (uint32_t)count > GOERTZEL_MAX_BINS) return false;
    if (orders == NULL)                               return false;

    for (uint32_t i = 0; i < count; i++) {
        const float o = orders[i];
        if (!isfinite(o) || o < 0.0f) return false;
        // Atomic 32-bit store: live update is safe even while RUNNING — the
        // ISR reads `coef[k]` (not `order[k]`), so a partial update only
        // takes effect at the next pump-driven coefficient refresh.
        GZ.order[offset + i] = o;
    }
    return true;
}

bool goertzel_start(void)
{
    if (GZ.state == GOERTZEL_STATE_RUNNING)         return false;
    if (GZ.order_count == 0u)                       return false;
    if (GZ.block_n < GOERTZEL_MIN_BLOCK_N)          return false;

    // Reset Goertzel state + accumulators (fresh capture).
    for (uint32_t k = 0; k < GZ.order_count; k++) {
        GZ.s1[k]       = 0.0f;
        GZ.s2[k]       = 0.0f;
        GZ.mag2_sum[k] = 0.0f;
    }
    GZ.sample_count          = 0u;
    GZ.block_ready           = 0u;
    GZ.n_blocks_accumulated  = 0u;
    GZ.n_blocks_total        = 0u;
    GZ.last_rpm              = 0.0f;
    GZ.rpm_change_rate       = 0.0f;
    GZ.gate_open             = 0u;

    // Encoder: ensure the hardware quadrature counter is running, then zero.
    hal_encoder_setup();
    hal_encoder_reset();
    GZ.last_encoder_count = 0;

    // Seed coefficient table at RPM=0 (any non-zero order → bin at 0 Hz);
    // first pump pass after the first block will refresh with real RPM.
    goertzel_refresh_coefs(0.0f);

    goertzel_adc_setup(GZ.channel);

    GZ.state = GOERTZEL_STATE_RUNNING;

    const uint32_t fs_hz = GOERTZEL_BASE_FS_HZ / (uint32_t)GZ.fs_code;
    mode_periodic_start(fs_hz);
    return true;
}

void goertzel_stop(void)
{
    mode_periodic_stop();
    R_ADC0->ADCSR = 0u;
    GZ.state = GOERTZEL_STATE_IDLE;
}

void goertzel_inject_rpm(float rpm)
{
    GZ.inject_rpm = rpm;  // NaN re-enables encoder via isfinite() test
}

goertzel_state_t goertzel_state(void)               { return GZ.state; }
uint32_t         goertzel_n_blocks_accumulated(void){ return GZ.n_blocks_accumulated; }
uint32_t         goertzel_n_blocks_total(void)      { return GZ.n_blocks_total; }
float            goertzel_last_rpm(void)            { return GZ.last_rpm; }
uint8_t          goertzel_gate_open(void)           { return GZ.gate_open; }

uint16_t goertzel_read(uint16_t offset, uint16_t count, uint8_t reset,
                       float* out_f32, uint32_t* out_n_blocks)
{
    if (out_f32 == NULL || out_n_blocks == NULL) return 0u;
    if (offset >= GZ.order_count)                { *out_n_blocks = 0u; return 0u; }

    uint16_t avail = (uint16_t)(GZ.order_count - offset);
    uint16_t n     = (count < avail) ? count : avail;

    // Snapshot then optionally clear — the pump only writes mag2_sum / counters
    // at block boundaries, so a brief read-modify-write here is safe enough for
    // bench testing. (A guard against block_ready being set during the copy
    // would be tighter, but block period ≥ 12 ms vs. memcpy ≈ µs.)
    *out_n_blocks = GZ.n_blocks_accumulated;
    for (uint32_t i = 0; i < n; i++) {
        out_f32[i] = GZ.mag2_sum[offset + i];
    }

    if (reset) {
        for (uint32_t k = 0; k < GZ.order_count; k++) {
            GZ.mag2_sum[k] = 0.0f;
        }
        GZ.n_blocks_accumulated = 0u;
        GZ.n_blocks_total       = 0u;
    }
    return n;
}
