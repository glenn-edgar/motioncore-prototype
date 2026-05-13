// ============================================================================
// register_dongle - Seeeduino Xiao SAMD21
// Phase-2 merge of:
//   * s_engine M-port            (blink_engine)
//   * libcomm SLIP+CRC framing   (blink_frame)
//   * register_dongle chain      (Linux: dsl_tests/register_dongle)
//
// Chain shape (locked):
//   io_call(send_register) once on first INIT
//   se_fork(
//     chain_flow{ o_call(send_heartbeat); tick_delay(3); pipeline_reset },
//     m_call(toggle_led)
//   )
//   se_return_halt()
//
// One USB-CDC port. All output goes through libcomm s2m frames staged in
// the shared TX ring; main loop drains the ring to CDC each iteration.
// ============================================================================

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"

#include "s_engine_types.h"
#include "s_engine_module.h"
#include "s_engine_node.h"
#include "s_engine_rom.h"

#include "register_dongle.h"

#include "frame.h"

extern const s_engine_rom_t register_dongle_module_rom;

// ----------------------------------------------------------------------------
// Bump allocator.
// Linux peak for this chain was 440 B; Cortex-M0+ struct alignment is
// smaller but we keep margin and start at 512 B.
// ----------------------------------------------------------------------------

#define BUMP_BUFFER_SIZE 512u

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
    (void)ctx; (void)ptr;
}

static double engine_get_time(void* ctx) {
    (void)ctx;
    return (double)board_millis() / 1000.0;
}

// ----------------------------------------------------------------------------
// Shared TX ring. user_functions.c references this via `extern`.
// Power-of-2 size (frame_ring_init requires it); 256 B comfortably fits the
// largest expected frame (24 B register payload + 7 B s2m header + escapes).
// ----------------------------------------------------------------------------

#define TX_RING_SIZE 256u
static uint8_t       g_tx_ring_buf[TX_RING_SIZE];
frame_ring_t         g_tx_ring;   // not static — exported to user_functions.c

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

    frame_ring_init(&g_tx_ring, g_tx_ring_buf, TX_RING_SIZE);

    s_expr_allocator_t alloc = {
        .malloc   = bump_malloc,
        .free     = bump_free,
        .ctx      = NULL,
        .get_time = engine_get_time,
    };

    s_expr_module_t module;
    s_expr_tree_instance_t* tree = NULL;
    uint8_t init_err = s_engine_init_rom(&module, &register_dongle_module_rom, alloc);
    (void)init_err;
    if (init_err == S_EXPR_ERR_OK) {
        tree = s_expr_tree_create_by_hash(&module, REGISTER_DONGLE_HASH, 0);
    }

    // Engine tick cadence: 250 ms (matches Linux waypoint; chain expects
    // 4 ticks/sec so the 3-tick delay = 1 Hz heartbeat).
    uint32_t next_tick_ms = 250;

    for (;;) {
        tud_task();

        uint32_t now = board_millis();

        if (tree != NULL && (int32_t)(now - next_tick_ms) >= 0) {
            next_tick_ms += 250;
            s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
        }

        // Drain a chunk of the ring to CDC every loop. CFG_TUD_CDC_TX_BUFSIZE
        // is 64 B; sizing buf to 64 B keeps each drain within one write.
        if (tud_cdc_connected()) {
            uint8_t buf[64];
            uint32_t n = frame_ring_read_drain(&g_tx_ring, buf, sizeof(buf));
            if (n > 0) {
                tud_cdc_write(buf, n);
                tud_cdc_write_flush();
            }
        }
    }
}
