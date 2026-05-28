/*
 * cfl_engine.c — event-driven dispatcher.
 *
 * Pump flow (one cfl_engine_pump call):
 *   while (queue not empty):
 *     pop event (id, data) FIFO
 *     for each chain c in declaration order (0..N-1):
 *       if chain not enabled: skip
 *       for each node n in chain c (declaration order, first-to-last):
 *         if node not enabled: skip
 *         if node not initialized:
 *           call kind handler with CFL_EVENT_INIT  (return ignored)
 *           set initialized
 *         call kind handler with (event_id, event_data)
 *         dispatch on return code:
 *           CONTINUE  -> next sibling
 *           HALT      -> stop this chain for this event
 *           DISABLE   -> fire TERMINATE on node, clear bits, next sibling
 *           RESET     -> reset-chain action, next chain
 *           TERMINATE -> disable-chain action, next chain
 *
 * INIT/TERMINATE are engine-synthesized callbacks delivered directly to a
 * specific node. They are never queued. Return values ignored.
 *
 * Queue overflow is a fatal invariant violation — engine calls the panic
 * hook then traps. On AVR the panic hook should arm the WDT for fastest
 * reset.
 */

#include "../include/cfl_engine.h"
#include "../include/cfl_progmem.h"

/* ---------------------------------------------------------------------------
 * Bit-bitmap helpers
 * -------------------------------------------------------------------------*/
static inline uint8_t bm_get(const uint8_t* bm, uint8_t idx) {
    return (uint8_t)((bm[idx >> 3] >> (idx & 7u)) & 1u);
}
static inline void bm_set(uint8_t* bm, uint8_t idx) {
    bm[idx >> 3] |= (uint8_t)(1u << (idx & 7u));
}
static inline void bm_clr(uint8_t* bm, uint8_t idx) {
    bm[idx >> 3] &= (uint8_t)~(uint8_t)(1u << (idx & 7u));
}
static inline void bm_zero(uint8_t* bm, uint8_t nbits) {
    uint8_t bytes = (uint8_t)((nbits + 7u) / 8u);
    for (uint8_t i = 0; i < bytes; i++) bm[i] = 0;
}

/* ---------------------------------------------------------------------------
 * Panic dispatch — call the user-supplied hook if present, then trap.
 * -------------------------------------------------------------------------*/
static void cfl_panic(cfl_engine_t* eng, cfl_panic_code_t code, uint16_t arg) {
    if (eng && eng->panic) {
        eng->panic(eng, code, arg);
    }
    /* If hook returns (or none registered), spin. On AVR the WDT will reset
     * the chip; on Linux the harness will hang and the test framework will
     * notice. */
    for (;;) { }
}

/* ---------------------------------------------------------------------------
 * Public node helpers (callable from user fns)
 * -------------------------------------------------------------------------*/
uint16_t cfl_node_param(const cfl_engine_t* eng, uint8_t node_idx,
                        uint8_t param_idx)
{
    uint16_t off = pgm_read_word(&eng->module->nodes[node_idx].param_off);
    return pgm_read_word(&eng->module->params_pool[off + param_idx]);
}

uint8_t cfl_node_param_count(const cfl_engine_t* eng, uint8_t node_idx) {
    return pgm_read_byte(&eng->module->nodes[node_idx].param_count);
}

uint16_t cfl_node_scratch_get(const cfl_engine_t* eng, uint8_t node_idx) {
    return eng->node_scratch[node_idx];
}

void cfl_node_scratch_set(cfl_engine_t* eng, uint8_t node_idx, uint16_t v) {
    eng->node_scratch[node_idx] = v;
}

/* Resolve fn slot 0 (primary) or 1 (secondary) to a callable or NULL. */
static cfl_user_fn_t node_fn_at(const cfl_engine_t* eng, uint8_t n_idx,
                                uint8_t slot)
{
    uint8_t fi = (slot == 0)
        ? pgm_read_byte(&eng->module->nodes[n_idx].fn_idx)
        : pgm_read_byte(&eng->module->nodes[n_idx].fn_idx_2);
    if (fi == CFL_FN_NONE) return (cfl_user_fn_t)0;
    return eng->module->user_fns[fi];
}
static inline cfl_user_fn_t node_fn  (const cfl_engine_t* e, uint8_t n) { return node_fn_at(e, n, 0); }
static inline cfl_user_fn_t node_fn_2(const cfl_engine_t* e, uint8_t n) { return node_fn_at(e, n, 1); }

/* ---------------------------------------------------------------------------
 * Per-kind event handler
 * -------------------------------------------------------------------------*/
static cfl_status_t node_handle(cfl_engine_t* eng, uint8_t n_idx,
                                uint8_t kind, uint8_t event_id,
                                uint16_t event_data)
{
    switch (kind) {
        case CFL_KIND_NOP:
            return CFL_CONTINUE;

        case CFL_KIND_M_CALL: {
            cfl_user_fn_t fn = node_fn(eng, n_idx);
            if (!fn) return CFL_CONTINUE;
            return fn(eng, n_idx, event_id, event_data);
        }

        case CFL_KIND_ONE_SHOT: {
            /* INIT fires the aux fn once. Subsequent events return DISABLE
             * (which the engine pairs with a TERMINATE callback — no-op
             * here since the work already ran on INIT). */
            if (event_id == CFL_EVENT_INIT) {
                cfl_user_fn_t fn = node_fn(eng, n_idx);
                if (fn) (void)fn(eng, n_idx, event_id, event_data);
                return CFL_CONTINUE;
            }
            if (event_id == CFL_EVENT_TERMINATE) {
                return CFL_CONTINUE;
            }
            return CFL_DISABLE;
        }

        case CFL_KIND_TIME_DELAY: {
            /* INIT: reset accumulator to 0.
             * TIME_TICK: accumulate event_data (elapsed ms or whatever unit
             * the application chose); DISABLE when accumulator >= param[0].
             * Other events: HALT (blocking — siblings don't see them while
             * we're waiting). */
            if (event_id == CFL_EVENT_INIT) {
                eng->node_scratch[n_idx] = 0;
                return CFL_CONTINUE;
            }
            if (event_id == CFL_EVENT_TERMINATE) {
                return CFL_CONTINUE;
            }
            if (event_id == CFL_EVENT_TIME_TICK) {
                uint16_t target = cfl_node_param(eng, n_idx, 0);
                uint16_t acc    = eng->node_scratch[n_idx];
                uint32_t sum    = (uint32_t)acc + (uint32_t)event_data;
                if (sum > 0xFFFFu) sum = 0xFFFFu;  /* saturate */
                eng->node_scratch[n_idx] = (uint16_t)sum;
                return (sum >= target) ? CFL_DISABLE : CFL_HALT;
            }
            return CFL_HALT;
        }

        case CFL_KIND_ENABLE_CHAINS: {
            /* Side-effect on INIT only; node disables on first event. */
            if (event_id == CFL_EVENT_INIT) {
                uint8_t pc = cfl_node_param_count(eng, n_idx);
                for (uint8_t i = 0; i < pc; i++) {
                    uint8_t cid = (uint8_t)cfl_node_param(eng, n_idx, i);
                    cfl_chain_enable(eng, cid);
                }
                return CFL_CONTINUE;
            }
            if (event_id == CFL_EVENT_TERMINATE) {
                return CFL_CONTINUE;
            }
            return CFL_DISABLE;
        }

        case CFL_KIND_DISABLE_CHAINS: {
            if (event_id == CFL_EVENT_INIT) {
                uint8_t pc = cfl_node_param_count(eng, n_idx);
                for (uint8_t i = 0; i < pc; i++) {
                    uint8_t cid = (uint8_t)cfl_node_param(eng, n_idx, i);
                    cfl_chain_disable(eng, cid);
                }
                return CFL_CONTINUE;
            }
            if (event_id == CFL_EVENT_TERMINATE) {
                return CFL_CONTINUE;
            }
            return CFL_DISABLE;
        }

        case CFL_KIND_RESET_SELF:
            if (event_id == CFL_EVENT_INIT || event_id == CFL_EVENT_TERMINATE)
                return CFL_CONTINUE;
            return CFL_RESET;

        case CFL_KIND_TERMINATE_SELF:
            if (event_id == CFL_EVENT_INIT || event_id == CFL_EVENT_TERMINATE)
                return CFL_CONTINUE;
            return CFL_TERMINATE;

        case CFL_KIND_WHILE: {
            if (event_id == CFL_EVENT_INIT || event_id == CFL_EVENT_TERMINATE)
                return CFL_CONTINUE;
            cfl_user_fn_t fn = node_fn(eng, n_idx);
            if (!fn) return CFL_DISABLE;
            cfl_status_t r = fn(eng, n_idx, event_id, event_data);
            if (r == CFL_CONTINUE) return CFL_HALT;     /* still in loop */
            if (r == CFL_DISABLE)  return CFL_DISABLE;  /* loop done     */
            return r;                                    /* propagate     */
        }

        case CFL_KIND_VERIFY: {
            /* Continuous monitor: every event after INIT runs the predicate.
             * fn OK -> CONTINUE (let event flow on). fn anything else ->
             * RESET (chain reset). */
            if (event_id == CFL_EVENT_INIT || event_id == CFL_EVENT_TERMINATE)
                return CFL_CONTINUE;
            cfl_user_fn_t fn = node_fn(eng, n_idx);
            if (!fn) return CFL_CONTINUE;
            cfl_status_t r = fn(eng, n_idx, event_id, event_data);
            return (r == CFL_CONTINUE) ? CFL_CONTINUE : CFL_RESET;
        }

        case CFL_KIND_INIT_TERM: {
            /* RAII-for-chains. INIT: acquire. TERMINATE: release.
             * TICK: passive (CONTINUE; let event flow on). */
            if (event_id == CFL_EVENT_INIT) {
                cfl_user_fn_t fn = node_fn(eng, n_idx);
                if (fn) (void)fn(eng, n_idx, event_id, event_data);
                return CFL_CONTINUE;
            }
            if (event_id == CFL_EVENT_TERMINATE) {
                cfl_user_fn_t fn2 = node_fn_2(eng, n_idx);
                if (fn2) (void)fn2(eng, n_idx, event_id, event_data);
                return CFL_CONTINUE;
            }
            return CFL_CONTINUE;
        }

        default:
            cfl_panic(eng, CFL_PANIC_INVALID_KIND, kind);
            return CFL_CONTINUE;
    }
}

/* ---------------------------------------------------------------------------
 * Engine action — TERMINATE callback. INIT is synthesized inline in
 * dispatch_chain (just before the actual event dispatch).
 * -------------------------------------------------------------------------*/
static void node_synth_terminate(cfl_engine_t* eng, uint8_t n_idx) {
    if (!bm_get(eng->node_initialized, n_idx)) return;  /* gate by init bit */
    uint8_t kind = pgm_read_byte(&eng->module->nodes[n_idx].kind);
    (void)node_handle(eng, n_idx, kind, CFL_EVENT_TERMINATE, 0);
}

/* ---------------------------------------------------------------------------
 * Engine actions on chains.
 *
 * Public APIs: enable / disable / reset. All three are "other chains only" —
 * calling on the chain currently being iterated is a panic.
 * -------------------------------------------------------------------------*/
static void self_modify_guard(cfl_engine_t* eng, uint8_t chain_id) {
    if (eng->current_chain != 0xFFu && eng->current_chain == chain_id) {
        cfl_panic(eng, CFL_PANIC_SELF_MODIFY, chain_id);
    }
}

static void chain_enable_internal(cfl_engine_t* eng, uint8_t chain_id) {
    cfl_chain_desc_t cd;
    memcpy_P(&cd, &eng->module->chains[chain_id], sizeof(cd));
    bm_set(eng->chain_enable, chain_id);
    /* Arm every node in the chain. INIT fires lazily on first event. */
    for (uint8_t i = 0; i < cd.node_count; i++) {
        uint8_t n_idx = (uint8_t)(cd.node_first + i);
        bm_set(eng->node_enable,      n_idx);
        bm_clr(eng->node_initialized, n_idx);
        eng->node_scratch[n_idx] = 0;
    }
}

static void chain_disable_internal(cfl_engine_t* eng, uint8_t chain_id) {
    cfl_chain_desc_t cd;
    memcpy_P(&cd, &eng->module->chains[chain_id], sizeof(cd));
    /* Reverse-order TERMINATE sweep. Only init'd nodes get the callback. */
    for (uint8_t i = cd.node_count; i-- > 0; ) {
        uint8_t n_idx = (uint8_t)(cd.node_first + i);
        node_synth_terminate(eng, n_idx);
    }
    /* Clear all bits + scratch. */
    bm_clr(eng->chain_enable, chain_id);
    for (uint8_t i = 0; i < cd.node_count; i++) {
        uint8_t n_idx = (uint8_t)(cd.node_first + i);
        bm_clr(eng->node_enable,      n_idx);
        bm_clr(eng->node_initialized, n_idx);
        eng->node_scratch[n_idx] = 0;
    }
}

void cfl_chain_enable(cfl_engine_t* eng, uint8_t chain_id) {
    if (chain_id >= eng->module->chain_count) {
        cfl_panic(eng, CFL_PANIC_INVALID_CHAIN, chain_id);
    }
    self_modify_guard(eng, chain_id);
    chain_enable_internal(eng, chain_id);
}

void cfl_chain_disable(cfl_engine_t* eng, uint8_t chain_id) {
    if (chain_id >= eng->module->chain_count) {
        cfl_panic(eng, CFL_PANIC_INVALID_CHAIN, chain_id);
    }
    self_modify_guard(eng, chain_id);
    chain_disable_internal(eng, chain_id);
}

void cfl_chain_reset(cfl_engine_t* eng, uint8_t chain_id) {
    if (chain_id >= eng->module->chain_count) {
        cfl_panic(eng, CFL_PANIC_INVALID_CHAIN, chain_id);
    }
    self_modify_guard(eng, chain_id);
    chain_disable_internal(eng, chain_id);
    chain_enable_internal(eng, chain_id);
}

/* Self-modify variants — used by the dispatcher when handling RESET /
 * TERMINATE return codes from inside the chain being iterated. Bypass the
 * guard because we know iteration is about to stop for this chain. */
static void chain_reset_self(cfl_engine_t* eng, uint8_t chain_id) {
    chain_disable_internal(eng, chain_id);
    chain_enable_internal(eng, chain_id);
}
static void chain_terminate_self(cfl_engine_t* eng, uint8_t chain_id) {
    chain_disable_internal(eng, chain_id);
}

/* ---------------------------------------------------------------------------
 * Event queue
 * -------------------------------------------------------------------------*/
void cfl_send_event_isr(cfl_engine_t* eng, uint8_t event_id, uint16_t data) {
    if (eng->q_count >= CFL_EVENT_QUEUE_SIZE) {
        cfl_panic(eng, CFL_PANIC_QUEUE_OVERFLOW, event_id);
    }
    eng->queue[eng->q_rx].event_id   = event_id;
    eng->queue[eng->q_rx].event_data = data;
    eng->q_rx = (uint8_t)((eng->q_rx + 1u) & CFL_EVENT_QUEUE_MASK);
    eng->q_count++;
}

void cfl_send_event(cfl_engine_t* eng, uint8_t event_id, uint16_t data) {
    uint8_t s = cfl_atomic_save();
    cfl_send_event_isr(eng, event_id, data);
    cfl_atomic_restore(s);
}

void cfl_engine_time_tick(cfl_engine_t* eng, uint16_t elapsed_ms) {
    cfl_send_event(eng, CFL_EVENT_TIME_TICK, elapsed_ms);
}

static uint8_t queue_pop(cfl_engine_t* eng, cfl_queued_event_t* out) {
    uint8_t s = cfl_atomic_save();
    uint8_t have = (eng->q_count > 0);
    if (have) {
        *out = eng->queue[eng->q_tx];
        eng->q_tx = (uint8_t)((eng->q_tx + 1u) & CFL_EVENT_QUEUE_MASK);
        eng->q_count--;
    }
    cfl_atomic_restore(s);
    return have;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------*/
void cfl_engine_init(cfl_engine_t* eng) {
    uint8_t cc = eng->module->chain_count;
    uint8_t nc = eng->module->node_count;

    bm_zero(eng->chain_enable,     cc);
    bm_zero(eng->node_enable,      nc);
    bm_zero(eng->node_initialized, nc);
    for (uint8_t i = 0; i < nc; i++) eng->node_scratch[i] = 0;

    eng->q_rx = 0;
    eng->q_tx = 0;
    eng->q_count = 0;
    eng->current_chain = 0xFFu;

    /* Auto-start chains: enable each chain whose const auto_start byte is
     * non-zero. INIT does NOT fire here — it fires lazily when the first
     * event reaches each node. */
    for (uint8_t c = 0; c < cc; c++) {
        cfl_chain_desc_t cd;
        memcpy_P(&cd, &eng->module->chains[c], sizeof(cd));
        if (cd.auto_start) chain_enable_internal(eng, c);
    }
}

/* ---------------------------------------------------------------------------
 * Per-event dispatch through a single chain.
 * -------------------------------------------------------------------------*/
static void dispatch_chain(cfl_engine_t* eng, uint8_t c,
                           uint8_t event_id, uint16_t event_data)
{
    cfl_chain_desc_t cd;
    memcpy_P(&cd, &eng->module->chains[c], sizeof(cd));

    for (uint8_t i = 0; i < cd.node_count; i++) {
        uint8_t n_idx = (uint8_t)(cd.node_first + i);
        if (!bm_get(eng->node_enable, n_idx)) continue;

        uint8_t kind = pgm_read_byte(&eng->module->nodes[n_idx].kind);

        if (!bm_get(eng->node_initialized, n_idx)) {
            (void)node_handle(eng, n_idx, kind, CFL_EVENT_INIT, 0);
            bm_set(eng->node_initialized, n_idx);
        }

        cfl_status_t r = node_handle(eng, n_idx, kind, event_id, event_data);

        switch (r) {
            case CFL_CONTINUE:
                continue;

            case CFL_HALT:
                return;

            case CFL_DISABLE:
                /* TERMINATE callback fires before bits clear (only if init'd,
                 * which it must be — we just set it above). */
                node_synth_terminate(eng, n_idx);
                bm_clr(eng->node_enable, n_idx);
                bm_clr(eng->node_initialized, n_idx);
                eng->node_scratch[n_idx] = 0;
                continue;

            case CFL_RESET:
                chain_reset_self(eng, c);
                return;

            case CFL_TERMINATE:
                chain_terminate_self(eng, c);
                return;

            default:
                cfl_panic(eng, CFL_PANIC_INVALID_KIND, (uint16_t)r);
                return;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Pump — drain the event queue completely.
 * -------------------------------------------------------------------------*/
void cfl_engine_pump(cfl_engine_t* eng) {
    cfl_queued_event_t ev;
    uint8_t cc = eng->module->chain_count;

    while (queue_pop(eng, &ev)) {
        for (uint8_t c = 0; c < cc; c++) {
            if (!bm_get(eng->chain_enable, c)) continue;
            eng->current_chain = c;
            dispatch_chain(eng, c, ev.event_id, ev.event_data);
        }
        eng->current_chain = 0xFFu;
    }
}
