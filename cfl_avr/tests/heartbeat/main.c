/*
 * main.c — Linux test harness for the heartbeat module.
 *
 * Drives the engine with 12 TIME_TICK events (data=1 each). Verifies the
 * trace: heartbeat on ticks 1,4,7,10; LED toggle every tick.
 */

#include "../../include/cfl_engine.h"
#include <stdio.h>

extern const cfl_module_t heartbeat_module;

static unsigned g_heartbeat_count = 0;
static unsigned g_led_toggles     = 0;
static int      g_led_state       = 0;
static unsigned g_tick            = 0;

cfl_status_t send_heartbeat(cfl_engine_t* eng, uint8_t node_idx,
                            uint8_t event_id, uint16_t event_data)
{
    (void)eng; (void)node_idx; (void)event_data;
    /* ONE_SHOT calls us on INIT for the side-effect. */
    if (event_id != CFL_EVENT_INIT) return CFL_CONTINUE;
    g_heartbeat_count++;
    printf("tick %3u  HEARTBEAT  (count=%u)\n", g_tick, g_heartbeat_count);
    return CFL_CONTINUE;
}

cfl_status_t toggle_led(cfl_engine_t* eng, uint8_t node_idx,
                        uint8_t event_id, uint16_t event_data)
{
    (void)eng; (void)node_idx; (void)event_data;
    if (event_id != CFL_EVENT_INIT) return CFL_CONTINUE;
    g_led_state ^= 1;
    g_led_toggles++;
    printf("tick %3u  LED=%d      (toggles=%u)\n",
           g_tick, g_led_state, g_led_toggles);
    return CFL_CONTINUE;
}

#define MAX_CHAINS 2
#define MAX_NODES  5
static uint8_t  ce[(MAX_CHAINS + 7) / 8];
static uint8_t  ne[(MAX_NODES  + 7) / 8];
static uint8_t  ni[(MAX_NODES  + 7) / 8];
static uint16_t ns[MAX_NODES];

static cfl_engine_t engine = {
    .module           = &heartbeat_module,
    .chain_enable     = ce,
    .node_enable      = ne,
    .node_initialized = ni,
    .node_scratch     = ns
};

int main(void) {
    cfl_engine_init(&engine);

    printf("--- heartbeat test: 12 TIME_TICK events (data=1 each) ---\n");
    printf("expected: heartbeat on ticks 1,4,7,10  |  LED toggle every tick\n\n");

    for (g_tick = 1; g_tick <= 12; g_tick++) {
        cfl_engine_time_tick(&engine, 1);
        cfl_engine_pump(&engine);
    }

    printf("\n--- summary ---\n");
    printf("heartbeats fired: %u (expected 4)\n", g_heartbeat_count);
    printf("led toggles:      %u (expected 12)\n", g_led_toggles);
    printf("final LED state:  %d\n", g_led_state);

    int ok = (g_heartbeat_count == 4) && (g_led_toggles == 12);
    printf("\nresult: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
