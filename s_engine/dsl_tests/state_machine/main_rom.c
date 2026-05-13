// ============================================================================
// main_rom.c — state_machine via M-port const-ROM path
// No blob loader, no s_engine_register_builtins. Linker has resolved fn ptrs.
// ============================================================================

#define _GNU_SOURCE
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "s_engine_types.h"
#include "s_engine_module.h"
#include "s_engine_eval.h"
#include "s_engine_node.h"
#include "s_engine_rom.h"

#include "state_machine_test.h"

extern const s_engine_rom_t state_machine_test_module_rom;

// ============================================================================
// Bump allocator — single pre-allocated buffer, cursor-advance malloc, no-op free.
// Matches the M-port deployment model: one buffer per module instance, no heap.
// ============================================================================

#define BUMP_BUFFER_SIZE 1024  // TODO: read from rom->bump_buffer_size when DSL emits it

static uint8_t g_bump_buffer[BUMP_BUFFER_SIZE] __attribute__((aligned(8)));
static size_t  g_bump_used   = 0;
static size_t  g_bump_peak   = 0;
static size_t  g_bump_allocs = 0;
static size_t  g_bump_frees  = 0;

static void* bump_malloc(void* ctx, size_t size) {
    (void)ctx;
    size_t aligned = (size + 7u) & ~(size_t)7u;
    if (g_bump_used + aligned > BUMP_BUFFER_SIZE) {
        fprintf(stderr, "bump_malloc: out of space (used=%zu + req=%zu > %d)\n",
                g_bump_used, aligned, BUMP_BUFFER_SIZE);
        return NULL;
    }
    void* p = &g_bump_buffer[g_bump_used];
    g_bump_used += aligned;
    if (g_bump_used > g_bump_peak) g_bump_peak = g_bump_used;
    g_bump_allocs++;
    return p;
}

static void bump_free(void* ctx, void* ptr) {
    (void)ctx; (void)ptr;
    // No-op: bump allocator releases everything at once via bump_reset
    if (ptr) g_bump_frees++;
}

static void bump_reset(void) {
    g_bump_used = 0;
}

static void print_alloc_stats(const char* phase) {
    printf("[%-22s] allocs=%zu, frees=%zu, used=%zu B, peak=%zu B\n",
        phase, g_bump_allocs, g_bump_frees, g_bump_used, g_bump_peak);
}
static double utc_realtime(void* ctx) {
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
static void debug_callback(s_expr_tree_instance_t* inst, const char* msg) {
    (void)inst;
    printf("  [DEBUG] %s\n", msg);
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    printf("\n=== STATE MACHINE TEST (M-port ROM path) ===\n\n");

    s_expr_allocator_t alloc = {
        .malloc   = bump_malloc,
        .free     = bump_free,
        .ctx      = NULL,
        .get_time = utc_realtime,
    };
    printf("Bump buffer: %d B pre-allocated, aligned 8\n\n", BUMP_BUFFER_SIZE);

    s_expr_module_t module;
    uint8_t err = s_engine_init_rom(&module, &state_machine_test_module_rom, alloc);
    if (err != S_EXPR_ERR_OK) {
        printf("FATAL: s_engine_init_rom failed: %u\n", err);
        return 1;
    }
    s_expr_module_set_debug(&module, debug_callback);
    print_alloc_stats("after init_rom");

    printf("Engine initialized from const ROM\n");
    printf("  Trees:    %u\n", module.def->tree_count);
    printf("  Records:  %u\n", module.def->record_count);
    printf("  Strings:  %u\n", module.def->string_count);
    printf("  Oneshot:  %u\n", module.def->oneshot_count);
    printf("  Main:     %u\n", module.def->main_count);
    printf("  Pred:     %u\n", module.def->pred_count);
    printf("\n");

    s_expr_tree_instance_t* tree = s_expr_tree_create_by_hash(
        &module, STATE_MACHINE_TEST_HASH, 0
    );
    if (!tree) {
        printf("FATAL: Could not create tree\n");
        return 1;
    }
    print_alloc_stats("after tree_create");

    int passed = 0;
    for (int i = 0; i < 500; i++) {
        s_expr_result_t result = s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
        if (result == SE_FUNCTION_TERMINATE) {
            printf("Tick %3d: SE_FUNCTION_TERMINATE\n", i + 1);
            printf("PASSED: tree reached terminate via const-ROM path\n");
            passed = 1;
            break;
        }
    }

    print_alloc_stats("after test run");
    s_expr_tree_free(tree);
    print_alloc_stats("after tree_free");
    bump_reset();
    print_alloc_stats("after bump_reset");

    printf("\nBump peak: %zu B / %d B buffer (%.1f%% used)\n",
        g_bump_peak, BUMP_BUFFER_SIZE, 100.0 * g_bump_peak / BUMP_BUFFER_SIZE);

    if (!passed) {
        printf("FAILED: did not reach SE_FUNCTION_TERMINATE in 500 ticks\n");
        return 1;
    }

    printf("\nAll tests completed.\n");
    return 0;
}
