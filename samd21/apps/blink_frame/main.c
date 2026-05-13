// ============================================================================
// blink_frame - Seeeduino Xiao SAMD21
// s_engine M-port: same engine (4 Hz LED toggle) as blink_engine, but the
// 1 Hz host-visible heartbeat is replaced with a libcomm s2m frame
// (SLIP + CRC-8/AUTOSAR) carrying a fixed payload string.
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

#include "blink_engine.h"

#include "frame.h"

extern const s_engine_rom_t blink_engine_module_rom;

// ----------------------------------------------------------------------------
// Bump allocator (unchanged from blink_engine).
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
    (void)ctx; (void)ptr;
}

static double engine_get_time(void* ctx) {
    (void)ctx;
    return (double)board_millis() / 1000.0;
}

// ----------------------------------------------------------------------------
// Frame TX ring (SLIP-encoded bytes staged here, drained to CDC each loop).
// 256 B is power-of-2 and comfortably fits one ~30 B encoded frame with
// margin. frame_ring_init requires power-of-2 size.
// ----------------------------------------------------------------------------

#define TX_RING_SIZE 256u
static uint8_t       tx_ring_buf[TX_RING_SIZE];
static frame_ring_t  tx_ring;

static uint32_t      g_frame_counter = 0;

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

    frame_ring_init(&tx_ring, tx_ring_buf, TX_RING_SIZE);

    s_expr_allocator_t alloc = {
        .malloc   = bump_malloc,
        .free     = bump_free,
        .ctx      = NULL,
        .get_time = engine_get_time,
    };

    s_expr_module_t module;
    s_expr_tree_instance_t* tree = NULL;
    uint8_t init_err = s_engine_init_rom(&module, &blink_engine_module_rom, alloc);
    (void)init_err;
    if (init_err == S_EXPR_ERR_OK) {
        tree = s_expr_tree_create_by_hash(&module, BLINK_ENGINE_HASH, 0);
    }

    uint32_t next_tick_ms   = 250;   // engine tick cadence (4 Hz visible blink)
    uint32_t next_frame_ms  = 1000;  // s2m frame heartbeat
    uint32_t tick_counter   = 0;
    (void)tick_counter;

    for (;;) {
        tud_task();

        uint32_t now = board_millis();

        if (tree != NULL && (int32_t)(now - next_tick_ms) >= 0) {
            next_tick_ms += 250;
            s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
            tick_counter++;
        }

        if ((int32_t)(now - next_frame_ms) >= 0) {
            next_frame_ms += 1000;
            static const uint8_t payload[] = "hello samd21 frame";
            frame_meta_t meta = {
                .addr        = 1,
                .cmd         = 0x0001,
                .seq         = (uint8_t)(g_frame_counter & 0xFFu),
                .ack_seq     = 0,
                .ack_status  = 0,
                .payload_len = (uint8_t)(sizeof(payload) - 1u),  // drop trailing NUL
            };
            (void)frame_encode_s2m(&meta, payload, &tx_ring);
            g_frame_counter++;
        }

        // Drain a chunk of the ring to CDC every loop. CFG_TUD_CDC_TX_BUFSIZE
        // is 64 B; a 64 B local buffer keeps every drained chunk within
        // one tud_cdc_write call.
        if (tud_cdc_connected()) {
            uint8_t buf[64];
            uint32_t n = frame_ring_read_drain(&tx_ring, buf, sizeof(buf));
            if (n > 0) {
                tud_cdc_write(buf, n);
                tud_cdc_write_flush();
            }
        }
    }
}
