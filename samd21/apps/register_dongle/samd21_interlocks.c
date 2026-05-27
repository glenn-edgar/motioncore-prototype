// ============================================================================
// samd21_interlocks.c — interlock framework foundation, slice 1.
// See samd21_interlocks.h for design summary.
// ============================================================================

#include "samd21_interlocks.h"
#include "samd21.h"
#include "samd21_hal_pin.h"
#include "samd21_pin_table.h"
#include "vendor/libcomm/opcodes.h"   // SHELL_STATUS_* values, OP_EVENT
#include "vendor/libcomm/frame.h"     // frame_encode_s2m for OP_EVENT push
#include <string.h>

// TX ring defined in main.c. We push OP_EVENT frames directly into it from
// interlock_tick_all when a slot state/tf transition is detected.
extern frame_ring_t g_tx_ring;
extern volatile uint16_t g_stack_hwm_bytes;   // defined in main.c

// Slice 6 — virtual input sources. board_millis() is the TinyUSB BSP wall clock
// (also used by the chain pump). g_last_m2s_rx_ms is set in rx_drain whenever a
// host frame finishes decoding; readers see at most ~1 tick of staleness.
extern uint32_t          board_millis(void);
extern volatile uint32_t g_last_m2s_rx_ms;

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
// Status buffer (slice 5). Rebuilt at the end of every interlock_tick_all()
// run. Lives in .data — does NOT need to survive WDT (host can re-poll the
// new snapshot post-boot).
//
// status_seq monotonically increments whenever any slot's state or tf flips.
// We track the previous tick's slot snapshot in a static below so the diff
// can be computed cheaply.
// ---------------------------------------------------------------------------

il_status_buffer_t g_il_status_buffer;

const il_status_buffer_t* interlock_get_status_buffer(void) {
    return &g_il_status_buffer;
}

// ---------------------------------------------------------------------------
// Slice 6 — runtime-only state. Kept out of .noinit because:
//   * Shift register on warm boot would hold stale samples; clearing is safer.
//   * Watch last-pass would falsely freeze hysteresis state across the bite.
// Both arrays zeroed on cold boot (.bss) and on every disarm / warm-restore.
// ---------------------------------------------------------------------------

static uint16_t g_input_shift_reg[INTERLOCK_MAX_SLOTS][IL_MAX_INPUTS];
static uint8_t  g_input_debounced[INTERLOCK_MAX_SLOTS][IL_MAX_INPUTS];
static uint8_t  g_watch_last_pass[INTERLOCK_MAX_SLOTS][IL_MAX_WATCHES];

static void clear_slot_runtime(uint8_t slot) {
    for (uint8_t i = 0; i < IL_MAX_INPUTS; i++) {
        g_input_shift_reg[slot][i] = 0;
        g_input_debounced[slot][i] = 0;
    }
    for (uint8_t i = 0; i < IL_MAX_WATCHES; i++) {
        g_watch_last_pass[slot][i] = 0;
    }
}

// ---------------------------------------------------------------------------
// Slice 6 — virtual input reader. Maps an IL_VIRT_* id to a u16 sample.
// Time-based virtuals saturate at u16 max so a 65 535 s ceiling (~18 h)
// can't roll over and look-like fresh data.
// ---------------------------------------------------------------------------

static uint16_t read_virtual_input(uint8_t virt_id) {
    uint32_t now_ms = board_millis();
    switch (virt_id) {
        case IL_VIRT_T_SINCE_M2S: {
            uint32_t dt_ms = now_ms - g_last_m2s_rx_ms;
            uint32_t dt_s  = dt_ms / 1000u;
            return (dt_s > 65535u) ? (uint16_t)65535u : (uint16_t)dt_s;
        }
        case IL_VIRT_UPTIME: {
            uint32_t up_s = now_ms / 1000u;
            return (up_s > 65535u) ? (uint16_t)65535u : (uint16_t)up_s;
        }
        case IL_VIRT_STACK_HWM:
            return g_stack_hwm_bytes;
        default:
            return 0;
    }
}

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
    if (g_interlock_persist.self_size != (uint16_t)sizeof(interlock_persist_t)) return false;
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

// ---------------------------------------------------------------------------
// Amendment B — verify magic + version + self_size at every public-API entry.
//
// Already validated at boot by persist_is_valid(); this catches mid-run
// corruption (stack overflow, errant write, future bug) at the boundary
// where it would otherwise propagate into a torn read or worse.
//
// Static inline so the compiler can drop arg setup on each call site. Each
// site adds ~10 B text after dedup; 7 sites total → ~70 B.
// ---------------------------------------------------------------------------

static inline void verify_persist_or_panic(void) {
    if (g_interlock_persist.magic != INTERLOCK_MAGIC) {
        panic(PANIC_PERSIST_MAGIC_BAD, g_interlock_persist.magic);
    }
    if (g_interlock_persist.version != INTERLOCK_PERSIST_VERSION) {
        panic(PANIC_PERSIST_VERSION_BAD, g_interlock_persist.version);
    }
    if (g_interlock_persist.self_size != (uint16_t)sizeof(interlock_persist_t)) {
        panic(PANIC_PERSIST_SIZE_BAD, g_interlock_persist.self_size);
    }
}

static void persist_cold_init(void) {
    g_interlock_persist.magic     = INTERLOCK_MAGIC;
    g_interlock_persist.version   = INTERLOCK_PERSIST_VERSION;
    g_interlock_persist.reserved  = 0;
    g_interlock_persist.self_size = (uint16_t)sizeof(interlock_persist_t);
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
    g_interlock_persist.crash.panic_code         = PANIC_NONE;
    g_interlock_persist.crash.reserved[0]        = 0;
    g_interlock_persist.crash.reserved[1]        = 0;
    g_interlock_persist.crash.panic_arg          = 0;
    g_interlock_persist.crash.panic_sp           = 0;
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
    verify_persist_or_panic();
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
    verify_persist_or_panic();
    if (slot >= INTERLOCK_MAX_SLOTS) return SHELL_STATUS_BAD_ARGS;
    interlock_slot_persist_t* s = &g_interlock_persist.slots[slot];
    // Release any HAL pin claims attached to this slot before clearing state.
    hal_pin_release_slot(slot);
    s->state        = INTERLOCK_SLOT_EMPTY;
    s->id           = INTERLOCK_ID_NONE;
    s->boot_counter = 0;
    memset(&g_interlock_persist.inst[slot], 0, sizeof(il_inst_t));
    g_interlock_persist.dsl_len[slot] = 0;
    clear_slot_runtime(slot);
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
        il_pin_mode_t imode = (il_pin_mode_t)inst->inputs[i].mode;
        if (imode == IL_PIN_MODE_VIRTUAL) {
            // Virtual input: no physical pin to claim. Skip.
            continue;
        }
        if (imode == IL_PIN_MODE_ADC) {
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
    verify_persist_or_panic();
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
    // Slice 6: fresh ARM starts with cleared debounce/hyst state — no false
    // edges from prior arm-cycle samples.
    clear_slot_runtime(slot);
    return SHELL_STATUS_OK;
}

// Re-parse + re-claim ARMED DSL slots after warm boot. Must run AFTER
// peripheral_init so reserved-pin enforcement is consistent. Slots that
// fail re-parse / re-claim are marked POISONED.
void interlock_warm_restore(void) {
    verify_persist_or_panic();
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
        // Slice 6: shift register + hyst memory don't survive the bite —
        // they're in .bss so already zero on cold boot, but explicit zero
        // here makes warm-restore identical to fresh arm (no stale samples).
        clear_slot_runtime(slot);
    }
}

// Evaluate one slot's watches against a fresh input snapshot. Updates
// inst->tf_state in place. Does NOT touch outputs — that's the drive phase.
// Writes captured input values into out_input_vals[IL_MAX_INPUTS] so the
// status buffer can include them without re-reading hardware.
//
// Slice 6 extensions:
//   - GPIO inputs with debounce_depth >= 2 are filtered through a shift
//     register: the output flips only when N consecutive samples agree.
//   - ADC inputs with hyst != 0 get a directional dead-band on gt/lt/ge/le
//     watches: once a watch is in the pass state, the threshold is relaxed
//     by `hyst` so noise near the boundary doesn't cause flutter.
//   - Virtual inputs read synthesized signals (no hardware access).
static void eval_slot(uint8_t slot, il_inst_t* inst, uint16_t* out_input_vals) {
    g_active_interlock_slot = slot;

    uint16_t input_vals[IL_MAX_INPUTS] = {0};
    for (uint8_t i = 0; i < inst->input_count; i++) {
        il_pin_mode_t imode = (il_pin_mode_t)inst->inputs[i].mode;
        uint16_t raw;
        if (imode == IL_PIN_MODE_ADC) {
            raw = hal_pin_read_adc(inst->inputs[i].phys_id);
        } else if (imode == IL_PIN_MODE_VIRTUAL) {
            raw = read_virtual_input(inst->inputs[i].phys_id);
        } else {
            raw = (uint16_t)(hal_pin_read(inst->inputs[i].phys_id) & 1u);
            uint8_t depth = inst->inputs[i].debounce_depth;
            if (depth >= 2u) {
                // Shift in new sample, mask to N bits, then flip the debounced
                // output only when all N bits agree (Schmitt-on-time).
                uint16_t mask = (uint16_t)((1u << depth) - 1u);
                uint16_t* sr = &g_input_shift_reg[slot][i];
                *sr = (uint16_t)(((*sr << 1) | (raw & 1u)) & mask);
                if      (*sr == mask) g_input_debounced[slot][i] = 1u;
                else if (*sr == 0u)   g_input_debounced[slot][i] = 0u;
                raw = g_input_debounced[slot][i];
            }
        }
        input_vals[i] = raw;
    }
    if (out_input_vals) {
        for (uint8_t i = 0; i < IL_MAX_INPUTS; i++) out_input_vals[i] = input_vals[i];
    }

    bool all_pass = true;
    for (uint8_t i = 0; i < inst->watch_count; i++) {
        uint8_t  idx  = inst->watches[i].input_idx;
        uint16_t v    = input_vals[idx];
        uint16_t t    = inst->watches[i].threshold;
        uint16_t hyst = inst->inputs[idx].hyst;
        uint16_t teff = t;
        // Hysteresis only adjusts directional comparisons. Parser already
        // rejected hyst+eq/ne (IL_PARSE_HYST_ON_EQ_NE) so we don't see them
        // here, but the switch defaults to plain compare for safety.
        if (hyst != 0u && g_watch_last_pass[slot][i]) {
            switch ((il_compare_op_t)inst->watches[i].op) {
                case IL_OP_GT:
                case IL_OP_GE:
                    teff = (t > hyst) ? (uint16_t)(t - hyst) : 0u;
                    break;
                case IL_OP_LT:
                case IL_OP_LE: {
                    uint32_t sum = (uint32_t)t + (uint32_t)hyst;
                    teff = (sum > 65535u) ? (uint16_t)65535u : (uint16_t)sum;
                    break;
                }
                default: break;
            }
        }
        bool pass = false;
        switch ((il_compare_op_t)inst->watches[i].op) {
            case IL_OP_EQ: pass = (v == t);    break;
            case IL_OP_NE: pass = (v != t);    break;
            case IL_OP_LT: pass = (v <  teff); break;
            case IL_OP_GT: pass = (v >  teff); break;
            case IL_OP_LE: pass = (v <= teff); break;
            case IL_OP_GE: pass = (v >= teff); break;
            default:       pass = false;       break;
        }
        g_watch_last_pass[slot][i] = pass ? 1u : 0u;
        if (!pass) all_pass = false;
        // Don't early-out: we want every watch's last_pass updated each tick
        // so hysteresis is consistent across all watches in the slot.
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
// Emit a 6 B OP_EVENT frame announcing a slot state or tf transition. Cheap;
// fire-and-forget into the TX ring. If the ring is full the frame is silently
// dropped — host can re-sync from the next OP_POLL.
static void emit_op_event(uint16_t seq, uint8_t slot,
                          uint8_t old_state, uint8_t new_state,
                          uint8_t old_tf,    uint8_t new_tf) {
    uint8_t payload[6];
    payload[0] = (uint8_t)(seq & 0xFFu);
    payload[1] = (uint8_t)((seq >> 8) & 0xFFu);
    payload[2] = slot;
    payload[3] = new_state;
    payload[4] = new_tf;
    // Pack the previous state+tf into one byte: high nibble=old_state, low=old_tf.
    payload[5] = (uint8_t)(((old_state & 0x0Fu) << 4) | (old_tf & 0x0Fu));
    frame_meta_t meta = {
        .addr        = 1,
        .cmd         = OP_EVENT,
        .seq         = (uint8_t)(seq & 0xFFu),
        .ack_seq     = 0,
        .ack_status  = 0,
        .payload_len = sizeof(payload),
    };
    (void)frame_encode_s2m(&meta, payload, &g_tx_ring);
}

void interlock_tick_all(void) {
    verify_persist_or_panic();
    // Phase 1 — evaluate. Capture per-input values into the status buffer's
    // input_vals slot so we don't re-read hardware in phase 3.
    for (uint8_t slot = 0; slot < INTERLOCK_MAX_SLOTS; slot++) {
        interlock_slot_persist_t* sp = &g_interlock_persist.slots[slot];
        if (sp->state != INTERLOCK_SLOT_ARMED) continue;
        if (sp->id != INTERLOCK_ID_DSL) continue;
        eval_slot(slot, &g_interlock_persist.inst[slot],
                  g_il_status_buffer.input_vals[slot]);
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

    // Phase 3 — populate status buffer + emit OP_EVENT on slot transitions.
    // Compare new slot snapshots against the previous tick's status buffer
    // (slots[] are valid because Phase 3 is the only writer). status_seq
    // bumps only on a real state or tf transition.
    bool any_change = false;
    uint16_t new_seq = g_il_status_buffer.status_seq;
    for (uint8_t slot = 0; slot < INTERLOCK_MAX_SLOTS; slot++) {
        const interlock_slot_persist_t* sp = &g_interlock_persist.slots[slot];
        uint8_t new_state = sp->state;
        uint8_t new_id    = sp->id;
        uint8_t new_tf    = (sp->state == INTERLOCK_SLOT_ARMED && sp->id == INTERLOCK_ID_DSL)
                            ? g_interlock_persist.inst[slot].tf_state
                            : (uint8_t)IL_TF_UNEVALUATED;
        uint8_t old_state = g_il_status_buffer.slots[slot].state;
        uint8_t old_tf    = g_il_status_buffer.slots[slot].tf;
        if (old_state != new_state || old_tf != new_tf) {
            any_change = true;
            // Bump seq first so emitted event carries the new value.
            new_seq++;
            emit_op_event(new_seq, slot, old_state, new_state, old_tf, new_tf);
        }
        il_status_slot_t* s = &g_il_status_buffer.slots[slot];
        s->state     = new_state;
        s->id        = new_id;
        s->tf        = new_tf;
        s->bc        = sp->boot_counter;
        s->veto_mask = (veto_mask >> slot) & 1u;
        s->reserved  = 0;
        // Copy first 8 bytes of inst.name (may not be null-terminated).
        if (new_state == INTERLOCK_SLOT_ARMED && new_id == INTERLOCK_ID_DSL) {
            for (uint8_t k = 0; k < IL_STATUS_SLOT_NAME_MAX; k++) {
                s->name[k] = g_interlock_persist.inst[slot].name[k];
            }
        } else {
            memset(s->name, 0, IL_STATUS_SLOT_NAME_MAX);
        }
    }
    g_il_status_buffer.version          = IL_STATUS_BUFFER_VERSION;
    g_il_status_buffer.num_slots        = INTERLOCK_MAX_SLOTS;
    g_il_status_buffer.status_seq       = new_seq;
    g_il_status_buffer.crash_panic_code = g_interlock_persist.crash.panic_code;
    g_il_status_buffer.reserved0        = 0;
    g_il_status_buffer.stack_hwm_bytes  = g_stack_hwm_bytes;
    if (!any_change) {
        // No transition; status_seq unchanged — host poll loop can use it
        // as a freshness sentinel.
    }
}

const interlock_slot_persist_t* interlock_get_slot(uint8_t slot) {
    verify_persist_or_panic();
    if (slot >= INTERLOCK_MAX_SLOTS) return 0;
    return &g_interlock_persist.slots[slot];
}

const interlock_crash_record_t* interlock_get_crash(void) {
    verify_persist_or_panic();
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
    // Panic record (only when this boot follows a software-detected panic).
    // Format: " pn=N pa=0xNNNN psp=0xNNNN". Skipped when panic_code == 0.
    if (g_interlock_persist.crash.panic_code != PANIC_NONE) {
        p = emit_str(buf, p, bufsize, " pn=");
        p = emit_dec_small(buf, p, bufsize, g_interlock_persist.crash.panic_code);
        p = emit_str(buf, p, bufsize, " pa=");
        p = emit_hex32(buf, p, bufsize, g_interlock_persist.crash.panic_arg);
        p = emit_str(buf, p, bufsize, " psp=");
        p = emit_hex32(buf, p, bufsize, g_interlock_persist.crash.panic_sp);
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
    g_interlock_persist.crash.panic_code         = PANIC_NONE;  // hardware path
    // NVIC_SystemReset() is inline; the asm DSB+DMB inside ensures the
    // .noinit writes commit before the reset takes effect.
    NVIC_SystemReset();
    for (;;) { /* unreachable */ }
}

// ---------------------------------------------------------------------------
// panic() — software-detected invariant violations
//
// Distinct from HardFault: panic() runs in normal context, so SP/LR live in
// the current frame rather than an exception stack frame. We grab SP via
// inline asm and use __builtin_return_address(0) for the caller PC. Records
// commit to .noinit, then immediate NVIC_SystemReset (per slice-5 Q5
// ratification — get out fast, crash record carries the post-mortem).
// ---------------------------------------------------------------------------

__attribute__((noreturn))
void panic(panic_code_t code, uint32_t arg) {
    uint32_t sp;
    __asm__ volatile ("mov %0, sp" : "=r"(sp));
    uint32_t caller_pc = (uint32_t)__builtin_return_address(0);

    g_interlock_persist.crash.last_pc            = caller_pc;
    g_interlock_persist.crash.last_lr            = 0;          // n/a for panic
    g_interlock_persist.crash.last_rstsr         = (uint32_t)PM->RCAUSE.reg;
    g_interlock_persist.crash.last_crashed_slot  = g_active_interlock_slot;
    g_interlock_persist.crash.panic_arg          = arg;
    g_interlock_persist.crash.panic_sp           = sp;
    g_interlock_persist.crash.panic_code         = (uint8_t)code;

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
