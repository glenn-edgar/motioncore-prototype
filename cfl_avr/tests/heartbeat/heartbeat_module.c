/*
 * heartbeat_module.c — heartbeat + LED test, event-driven.
 *
 * Chain 0 "heartbeat" (auto_start):
 *   node 0  ONE_SHOT(send_heartbeat)   — INIT fires the print
 *   node 1  TIME_DELAY(3)              — wait for 3 TIME_TICK units
 *   node 2  RESET_SELF                  — cycle the chain
 *
 * Chain 1 "led_blink" (auto_start):
 *   node 3  ONE_SHOT(toggle_led)
 *   node 4  RESET_SELF                  — cycle every tick
 *
 * Driven by TIME_TICK events from the harness. Heartbeat fires every 3 ticks;
 * LED toggles every tick.
 */

#include "../../include/cfl_engine.h"
#include "../../include/cfl_progmem.h"

enum {
    FN_SEND_HEARTBEAT = 0,
    FN_TOGGLE_LED     = 1
};

extern cfl_status_t send_heartbeat(cfl_engine_t* eng, uint8_t node_idx,
                                   uint8_t event_id, uint16_t event_data);
extern cfl_status_t toggle_led    (cfl_engine_t* eng, uint8_t node_idx,
                                   uint8_t event_id, uint16_t event_data);

static const cfl_user_fn_t heartbeat_user_fns[] = {
    [FN_SEND_HEARTBEAT] = send_heartbeat,
    [FN_TOGGLE_LED]     = toggle_led
};

/* Only TIME_DELAY needs a param (the wait length in TIME_TICK units). */
static const PROGMEM uint16_t heartbeat_params[] = {
    3   /* off 0: TIME_DELAY target */
};

static const PROGMEM cfl_node_desc_t heartbeat_nodes[] = {
    /* chain 0 — heartbeat */
    { CFL_KIND_ONE_SHOT,   FN_SEND_HEARTBEAT, CFL_FN_NONE, 0, 0 },  /* 0 */
    { CFL_KIND_TIME_DELAY, CFL_FN_NONE,       CFL_FN_NONE, 1, 0 },  /* 1 */
    { CFL_KIND_RESET_SELF, CFL_FN_NONE,       CFL_FN_NONE, 0, 0 },  /* 2 */

    /* chain 1 — led_blink */
    { CFL_KIND_ONE_SHOT,   FN_TOGGLE_LED,     CFL_FN_NONE, 0, 0 },  /* 3 */
    { CFL_KIND_RESET_SELF, CFL_FN_NONE,       CFL_FN_NONE, 0, 0 }   /* 4 */
};

static const PROGMEM cfl_chain_desc_t heartbeat_chains[] = {
    /*    first count auto_start                                */
    {     0,    3,    1, 0   },   /* chain 0 — heartbeat        */
    {     3,    2,    1, 0   }    /* chain 1 — led_blink        */
};

const cfl_module_t heartbeat_module = {
    .chains        = heartbeat_chains,
    .nodes         = heartbeat_nodes,
    .params_pool   = heartbeat_params,
    .user_fns      = heartbeat_user_fns,
    .chain_count   = 2,
    .node_count    = 5,
    .user_fn_count = 2
};
