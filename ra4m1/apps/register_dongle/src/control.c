// ============================================================================
// control.c — RA4M1 motor-control core (Tier 1 / Tier 2 / Tier 0).
//
// See control.h for the architecture. Bare CMSIS registers throughout, matching
// ra4m1_hal.c / mode.c. Increment 1 implements MOTOR_IDLE + MOTOR_MANUAL; the
// closed-loop modes (PID / SCURVE / WINDOW) plug into g_motor_modes[] + the
// PendSV ticks without touching the dispatch core.
//
// Register facts (RA4M1 User's Manual R01UH0887EJ0100, FSP r_gpt.c):
//   * GPT2 = the 20 kHz sample timer (16-bit channel GPT162; module-stop bit is
//     MSTPD6 in MSTPCRD, shared with GPT3..7). 20 kHz @ PCLKD 48 MHz → period
//     2400 counts. Its overflow event is routed to NVIC slot CONTROL_SAMPLE_IRQ.
//   * GPT3 = the PWM (owned by ra4m1_hal.c hal_pwm_*); the control core drives
//     it via hal_pwm_set(). GPT1 = the encoder (boot-fixed in control_init).
//   * PendSV is a Cortex-M core exception (no ICU slot). Pended by the sample
//     ISR every 20th sample → 1 kHz. The strong PendSV_Handler() below overrides
//     the weak FSP one and is picked up automatically by the relocated vector
//     table (mode.c copies it from flash, where the linker already resolved it).
// ============================================================================

#include "control.h"
#include "mode.h"          // mode_vector_install, mode_set, MODE_WORKBENCH
#include "ra4m1_hal.h"     // hal_adc_scan3_*, hal_pwm_*, hal_encoder_*, hal_gpio_*
#include "bsp_api.h"       // CMSIS core (SCB, NVIC_*, PendSV_IRQn) + R_* regs

// ---- H-bridge pins (Mode 1 layout: H-bridge GPIO on D4/D5) -----------------
// D4 = P206 (port 2 pin 6) = IN1 ; D5 = P100 (port 1 pin 0) = IN2.
// Direction via IN1/IN2, magnitude via the D8 PWM (TB6612-style driver).
#define HB_IN1_PORT  2u
#define HB_IN1_PIN   6u
#define HB_IN2_PORT  1u
#define HB_IN2_PIN   0u

// ---- ADC scan channels (A1 current / A2 / A3) ------------------------------
// A1 = AN000 = motor current-sense (index 0 of the scan).

// ---- timing / rates --------------------------------------------------------
#define CONTROL_SAMPLE_IRQ  5u            // free ICU/NVIC slot (USB 0..3, DSP 4)
#define GPT_PCLKD_HZ        48000000u
#define SAMPLE_RATE_HZ      20000u
#define SAMPLE_PERIOD       (GPT_PCLKD_HZ / SAMPLE_RATE_HZ)   // 2400
#define DECIM_1K            20u            // 20 kHz → 1 kHz boxcar
#define DECIM_100           10u            // 1 kHz  → 100 Hz boxcar (cascade)
#define OUTER_DIV           10u            // PendSV 1 kHz → 100 Hz outer tick
#define EMA_ALPHA           0.3f           // speed 1-pole IIR (M-method smoothing)
#define PROBE_SHIFT         8              // dac_probe fixed-point: scale>>8

// ---- GPT register bits (mirror mode.c) -------------------------------------
#define GPT_WP_UNLOCK       0x0000A500u
#define GPT_WP_LOCK         0x0000A501u
#define MSTPD_GPT27         (1u << 6)      // MSTPCRD: GPT162..167 (GPT2..GPT7)
#define GTCR_CST            (1u << 0)
#define GTINTAD_OVF         (1u << 6)      // GTINTPR = 01b: overflow IRQ
#define GTST_TCFPO          (1u << 6)
#define IELSR_IR            (1u << 16)

// GPT2 overflow ICU event. FSP elc_event_t enumerator (bsp_elc_event.h, pulled
// via bsp_api.h). If a build host can't resolve the symbol, override it with the
// literal from the RA4M1 ICU event table, e.g. -DCONTROL_GPT2_OVF_EVENT=0x06F.
// TODO-verify (Pi): confirm the symbol resolves and the timer actually fires.
#ifndef CONTROL_GPT2_OVF_EVENT
#define CONTROL_GPT2_OVF_EVENT  ELC_EVENT_GPT2_COUNTER_OVERFLOW
#endif

// ============================================================================
// Shared state. 32-bit / 16-bit scalars are read torn-free on Cortex-M4
// (single LDR/STR), so the sample ISR (highest prio) and PendSV (lower) share
// them without critical sections. `volatile` where an ISR is the writer.
// ============================================================================

static volatile motor_mode_t  g_motor_mode  = MOTOR_IDLE;
static volatile motor_state_t g_motor_state = MOTOR_STATE_IDLE;
static volatile uint8_t       g_fault       = MOTOR_FAULT_NONE;
static volatile int16_t       g_duty        = 0;        // signed PWM duty

static volatile uint16_t g_adc_raw[3] = { 0, 0, 0 };    // latest 20 kHz scan
static volatile int32_t  g_enc_pos    = 0;              // latest encoder snapshot

// Decimators (boxcar): 20k→1k then 1k→100 cascade.
static uint32_t g_acc1k[3]  = { 0, 0, 0 };
static uint32_t g_acc100[3] = { 0, 0, 0 };
static uint16_t g_cnt1k     = 0;
static uint16_t g_cnt100    = 0;
static volatile uint16_t g_out1k[3]  = { 0, 0, 0 };
static volatile uint16_t g_out100[3] = { 0, 0, 0 };

// Speed estimate (M-method @100 Hz + EMA).
static int32_t          g_enc_prev100 = 0;
static float            g_vel_f       = 0.0f;
static volatile int32_t g_velocity    = 0;             // counts/s

// PendSV outer-tick divider.
static uint16_t g_outer = 0;

// Tuning.
static volatile uint16_t g_oc_thresh = 0;              // overcurrent raw (0=off)
static uint32_t          g_counts_per_rev = 0;

// DAC probe mux.
static struct {
    volatile bool active;
    uint8_t       source;
    int16_t       scale;
    int16_t       offset;
} g_probe = { false, 0, 0, 0 };

// ============================================================================
// H-bridge helpers (register-level writes; ISR-safe).
// ============================================================================

static inline void hbridge_coast(void)
{
    hal_gpio_write(HB_IN1_PORT, HB_IN1_PIN, false);
    hal_gpio_write(HB_IN2_PORT, HB_IN2_PIN, false);
    hal_pwm_set(0u);
}

// Apply a signed duty: direction via IN1/IN2, magnitude via PWM. A latched
// fault forces coast regardless of the requested duty.
static void hbridge_apply(int16_t duty)
{
    if (g_fault != MOTOR_FAULT_NONE) {
        hbridge_coast();
        g_duty = 0;
        g_motor_state = MOTOR_STATE_FAULT;
        return;
    }
    int32_t mag = (duty < 0) ? -(int32_t)duty : (int32_t)duty;
    if (mag > 4095) mag = 4095;

    if (duty > 0) {
        hal_gpio_write(HB_IN1_PORT, HB_IN1_PIN, true);
        hal_gpio_write(HB_IN2_PORT, HB_IN2_PIN, false);
    } else if (duty < 0) {
        hal_gpio_write(HB_IN1_PORT, HB_IN1_PIN, false);
        hal_gpio_write(HB_IN2_PORT, HB_IN2_PIN, true);
    } else {
        hal_gpio_write(HB_IN1_PORT, HB_IN1_PIN, false);
        hal_gpio_write(HB_IN2_PORT, HB_IN2_PIN, false);
    }
    hal_pwm_set((uint16_t)mag);
    g_duty = duty;
    g_motor_state = (mag > 0) ? MOTOR_STATE_RUNNING : MOTOR_STATE_IDLE;
}

// ============================================================================
// DAC probe mux.
// ============================================================================

static void probe_write(int32_t value)
{
    int32_t v = ((value * (int32_t)g_probe.scale) >> PROBE_SHIFT)
              + (int32_t)g_probe.offset;
    if (v < 0)    v = 0;
    if (v > 4095) v = 4095;
    hal_dac_write((uint16_t)v);
}

// Sources freshest at the 20 kHz tick: raw scan 0..2 + motor current.
static void probe_emit_20k(const uint16_t s[3])
{
    if (!g_probe.active) return;
    switch (g_probe.source) {
        case PROBE_A1_20K:  probe_write(s[0]); break;
        case PROBE_A2_20K:  probe_write(s[1]); break;
        case PROBE_A3_20K:  probe_write(s[2]); break;
        case PROBE_CURRENT: probe_write(s[0]); break;
        default: break;     // other sources emitted from the PendSV ticks
    }
}

// Sources freshest at the control ticks: decimated streams + control vars.
static void probe_emit_ctrl(void)
{
    if (!g_probe.active) return;
    switch (g_probe.source) {
        case PROBE_A1_1K:    probe_write(g_out1k[0]);  break;
        case PROBE_A2_1K:    probe_write(g_out1k[1]);  break;
        case PROBE_A3_1K:    probe_write(g_out1k[2]);  break;
        case PROBE_A1_100:   probe_write(g_out100[0]); break;
        case PROBE_A2_100:   probe_write(g_out100[1]); break;
        case PROBE_A3_100:   probe_write(g_out100[2]); break;
        case PROBE_SPEED:    probe_write(g_velocity);  break;
        case PROBE_POSITION: probe_write(g_enc_pos);   break;
        case PROBE_DUTY:     probe_write(g_duty);      break;
        default: break;     // 20 kHz sources emitted from the sample ISR
    }
}

// ============================================================================
// Speed estimate — M-method (fixed-time counting) @100 Hz + 1-pole EMA.
// ============================================================================

static void speed_update(void)
{
    int32_t pos = g_enc_pos;                       // atomic 32-bit read
    int32_t d   = pos - g_enc_prev100;             // modular: 32-bit wrap-safe
    g_enc_prev100 = pos;
    float vraw = (float)d * (float)(SAMPLE_RATE_HZ / DECIM_1K / OUTER_DIV); // ×100
    g_vel_f += EMA_ALPHA * (vraw - g_vel_f);
    g_velocity = (int32_t)g_vel_f;
}

// ============================================================================
// Motor-mode descriptor table.
// ============================================================================

typedef struct {
    void (*on_enter)(void);
    void (*on_exit)(void);
    void (*tick_1k)(void);     // PendSV inner — torque/current loop + safety
    void (*tick_100)(void);    // PendSV outer — speed/velocity/position/profile
} motor_desc_t;

// Forward decls of the sampling lifecycle (used by mode enters).
static void sampling_start(void);
static void sampling_stop(void);

// ---- MOTOR_IDLE ------------------------------------------------------------
static void idle_on_enter(void)
{
    sampling_stop();
    hbridge_coast();
    g_duty = 0;
    g_motor_state = MOTOR_STATE_IDLE;
}

// ---- MOTOR_MANUAL ----------------------------------------------------------
static void manual_on_enter(void)
{
    g_fault = MOTOR_FAULT_NONE;
    hal_pwm_config(20000u);          // boot/mode-fixed 20 kHz motor PWM
    hal_pwm_set(0u);
    hal_gpio_config(HB_IN1_PORT, HB_IN1_PIN, true, false);
    hal_gpio_config(HB_IN2_PORT, HB_IN2_PIN, true, false);
    hbridge_coast();
    g_duty = 0;
    sampling_start();                // 20 kHz ADC scan + control ticks live
    g_motor_state = MOTOR_STATE_IDLE;
}

static void manual_on_exit(void)
{
    sampling_stop();
    hbridge_coast();
}

static const motor_desc_t g_motor_modes[MOTOR_MODE_COUNT] = {
    [MOTOR_IDLE]   = { idle_on_enter,   NULL,          NULL, NULL },
    [MOTOR_MANUAL] = { manual_on_enter, manual_on_exit, NULL, NULL },
    // [MOTOR_PID] / [MOTOR_SCURVE] / [MOTOR_WINDOW] — added with their increments.
};

#define MOTOR_MODE_MAX_IMPLEMENTED  MOTOR_MANUAL

// ============================================================================
// Tier 1 — 20 kHz sample ISR (GPT2 overflow).
// ============================================================================

void control_sample_isr(void)
{
    // Clear the GPT2 overflow + ICU latch (mode.c ordering).
    R_GPT2->GTST &= ~GTST_TCFPO;
    (void)R_GPT2->GTST;
    R_ICU->IELSR[CONTROL_SAMPLE_IRQ] &= ~IELSR_IR;

    // 3-channel scan (A1 current / A2 / A3) + encoder snapshot.
    uint16_t s[3];
    hal_adc_scan3_read(s);
    g_adc_raw[0] = s[0];
    g_adc_raw[1] = s[1];
    g_adc_raw[2] = s[2];
    g_enc_pos = hal_encoder_read();

    // Fast-path overcurrent cutout (safety reacts here, not at 1 kHz).
    if (g_oc_thresh != 0u && s[0] > g_oc_thresh) {
        hbridge_coast();
        g_fault = MOTOR_FAULT_OVERCURRENT;
        g_duty = 0;
        g_motor_state = MOTOR_STATE_FAULT;
    }

    // Boxcar decimation 20k→1k, cascaded 1k→100.
    for (uint32_t i = 0; i < 3; i++) {
        g_acc1k[i] += s[i];
    }
    if (++g_cnt1k >= DECIM_1K) {
        g_cnt1k = 0;
        for (uint32_t i = 0; i < 3; i++) {
            uint16_t v = (uint16_t)(g_acc1k[i] / DECIM_1K);
            g_out1k[i]  = v;
            g_acc100[i] += v;
            g_acc1k[i]   = 0;
        }
        if (++g_cnt100 >= DECIM_100) {
            g_cnt100 = 0;
            for (uint32_t i = 0; i < 3; i++) {
                g_out100[i]  = (uint16_t)(g_acc100[i] / DECIM_100);
                g_acc100[i]  = 0;
            }
        }
        // 1 kHz boundary → pend the control math (PendSV, lower priority).
        SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    }

    probe_emit_20k(s);
}

// ============================================================================
// Tier 2 — PendSV (1 kHz inner + 100 Hz outer). Pended by the sample ISR.
// ============================================================================

void PendSV_Handler(void)
{
    motor_mode_t m = g_motor_mode;
    const motor_desc_t* d = &g_motor_modes[m];

    // 1 kHz inner: torque/current loop + safety (per active mode).
    if (d->tick_1k) d->tick_1k();

    // 100 Hz outer: shared speed estimate + the mode's velocity/position/profile.
    if (++g_outer >= OUTER_DIV) {
        g_outer = 0;
        speed_update();
        if (d->tick_100) d->tick_100();
    }

    probe_emit_ctrl();
}

// ============================================================================
// GPT2 20 kHz sample timer.
// ============================================================================

static void gpt2_start(void)
{
    R_MSTP->MSTPCRD &= ~MSTPD_GPT27;            // ungate GPT2..7
    (void)R_MSTP->MSTPCRD;

    R_GPT2->GTWP     = GPT_WP_UNLOCK;
    R_GPT2->GTCR     = 0u;                      // MD=000 saw-wave, /1, stopped
    R_GPT2->GTUDDTYC = 3u;                      // latch count-up...
    R_GPT2->GTUDDTYC = 1u;                      // ...clear the force bit
    R_GPT2->GTPR     = SAMPLE_PERIOD - 1u;
    R_GPT2->GTPBR    = SAMPLE_PERIOD - 1u;
    R_GPT2->GTCNT    = 0u;
    R_GPT2->GTST     = 0u;
    R_GPT2->GTINTAD  = GTINTAD_OVF;             // overflow IRQ
    R_GPT2->GTWP     = GPT_WP_LOCK;

    R_ICU->IELSR[CONTROL_SAMPLE_IRQ] = (uint32_t)CONTROL_GPT2_OVF_EVENT;
    NVIC_ClearPendingIRQ((IRQn_Type)CONTROL_SAMPLE_IRQ);
    NVIC_SetPriority((IRQn_Type)CONTROL_SAMPLE_IRQ, 1u);   // above PendSV (3)
    NVIC_EnableIRQ((IRQn_Type)CONTROL_SAMPLE_IRQ);

    R_GPT2->GTWP  = GPT_WP_UNLOCK;
    R_GPT2->GTCR |= GTCR_CST;
    R_GPT2->GTWP  = GPT_WP_LOCK;
}

static void gpt2_stop(void)
{
    R_GPT2->GTWP  = GPT_WP_UNLOCK;
    R_GPT2->GTCR &= ~GTCR_CST;
    R_GPT2->GTINTAD = 0u;
    R_GPT2->GTWP  = GPT_WP_LOCK;

    NVIC_DisableIRQ((IRQn_Type)CONTROL_SAMPLE_IRQ);
    R_ICU->IELSR[CONTROL_SAMPLE_IRQ] = 0u;
}

// ============================================================================
// Sampling lifecycle — ADC scan + decimator/speed reset + the 20 kHz tick.
// ============================================================================

static void sampling_start(void)
{
    hal_adc_scan3_setup();

    for (uint32_t i = 0; i < 3; i++) {
        g_acc1k[i] = 0; g_acc100[i] = 0; g_out1k[i] = 0; g_out100[i] = 0;
    }
    g_cnt1k = 0; g_cnt100 = 0; g_outer = 0;
    g_enc_prev100 = g_enc_pos;
    g_vel_f = 0.0f; g_velocity = 0;

    gpt2_start();
}

static void sampling_stop(void)
{
    gpt2_stop();
}

// ============================================================================
// Public API.
// ============================================================================

void control_init(void)
{
    // PendSV below the sample ISR so torque math can't block sampling.
    NVIC_SetPriority(PendSV_IRQn, 3u);

    // Install the 20 kHz sample ISR into the relocated RAM vector table.
    mode_vector_install(CONTROL_SAMPLE_IRQ, control_sample_isr);

    // Park the H-bridge safe (outputs low → coast).
    hal_gpio_config(HB_IN1_PORT, HB_IN1_PIN, true, false);
    hal_gpio_config(HB_IN2_PORT, HB_IN2_PIN, true, false);
    hbridge_coast();

    // Encoder is boot-fixed (always readable, even at idle).
    hal_encoder_setup();

    g_motor_mode  = MOTOR_IDLE;
    g_motor_state = MOTOR_STATE_IDLE;
    g_fault       = MOTOR_FAULT_NONE;
}

void control_service(void)
{
    // Tier 0 seam: pseudo-interlock op-complete event delivery lands here with
    // MODE_SCURVE. Nothing to do for IDLE / MANUAL.
}

uint8_t control_set_motor_mode(motor_mode_t m)
{
    if (m > MOTOR_MODE_MAX_IMPLEMENTED) return 1u;
    if (m == g_motor_mode) return 0u;

    // Entering a driving mode: free the ADC from the DSP slot.
    if (m != MOTOR_IDLE) {
        mode_set(MODE_WORKBENCH);     // DSP slot → NONE
    }

    if (g_motor_modes[g_motor_mode].on_exit) {
        g_motor_modes[g_motor_mode].on_exit();
    }
    g_motor_mode = m;
    g_fault = MOTOR_FAULT_NONE;
    if (g_motor_modes[m].on_enter) {
        g_motor_modes[m].on_enter();
    }
    return 0u;
}

motor_mode_t  control_get_motor_mode(void)  { return g_motor_mode; }
motor_state_t control_get_motor_state(void) { return g_motor_state; }

uint8_t control_motor_drive(int16_t duty)
{
    if (g_motor_mode != MOTOR_MANUAL) return 1u;
    if (duty == 0) {
        g_fault = MOTOR_FAULT_NONE;   // a stop command acknowledges/clears fault
    }
    hbridge_apply(duty);
    return 0u;
}

int32_t  control_position(void)    { return g_enc_pos; }
int32_t  control_velocity(void)    { return g_velocity; }
uint16_t control_current_raw(void) { return g_adc_raw[0]; }
int16_t  control_duty(void)        { return g_duty; }
uint8_t  control_fault(void)       { return g_fault; }

void control_encoder_reset(void)
{
    hal_encoder_reset();
    g_enc_pos      = hal_encoder_read();
    g_enc_prev100  = g_enc_pos;
    g_vel_f        = 0.0f;
    g_velocity     = 0;
}

void control_set_overcurrent(uint16_t overcurrent_raw) { g_oc_thresh = overcurrent_raw; }
void control_set_counts_per_rev(uint32_t cpr)          { g_counts_per_rev = cpr; }
uint32_t control_counts_per_rev(void)                  { return g_counts_per_rev; }

uint8_t control_dac_probe(uint8_t source, int16_t scale, int16_t offset)
{
    if (source >= PROBE_SOURCE_COUNT) return 1u;
    g_probe.source = source;
    g_probe.scale  = scale;
    g_probe.offset = offset;
    g_probe.active = true;
    return 0u;
}

void control_dac_probe_stop(void)
{
    g_probe.active = false;
}
