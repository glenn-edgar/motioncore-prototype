/*
 * main.c — Linux test of state-machine-via-chain-enable, event-driven.
 */

#include "../../include/cfl_engine.h"
#include <stdio.h>
#include <string.h>

extern const cfl_module_t state_machine_module;

static unsigned g_tick = 0;
static char     g_trace[64];
static unsigned g_trace_pos = 0;

static void record(char c) {
    if (g_trace_pos + 1 < sizeof(g_trace)) {
        g_trace[g_trace_pos++] = c;
        g_trace[g_trace_pos]   = '\0';
    }
}

cfl_status_t print_a(cfl_engine_t* eng, uint8_t node_idx,
                     uint8_t event_id, uint16_t event_data)
{
    (void)eng; (void)node_idx; (void)event_data;
    if (event_id != CFL_EVENT_INIT) return CFL_CONTINUE;
    printf("tick %3u  STATE = A\n", g_tick); record('A');
    return CFL_CONTINUE;
}
cfl_status_t print_b(cfl_engine_t* eng, uint8_t node_idx,
                     uint8_t event_id, uint16_t event_data)
{
    (void)eng; (void)node_idx; (void)event_data;
    if (event_id != CFL_EVENT_INIT) return CFL_CONTINUE;
    printf("tick %3u  STATE = B\n", g_tick); record('B');
    return CFL_CONTINUE;
}
cfl_status_t print_c(cfl_engine_t* eng, uint8_t node_idx,
                     uint8_t event_id, uint16_t event_data)
{
    (void)eng; (void)node_idx; (void)event_data;
    if (event_id != CFL_EVENT_INIT) return CFL_CONTINUE;
    printf("tick %3u  STATE = C\n", g_tick); record('C');
    return CFL_CONTINUE;
}

#define MAX_CHAINS 3
#define MAX_NODES  12
static uint8_t  ce[(MAX_CHAINS + 7) / 8];
static uint8_t  ne[(MAX_NODES  + 7) / 8];
static uint8_t  ni[(MAX_NODES  + 7) / 8];
static uint16_t ns[MAX_NODES];

static cfl_engine_t engine = {
    .module           = &state_machine_module,
    .chain_enable     = ce,
    .node_enable      = ne,
    .node_initialized = ni,
    .node_scratch     = ns
};

int main(void) {
    cfl_engine_init(&engine);

    printf("--- state machine test: 12 ticks, TIME_DELAY(3) per state ---\n");
    printf("forward A->B->C transitions same tick; C->A is next tick\n\n");

    for (g_tick = 1; g_tick <= 12; g_tick++) {
        cfl_engine_time_tick(&engine, 1);
        cfl_engine_pump(&engine);
    }

    const char* expected = "ABCABC";
    int ok = (strcmp(g_trace, expected) == 0);
    printf("\n--- summary ---\n");
    printf("trace:    %s\n", g_trace);
    printf("expected: %s\n", expected);
    printf("\nresult: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
