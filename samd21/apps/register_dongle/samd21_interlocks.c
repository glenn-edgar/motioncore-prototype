// ============================================================================
// samd21_interlocks.c — interlock framework foundation, slice 1.
// See samd21_interlocks.h for design summary.
// ============================================================================

#include "samd21_interlocks.h"
#include "samd21.h"
#include "samd21_hal_pin.h"
#include "samd21_pin_table.h"
#include "vendor/libcomm/opcodes.h"   // SHELL_STATUS_* values
#include <string.h>

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
        // Valid IDs: 0 (none), 1 (compiled-in registry like noop), 2 (DSL).
        // Future slices may add more well-known IDs; check against the
        // hardcoded set rather than g_interlock_count (which only sizes
        // the compiled registry).
        if (s->id != INTERLOCK_ID_NONE
            && s->id != INTERLOCK_ID_NOOP
            && s->id != INTERLOCK_ID_DSL) return false;
        if (s->state == INTERLOCK_SLOT_ARMED && s->id == INTERLOCK_ID_NONE) return false;
        // DSL slot must have a non-empty persisted DSL text.
        if (s->state == INTERLOCK_SLOT_ARMED && s->id == INTERLOCK_ID_DSL) {
            uint16_t L = g_interlock_persist.dsl_len[i];
            if (L == 0u || L >= IL_DSL_MAX) return false;
        }
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
        memset(&g_interlock_persist.inst[i], 0, sizeof(il_inst_t));
        g_interlock_persist.dsl_len[i] = 0;
        memset(g_interlock_persist.dsl_text[i], 0, IL_DSL_MAX);
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
    // Release any HAL pin claims attached to this slot before clearing state.
    hal_pin_release_slot(slot);
    s->state        = INTERLOCK_SLOT_EMPTY;
    s->id           = INTERLOCK_ID_NONE;
    s->boot_counter = 0;
    memset(&g_interlock_persist.inst[slot], 0, sizeof(il_inst_t));
    g_interlock_persist.dsl_len[slot] = 0;
    return SHELL_STATUS_OK;
}

// ---------------------------------------------------------------------------
// Slice 2 — DSL-driven slot admin + tick loop
// ---------------------------------------------------------------------------

static hal_pin_mode_t map_input_mode(uint8_t il_mode) {
    switch ((il_pin_mode_t)il_mode) {
        case IL_PIN_MODE_IN:    return HAL_PIN_MODE_GPIO_IN;
        case IL_PIN_MODE_IN_PU: return HAL_PIN_MODE_GPIO_IN_PU;
        case IL_PIN_MODE_IN_PD: return HAL_PIN_MODE_GPIO_IN_PD;
        case IL_PIN_MODE_ADC:   return HAL_PIN_MODE_ADC_SCAN;
        default:                return HAL_PIN_MODE_UNCLAIMED;
    }
}

// Claim every input + output pin declared by `inst`, owned by `slot`. On any
// failure releases all claims this call made and returns the HAL status of
// the failing claim. On success returns HAL_PIN_CLAIM_OK.
//
// Outputs go through hal_pin_claim_output() so shared-output sharing rules
// (matching ok/err values) apply across slots; inputs are still single-owner.
// ADC inputs go through hal_pin_claim_adc() with the per-input oversample/sh.
static hal_pin_claim_status_t claim_inst_pins(uint8_t slot, const il_inst_t* inst) {
    hal_pin_claim_status_t cs = HAL_PIN_CLAIM_OK;
    for (uint8_t i = 0; i < inst->input_count; i++) {
        if ((il_pin_mode_t)inst->inputs[i].mode == IL_PIN_MODE_ADC) {
            cs = hal_pin_claim_adc(inst->inputs[i].phys_id, slot,
                                   inst->inputs[i].oversample_exp,
                                   inst->inputs[i].sh_cyc);
        } else {
            hal_pin_mode_t mode = map_input_mode(inst->inputs[i].mode);
            if (mode == HAL_PIN_MODE_UNCLAIMED) { cs = HAL_PIN_CLAIM_BAD_MODE; goto rollback; }
            cs = hal_pin_claim(inst->inputs[i].phys_id, slot, mode);
        }
        if (cs != HAL_PIN_CLAIM_OK) goto rollback;
    }
    for (uint8_t i = 0; i < inst->output_count; i++) {
        cs = hal_pin_claim_output(inst->outputs[i].phys_id, slot,
                                  inst->outputs[i].ok_value,
                                  inst->outputs[i].err_value);
        if (cs != HAL_PIN_CLAIM_OK) goto rollback;
    }
    return HAL_PIN_CLAIM_OK;
rollback:
    hal_pin_release_slot(slot);
    return cs;
}

uint8_t interlock_set_slot_dsl(uint8_t slot,
                               const char* text, uint16_t text_len,
                               uint8_t err_payload[3]) {
    if (err_payload) { err_payload[0] = 0; err_payload[1] = 0; err_payload[2] = 0; }
    if (slot >= INTERLOCK_MAX_SLOTS)             return SHELL_STATUS_BAD_ARGS;
    if (text_len == 0u || text_len >= IL_DSL_MAX) return SHELL_STATUS_BAD_ARGS;

    interlock_slot_persist_t* sp = &g_interlock_persist.slots[slot];
    if (sp->state == INTERLOCK_SLOT_ARMED) return SHELL_STATUS_BUSY;

    il_inst_t parsed;
    uint16_t  err_off = 0;
    il_parse_status_t pst = il_parse(text, text_len, &parsed, &err_off);
    if (pst != IL_PARSE_OK) {
        if (err_payload) {
            err_payload[0] = (uint8_t)pst;
            err_payload[1] = (uint8_t)(err_off & 0xFFu);
            err_payload[2] = (uint8_t)((err_off >> 8) & 0xFFu);
        }
        return SHELL_STATUS_BAD_ARGS;
    }

    hal_pin_claim_status_t cs = claim_inst_pins(slot, &parsed);
    if (cs != HAL_PIN_CLAIM_OK) {
        if (err_payload) {
            err_payload[0] = 0xFFu;        // claim-conflict marker (not a parse error)
            err_payload[1] = (uint8_t)cs;  // sub-reason: hal_pin_claim_status_t
        }
        return SHELL_STATUS_BUSY;
    }

    // Commit to .noinit. The text is the source of truth; on warm boot we
    // re-parse from text rather than trusting the parsed struct directly.
    g_interlock_persist.inst[slot]    = parsed;
    g_interlock_persist.dsl_len[slot] = text_len;
    memcpy(g_interlock_persist.dsl_text[slot], text, text_len);
    if (text_len < IL_DSL_MAX) g_interlock_persist.dsl_text[slot][text_len] = '\0';

    sp->state        = INTERLOCK_SLOT_ARMED;
    sp->id           = INTERLOCK_ID_DSL;
    sp->boot_counter = 0;
    return SHELL_STATUS_OK;
}

// Re-parse + re-claim ARMED DSL slots after warm boot. Must run AFTER
// peripheral_init so reserved-pin enforcement is consistent. Slots that
// fail re-parse / re-claim are marked POISONED.
void interlock_warm_restore(void) {
    for (uint8_t slot = 0; slot < INTERLOCK_MAX_SLOTS; slot++) {
        interlock_slot_persist_t* sp = &g_interlock_persist.slots[slot];
        if (sp->state != INTERLOCK_SLOT_ARMED) continue;
        if (sp->id != INTERLOCK_ID_DSL)        continue;  // noop has no pins

        uint16_t text_len = g_interlock_persist.dsl_len[slot];
        if (text_len == 0u || text_len >= IL_DSL_MAX) {
            sp->state = INTERLOCK_SLOT_POISONED;
            continue;
        }
        il_inst_t parsed;
        il_parse_status_t pst = il_parse(g_interlock_persist.dsl_text[slot],
                                         text_len, &parsed, 0);
        if (pst != IL_PARSE_OK) {
            sp->state = INTERLOCK_SLOT_POISONED;
            continue;
        }
        if (claim_inst_pins(slot, &parsed) != HAL_PIN_CLAIM_OK) {
            sp->state = INTERLOCK_SLOT_POISONED;
            continue;
        }
        g_interlock_persist.inst[slot] = parsed;
        // tf_state reset to UNEVALUATED — outputs will be re-asserted on
        // first tick after restore.
        g_interlock_persist.inst[slot].tf_state = (uint8_t)IL_TF_UNEVALUATED;
    }
}

// Evaluate one slot's watches against a fresh input snapshot. Updates
// inst->tf_state in place. Does NOT touch outputs — that's the drive phase.
static void eval_slot(uint8_t slot, il_inst_t* inst) {
    g_active_interlock_slot = slot;

    uint16_t input_vals[IL_MAX_INPUTS] = {0};
    for (uint8_t i = 0; i < inst->input_count; i++) {
        if ((il_pin_mode_t)inst->inputs[i].mode == IL_PIN_MODE_ADC) {
            input_vals[i] = hal_pin_read_adc(inst->inputs[i].phys_id);
        } else {
            input_vals[i] = hal_pin_read(inst->inputs[i].phys_id);
        }
    }

    bool all_pass = true;
    for (uint8_t i = 0; i < inst->watch_count; i++) {
        uint16_t v = input_vals[inst->watches[i].input_idx];
        uint16_t t = inst->watches[i].threshold;
        bool pass = false;
        switch ((il_compare_op_t)inst->watches[i].op) {
            case IL_OP_EQ: pass = (v == t); break;
            case IL_OP_NE: pass = (v != t); break;
            case IL_OP_LT: pass = (v <  t); break;
            case IL_OP_GT: pass = (v >  t); break;
            case IL_OP_LE: pass = (v <= t); break;
            case IL_OP_GE: pass = (v >= t); break;
            default: pass = false; break;
        }
        if (!pass) { all_pass = false; break; }
    }
    inst->tf_state = all_pass ? (uint8_t)IL_TF_TRUE : (uint8_t)IL_TF_FALSE;
}

// Per-chain-pump tick. Two phases:
//   1. Eval: every ARMED DSL slot reads inputs + evaluates watches → tf_state
//   2. Drive: build veto_mask (bit i = slot i is F) and hand to the HAL,
//      which writes every shared output exactly once with OR-of-vetoes
//      semantics.
//
// Splitting into phases is what makes multi-slot output sharing well-defined:
// without it the second slot would silently overwrite the first slot's vote
// every tick. With it, both slots contribute to a single combined value.
void interlock_tick_all(void) {
    // Phase 1 — evaluate.
    for (uint8_t slot = 0; slot < INTERLOCK_MAX_SLOTS; slot++) {
        interlock_slot_persist_t* sp = &g_interlock_persist.slots[slot];
        if (sp->state != INTERLOCK_SLOT_ARMED) continue;
        if (sp->id != INTERLOCK_ID_DSL) continue;
        eval_slot(slot, &g_interlock_persist.inst[slot]);
    }
    g_active_interlock_slot = INTERLOCK_CRASHED_SLOT_NONE;

    // Phase 2 — drive outputs. Build the veto mask from slots that
    // evaluated to FALSE. EMPTY/POISONED/non-DSL slots contribute no
    // claims to the HAL so they're naturally excluded.
    uint8_t veto_mask = 0;
    for (uint8_t slot = 0; slot < INTERLOCK_MAX_SLOTS; slot++) {
        const interlock_slot_persist_t* sp = &g_interlock_persist.slots[slot];
        if (sp->state != INTERLOCK_SLOT_ARMED) continue;
        if (sp->id != INTERLOCK_ID_DSL) continue;
        if (g_interlock_persist.inst[slot].tf_state == (uint8_t)IL_TF_FALSE) {
            veto_mask |= (uint8_t)(1u << slot);
        }
    }
    hal_pin_drive_outputs(veto_mask);
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
