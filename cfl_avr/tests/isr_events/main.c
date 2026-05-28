/*
 * main.c — Linux test of the ISR-safe event-send path.
 *
 * Models a Modbus ISR (simulated) that decodes a frame and pushes a
 * user event EVENT_RX_FRAME into the engine queue via the ISR-safe API.
 * The engine's main-context pump drains the queue, delivering both the
 * TIME_TICK events (from the periodic timer) and the RX events (from the
 * "ISR") to the chain listeners.
 *
 * On AVR this would compile to the same code; cfl_send_event_isr skips the
 * cli/sei guard because the ISR has already disabled interrupts on entry.
 */

#include "../../include/cfl_engine.h"
#include <stdio.h>

#define EVENT_RX_FRAME  (CFL_EVENT_USER_BASE + 0)

extern const cfl_module_t isr_events_module;

static unsigned g_tick      = 0;
static unsigned g_rx_count  = 0;
static unsigned g_beat_count = 0;

cfl_status_t rx_listener(cfl_engine_t* eng, uint8_t node_idx,
                         uint8_t event_id, uint16_t event_data)
{
    (void)eng; (void)node_idx;
    if (event_id == EVENT_RX_FRAME) {
        g_rx_count++;
        printf("tick %3u  RX_FRAME data=0x%04x (count=%u)\n",
               g_tick, event_data, g_rx_count);
    }
    return CFL_CONTINUE;
}

cfl_status_t beat(cfl_engine_t* eng, uint8_t node_idx,
                  uint8_t event_id, uint16_t event_data)
{
    (void)eng; (void)node_idx; (void)event_data;
    if (event_id != CFL_EVENT_INIT) return CFL_CONTINUE;
    g_beat_count++;
    printf("tick %3u  HEARTBEAT (#%u)\n", g_tick, g_beat_count);
    return CFL_CONTINUE;
}

#define MAX_CHAINS 2
#define MAX_NODES  4
static uint8_t  ce[(MAX_CHAINS + 7) / 8];
static uint8_t  ne[(MAX_NODES  + 7) / 8];
static uint8_t  ni[(MAX_NODES  + 7) / 8];
static uint16_t ns[MAX_NODES];

static cfl_engine_t engine = {
    .module           = &isr_events_module,
    .chain_enable     = ce,
    .node_enable      = ne,
    .node_initialized = ni,
    .node_scratch     = ns
};

/* Simulated ISR — pushes via the ISR-safe path. On AVR the ISR has already
 * cli'd; cfl_send_event_isr skips the guard. On Linux it's identical. */
static void simulated_modbus_isr(uint16_t frame_data) {
    cfl_send_event_isr(&engine, EVENT_RX_FRAME, frame_data);
}

int main(void) {
    cfl_engine_init(&engine);

    printf("--- isr_events test: 10 TIME_TICK + 3 simulated RX frames ---\n");
    printf("RX frames injected on ticks 2, 5, 8\n\n");

    for (g_tick = 1; g_tick <= 10; g_tick++) {
        /* Periodic time tick from main loop (could be a SysTick handler). */
        cfl_engine_time_tick(&engine, 1);

        /* Simulated ISR fires on ticks 2, 5, 8 — pushes via ISR-safe API. */
        if (g_tick == 2) simulated_modbus_isr(0xCAFE);
        if (g_tick == 5) simulated_modbus_isr(0xBEEF);
        if (g_tick == 8) simulated_modbus_isr(0xF00D);

        cfl_engine_pump(&engine);
    }

    printf("\n--- summary ---\n");
    printf("rx frames received: %u (expected 3)\n", g_rx_count);
    printf("heartbeats:         %u (expected 2 — on ticks 1 and 6)\n",
           g_beat_count);

    int ok = (g_rx_count == 3) && (g_beat_count == 2);
    printf("\nresult: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
