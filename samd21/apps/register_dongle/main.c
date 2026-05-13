// ============================================================================
// register_dongle - Seeeduino Xiao SAMD21
// Phase-2d merge:
//   * s_engine M-port            (blink_engine)
//   * libcomm SLIP+CRC framing   (blink_frame)
//   * register_dongle_v2 chain   (Linux waypoint, state machine + dispatch)
//
// Chain shape (v2; locked 2026-05-13 — see register_dongle_v2.lua):
//   io_call(send_register) once on first INIT
//   se_state_machine("dongle_state", {
//     BOOT case:        event_dispatch{ OP_REGISTER_ACK -> set state=OPERATIONAL }
//     OPERATIONAL case: se_fork(
//                         chain_flow{ o_call(send_heartbeat); tick_delay(3); reset },
//                         m_call(toggle_led),
//                         event_dispatch{ OP_PING -> o_call(send_pong) }
//                       )
//   })
//   se_return_halt()
//
// One USB-CDC port. s2m frames staged in the shared TX ring; main loop drains
// to CDC each iteration. RX path: tud_cdc_read -> frame_decoder_feed -> on
// FRAME_READY, s_expr_event_push(tree, SE_EVENT_TICK, meta.cmd, NULL). After
// every engine tick the main loop drains the event_queue, ticking the tree
// once per popped event so se_event_dispatch handlers fire.
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
#include "s_engine_event_queue.h"

#include "register_dongle_v2.h"

#include "frame.h"
#include "opcodes.h"

extern const s_engine_rom_t register_dongle_v2_module_rom;

// ----------------------------------------------------------------------------
// Bump allocator.
// Linux v2 chain peak: 520 B with one tree active. Cortex-M0+ alignment is
// similar; 768 B leaves comfortable headroom for the larger op state machine.
// ----------------------------------------------------------------------------

#define BUMP_BUFFER_SIZE 768u

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
// 256 B sized to comfortably fit the largest expected frame (24 B register
// payload + 7 B s2m header + worst-case SLIP escapes).
// ----------------------------------------------------------------------------

#define TX_RING_SIZE 256u
static uint8_t       g_tx_ring_buf[TX_RING_SIZE];
frame_ring_t         g_tx_ring;

// ----------------------------------------------------------------------------
// RX frame decoder. Direction = M2S (5-byte header). Persistent state — the
// in_escape flag does NOT reset between calls. Frames feed in one byte at a
// time from tud_cdc_read; on FRAME_READY we push the cmd to the engine.
// ----------------------------------------------------------------------------

static frame_decoder_t g_rx_decoder;

// ----------------------------------------------------------------------------
// debug_packet_fn — bridges s_engine's debug_fn callback to libcomm OP_DBG_LOG.
// Every se_log / se_log_int / etc. invocation arrives here with a formatted
// "[timestamp] message" line; we wrap it in an s2m frame and stage it in the
// TX ring. Same drain path as heartbeats/pongs, so it competes for the same
// CFG_TUD_CDC_TX_BUFSIZE bytes — keep log output sparse.
// ----------------------------------------------------------------------------

static uint8_t g_dbg_seq = 0;

static void debug_packet_fn(s_expr_tree_instance_t* inst, const char* msg) {
    (void)inst;
    if (!msg) return;
    size_t len = strlen(msg);
    if (len > COMM_PAYLOAD_MAX) len = COMM_PAYLOAD_MAX;
    frame_meta_t meta = {
        .addr        = 1,
        .cmd         = OP_DBG_LOG,
        .seq         = g_dbg_seq++,
        .ack_seq     = 0,
        .ack_status  = 0,
        .payload_len = (uint8_t)len,
    };
    (void)frame_encode_s2m(&meta, (const uint8_t*)msg, &g_tx_ring);
}

// ----------------------------------------------------------------------------
// Tick the tree and drain its event queue. Mirrors the
// tick_with_event_queue() pattern from s_engine_builtins_spawn.h: each popped
// event is delivered as a fresh node_tick with that event_id, so chain
// dispatchers see the same event_id they would on Linux.
// ----------------------------------------------------------------------------

static void tick_and_drain(s_expr_tree_instance_t* tree) {
    (void)s_expr_node_tick(tree, SE_EVENT_TICK, NULL);
    while (s_expr_event_queue_count(tree) > 0) {
        uint16_t tick_type;
        uint16_t event_id;
        void* event_data;
        s_expr_event_pop(tree, &tick_type, &event_id, &event_data);
        uint16_t saved = tree->tick_type;
        tree->tick_type = tick_type;
        (void)s_expr_node_tick(tree, event_id, event_data);
        tree->tick_type = saved;
    }
}

// ----------------------------------------------------------------------------
// Drain inbound CDC bytes through the decoder. On FRAME_READY push the cmd to
// the tree's event queue; tick_and_drain on the next tick will dispatch it.
// payload pointer is *not* retained — the chain currently dispatches on
// event_id only, no PING payload. When that changes, copy bytes here.
// ----------------------------------------------------------------------------

static uint8_t g_rx_buf[64];
static uint8_t g_rx_payload[COMM_PAYLOAD_MAX];

static void rx_drain_to_event_queue(s_expr_tree_instance_t* tree) {
    if (!tud_cdc_connected() || !tud_cdc_available()) return;
    uint32_t n = tud_cdc_read(g_rx_buf, sizeof(g_rx_buf));
    for (uint32_t i = 0; i < n; i++) {
        frame_meta_t meta;
        frame_decode_result_t r =
            frame_decoder_feed(&g_rx_decoder, g_rx_buf[i], &meta, g_rx_payload);
        if (r == FRAME_DECODE_FRAME_READY) {
            // Push as a "tick"-type event so the chain dispatchers see a
            // regular event_id (matches how se_queue_event behaves internally).
            s_expr_event_push(tree, SE_EVENT_TICK, meta.cmd, NULL);
        }
        // BAD_CRC / BAD_LEN / OVERFLOW: decoder auto-resets; nothing to do
        // here — a host-side retry will re-sync on the next leading END.
    }
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

    frame_ring_init(&g_tx_ring, g_tx_ring_buf, TX_RING_SIZE);
    frame_decoder_init(&g_rx_decoder, FRAME_DIR_M2S);

    s_expr_allocator_t alloc = {
        .malloc   = bump_malloc,
        .free     = bump_free,
        .ctx      = NULL,
        .get_time = engine_get_time,
    };

    s_expr_module_t module;
    s_expr_tree_instance_t* tree = NULL;
    uint8_t init_err = s_engine_init_rom(&module, &register_dongle_v2_module_rom, alloc);
    (void)init_err;
    if (init_err == S_EXPR_ERR_OK) {
        s_expr_module_set_debug(&module, debug_packet_fn);
        tree = s_expr_tree_create_by_hash(&module, REGISTER_DONGLE_V2_HASH, 0);
    }

    // Engine tick cadence: 250 ms (chain expects 4 ticks/sec).
    //
    // KNOWN ISSUE — OP_REGISTER loss on cold boot: the io_call fires once on
    // first INIT and goes to the CDC TX FIFO. If the host isn't actively
    // reading at that moment, the FIFO fills up over the next few seconds and
    // the OP_REGISTER bytes are overwritten. Heartbeats eventually arrive
    // (once host attaches + sends ACK + state transitions to OPERATIONAL),
    // but the registration packet is gone. Proper fix is protocol-level:
    // BOOT state should re-emit OP_REGISTER on a tick_delay loop until
    // OP_REGISTER_ACK is received. That's a DSL chain change, deferred to
    // Phase 2f. A C-side startup gate (tried 2026-05-13) cannot fix this
    // because it gates emission, not delivery — bytes still accumulate in
    // the CDC FIFO and get dropped while the host's userspace isn't reading.
    uint32_t next_tick_ms = 250;

    for (;;) {
        tud_task();

        // Always drain RX, even between ticks — events accumulate in the
        // tree's queue and are dispatched at the next tick_and_drain().
        if (tree != NULL) {
            rx_drain_to_event_queue(tree);
        }

        uint32_t now = board_millis();
        if (tree != NULL && (int32_t)(now - next_tick_ms) >= 0) {
            next_tick_ms += 250;
            tick_and_drain(tree);
        }

        // Drain a chunk of the TX ring to CDC every loop. CFG_TUD_CDC_TX_BUFSIZE
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
