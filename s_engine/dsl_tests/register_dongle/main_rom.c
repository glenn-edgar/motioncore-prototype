// ============================================================================
// main_rom.c — register_dongle Linux prototype harness
// Validates the chain shape (boot-once + heartbeat loop + LED toggle in fork)
// before porting to SAMD21. Runs 25 ticks at simulated 250 ms per tick so
// we can observe REGISTER once + ~6 HEARTBEATs + LED toggles in real time.
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

#include "register_dongle.h"

extern const s_engine_rom_t register_dongle_module_rom;

// ============================================================================
// Bump allocator (same shape as state_machine/main_rom.c)
// ============================================================================

#define BUMP_BUFFER_SIZE 1024

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
static void debug_callback(s_expr_tree_instance_t* inst, const char* msg) {
    (void)inst; (void)msg;
    // silent — would interleave with structured printf
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    printf("\n=== register_dongle Linux prototype (M-port ROM path) ===\n");
    printf("Chain: io_call(send_register) ; se_fork(heartbeat_loop, toggle_led_loop)\n");
    printf("Tick rate: 250 ms (4 Hz)  •  Heartbeat: every 4 ticks (1 Hz)\n");
    printf("Bump buffer: %d B static\n\n", BUMP_BUFFER_SIZE);

    s_expr_allocator_t alloc = {
        .malloc   = bump_malloc,
        .free     = bump_free,
        .ctx      = NULL,
        .get_time = utc_realtime,
    };

    s_expr_module_t module;
    uint8_t err = s_engine_init_rom(&module, &register_dongle_module_rom, alloc);
    if (err != S_EXPR_ERR_OK) { printf("FATAL: s_engine_init_rom err=%u\n", err); return 1; }
    s_expr_module_set_debug(&module, debug_callback);

    printf("Engine initialized from const ROM\n");
    printf("  Trees=%u  Records=%u  Strings=%u  Oneshot=%u  Main=%u  Pred=%u\n\n",
        module.def->tree_count, module.def->record_count, module.def->string_count,
        module.def->oneshot_count, module.def->main_count, module.def->pred_count);

    s_expr_tree_instance_t* tree = s_expr_tree_create_by_hash(
        &module, REGISTER_DONGLE_HASH, 0);
    if (!tree) { printf("FATAL: create tree\n"); return 1; }

    printf("Running 25 ticks @ 250 ms each (~6.25 s real time)...\n");
    printf("Expected trace: REGISTER once + HEARTBEAT every 4 ticks + LED toggle every tick\n\n");

    const int TICKS = 25;
    for (int i = 0; i < TICKS; i++) {
        s_expr_result_t r = s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
        if (r == SE_PIPELINE_DISABLE || r == SE_DISABLE || r == SE_FUNCTION_DISABLE) {
            printf("[engine] pipeline disabled at tick %d — chain unexpectedly stopped\n", i + 1);
            break;
        }
        usleep(250 * 1000);   // 250 ms — simulate the embedded tick rate
    }

    s_expr_tree_free(tree);
    printf("\nBump peak: %zu B / %d B (%.1f%%)\n",
        g_bump_peak, BUMP_BUFFER_SIZE, 100.0 * g_bump_peak / BUMP_BUFFER_SIZE);
    printf("Allocs=%zu  Frees=%zu\n", g_bump_allocs, g_bump_frees);

    printf("\nDone.\n");
    return 0;
}
