/*
 * main.c — Linux test of CFL_KIND_INIT_TERM session-scope RAII, event-driven.
 */

#include "../../include/cfl_engine.h"
#include <stdio.h>

extern const cfl_module_t init_term_module;

static unsigned g_tick      = 0;
static unsigned g_acq_count = 0;
static unsigned g_rel_count = 0;
static unsigned g_a_count   = 0;
static unsigned g_b_count   = 0;
static int      g_halted    = 0;

cfl_status_t acquire_workers(cfl_engine_t* eng, uint8_t node_idx,
                             uint8_t event_id, uint16_t event_data)
{
    (void)event_data;
    if (event_id != CFL_EVENT_INIT) return CFL_CONTINUE;
    uint8_t pc = cfl_node_param_count(eng, node_idx);
    printf("tick %3u  ACQUIRE [", g_tick);
    for (uint8_t i = 0; i < pc; i++) {
        uint8_t cid = (uint8_t)cfl_node_param(eng, node_idx, i);
        printf("%s%u", i ? "," : "", cid);
        cfl_chain_enable(eng, cid);
    }
    printf("]\n");
    g_acq_count++;
    return CFL_CONTINUE;
}

cfl_status_t release_workers(cfl_engine_t* eng, uint8_t node_idx,
                             uint8_t event_id, uint16_t event_data)
{
    (void)event_data;
    if (event_id != CFL_EVENT_TERMINATE) return CFL_CONTINUE;
    uint8_t pc = cfl_node_param_count(eng, node_idx);
    printf("tick %3u  RELEASE [", g_tick);
    for (uint8_t i = 0; i < pc; i++) {
        uint8_t cid = (uint8_t)cfl_node_param(eng, node_idx, i);
        printf("%s%u", i ? "," : "", cid);
        cfl_chain_disable(eng, cid);
    }
    printf("]\n");
    g_rel_count++;
    return CFL_CONTINUE;
}

cfl_status_t halt_session(cfl_engine_t* eng, uint8_t node_idx,
                          uint8_t event_id, uint16_t event_data)
{
    (void)eng; (void)node_idx; (void)event_data;
    if (event_id == CFL_EVENT_INIT) {
        printf("tick %3u  HALT_SESSION\n", g_tick);
        g_halted = 1;
        return CFL_CONTINUE;  /* INIT return ignored anyway */
    }
    if (event_id == CFL_EVENT_TERMINATE) return CFL_CONTINUE;
    /* On the first non-INIT event after we ran, terminate the chain. */
    return CFL_TERMINATE;
}

cfl_status_t print_a(cfl_engine_t* eng, uint8_t node_idx,
                     uint8_t event_id, uint16_t event_data)
{
    (void)eng; (void)node_idx; (void)event_data;
    if (event_id != CFL_EVENT_INIT) return CFL_CONTINUE;
    printf("tick %3u  WORKER A\n", g_tick);
    g_a_count++;
    return CFL_CONTINUE;
}

cfl_status_t print_b(cfl_engine_t* eng, uint8_t node_idx,
                     uint8_t event_id, uint16_t event_data)
{
    (void)eng; (void)node_idx; (void)event_data;
    if (event_id != CFL_EVENT_INIT) return CFL_CONTINUE;
    printf("tick %3u  WORKER B\n", g_tick);
    g_b_count++;
    return CFL_CONTINUE;
}

#define MAX_CHAINS 3
#define MAX_NODES  7
static uint8_t  ce[(MAX_CHAINS + 7) / 8];
static uint8_t  ne[(MAX_NODES  + 7) / 8];
static uint8_t  ni[(MAX_NODES  + 7) / 8];
static uint16_t ns[MAX_NODES];

static cfl_engine_t engine = {
    .module           = &init_term_module,
    .chain_enable     = ce,
    .node_enable      = ne,
    .node_initialized = ni,
    .node_scratch     = ns
};

int main(void) {
    cfl_engine_init(&engine);

    printf("--- init_term test: 10 TIME_TICK events, session TIME_DELAY(4) ---\n\n");

    for (g_tick = 1; g_tick <= 10; g_tick++) {
        cfl_engine_time_tick(&engine, 1);
        cfl_engine_pump(&engine);
    }

    printf("\n--- summary ---\n");
    printf("acquires:    %u (expected 1)\n", g_acq_count);
    printf("releases:    %u (expected 1)\n", g_rel_count);
    printf("worker A:    %u\n", g_a_count);
    printf("worker B:    %u\n", g_b_count);
    printf("halted:      %d (expected 1)\n", g_halted);

    int ok = (g_acq_count == 1)
          && (g_rel_count == 1)
          && (g_halted    == 1)
          && (g_a_count   == g_b_count)
          && (g_a_count   >= 3);
    printf("\nresult: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
