// ============================================================================
// control.h — RA4M1 motor-control core (the "motor slot" of the two-slot model).
//
// This is the 3-tier real-time control core locked in the 2026-06-03/04 RA4M1
// API redesign. It runs INDEPENDENTLY of the mode.c DSP framework (spectral /
// goertzel / workbench), realising the design's "two slots run concurrently":
//
//   * motor slot  = THIS module   (IDLE / MANUAL / PID / SCURVE / WINDOW)
//   * DSP   slot  = mode.c        (NONE  / GOERTZEL / SPECTRAL / ANALOG)
//
// The two are mutually exclusive on the ADC by the heavy-FFT gate: entering a
// non-IDLE motor mode forces the DSP slot back to NONE (mode WORKBENCH) so the
// motor's PWM-rate ADC scan owns the converter.
//
// ---- the tier stack (see ra4m1-api-redesign-2026-06-03) --------------------
//   Tier 1 — 20 kHz sample ISR (control_sample_isr, GPT2 overflow): reads the
//            3-channel ADC scan (A1 current / A2 / A3) + encoder snapshot,
//            fast-path overcurrent compare, boxcar decimation 20k→1k→100, DAC
//            probe write for the 20 kHz sources, and pends PendSV every 20th
//            sample (→ 1 kHz). Time-critical minimum only.
//   Tier 2 — PendSV (1 kHz inner + 100 Hz outer via mod-10): the active motor
//            mode's torque/safety tick (1 kHz) and speed/velocity/position/
//            profile tick (100 Hz). 0 ICU slots — PendSV is a core exception.
//   Tier 0 — control_service() in the main loop: mode lifecycle + (later) the
//            pseudo-interlock op-complete event delivery. Never blocks the ISRs.
//
// Bring-up note (Increment 1): only MOTOR_IDLE + MOTOR_MANUAL are implemented.
// MANUAL is open-loop `motor_drive` (signed duty straight to the H-bridge) with
// fast-path overcurrent cutout — the bring-up / tuning state. PID / SCURVE /
// WINDOW plug into the same mode table + PendSV ticks in later increments.
//
// Sampling is driven by a dedicated GPT2 timer rather than the GTADTRA PWM
// trigger of the design; phase-locking the scan to the PWM mid-period is a
// hardware-tuning refinement (see control.c TODO-verify notes).
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

// ---- motor-slot modes ------------------------------------------------------
// Matches the design's motor enum: IDLE=0 / MANUAL=1 / PID=2 / SCURVE=3 /
// WINDOW=4. Only IDLE + MANUAL are implemented in Increment 1.

typedef enum {
    MOTOR_IDLE   = 0,   // safe default: H-bridge coast, PWM 0, no sampling
    MOTOR_MANUAL = 1,   // open-loop direct drive (motor_drive), bring-up/tuning
    MOTOR_PID    = 2,   // (future) cascade pos→vel→current closed loop
    MOTOR_SCURVE = 3,   // (future) jerk-limited motion profile + op event
    MOTOR_WINDOW = 4,   // (future) S-curve + autonomous pinch
    MOTOR_MODE_COUNT
} motor_mode_t;

// ---- motor run state (reported by get_status) ------------------------------
typedef enum {
    MOTOR_STATE_IDLE    = 0,   // no mode active / coasting
    MOTOR_STATE_RUNNING = 1,   // driving
    MOTOR_STATE_FAULT   = 2,   // latched fault (overcurrent) — drive cut
} motor_state_t;

// ---- fault reasons (control_fault) -----------------------------------------
#define MOTOR_FAULT_NONE         0u
#define MOTOR_FAULT_OVERCURRENT  1u
#define MOTOR_FAULT_STALL        2u

// ---- DAC probe-mux sources -------------------------------------------------
// 0..8 = the 9 ADC streams (3 channels × 3 rates). 9+ = internal control vars.
// Written by the producing tier (20 kHz streams in the sample ISR; the rest in
// the PendSV ticks). See control_dac_probe().
typedef enum {
    PROBE_A1_20K = 0, PROBE_A2_20K, PROBE_A3_20K,   // raw 20 kHz scan
    PROBE_A1_1K,      PROBE_A2_1K,  PROBE_A3_1K,     // 1 kHz decimated
    PROBE_A1_100,     PROBE_A2_100, PROBE_A3_100,    // 100 Hz decimated
    PROBE_SPEED  = 9,         // velocity estimate (counts/s)
    PROBE_POSITION,           // encoder position (counts)
    PROBE_CURRENT,            // motor current (A1 raw, 14-bit)
    PROBE_DUTY,               // signed PWM duty / control output
    PROBE_SOURCE_COUNT
} probe_source_t;

// ---- lifecycle -------------------------------------------------------------
// control_init(): call once from main() AFTER mode_init() (needs the relocated
// RAM vector table to install the GPT2 sample ISR). Sets PendSV priority, parks
// the H-bridge safe, brings up the encoder (boot-fixed), enters MOTOR_IDLE.
void control_init(void);

// control_service(): call every main-loop iteration (Tier 0). No-op in
// Increment 1 beyond reserving the seam for the pseudo-interlock event.
void control_service(void);

// ---- motor-slot selection --------------------------------------------------
// 0 = ok, 1 = bad/unimplemented mode. Entering a non-IDLE mode forces the DSP
// slot to NONE (mode WORKBENCH) so the control ADC scan owns the converter.
uint8_t      control_set_motor_mode(motor_mode_t m);
motor_mode_t control_get_motor_mode(void);
motor_state_t control_get_motor_state(void);

// ---- MANUAL open-loop drive ------------------------------------------------
// Signed duty -4095..+4095 → H-bridge direction + |duty| PWM. Only valid in
// MOTOR_MANUAL (returns 1 otherwise). Clears a latched fault on a 0 command.
uint8_t control_motor_drive(int16_t duty);

// ---- shared state (Tier-0 readers; ISR/PendSV writers) ---------------------
int32_t  control_position(void);      // encoder counts (signed, x4)
int32_t  control_velocity(void);      // counts/s (M-method @100 Hz + EMA)
uint16_t control_current_raw(void);   // A1 latest raw ADC (14-bit)
int16_t  control_duty(void);          // current signed duty
uint8_t  control_fault(void);         // MOTOR_FAULT_*

void control_encoder_reset(void);     // zero position + speed state

// ---- tuning / config -------------------------------------------------------
// overcurrent_raw: A1 raw-count cutout (0 = disabled). counts_per_rev: enables
// on-chip RPM / order-tracking conversions (0 = unknown).
void     control_set_overcurrent(uint16_t overcurrent_raw);
void     control_set_counts_per_rev(uint32_t cpr);
uint32_t control_counts_per_rev(void);

// ---- DAC probe mux ---------------------------------------------------------
// DAC(A0) = clamp((value * scale >> PROBE_SHIFT) + offset, 0, 4095). Mutually
// exclusive with dac_write / dac_waveform (one DAC). 0 = ok, 1 = bad source.
uint8_t control_dac_probe(uint8_t source, int16_t scale, int16_t offset);
void    control_dac_probe_stop(void);

// ---- bench PWM characterization --------------------------------------------
// Lazy-configures GPT3 at 20 kHz on D8 (period 2400 counts, ~11.2-bit) on first
// call, then sets the duty directly in raw period counts (0..period). Decoupled
// from the motor modes AND the ISRs — for scoping the drive output in IDLE
// before any real-time code is enabled. Returns the period (= max counts = 100%
// duty); on the scope, high-time = counts × (1/48 MHz) = counts × 20.8 ns.
uint16_t control_pwm_test(uint16_t counts);

// ---- DAC test-signal generator (A0→A1 loopback decimation test) ------------
// Jumper A0 to A1, run a sampling mode (MANUAL), drive a known signal here, and
// read the 20k/1k/100 taps back via control_streams() to verify the boxcar
// decimators. Two modes (mutually exclusive with the dac_probe mux — one DAC):
//   const  — DAC held at `value` (written directly; works any time)
//   square — DAC toggles low↔high at freq_hz, generated in the 20 kHz sample ISR
//            (phase-locked to sampling; requires a sampling mode active).
void     control_dac_const(uint16_t value);
uint16_t control_dac_square(uint16_t freq_hz, uint16_t low, uint16_t high); // → realized Hz (0=off)

// Latest decimator taps: [A1,A2,A3]@20k, [A1,A2,A3]@1k, [A1,A2,A3]@100 (raw counts).
void     control_streams(uint16_t out[9]);
