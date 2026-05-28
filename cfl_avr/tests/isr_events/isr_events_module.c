/*
 * isr_events_module.c — exercises ISR-safe event push.
 *
 * Models the PSoC pattern where a UART/Modbus ISR decodes a frame and
 * pushes a user event into the queue, which main-context chains then
 * react to.
 *
 * Chain 0 (always on):
 *   node 0  M_CALL(rx_listener)
 *     — on EVENT_RX_FRAME: print + count it (CONTINUE)
 *     — on every other event: CONTINUE (idle)
 *
 * Chain 1 (heartbeat, parallel):
 *   node 1  ONE_SHOT(beat)
 *   node 2  TIME_DELAY(5)
 *   node 3  RESET_SELF
 */

#include "../../include/cfl_engine.h"
#include "../../include/cfl_progmem.h"

enum {
    CHAIN_LISTENER  = 0,
    CHAIN_HEARTBEAT = 1
};

enum {
    FN_RX_LISTENER = 0,
    FN_BEAT        = 1
};

extern cfl_status_t rx_listener(cfl_engine_t*, uint8_t, uint8_t, uint16_t);
extern cfl_status_t beat       (cfl_engine_t*, uint8_t, uint8_t, uint16_t);

static const cfl_user_fn_t isr_user_fns[] = {
    [FN_RX_LISTENER] = rx_listener,
    [FN_BEAT]        = beat
};

static const PROGMEM uint16_t isr_params[] = {
    5   /* off 0: TIME_DELAY target */
};

#define NODE(kind, fn, count, off) \
    { (uint8_t)(kind), (fn), CFL_FN_NONE, (count), (off) }

static const PROGMEM cfl_node_desc_t isr_nodes[] = {
    NODE(CFL_KIND_M_CALL,     FN_RX_LISTENER, 0, 0),  /* 0 */
    NODE(CFL_KIND_ONE_SHOT,   FN_BEAT,        0, 0),  /* 1 */
    NODE(CFL_KIND_TIME_DELAY, CFL_FN_NONE,    1, 0),  /* 2 */
    NODE(CFL_KIND_RESET_SELF, CFL_FN_NONE,    0, 0)   /* 3 */
};

static const PROGMEM cfl_chain_desc_t isr_chains[] = {
    {  0, 1, 1, 0 },   /* listener  — auto_start */
    {  1, 3, 1, 0 }    /* heartbeat — auto_start */
};

const cfl_module_t isr_events_module = {
    .chains        = isr_chains,
    .nodes         = isr_nodes,
    .params_pool   = isr_params,
    .user_fns      = isr_user_fns,
    .chain_count   = 2,
    .node_count    = 4,
    .user_fn_count = 2
};
