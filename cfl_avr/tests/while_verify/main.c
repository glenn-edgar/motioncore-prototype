/*
 * main.c — Linux test of WHILE and (continuous) VERIFY, event-driven.
 */

#include "../../include/cfl_engine.h"
#include <stdio.h>

extern const cfl_module_t while_verify_module;

static unsigned g_tick    = 0;
static int      g_counter = 0;
static unsigned g_passes  = 0;

cfl_status_t reset_counter(cfl_engine_t* eng, uint8_t node_idx,
                           uint8_t event_id, uint16_t event_data)
{
    (void)eng; (void)node_idx; (void)event_data;
    if (event_id != CFL_EVENT_INIT) return CFL_CONTINUE;
    g_counter = 0;
    printf("tick %3u  reset counter -> 0\n", g_tick);
    return CFL_CONTINUE;
}

/* WHILE polls this on each event. Return CONTINUE while counting,
 * DISABLE when done. */
cfl_status_t wait_for_3(cfl_engine_t* eng, uint8_t node_idx,
                        uint8_t event_id, uint16_t event_data)
{
    (void)eng; (void)node_idx; (void)event_data; (void)event_id;
    if (g_counter >= 3) {
        printf("tick %3u  WHILE exit (counter=%d)\n", g_tick, g_counter);
        return CFL_DISABLE;
    }
    g_counter++;
    printf("tick %3u  WHILE poll (counter=%d)\n", g_tick, g_counter);
    return CFL_CONTINUE;
}

/* VERIFY (continuous): every event, check invariant. OK -> CONTINUE,
 * anything else -> the engine triggers chain RESET. */
cfl_status_t verify_is_3(cfl_engine_t* eng, uint8_t node_idx,
                         uint8_t event_id, uint16_t event_data)
{
    (void)eng; (void)node_idx; (void)event_data; (void)event_id;
    if (g_counter == 3) return CFL_CONTINUE;
    printf("tick %3u  VERIFY FAIL (counter=%d) -> chain reset\n",
           g_tick, g_counter);
    return CFL_TERMINATE;   /* any non-CONTINUE triggers RESET in VERIFY kind */
}

cfl_status_t print_pass(cfl_engine_t* eng, uint8_t node_idx,
                        uint8_t event_id, uint16_t event_data)
{
    (void)eng; (void)node_idx; (void)event_data;
    if (event_id != CFL_EVENT_INIT) return CFL_CONTINUE;
    g_passes++;
    printf("tick %3u  PASS #%u\n", g_tick, g_passes);
    return CFL_CONTINUE;
}

#define MAX_CHAINS 1
#define MAX_NODES  6
static uint8_t  ce[(MAX_CHAINS + 7) / 8];
static uint8_t  ne[(MAX_NODES  + 7) / 8];
static uint8_t  ni[(MAX_NODES  + 7) / 8];
static uint16_t ns[MAX_NODES];

static cfl_engine_t engine = {
    .module           = &while_verify_module,
    .chain_enable     = ce,
    .node_enable      = ne,
    .node_initialized = ni,
    .node_scratch     = ns
};

int main(void) {
    cfl_engine_init(&engine);

    printf("--- while_verify test: 15 TIME_TICK events ---\n");
    printf("expected: PASS on ticks 4, 9, 14  (3 passes)\n\n");

    for (g_tick = 1; g_tick <= 15; g_tick++) {
        cfl_engine_time_tick(&engine, 1);
        cfl_engine_pump(&engine);
    }

    printf("\n--- summary ---\n");
    printf("passes:   %u (expected 3)\n", g_passes);
    int ok = (g_passes == 3);
    printf("\nresult: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
