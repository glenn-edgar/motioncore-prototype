// ============================================================================
// mode.c — RA4M1 multi-mode foundation implementation.
//
// See mode.h for the architecture. Bare CMSIS registers throughout (no FSP
// HAL), consistent with ra4m1_hal.c.
//
// Register facts (RA4M1 User's Manual R01UH0887EJ0100, FSP r_gpt.c):
//   * GPT0 = the periodic timer here. Channels 0/1 are 32-bit; module-stop bit
//     is MSTPD5 in MSTPCRD. GTWP write-protect key = 0xA5.
//   * The periodic interrupt is GPT0 counter-overflow (event 0x05D), routed by
//     R_ICU->IELSR[MODE_PERIODIC_IRQ] to NVIC slot MODE_PERIODIC_IRQ.
//   * Vector table = 16 Cortex-M system entries + 32 ICU entries (48 total);
//     g_vector_table in the BSP is `const` flash, so we relocate a RAM copy.
// ============================================================================

#include "mode.h"

#include "bsp_api.h"   // CMSIS core (SCB, NVIC_*, __DMB/__DSB/__ISB) + R_* regs

// ---- definitions exported via mode.h --------------------------------------

volatile device_mode_t g_device_mode = MODE_WORKBENCH;
uint8_t g_mode_arena[MODE_ARENA_SIZE] __attribute__((aligned(8)));

// ---- foundation constants --------------------------------------------------

#define VT_ENTRIES          48u          // 16 system exceptions + 32 ICU slots
#define VT_SYSTEM_ENTRIES   16u          // ICU slot n -> vector table index 16+n

#define MODE_PERIODIC_IRQ   4u           // free ICU/NVIC slot (USB owns 0..3)
#define EVENT_GPT0_OVF      0x05Du       // ICU event: GPT0 counter overflow

#define GPT_PCLKD_HZ        48000000u    // PCLKD = 48 MHz (bsp_clock_cfg: /1)
#define GPT_WP_UNLOCK       0x0000A500u  // GTWP: key 0xA5, WP=0
#define GPT_WP_LOCK         0x0000A501u  // GTWP: key 0xA5, WP=1

#define MSTPD_GPT01         (1u << 5)    // MSTPCRD bit 5: GPT320/GPT321 block
#define GTCR_CST            (1u << 0)    // GTCR counter start
#define GTINTAD_GTINTPR_OVF (1u << 6)    // GTINTPR field = 01b: overflow IRQ
#define GTST_TCFPO          (1u << 6)    // GTST overflow flag
#define IELSR_IR            (1u << 16)   // IELSR interrupt-status flag

// ---- relocated vector table ------------------------------------------------
// 256-byte aligned: the table is 48*4 = 192 B; Cortex-M requires the VTOR base
// aligned to the next power of two >= the table size.

static uint32_t g_ram_vectors[VT_ENTRIES] __attribute__((aligned(256)));

// ---- mode descriptor table -------------------------------------------------
// One row per mode. Step 4 implements only MODE_WORKBENCH; the workbench
// periodic callback is the DAC waveform stepper in ra4m1_commands.c. Modes 2-4
// fill their rows when those ports land — no change to the dispatch core.

extern void workbench_periodic_isr(void);   // ra4m1_commands.c
extern void spectral_on_enter   (void);     // spectral.c
extern void spectral_on_exit    (void);     // spectral.c
extern void spectral_periodic_isr(void);    // spectral.c — ADC sample tick
extern void goertzel_on_enter   (void);     // goertzel.c
extern void goertzel_on_exit    (void);     // goertzel.c
extern void goertzel_periodic_isr(void);    // goertzel.c — ADC sample tick + recursion

static const mode_descriptor_t g_modes[MODE_COUNT] = {
    [MODE_WORKBENCH] = { NULL,              NULL,             workbench_periodic_isr },
    [MODE_SPECTRAL]  = { spectral_on_enter, spectral_on_exit, spectral_periodic_isr  },
    [MODE_GOERTZEL]  = { goertzel_on_enter, goertzel_on_exit, goertzel_periodic_isr  },
    // [MODE_PID] / [MODE_SCURVE] — added with their ports.
};

// Highest mode that is actually implemented. mode_set() rejects anything above
// it. Bump this (one line) as new modes land.
#define MODE_MAX_IMPLEMENTED  MODE_GOERTZEL

// ---- mode periodic-timer ISR ----------------------------------------------
// Installed at vector index VT_SYSTEM_ENTRIES + MODE_PERIODIC_IRQ. Shared by
// every mode: it clears the hardware flags and dispatches to the active mode's
// on_periodic callback. The per-ISR table lookup costs a few cycles — nil at
// the ISR rates involved.

void mode_periodic_isr_entry(void)
{
    // Negate the source, then clear the ICU latch (manual §13.2.6 ordering).
    R_GPT0->GTST &= ~GTST_TCFPO;
    (void)R_GPT0->GTST;                          // read-back so the clear settles
    R_ICU->IELSR[MODE_PERIODIC_IRQ] &= ~IELSR_IR;

    void (*fn)(void) = g_modes[g_device_mode].on_periodic;
    if (fn != NULL) {
        fn();
    }
}

// ---- GPT0 periodic timer ---------------------------------------------------

static void periodic_timer_prepare(void)
{
    // Ungate the GPT320/GPT321 block (read-modify-write — reserved MSTP bits
    // must be written back as 1).
    R_MSTP->MSTPCRD &= ~MSTPD_GPT01;
    (void)R_MSTP->MSTPCRD;                       // settle before touching GPT0

    R_GPT0->GTWP = GPT_WP_UNLOCK;
    R_GPT0->GTCR = 0;                            // MD=000 saw-wave, TPCS=/1, stopped
    R_GPT0->GTUDDTYC = 3u;                       // UD=1 UDF=1: latch count-up...
    R_GPT0->GTUDDTYC = 1u;                       // ...then clear the force bit
    R_GPT0->GTINTAD = 0;                         // overflow IRQ off until _start
    R_GPT0->GTWP = GPT_WP_LOCK;
}

void mode_periodic_start(uint32_t rate_hz)
{
    if (rate_hz == 0u) {
        return;
    }
    uint32_t period = GPT_PCLKD_HZ / rate_hz;
    if (period == 0u) {
        period = 1u;
    }

    R_GPT0->GTWP = GPT_WP_UNLOCK;
    R_GPT0->GTCR &= ~GTCR_CST;                   // stop while reprogramming
    R_GPT0->GTPR  = period - 1u;                 // counter runs 0..GTPR
    R_GPT0->GTPBR = period - 1u;
    R_GPT0->GTCNT = 0;
    R_GPT0->GTST  = 0;                           // clear stale overflow flag
    R_GPT0->GTINTAD = GTINTAD_GTINTPR_OVF;       // request IRQ on overflow
    R_GPT0->GTWP = GPT_WP_LOCK;

    // Route GPT0 overflow -> NVIC slot MODE_PERIODIC_IRQ.
    R_ICU->IELSR[MODE_PERIODIC_IRQ] = EVENT_GPT0_OVF;
    NVIC_ClearPendingIRQ((IRQn_Type)MODE_PERIODIC_IRQ);
    NVIC_SetPriority((IRQn_Type)MODE_PERIODIC_IRQ, 2u);   // below USB
    NVIC_EnableIRQ((IRQn_Type)MODE_PERIODIC_IRQ);

    R_GPT0->GTWP = GPT_WP_UNLOCK;
    R_GPT0->GTCR |= GTCR_CST;                    // start counting
    R_GPT0->GTWP = GPT_WP_LOCK;
}

void mode_periodic_stop(void)
{
    R_GPT0->GTWP = GPT_WP_UNLOCK;
    R_GPT0->GTCR &= ~GTCR_CST;                   // stop
    R_GPT0->GTINTAD = 0;
    R_GPT0->GTWP = GPT_WP_LOCK;

    NVIC_DisableIRQ((IRQn_Type)MODE_PERIODIC_IRQ);
    R_ICU->IELSR[MODE_PERIODIC_IRQ] = 0;         // unlink the event
}

// ---- VTOR relocation -------------------------------------------------------

static void relocate_vector_table(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    // Copy the live table whole — system entries [0..15] and ICU entries
    // [16..47] are separate symbols in flash but laid contiguously, and the
    // USB handlers (slots 0..3) must be preserved.
    const uint32_t *src = (const uint32_t *)SCB->VTOR;
    for (uint32_t i = 0; i < VT_ENTRIES; i++) {
        g_ram_vectors[i] = src[i];
    }
    g_ram_vectors[VT_SYSTEM_ENTRIES + MODE_PERIODIC_IRQ] =
        (uint32_t)&mode_periodic_isr_entry;

    __DMB();
    SCB->VTOR = (uint32_t)&g_ram_vectors[0];
    __DSB();
    __ISB();

    if (primask == 0u) {
        __enable_irq();
    }
}

void mode_vector_install(uint32_t icu_slot, void (*handler)(void))
{
    // icu_slot indexes the ICU portion of the table (entry VT_SYSTEM_ENTRIES +
    // icu_slot). The relocated table must already exist — call after mode_init.
    if (icu_slot >= (VT_ENTRIES - VT_SYSTEM_ENTRIES)) {
        return;
    }
    g_ram_vectors[VT_SYSTEM_ENTRIES + icu_slot] = (uint32_t)handler;
    __DSB();
    __ISB();
}

// ---- public API ------------------------------------------------------------

void mode_init(void)
{
    relocate_vector_table();
    periodic_timer_prepare();

    g_device_mode = MODE_WORKBENCH;
    if (g_modes[MODE_WORKBENCH].on_enter != NULL) {
        g_modes[MODE_WORKBENCH].on_enter();
    }
}

uint8_t mode_set(device_mode_t new_mode)
{
    if (new_mode > MODE_MAX_IMPLEMENTED) {
        return 1u;                               // out of range / not implemented
    }
    if (new_mode == g_device_mode) {
        return 0u;
    }

    // Quiesce the periodic ISR across the swap so on_exit / on_enter run
    // without the dispatcher racing on g_device_mode.
    NVIC_DisableIRQ((IRQn_Type)MODE_PERIODIC_IRQ);

    if (g_modes[g_device_mode].on_exit != NULL) {
        g_modes[g_device_mode].on_exit();
    }
    mode_periodic_stop();

    g_device_mode = new_mode;

    if (g_modes[new_mode].on_enter != NULL) {
        g_modes[new_mode].on_enter();
    }
    return 0u;
}

device_mode_t mode_get(void)
{
    return g_device_mode;
}
