// ============================================================================
// mode.h — RA4M1 multi-mode foundation.
//
// The RA4M1 dongle runs ONE operating mode at a time. Mode 1 (workbench) is
// the reactive analytical-HIL command shell — host sends OP_SHELL_EXEC, the
// device runs one command and replies. Modes 2-4 (spectral / closed-loop
// encoder control / S-curve motion profile) are autonomous behaviours added
// later: the device runs a loop, the host only configures and monitors.
//
// A mode is device state — g_device_mode — selected by CMD_SET_MODE. The
// foundation here is deliberately cheap (~1.5 KB flash, a few bytes RAM); the
// real cost is each mode's own code/buffers, paid only when that mode exists.
//
// Foundation pieces, all set up once by mode_init():
//   * VTOR relocation — the BSP vector table is `const` in flash with only the
//     USB slots populated; we copy all 48 entries to a RAM table so mode
//     dispatcher handlers can be installed in the free slots.
//   * mode descriptor table (g_modes[] in mode.c) — one row per mode with
//     enter/exit/periodic callbacks. Adding a mode = one row + its functions,
//     no change to the dispatch core.
//   * mode periodic timer — GPT0 + ICU NVIC slot MODE_PERIODIC_IRQ; its ISR
//     calls the active mode's on_periodic callback. Workbench drives the DAC
//     waveform generator from it; modes 3/4 will run their control loop here.
//   * g_mode_arena — a shared RAM buffer. Only one mode runs at a time, so
//     per-mode state OVERLAYS here (sized to the largest mode) instead of
//     every mode statically summing its own buffers.
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

// ---- mode identifiers ------------------------------------------------------
// Only MODE_WORKBENCH is implemented in step 4. The others are reserved so the
// enum + CMD_SET_MODE wire contract is stable as modes 2-4 land.

typedef enum {
    MODE_WORKBENCH = 0,   // reactive HIL command shell
    MODE_SPECTRAL  = 1,   // (future) FFT / analog processing
    MODE_PID       = 2,   // (future) closed-loop encoder motor control
    MODE_SCURVE    = 3,   // (future) S-curve motion profile (e.g. wiper)
    MODE_COUNT
} device_mode_t;

extern volatile device_mode_t g_device_mode;

// ---- per-mode descriptor ---------------------------------------------------
// on_enter    — configure the mode's peripherals (called by mode_set on the
//               NEW mode). NULL = nothing to do (workbench inits lazily).
// on_exit     — tear the mode down: stop its timers, disable its IRQs (called
//               by mode_set on the OLD mode). NULL = nothing to do.
// on_periodic — runs inside the mode periodic-timer ISR. NULL = the mode has
//               no periodic behaviour (the timer is simply left stopped).

typedef struct {
    void (*on_enter)(void);
    void (*on_exit)(void);
    void (*on_periodic)(void);
} mode_descriptor_t;

// ---- shared mode RAM arena -------------------------------------------------
// Per-mode working state overlays here — only the active mode owns it, and
// nothing in it survives a mode switch. Step 4 stores only the workbench DAC
// waveform state; modes 2-4 add their own (larger) views later, at which point
// MODE_ARENA_SIZE grows to the largest mode. 8-aligned for u32/struct members.

#define MODE_ARENA_SIZE  256u
extern uint8_t g_mode_arena[MODE_ARENA_SIZE] __attribute__((aligned(8)));

// ---- foundation API --------------------------------------------------------

// Call once from main(), after tusb_init() (so the live USB vector entries are
// captured into the relocated table). Relocates the vector table to RAM,
// prepares the mode periodic timer (stopped), and enters MODE_WORKBENCH.
void mode_init(void);

// Switch modes: runs the old mode's on_exit, stops the periodic timer, sets
// g_device_mode, runs the new mode's on_enter. Returns 0 on success, 1 if
// new_mode is out of range. Runs with the periodic IRQ masked during the swap.
uint8_t mode_set(device_mode_t new_mode);

device_mode_t mode_get(void);

// Install an interrupt handler into the relocated RAM vector table at ICU/NVIC
// slot `icu_slot` (table index VT_SYSTEM_ENTRIES + icu_slot). For workbench /
// mode peripherals that own an interrupt beyond the mode periodic timer — e.g.
// the PWM duty-dither ISR. Slots 0-3 are USB, slot 4 is the mode periodic
// timer; use 5+. Call after mode_init().
void mode_vector_install(uint32_t icu_slot, void (*handler)(void));

// ---- mode periodic timer (GPT0) --------------------------------------------
// The active mode's on_periodic callback runs at rate_hz. Used by the
// workbench DAC waveform generator and (later) the modes 2-4 control loops.
// Calling _start while running re-programs the rate. _stop is idempotent.

void mode_periodic_start(uint32_t rate_hz);
void mode_periodic_stop(void);
