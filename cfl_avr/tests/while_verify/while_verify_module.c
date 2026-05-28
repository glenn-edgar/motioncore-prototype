/*
 * while_verify_module.c — exercises WHILE and (continuous) VERIFY.
 *
 * Chain shape:
 *   node 0  ONE_SHOT(reset_counter)
 *   node 1  WHILE(wait_for_3)
 *   node 2  VERIFY(verify_is_3)    — continuous monitor; OK every tick
 *   node 3  ONE_SHOT(print_pass)
 *   node 4  TIME_DELAY(2)
 *   node 5  RESET_SELF
 */

#include "../../include/cfl_engine.h"
#include "../../include/cfl_progmem.h"

enum {
    FN_RESET_COUNTER = 0,
    FN_WAIT_FOR_3    = 1,
    FN_VERIFY_IS_3   = 2,
    FN_PRINT_PASS    = 3
};

extern cfl_status_t reset_counter(cfl_engine_t* eng, uint8_t node_idx,
                                  uint8_t event_id, uint16_t event_data);
extern cfl_status_t wait_for_3   (cfl_engine_t* eng, uint8_t node_idx,
                                  uint8_t event_id, uint16_t event_data);
extern cfl_status_t verify_is_3  (cfl_engine_t* eng, uint8_t node_idx,
                                  uint8_t event_id, uint16_t event_data);
extern cfl_status_t print_pass   (cfl_engine_t* eng, uint8_t node_idx,
                                  uint8_t event_id, uint16_t event_data);

static const cfl_user_fn_t wv_user_fns[] = {
    [FN_RESET_COUNTER] = reset_counter,
    [FN_WAIT_FOR_3   ] = wait_for_3,
    [FN_VERIFY_IS_3  ] = verify_is_3,
    [FN_PRINT_PASS   ] = print_pass
};

static const PROGMEM uint16_t wv_params[] = {
    2   /* off 0: TIME_DELAY target */
};

#define NODE(kind, fn, count, off) \
    { (uint8_t)(kind), (fn), CFL_FN_NONE, (count), (off) }

static const PROGMEM cfl_node_desc_t wv_nodes[] = {
    NODE(CFL_KIND_ONE_SHOT,   FN_RESET_COUNTER, 0, 0),
    NODE(CFL_KIND_WHILE,      FN_WAIT_FOR_3,    0, 0),
    NODE(CFL_KIND_VERIFY,     FN_VERIFY_IS_3,   0, 0),
    NODE(CFL_KIND_ONE_SHOT,   FN_PRINT_PASS,    0, 0),
    NODE(CFL_KIND_TIME_DELAY, CFL_FN_NONE,      1, 0),
    NODE(CFL_KIND_RESET_SELF, CFL_FN_NONE,      0, 0)
};

static const PROGMEM cfl_chain_desc_t wv_chains[] = {
    {  0, 6, 1, 0 }
};

const cfl_module_t while_verify_module = {
    .chains        = wv_chains,
    .nodes         = wv_nodes,
    .params_pool   = wv_params,
    .user_fns      = wv_user_fns,
    .chain_count   = 1,
    .node_count    = 6,
    .user_fn_count = 4
};
