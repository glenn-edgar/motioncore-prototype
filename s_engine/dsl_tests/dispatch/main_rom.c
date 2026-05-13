// main_rom.c — dispatch via M-port const-ROM path
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
#include "s_engine_event_queue.h"

#include "dispatch_test.h"

extern const s_engine_rom_t dispatch_test_module_rom;

#define BUMP_BUFFER_SIZE 1024
static uint8_t g_bump_buffer[BUMP_BUFFER_SIZE] __attribute__((aligned(8)));
static size_t  g_bump_used = 0, g_bump_peak = 0;

static void* bump_malloc(void* ctx, size_t size) {
    (void)ctx;
    size_t aligned = (size + 7u) & ~(size_t)7u;
    if (g_bump_used + aligned > BUMP_BUFFER_SIZE) { fprintf(stderr,"bump OOM\n"); return NULL; }
    void* p = &g_bump_buffer[g_bump_used];
    g_bump_used += aligned;
    if (g_bump_used > g_bump_peak) g_bump_peak = g_bump_used;
    return p;
}
static void bump_free(void* ctx, void* ptr) { (void)ctx; (void)ptr; }
static double utc_realtime(void* ctx) {
    (void)ctx;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
static void debug_callback(s_expr_tree_instance_t* i, const char* m) { (void)i; (void)m; }

int main(void) {
    printf("=== dispatch (M-port ROM) ===\n");
    s_expr_allocator_t alloc = { .malloc=bump_malloc, .free=bump_free, .ctx=NULL, .get_time=utc_realtime };
    s_expr_module_t module;
    if (s_engine_init_rom(&module, &dispatch_test_module_rom, alloc) != S_EXPR_ERR_OK) {
        printf("FATAL: init_rom failed\n"); return 1;
    }
    s_expr_module_set_debug(&module, debug_callback);
    printf("Trees=%u Records=%u Strings=%u Oneshot=%u Main=%u Pred=%u\n",
        module.def->tree_count, module.def->record_count, module.def->string_count,
        module.def->oneshot_count, module.def->main_count, module.def->pred_count);

    s_expr_tree_instance_t* tree = s_expr_tree_create_by_hash(&module, DISPATCH_TEST_HASH, 0);
    if (!tree) { printf("FATAL: create tree\n"); return 1; }

    int ticks = 0, passed = 0;
    for (int i = 0; i < 1000; i++) {
        ticks++;
        s_expr_result_t r = s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
        while (s_expr_event_queue_count(tree) > 0) {
            uint16_t tt, eid; void* edata;
            s_expr_event_pop(tree, &tt, &eid, &edata);
            uint16_t saved = tree->tick_type;
            tree->tick_type = tt;
            s_expr_result_t er = s_expr_node_tick(tree, eid, edata);
            tree->tick_type = saved;
            if (er == SE_FUNCTION_TERMINATE || er == SE_TERMINATE || er == SE_DISABLE || er == SE_FUNCTION_DISABLE) {
                r = er; break;
            }
        }
        if (r == SE_FUNCTION_TERMINATE || r == SE_TERMINATE || r == SE_DISABLE || r == SE_FUNCTION_DISABLE) {
            passed = 1; break;
        }
    }
    s_expr_tree_free(tree);
    printf("Ticks: %d  Terminated: %s\n", ticks, passed ? "yes" : "no(capped)");
    printf("Bump peak: %zu B / %d B\n", g_bump_peak, BUMP_BUFFER_SIZE);
    return 0;
}
