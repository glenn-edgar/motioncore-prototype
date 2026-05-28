/*
 * cfl_engine.h — public engine API (event-driven).
 *
 * Lifecycle:
 *   cfl_engine_init(eng)             — clears state, auto-enables chains
 *   cfl_send_event(eng, id, data)    — main-context queue push (cli/sei)
 *   cfl_send_event_isr(eng, id, data)— ISR-context queue push (no guard)
 *   cfl_engine_time_tick(eng, ms)    — convenience for TIME_TICK
 *   cfl_engine_pump(eng)             — drain the queue completely
 *
 * Chain control (other chains only — self uses return codes):
 *   cfl_chain_enable / cfl_chain_disable / cfl_chain_reset
 */
#ifndef CFL_ENGINE_H
#define CFL_ENGINE_H

#include "cfl_types.h"

#ifndef CFL_EVENT_QUEUE_SIZE
#define CFL_EVENT_QUEUE_SIZE 16   /* must be power of 2 */
#endif
#define CFL_EVENT_QUEUE_MASK (CFL_EVENT_QUEUE_SIZE - 1)

/* Panic codes — engine signals these via the registered panic hook before
 * the AVR watchdog (or platform reset) takes over. */
typedef enum {
    CFL_PANIC_OK              = 0,
    CFL_PANIC_QUEUE_OVERFLOW  = 1,
    CFL_PANIC_INVALID_KIND    = 2,
    CFL_PANIC_INVALID_CHAIN   = 3,
    CFL_PANIC_INVALID_NODE    = 4,
    CFL_PANIC_SELF_MODIFY     = 5
} cfl_panic_code_t;

struct cfl_engine_s;
typedef struct cfl_engine_s cfl_engine_t;

/* User function signature.
 *
 *   eng       - engine pointer (for chain control + helper APIs)
 *   node_idx  - the calling node's index in the global node table
 *   event_id  - which event is being dispatched (INIT, TERMINATE, TIME_TICK,
 *               or a user event)
 *   event_data- per-event 16-bit payload (e.g. elapsed ms for TIME_TICK)
 *
 * Return is significant only for the dispatch path (non-INIT, non-TERMINATE
 * events). INIT and TERMINATE return values are ignored by the engine. */
typedef cfl_status_t (*cfl_user_fn_t)(cfl_engine_t* eng,
                                      uint8_t       node_idx,
                                      uint8_t       event_id,
                                      uint16_t      event_data);

/* Panic hook signature. The hook should record the cause (in .noinit ideally)
 * and trigger reset. On AVR: enable the watchdog with shortest timeout and
 * spin. Engine never returns from panic. */
typedef void (*cfl_panic_hook_t)(cfl_engine_t*    eng,
                                 cfl_panic_code_t code,
                                 uint16_t         arg);

/* ---------------------------------------------------------------------------
 * Module = the const, flash-resident half of a compiled application.
 * -------------------------------------------------------------------------*/
typedef struct {
    const cfl_chain_desc_t* chains;       /* PROGMEM array, chain_count long */
    const cfl_node_desc_t*  nodes;        /* PROGMEM array, node_count long  */
    const uint16_t*         params_pool;  /* PROGMEM                          */
    const cfl_user_fn_t*    user_fns;     /* RAM-resident, one slot per fn   */
    uint8_t                 chain_count;
    uint8_t                 node_count;
    uint8_t                 user_fn_count;
} cfl_module_t;

/* ---------------------------------------------------------------------------
 * One slot in the event queue.
 * -------------------------------------------------------------------------*/
typedef struct {
    uint8_t  event_id;
    uint16_t event_data;
} cfl_queued_event_t;

/* ---------------------------------------------------------------------------
 * Engine = the RAM-resident half.
 *
 * The caller owns the bitmap and scratch buffers — typically static arrays
 * in .bss sized at compile time to match the module's counts.
 *
 * No chain_initialized bitmap — enable arms all nodes; INIT fires lazily on
 * first event delivery to each node.
 * -------------------------------------------------------------------------*/
struct cfl_engine_s {
    const cfl_module_t* module;
    uint8_t*  chain_enable;    /* (chain_count + 7) / 8 bytes */
    uint8_t*  node_enable;     /* (node_count  + 7) / 8 bytes */
    uint8_t*  node_initialized;/* (node_count  + 7) / 8 bytes */
    uint16_t* node_scratch;    /* node_count slots, 2 B each  */

    /* Event queue (ring buffer). Power-of-2 size, mask for wrap. */
    cfl_queued_event_t queue[CFL_EVENT_QUEUE_SIZE];
    volatile uint8_t   q_rx;       /* write index (producer)  */
    volatile uint8_t   q_tx;       /* read index (consumer)   */
    volatile uint8_t   q_count;    /* current depth           */

    /* current_chain is set during pump dispatch; APIs use it to enforce
     * "no self-modify via cfl_chain_*" rule. 0xFF when no chain is current. */
    uint8_t  current_chain;

    /* Panic hook — optional; engine calls it on invariant violations. */
    cfl_panic_hook_t panic;
};

/* Lifecycle */
void cfl_engine_init(cfl_engine_t* eng);
void cfl_engine_pump(cfl_engine_t* eng);

/* Event queue */
void cfl_send_event    (cfl_engine_t* eng, uint8_t event_id, uint16_t data);
void cfl_send_event_isr(cfl_engine_t* eng, uint8_t event_id, uint16_t data);
void cfl_engine_time_tick(cfl_engine_t* eng, uint16_t elapsed_ms);

/* Chain control — other chains only. Self uses return codes. */
void cfl_chain_enable (cfl_engine_t* eng, uint8_t chain_id);
void cfl_chain_disable(cfl_engine_t* eng, uint8_t chain_id);
void cfl_chain_reset  (cfl_engine_t* eng, uint8_t chain_id);

/* Helpers usable from user functions */
uint16_t cfl_node_param      (const cfl_engine_t* eng, uint8_t node_idx,
                              uint8_t param_idx);
uint8_t  cfl_node_param_count(const cfl_engine_t* eng, uint8_t node_idx);
uint16_t cfl_node_scratch_get(const cfl_engine_t* eng, uint8_t node_idx);
void     cfl_node_scratch_set(cfl_engine_t* eng, uint8_t node_idx,
                              uint16_t value);

static inline void* cfl_node_scratch_ptr(const cfl_engine_t* eng,
                                         uint8_t node_idx)
{
    return (void*)(uintptr_t)eng->node_scratch[node_idx];
}

#endif /* CFL_ENGINE_H */
