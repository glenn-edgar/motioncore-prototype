// ============================================================================
// main_rom.c — basic_primitive_test via M-port const-ROM path
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

#include "basic_primitive_test.h"

extern const s_engine_rom_t basic_primitive_test_module_rom;

// Globals referenced by user_functions.c
uint32_t g_trigger_events = 0;
uint32_t g_bitmap = 0;

#define BUMP_BUFFER_SIZE 1024

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
    if (ptr) g_bump_frees++;
}
static void bump_reset(void) { g_bump_used = 0; }
static double utc_realtime(void* ctx) {
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
static void debug_callback(s_expr_tree_instance_t* inst, const char* msg) {
    (void)inst; (void)msg;
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    printf("\n=== basic_primitive_test (M-port ROM path) ===\n\n");

    s_expr_allocator_t alloc = {
        .malloc   = bump_malloc,
        .free     = bump_free,
        .ctx      = NULL,
        .get_time = utc_realtime,
    };

    s_expr_module_t module;
    uint8_t err = s_engine_init_rom(&module, &basic_primitive_test_module_rom, alloc);
    if (err != S_EXPR_ERR_OK) {
        printf("FATAL: s_engine_init_rom failed: %u\n", err);
        return 1;
    }
    s_expr_module_set_debug(&module, debug_callback);

    printf("Trees=%u Records=%u Strings=%u Oneshot=%u Main=%u Pred=%u\n",
        module.def->tree_count, module.def->record_count, module.def->string_count,
        module.def->oneshot_count, module.def->main_count, module.def->pred_count);

    s_expr_tree_instance_t* tree = s_expr_tree_create_by_hash(
        &module, BASIC_PRIMITIVE_TEST_HASH, 0
    );
    if (!tree) {
        printf("FATAL: Could not create tree\n");
        return 1;
    }
    tree->user_ctx = &g_bitmap;

    int ticks = 0;
    int passed = 0;
    for (int i = 0; i < 1000; i++) {
        ticks++;
        // Stimulus pattern: toggle bits as we tick to drive event triggers
        if (i == 5)   g_bitmap |= (1U << 0);
        if (i == 10)  g_bitmap &= ~(1U << 0);
        if (i == 15)  g_bitmap |= (1U << 1);
        if (i == 20)  g_bitmap |= (1U << 2);
        if (i == 25)  g_bitmap &= ~(1U << 1);
        if (i == 30)  g_bitmap |= (1U << 3);
        if (i == 35)  g_bitmap |= (1U << 4);
        if (i == 40)  g_bitmap &= ~(1U << 3);
        if (i == 45)  g_bitmap &= ~(1U << 4);
        if (i == 50)  g_bitmap |= (1U << 5);
        if (i == 55)  g_bitmap &= ~(1U << 5);

        s_expr_result_t result = s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
        if (result == SE_FUNCTION_TERMINATE ||
            result == SE_TERMINATE ||
            result == SE_DISABLE ||
            result == SE_FUNCTION_DISABLE) {
            passed = 1;
            break;
        }
    }

    s_expr_tree_free(tree);
    bump_reset();

    printf("Ticks: %d  Terminated: %s\n", ticks, passed ? "yes" : "no(capped)");
    printf("Bump peak: %zu B / %d B\n", g_bump_peak, BUMP_BUFFER_SIZE);
    return 0;
}
