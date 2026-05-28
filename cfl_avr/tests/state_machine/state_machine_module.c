/*
 * state_machine_module.c — three-state cycle A -> B -> C -> A.
 *
 * State = chain. Transitions via enable_chains/disable_chains.
 * Drives by TIME_TICK events with data=1.
 */

#include "../../include/cfl_engine.h"
#include "../../include/cfl_progmem.h"

enum {
    CHAIN_A = 0,
    CHAIN_B = 1,
    CHAIN_C = 2,
    CHAIN_COUNT = 3
};

enum {
    FN_PRINT_A = 0,
    FN_PRINT_B = 1,
    FN_PRINT_C = 2
};

extern cfl_status_t print_a(cfl_engine_t* eng, uint8_t node_idx,
                            uint8_t event_id, uint16_t event_data);
extern cfl_status_t print_b(cfl_engine_t* eng, uint8_t node_idx,
                            uint8_t event_id, uint16_t event_data);
extern cfl_status_t print_c(cfl_engine_t* eng, uint8_t node_idx,
                            uint8_t event_id, uint16_t event_data);

static const cfl_user_fn_t sm_user_fns[] = {
    [FN_PRINT_A] = print_a,
    [FN_PRINT_B] = print_b,
    [FN_PRINT_C] = print_c
};

/* Params: per state, TIME_DELAY target + transition target chain id.
 * (Self-disable is now via TERMINATE_SELF, not DISABLE_CHAINS(self).) */
static const PROGMEM uint16_t sm_params[] = {
    3, CHAIN_B,
    3, CHAIN_C,
    3, CHAIN_A
};

#define NODE(kind, fn, count, off) \
    { (uint8_t)(kind), (fn), CFL_FN_NONE, (count), (off) }

static const PROGMEM cfl_node_desc_t sm_nodes[] = {
    /* chain A — nodes 0..3 */
    NODE(CFL_KIND_ONE_SHOT,       FN_PRINT_A,  0, 0),
    NODE(CFL_KIND_TIME_DELAY,     CFL_FN_NONE, 1, 0),
    NODE(CFL_KIND_ENABLE_CHAINS,  CFL_FN_NONE, 1, 1),
    NODE(CFL_KIND_TERMINATE_SELF, CFL_FN_NONE, 0, 0),

    /* chain B — nodes 4..7 */
    NODE(CFL_KIND_ONE_SHOT,       FN_PRINT_B,  0, 0),
    NODE(CFL_KIND_TIME_DELAY,     CFL_FN_NONE, 1, 2),
    NODE(CFL_KIND_ENABLE_CHAINS,  CFL_FN_NONE, 1, 3),
    NODE(CFL_KIND_TERMINATE_SELF, CFL_FN_NONE, 0, 0),

    /* chain C — nodes 8..11 */
    NODE(CFL_KIND_ONE_SHOT,       FN_PRINT_C,  0, 0),
    NODE(CFL_KIND_TIME_DELAY,     CFL_FN_NONE, 1, 4),
    NODE(CFL_KIND_ENABLE_CHAINS,  CFL_FN_NONE, 1, 5),
    NODE(CFL_KIND_TERMINATE_SELF, CFL_FN_NONE, 0, 0)
};

static const PROGMEM cfl_chain_desc_t sm_chains[] = {
    {    0,    4,    1,          0   },
    {    4,    4,    0,          0   },
    {    8,    4,    0,          0   }
};

const cfl_module_t state_machine_module = {
    .chains        = sm_chains,
    .nodes         = sm_nodes,
    .params_pool   = sm_params,
    .user_fns      = sm_user_fns,
    .chain_count   = CHAIN_COUNT,
    .node_count    = 12,
    .user_fn_count = 3
};
