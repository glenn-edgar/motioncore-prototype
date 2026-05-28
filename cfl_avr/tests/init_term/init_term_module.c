/*
 * init_term_module.c — session-scope test (INIT_TERM RAII).
 *
 * Chain 0 (session, auto_start):
 *   node 0  INIT_TERM(acquire_workers, release_workers, [W_A, W_B])
 *   node 1  TIME_DELAY(4)
 *   node 2  ONE_SHOT(halt_session)  — INIT returns TERMINATE → chain ends
 *                                     → INIT_TERM TERMINATE fires release
 *
 * Chain 1 (worker_a, NOT auto_start): ONE_SHOT print_a + RESET_SELF
 * Chain 2 (worker_b, NOT auto_start): ONE_SHOT print_b + RESET_SELF
 */

#include "../../include/cfl_engine.h"
#include "../../include/cfl_progmem.h"

enum {
    CHAIN_SESSION  = 0,
    CHAIN_WORKER_A = 1,
    CHAIN_WORKER_B = 2,
    CHAIN_COUNT    = 3
};

enum {
    FN_ACQUIRE      = 0,
    FN_RELEASE      = 1,
    FN_HALT_SESSION = 2,
    FN_PRINT_A      = 3,
    FN_PRINT_B      = 4
};

extern cfl_status_t acquire_workers(cfl_engine_t*, uint8_t, uint8_t, uint16_t);
extern cfl_status_t release_workers(cfl_engine_t*, uint8_t, uint8_t, uint16_t);
extern cfl_status_t halt_session   (cfl_engine_t*, uint8_t, uint8_t, uint16_t);
extern cfl_status_t print_a        (cfl_engine_t*, uint8_t, uint8_t, uint16_t);
extern cfl_status_t print_b        (cfl_engine_t*, uint8_t, uint8_t, uint16_t);

static const cfl_user_fn_t it_user_fns[] = {
    [FN_ACQUIRE]      = acquire_workers,
    [FN_RELEASE]      = release_workers,
    [FN_HALT_SESSION] = halt_session,
    [FN_PRINT_A]      = print_a,
    [FN_PRINT_B]      = print_b
};

static const PROGMEM uint16_t it_params[] = {
    CHAIN_WORKER_A, CHAIN_WORKER_B,   /* off 0..1: INIT_TERM chain ids */
    4                                  /* off 2: TIME_DELAY target      */
};

#define NODE(kind, fn1, fn2, count, off) \
    { (uint8_t)(kind), (fn1), (fn2), (count), (off) }

static const PROGMEM cfl_node_desc_t it_nodes[] = {
    /* chain 0 — session */
    NODE(CFL_KIND_INIT_TERM,  FN_ACQUIRE,      FN_RELEASE,  2, 0),  /* 0 */
    NODE(CFL_KIND_TIME_DELAY, CFL_FN_NONE,     CFL_FN_NONE, 1, 2),  /* 1 */
    NODE(CFL_KIND_M_CALL,     FN_HALT_SESSION, CFL_FN_NONE, 0, 0),  /* 2 */

    /* chain 1 — worker A */
    NODE(CFL_KIND_ONE_SHOT,   FN_PRINT_A,      CFL_FN_NONE, 0, 0),  /* 3 */
    NODE(CFL_KIND_RESET_SELF, CFL_FN_NONE,     CFL_FN_NONE, 0, 0),  /* 4 */

    /* chain 2 — worker B */
    NODE(CFL_KIND_ONE_SHOT,   FN_PRINT_B,      CFL_FN_NONE, 0, 0),  /* 5 */
    NODE(CFL_KIND_RESET_SELF, CFL_FN_NONE,     CFL_FN_NONE, 0, 0)   /* 6 */
};

static const PROGMEM cfl_chain_desc_t it_chains[] = {
    {  0, 3, 1, 0 },
    {  3, 2, 0, 0 },
    {  5, 2, 0, 0 }
};

const cfl_module_t init_term_module = {
    .chains        = it_chains,
    .nodes         = it_nodes,
    .params_pool   = it_params,
    .user_fns      = it_user_fns,
    .chain_count   = CHAIN_COUNT,
    .node_count    = 7,
    .user_fn_count = 5
};
