// ============================================================================
// samd21_interlocks.c — interlock framework foundation, slice 1.
// See samd21_interlocks.h for design summary.
// ============================================================================

#include "samd21_interlocks.h"
#include "samd21.h"
#include "vendor/libcomm/opcodes.h"   // SHELL_STATUS_* values

// ---------------------------------------------------------------------------
// Persistent state — lives in the .noinit linker section so RAM contents
// survive WDT / software / external resets. On POR + brown-out the section
// holds undefined garbage; interlock_boot_decide() recognises this via
// magic mismatch and re-initialises.
//
// Linker script seeeduino_xiao.ld MUST contain:
//   .noinit (NOLOAD) :
//   {
//       . = ALIGN(4);
//       KEEP(*(.noinit*))
//       . = ALIGN(4);
//   } > ram
//
// placed AFTER .bss so it isn't zeroed by the startup .bss loop, and AFTER
// .data so the .data init loop doesn't try to source from flash.
// ---------------------------------------------------------------------------

interlock_persist_t g_interlock_persist __attribute__((section(".noinit")));

// Tracks the currently-executing slot for HardFault attribution. Lives in
// .data (gets zeroed at startup is fine — fault attribution applies to the
// crash that NEXT happens, not a prior one). Initialise to "none" so a
// HardFault occurring outside any interlock tick is recorded that way.
volatile uint8_t g_active_interlock_slot = INTERLOCK_CRASHED_SLOT_NONE;

// ---------------------------------------------------------------------------
// Compile-time interlock registry. Slice-1 stub: the only entry is a no-op
// used to verify slot lifecycle + persistence across WDT bite. Slice 2 will
// add gpio_int / adc_int entries driven by parsed DSL.
// ---------------------------------------------------------------------------

static void noop_init(void)      { /* nothing */ }
static void noop_tick(void)      { /* nothing */ }
static void noop_terminate(void) { /* nothing */ }

const interlock_def_t g_interlocks[] = {
    // id = 1 (INTERLOCK_ID_NOOP)
    { "noop", noop_init, noop_tick, noop_terminate },
};

const uint8_t g_interlock_count = (uint8_t)(sizeof(g_interlocks) / sizeof(g_interlocks[0]));

// ---------------------------------------------------------------------------
// Boot decision
// ---------------------------------------------------------------------------

static bool persist_is_valid(void) {
    if (g_interlock_persist.magic != INTERLOCK_MAGIC) return false;
    if (g_interlock_persist.version != INTERLOCK_PERSIST_VERSION) return false;
    for (uint8_t i = 0; i < INTERLOCK_MAX_SLOTS; i++) {
        const interlock_slot_persist_t* s = &g_interlock_persist.slots[i];
        if (s->state > INTERLOCK_SLOT_POISONED) return false;
        if (s->id > g_interlock_count) return false;
        if (s->state == INTERLOCK_SLOT_ARMED && s->id == INTERLOCK_ID_NONE) return false;
    }
    if (g_interlock_persist.crash.last_crashed_slot != INTERLOCK_CRASHED_SLOT_NONE
        && g_interlock_persist.crash.last_crashed_slot >= INTERLOCK_MAX_SLOTS) {
        return false;
    }
    return true;
}

static void persist_cold_init(void) {
    g_interlock_persist.magic   = INTERLOCK_MAGIC;
    g_interlock_persist.version = INTERLOCK_PERSIST_VERSION;
    g_interlock_persist.reserved[0] = 0;
    g_interlock_persist.reserved[1] = 0;
    g_interlock_persist.reserved[2] = 0;
    for (uint8_t i = 0; i < INTERLOCK_MAX_SLOTS; i++) {
        g_interlock_persist.slots[i].state        = INTERLOCK_SLOT_EMPTY;
        g_interlock_persist.slots[i].id           = INTERLOCK_ID_NONE;
        g_interlock_persist.slots[i].boot_counter = 0;
        g_interlock_persist.slots[i].reserved     = 0;
    }
    g_interlock_persist.crash.last_pc            = 0;
    g_interlock_persist.crash.last_lr            = 0;
    g_interlock_persist.crash.last_rstsr         = 0;
    g_interlock_persist.crash.last_crashed_slot  = INTERLOCK_CRASHED_SLOT_NONE;
    g_interlock_persist.crash.reserved[0] = 0;
    g_interlock_persist.crash.reserved[1] = 0;
    g_interlock_persist.crash.reserved[2] = 0;
}

void interlock_boot_decide(void) {
    if (!persist_is_valid()) {
        // Cold boot, corrupted noinit, or firmware version mismatch.
        persist_cold_init();
        return;
    }

    // Warm boot with valid state. Bump boot_counter for each ARMED slot;
    // if it exceeds MAX_BOOT_ATTEMPTS the slot is poisoned (the interlock
    // itself is likely the cause of the bootloop — don't re-arm it).
    for (uint8_t i = 0; i < INTERLOCK_MAX_SLOTS; i++) {
        interlock_slot_persist_t* s = &g_interlock_persist.slots[i];
        if (s->state == INTERLOCK_SLOT_ARMED) {
            if (s->boot_counter >= INTERLOCK_MAX_BOOT_ATTEMPTS) {
                s->state = INTERLOCK_SLOT_POISONED;
                // boot_counter left at limit so host can see how many tries.
            } else {
                s->boot_counter++;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Slot admin (slice 1: arm/disarm only the hardcoded no-op)
// ---------------------------------------------------------------------------

uint8_t interlock_armed_count(void) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < INTERLOCK_MAX_SLOTS; i++) {
        if (g_interlock_persist.slots[i].state == INTERLOCK_SLOT_ARMED) n++;
    }
    return n;
}

uint8_t interlock_arm_slot_noop(uint8_t slot) {
    if (slot >= INTERLOCK_MAX_SLOTS)             return SHELL_STATUS_BAD_ARGS;
    interlock_slot_persist_t* s = &g_interlock_persist.slots[slot];
    if (s->state == INTERLOCK_SLOT_ARMED)        return SHELL_STATUS_BUSY;
    // POISONED slot is OK to overwrite — explicit re-arm clears the poison.
    s->state        = INTERLOCK_SLOT_ARMED;
    s->id           = INTERLOCK_ID_NOOP;
    s->boot_counter = 0;
    return SHELL_STATUS_OK;
}

uint8_t interlock_disarm_slot(uint8_t slot) {
    if (slot >= INTERLOCK_MAX_SLOTS) return SHELL_STATUS_BAD_ARGS;
    interlock_slot_persist_t* s = &g_interlock_persist.slots[slot];
    s->state        = INTERLOCK_SLOT_EMPTY;
    s->id           = INTERLOCK_ID_NONE;
    s->boot_counter = 0;
    return SHELL_STATUS_OK;
}

const interlock_slot_persist_t* interlock_get_slot(uint8_t slot) {
    if (slot >= INTERLOCK_MAX_SLOTS) return 0;
    return &g_interlock_persist.slots[slot];
}

const interlock_crash_record_t* interlock_get_crash(void) {
    return &g_interlock_persist.crash;
}

// ---------------------------------------------------------------------------
// Boot-line formatter
// ---------------------------------------------------------------------------

static const char k_hex[] = "0123456789abcdef";

static uint16_t emit_str(char* dst, uint16_t pos, uint16_t cap, const char* s) {
    while (*s && pos + 1u < cap) dst[pos++] = *s++;
    return pos;
}

static uint16_t emit_hex32(char* dst, uint16_t pos, uint16_t cap, uint32_t v) {
    if (pos + 10u >= cap) return pos;
    dst[pos++] = '0';
    dst[pos++] = 'x';
    for (int i = 7; i >= 0; i--) {
        dst[pos++] = k_hex[(v >> (i * 4)) & 0xFu];
    }
    return pos;
}

static uint16_t emit_dec_small(char* dst, uint16_t pos, uint16_t cap, uint8_t v) {
    // 0..255
    char tmp[4];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    else while (v > 0 && n < 4) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n-- > 0 && pos + 1u < cap) dst[pos++] = tmp[n];
    return pos;
}

static char slot_state_char(uint8_t state) {
    switch (state) {
        case INTERLOCK_SLOT_EMPTY:    return 'E';
        case INTERLOCK_SLOT_ARMED:    return 'A';
        case INTERLOCK_SLOT_POISONED: return 'P';
        default:                      return '?';
    }
}

uint16_t interlock_format_boot_line(char* buf, uint16_t bufsize) {
    if (bufsize == 0) return 0;
    uint16_t p = 0;
    p = emit_str(buf, p, bufsize, "[BOOT_IL]");
    for (uint8_t i = 0; i < INTERLOCK_MAX_SLOTS; i++) {
        const interlock_slot_persist_t* s = &g_interlock_persist.slots[i];
        p = emit_str(buf, p, bufsize, " sl");
        p = emit_dec_small(buf, p, bufsize, i);
        p = emit_str(buf, p, bufsize, "=");
        if (p + 1u < bufsize) buf[p++] = slot_state_char(s->state);
        p = emit_str(buf, p, bufsize, ":");
        p = emit_dec_small(buf, p, bufsize, s->id);
        p = emit_str(buf, p, bufsize, ":");
        p = emit_dec_small(buf, p, bufsize, s->boot_counter);
    }
    p = emit_str(buf, p, bufsize, " pc=");
    p = emit_hex32(buf, p, bufsize, g_interlock_persist.crash.last_pc);
    p = emit_str(buf, p, bufsize, " lr=");
    p = emit_hex32(buf, p, bufsize, g_interlock_persist.crash.last_lr);
    p = emit_str(buf, p, bufsize, " rs=");
    p = emit_hex32(buf, p, bufsize, g_interlock_persist.crash.last_rstsr);
    p = emit_str(buf, p, bufsize, " cs=");
    if (g_interlock_persist.crash.last_crashed_slot == INTERLOCK_CRASHED_SLOT_NONE) {
        if (p + 1u < bufsize) buf[p++] = '-';
    } else {
        p = emit_dec_small(buf, p, bufsize, g_interlock_persist.crash.last_crashed_slot);
    }
    if (p < bufsize) buf[p] = '\0';
    return p;
}

// ---------------------------------------------------------------------------
// HardFault handling
//
// CMSIS provides a weak HardFault_Handler that infinite-loops. We override
// it with a naked thunk that grabs MSP and calls the C recorder. The
// recorder writes crash context into .noinit then triggers system reset
// (which loops back through main, where interlock_boot_decide() sees the
// bumped boot_counter and the new crash record).
//
// Cortex-M0+ exception stack frame (basic, no FP):
//   sp[0]..sp[3] = R0..R3
//   sp[4]        = R12
//   sp[5]        = LR  (caller's return address at exception entry)
//   sp[6]        = ReturnAddress (PC of the faulting/next instruction)
//   sp[7]        = xPSR
// ---------------------------------------------------------------------------

void interlock_hardfault_record(uint32_t* msp) {
    g_interlock_persist.crash.last_pc            = msp[6];
    g_interlock_persist.crash.last_lr            = msp[5];
    g_interlock_persist.crash.last_rstsr         = (uint32_t)PM->RCAUSE.reg;
    g_interlock_persist.crash.last_crashed_slot  = g_active_interlock_slot;
    // NVIC_SystemReset() is inline; the asm DSB+DMB inside ensures the
    // .noinit writes commit before the reset takes effect.
    NVIC_SystemReset();
    for (;;) { /* unreachable */ }
}

__attribute__((naked, noreturn)) void HardFault_Handler(void) {
    __asm__ volatile (
        "mrs  r0, msp                   \n"
        "ldr  r1, =interlock_hardfault_record  \n"
        "bx   r1                        \n"
    );
}
