// ============================================================================
// blink_engine — Seeed XIAO RA4M1 (Renesas R7FA4M1AB, Cortex-M4)
//
// s_engine M-port bring-up on the second chip family. Stands up the engine
// from the const ROM and ticks a one-node chain every 250 ms.
//
// The chain ROM (blink_engine_module_rom.c) and blink_engine.h are reused
// BYTE-FOR-BYTE from samd21/apps/blink_engine — the portability keystone:
// same chain ROM, recompiled engine, new chip layer.
//
// The chain's single node calls toggle_led(), which increments g_engine_calls.
// If engine_calls climbs in lockstep with the main-loop tick counter, the
// reused ROM is dispatching the node correctly on Cortex-M4.
// ============================================================================

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "bsp/board_api.h"
#include "tusb.h"

#include "s_engine_types.h"
#include "s_engine_module.h"
#include "s_engine_node.h"
#include "s_engine_rom.h"

#include "blink_engine.h"

extern const s_engine_rom_t blink_engine_module_rom;

// Incremented by toggle_led() (user_functions.c) on each node dispatch.
extern volatile uint32_t g_engine_calls;

// ----------------------------------------------------------------------------
// Bump allocator — single static buffer, cursor advance, no-op free.
// Mirrors the SAMD21 blink_engine / dsl_tests main_rom.c pattern.
// ----------------------------------------------------------------------------
#define BUMP_BUFFER_SIZE 1024u

static uint8_t g_bump_buffer[BUMP_BUFFER_SIZE] __attribute__((aligned(8)));
static size_t  g_bump_used = 0;
static size_t  g_bump_peak = 0;

static void* bump_malloc(void* ctx, size_t size) {
    (void)ctx;
    size_t aligned = (size + 7u) & ~(size_t)7u;
    if (g_bump_used + aligned > BUMP_BUFFER_SIZE) {
        return NULL;
    }
    void* p = &g_bump_buffer[g_bump_used];
    g_bump_used += aligned;
    if (g_bump_used > g_bump_peak) g_bump_peak = g_bump_used;
    return p;
}

static void bump_free(void* ctx, void* ptr) {
    (void)ctx; (void)ptr;  // bump allocator: free is a no-op
}

// board_millis() is ms since boot; the engine wants seconds.
// Divisor is (double)1000, not 1000.0 — the RA build uses
// -fsingle-precision-constant, which would make a 1000.0 literal a float.
static double engine_get_time(void* ctx) {
    (void)ctx;
    return (double)board_millis() / (double)1000;
}

// ----------------------------------------------------------------------------
// Entry
// ----------------------------------------------------------------------------

int main(void) {
    board_init();

    tusb_rhport_init_t dev_init = {
        .role  = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO,
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);

    s_expr_allocator_t alloc = {
        .malloc   = bump_malloc,
        .free     = bump_free,
        .ctx      = NULL,
        .get_time = engine_get_time,
    };

    s_expr_module_t module;
    s_expr_tree_instance_t* tree = NULL;
    uint8_t init_err = s_engine_init_rom(&module, &blink_engine_module_rom, alloc);
    if (init_err == S_EXPR_ERR_OK) {
        tree = s_expr_tree_create_by_hash(&module, BLINK_ENGINE_HASH, 0);
    }

    uint32_t next_tick_ms  = 250;   // engine tick cadence
    uint32_t next_print_ms = 1000;  // host-visible heartbeat
    uint32_t tick_counter  = 0;

    for (;;) {
        tud_task();

        uint32_t now = board_millis();

        if (tree != NULL && (int32_t)(now - next_tick_ms) >= 0) {
            next_tick_ms += 250;
            s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
            tick_counter++;
        }

        if ((int32_t)(now - next_print_ms) >= 0) {
            next_print_ms += 1000;
            if (tud_cdc_connected()) {
                char line[80];
                int n;
                if (tree != NULL) {
                    n = snprintf(line, sizeof line,
                                 "blink_engine ticks=%lu engine_calls=%lu bump_peak=%u\r\n",
                                 (unsigned long)tick_counter,
                                 (unsigned long)g_engine_calls,
                                 (unsigned)g_bump_peak);
                } else {
                    n = snprintf(line, sizeof line,
                                 "blink_engine ENGINE_INIT_ERR=%u\r\n",
                                 (unsigned)init_err);
                }
                if (n > 0) {
                    tud_cdc_write(line, (uint32_t)n);
                    tud_cdc_write_flush();
                }
            }
        }
    }
}
