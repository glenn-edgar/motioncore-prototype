// ============================================================================
// blink_engine - Seeeduino Xiao SAMD21
// s_engine M-port bring-up: single tree, single m_call("toggle_led") that
// toggles the user LED every engine tick and reports progress over USB-CDC.
// ============================================================================

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"

#include "s_engine_types.h"
#include "s_engine_module.h"
#include "s_engine_node.h"
#include "s_engine_rom.h"

#include "blink_engine.h"

extern const s_engine_rom_t blink_engine_module_rom;

// ----------------------------------------------------------------------------
// Bump allocator — single static buffer, cursor advance, no-op free.
// Mirrors the M-port pattern from dsl_tests/state_machine/main_rom.c.
// 1024 B keeps headroom for engine internals over a single-node tree.
// ----------------------------------------------------------------------------

#define BUMP_BUFFER_SIZE 1024u

static uint8_t  g_bump_buffer[BUMP_BUFFER_SIZE] __attribute__((aligned(8)));
static size_t   g_bump_used = 0;
static size_t   g_bump_peak = 0;

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

// SAMD21 board_millis() returns ms since boot; ticks->seconds for the engine.
static double engine_get_time(void* ctx) {
    (void)ctx;
    return (double)board_millis() / 1000.0;
}

// ----------------------------------------------------------------------------
// Entry
// ----------------------------------------------------------------------------

int main(void) {
    board_init();

    tusb_rhport_init_t const rhport_init = {
        .role  = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO,
    };
    tusb_init(BOARD_TUD_RHPORT, &rhport_init);

    // Bring up the engine via the const-ROM path.
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

    uint32_t next_tick_ms  = 250;   // engine tick cadence (4 Hz visible blink)
    uint32_t next_print_ms = 1000;  // host-visible heartbeat
    uint32_t tick_counter  = 0;

    for (;;) {
        tud_task();

        if (tud_cdc_connected()) {
            tud_cdc_write_flush();
        }

        uint32_t now = board_millis();

        if (tree != NULL && (int32_t)(now - next_tick_ms) >= 0) {
            next_tick_ms += 250;
            s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
            tick_counter++;
        }

        if ((int32_t)(now - next_print_ms) >= 0) {
            next_print_ms += 1000;
            if (tree != NULL) {
                printf("tick=%lu bump_peak=%u\r\n",
                       (unsigned long)tick_counter,
                       (unsigned)g_bump_peak);
            } else {
                printf("engine_init_err=%u\r\n", (unsigned)init_err);
            }
            fflush(stdout);
            if (tud_cdc_connected()) {
                tud_cdc_write_flush();
            }
        }
    }
}
