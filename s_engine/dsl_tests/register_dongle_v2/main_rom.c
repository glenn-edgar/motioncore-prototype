// ============================================================================
// main_rom.c — register_dongle_v2 Linux prototype harness
//
// Validates the state-machine chain shape end-to-end before the SAMD21 port:
//   • Sends OP_REGISTER once on boot (BOOT state)
//   • At tick K, injects OP_REGISTER_ACK -> transition to OPERATIONAL
//   • In OPERATIONAL: heartbeats + LED toggles + listens for OP_PING
//   • At later ticks, injects OP_PING -> expect [PONG] response
//
// Event injection uses s_expr_event_push directly (no real wire). The tick
// loop drains the event queue after each tick, calling s_expr_node_tick
// once per popped event — mirrors the spawn_and_tick_tree pattern.
// ============================================================================

#define _GNU_SOURCE
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#include "s_engine_types.h"
#include "s_engine_module.h"
#include "s_engine_eval.h"
#include "s_engine_node.h"
#include "s_engine_rom.h"
#include "s_engine_event_queue.h"

#include "register_dongle_v2.h"

extern const s_engine_rom_t register_dongle_v2_module_rom;

// Opcodes (mirror register_dongle_v2.lua). m2s opcodes must avoid the engine-
// reserved event_id values: SE_EVENT_TICK=4, SE_EVENT_INIT=0xfffe,
// SE_EVENT_TERMINATE=0xfffd. We allocate m2s in 0x0100+.
#define OP_REGISTER_ACK  0x0103
#define OP_PING          0x0104

// Engine-internal events (never on the wire). Range 0xFE00+.
// Pushed from main_rom.c (Linux harness) or main.c (SAMD21 firmware) when
// some firmware-internal condition is detected — e.g., host reattach on
// SAMD21. Linux harness fakes the same event to exercise the chain path.
#define EV_HOST_REATTACH 0xFE00

// ============================================================================
// Bump allocator (same shape as state_machine/main_rom.c)
// ============================================================================

#define BUMP_BUFFER_SIZE 2048

static uint8_t g_bump_buffer[BUMP_BUFFER_SIZE] __attribute__((aligned(8)));
static size_t  g_bump_used = 0, g_bump_peak = 0, g_bump_allocs = 0, g_bump_frees = 0;

static void* bump_malloc(void* ctx, size_t size) {
    (void)ctx;
    size_t aligned = (size + 7u) & ~(size_t)7u;
    if (g_bump_used + aligned > BUMP_BUFFER_SIZE) {
        fprintf(stderr, "bump OOM: used=%zu req=%zu cap=%d\n", g_bump_used, aligned, BUMP_BUFFER_SIZE);
        return NULL;
    }
    void* p = &g_bump_buffer[g_bump_used];
    g_bump_used += aligned;
    if (g_bump_used > g_bump_peak) g_bump_peak = g_bump_used;
    g_bump_allocs++;
    return p;
}
static void bump_free(void* ctx, void* ptr) { (void)ctx; if (ptr) g_bump_frees++; }
static double utc_realtime(void* ctx) {
    (void)ctx;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
static uint32_t utc_realtime_ms(void* ctx) {
    (void)ctx;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static void debug_callback(s_expr_tree_instance_t* inst, const char* msg) {
    (void)inst; (void)msg;
}

// ============================================================================
// Helper: run one tick + drain any queued events spawned during that tick
// ============================================================================
static s_expr_result_t tick_and_drain(s_expr_tree_instance_t* tree) {
    s_expr_result_t r = s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    while (s_expr_event_queue_count(tree) > 0) {
        uint16_t tt, eid; void* edata;
        s_expr_event_pop(tree, &tt, &eid, &edata);
        uint16_t saved = tree->tick_type;
        tree->tick_type = tt;
        s_expr_result_t er = s_expr_node_tick(tree, eid, edata);
        tree->tick_type = saved;
        if (er == SE_FUNCTION_TERMINATE || er == SE_TERMINATE
         || er == SE_DISABLE || er == SE_FUNCTION_DISABLE) {
            return er;
        }
    }
    return r;
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    printf("\n=== register_dongle_v2 Linux prototype (state-machine, M-port ROM path) ===\n");
    printf("Chain: io_call(send_register) ; se_state_machine{BOOT, OPERATIONAL}\n");
    printf("  BOOT:        event_dispatch{OP_REGISTER_ACK -> set state=OPERATIONAL}\n");
    printf("  OPERATIONAL: fork{heartbeat, LED, event_dispatch{OP_PING -> send_pong}}\n");
    printf("Tick rate: 250 ms (4 Hz)  •  Heartbeat: every 4 ticks (1 Hz)\n");
    printf("Bump buffer: %d B static\n\n", BUMP_BUFFER_SIZE);

    s_expr_allocator_t alloc = {
        .malloc      = bump_malloc,
        .free        = bump_free,
        .ctx         = NULL,
        .get_time    = utc_realtime,
        .get_time_ms = utc_realtime_ms,
    };

    s_expr_module_t module;
    uint8_t err = s_engine_init_rom(&module, &register_dongle_v2_module_rom, alloc);
    if (err != S_EXPR_ERR_OK) { printf("FATAL: s_engine_init_rom err=%u\n", err); return 1; }
    s_expr_module_set_debug(&module, debug_callback);

    printf("Engine initialized from const ROM\n");
    printf("  Trees=%u  Records=%u  Strings=%u  Oneshot=%u  Main=%u  Pred=%u\n\n",
        module.def->tree_count, module.def->record_count, module.def->string_count,
        module.def->oneshot_count, module.def->main_count, module.def->pred_count);

    s_expr_tree_instance_t* tree = s_expr_tree_create_by_hash(
        &module, REGISTER_DONGLE_V2_HASH, 0);
    if (!tree) { printf("FATAL: create tree\n"); return 1; }

    // Schedule of injected events (tick index, opcode, label). Phase 2g
    // adds EV_HOST_REATTACH at tick 35: after the dongle is in OPERATIONAL
    // and exchanging heartbeats/pongs, fake a host reset. Expect:
    // OP_REGISTERs resume at 1 Hz until a second ACK arrives at tick 50.
    struct { int tick; uint16_t op; const char* label; } injections[] = {
        { 18, OP_REGISTER_ACK, "OP_REGISTER_ACK" },
        { 26, OP_PING,         "OP_PING #1" },
        { 32, OP_PING,         "OP_PING #2" },
        { 35, EV_HOST_REATTACH,"EV_HOST_REATTACH (simulated host reset)" },
        { 50, OP_REGISTER_ACK, "OP_REGISTER_ACK (post-reattach)" },
    };
    const int N_INJ = (int)(sizeof(injections) / sizeof(injections[0]));
    int next_inj = 0;

    const int TICKS = 60;
    printf("Running %d ticks @ 250 ms each (~%.1f s real time)\n", TICKS, TICKS * 0.25);
    printf("Expected: REGISTER once -> idle ~5 ticks in BOOT -> ACK -> HEARTBEATs + LED + 3x PONG\n\n");

    for (int i = 0; i < TICKS; i++) {
        // Inject any scheduled host events BEFORE the tick (they will be popped
        // during the post-tick drain in tick_and_drain).
        while (next_inj < N_INJ && injections[next_inj].tick == i + 1) {
            printf("  -> tick %d: injecting %s\n", i + 1, injections[next_inj].label);
            fflush(stdout);
            s_expr_event_push(tree, SE_EVENT_TICK, injections[next_inj].op, NULL);
            next_inj++;
        }

        s_expr_result_t r = tick_and_drain(tree);
        if (r == SE_PIPELINE_DISABLE || r == SE_DISABLE
         || r == SE_FUNCTION_DISABLE || r == SE_FUNCTION_TERMINATE) {
            printf("[engine] chain ended at tick %d (result=%d)\n", i + 1, r);
            break;
        }
        usleep(250 * 1000);
    }

    s_expr_tree_free(tree);
    printf("\nBump peak: %zu B / %d B (%.1f%%)\n",
        g_bump_peak, BUMP_BUFFER_SIZE, 100.0 * g_bump_peak / BUMP_BUFFER_SIZE);
    printf("Allocs=%zu  Frees=%zu\n", g_bump_allocs, g_bump_frees);

    printf("\nDone.\n");
    return 0;
}
