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
#include <stdbool.h>
#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"
#include "samd21.h"     // CMSIS: NVIC_SystemReset (inline)

#include "s_engine_types.h"
#include "s_engine_module.h"
#include "s_engine_node.h"
#include "s_engine_rom.h"
#include "s_engine_event_queue.h"

#include "register_dongle_v2.h"

#include "frame.h"
#include "opcodes.h"
#include "flash_storage.h"
#include "shell_commands.h"  // firmware_sysinfo_t
#include "samd21_hal.h"      // hal_wdt_init/pet, hal_capture_reset_cause
#include "samd21_interlocks.h"

// Implemented in user_functions.c.
extern void     register_dongle_load_commissioning(void);
extern uint32_t g_pending_commission_instance_id;
extern bool     shell_pending_push(const uint8_t* payload, uint8_t len);

// Deferred-reboot plumbing: handle_commission_set/clear sets the reboot time;
// the main loop waits for TX to drain (~200 ms), then triggers NVIC_SystemReset
// so the OP_COMMISSION_REPLY frame and any other in-flight bytes actually
// reach the host before USB renegotiates.
static volatile uint32_t g_reboot_at_ms = 0;   // 0 = no reboot pending

void firmware_request_reboot(uint32_t delay_ms) {
    g_reboot_at_ms = board_millis() + delay_ms;
    if (g_reboot_at_ms == 0) g_reboot_at_ms = 1;   // 0 reserved as sentinel
}

extern const s_engine_rom_t register_dongle_v2_module_rom;

// Layer-2 WDT pet — strong override of the weak no-op in s_engine_node.c.
// Called from s_expr_node_tick once per chain pump cycle (every 250 ms in
// this build). See wdt-layer2-pet-from-s-engine memory.
void s_engine_chip_wdt_pet(void) { hal_wdt_pet(); }

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

// ----------------------------------------------------------------------------
// firmware_get_sysinfo — SAMD21G18A implementation. Called by CMD_SYSINFO
// shell handler. Linker symbols are provided by seeeduino_xiao.ld.
//
// Flash layout: app starts at 0x2000 (8 KB bootloader reserved). _etext is
// end of code+rodata. The .data initializer follows _etext in flash and is
// copied to _srelocate..._erelocate in RAM at startup.
// ----------------------------------------------------------------------------
extern char _etext[];        // end of text+rodata in flash
extern char _srelocate[];    // start of .data in RAM
extern char _erelocate[];    // end   of .data in RAM
extern char _sbss[];         // start of .bss in RAM
extern char _ebss[];         // end   of .bss in RAM
extern char _sstack[];       // start of stack region in RAM
extern char _estack[];       // top of stack

#define SAMD21G18A_FLASH_TOTAL_KB  256u
#define SAMD21G18A_RAM_TOTAL_KB    32u
#define SAMD21_APP_FLASH_ORIGIN    0x2000u

void firmware_get_sysinfo(firmware_sysinfo_t* out) {
    out->flash_total_kb   = SAMD21G18A_FLASH_TOTAL_KB;
    out->flash_text_b     = (uint32_t)((uintptr_t)_etext - SAMD21_APP_FLASH_ORIGIN);
    out->flash_data_b     = (uint32_t)((uintptr_t)_erelocate - (uintptr_t)_srelocate);
    out->ram_total_kb     = SAMD21G18A_RAM_TOTAL_KB;
    out->ram_bss_b        = (uint32_t)((uintptr_t)_ebss - (uintptr_t)_sbss);
    out->ram_stack_b      = (uint32_t)((uintptr_t)_estack - (uintptr_t)_sstack);
    out->bump_capacity_b  = (uint32_t)BUMP_BUFFER_SIZE;
    out->bump_peak_b      = (uint32_t)g_bump_peak;
    out->uptime_ms        = (uint32_t)board_millis();
    out->cpu_clock_hz     = SystemCoreClock;
}

// Skips the double-precision math in se_log* — board_millis() returns
// uint32 ms directly, no __aeabi_dmul/ddiv pulled in (~3 KB flash saved).
// We don't register a get_time (double seconds) callback at all — leaving
// it NULL lets --gc-sections drop the dmul/ddiv code entirely.
static uint32_t engine_get_time_ms(void* ctx) {
    (void)ctx;
    return board_millis();
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
            // OP_COMMISSION_SET carries a u32 new_instance_id. Stage it into
            // a single-producer/single-consumer global so the chain handler
            // can read it; libcomm's one-in-flight rule keeps this race-free.
            if (meta.cmd == OP_COMMISSION_SET && meta.payload_len >= 4) {
                g_pending_commission_instance_id =
                    (uint32_t)g_rx_payload[0] |
                    ((uint32_t)g_rx_payload[1] <<  8) |
                    ((uint32_t)g_rx_payload[2] << 16) |
                    ((uint32_t)g_rx_payload[3] << 24);
            }
            // OP_SHELL_EXEC carries a variable-length binary message that
            // outlives a single rx_drain pass. Queue it; only push the engine
            // event if the queue accepted it, otherwise the handler would
            // replay an older payload (was the request_id-cluster bug).
            if (meta.cmd == OP_SHELL_EXEC) {
                if (meta.payload_len <= COMM_PAYLOAD_MAX
                    && shell_pending_push(g_rx_payload, meta.payload_len)) {
                    s_expr_event_push(tree, SE_EVENT_TICK, meta.cmd, NULL);
                }
                // else: queue full or oversize — drop. Host's request times out.
            } else {
                // All non-shell opcodes: just push (event_data NULL).
                s_expr_event_push(tree, SE_EVENT_TICK, meta.cmd, NULL);
            }
        }
        // BAD_CRC / BAD_LEN / OVERFLOW: decoder auto-resets; nothing to do
        // here — a host-side retry will re-sync on the next leading END.
    }
}

// ----------------------------------------------------------------------------
// Entry
// ----------------------------------------------------------------------------

int main(void) {
    // Snapshot PM->RCAUSE BEFORE anything else can clobber it. Distinguishes
    // WDT bite (the layer-2 chain-stuck signal) from power-on / external
    // reset / NVIC_SystemReset.
    hal_capture_reset_cause();

    // Interlock framework boot decision. Reads .noinit persistence, applies
    // bootloop guard, marks slots POISONED if they've crashed too many times
    // in a row. Must run before board_init / engine setup so the framework
    // can also report attribution if the very next thing crashes.
    interlock_boot_decide();

    board_init();

    // Arm the WDT immediately after board_init so any subsequent hang in
    // tusb_init / engine bring-up triggers recovery within ~4 s. The pet
    // site lives in the s_engine chain pump (s_expr_node_tick).
    hal_wdt_init();

    // Bring up always-on peripherals (DAC, ADC, and future I2C/RS-485) once,
    // pre-engine. Pins they own are reserved from GPIO commands by
    // pin_is_reserved() in samd21_commands.c.
    samd21_peripherals_init();

    tusb_rhport_init_t const rhport_init = {
        .role  = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO,
    };
    tusb_init(BOARD_TUD_RHPORT, &rhport_init);

    frame_ring_init(&g_tx_ring, g_tx_ring_buf, TX_RING_SIZE);
    frame_decoder_init(&g_rx_decoder, FRAME_DIR_M2S);

    // L0: read commissioning blob from flash before engine starts.
    // Factory-fresh dongles read nothing; defaults to UNCOMMISSIONED.
    register_dongle_load_commissioning();

    s_expr_allocator_t alloc = {
        .malloc      = bump_malloc,
        .free        = bump_free,
        .ctx         = NULL,
        .get_time    = NULL,    // skip double-precision path; see comment below
        .get_time_ms = engine_get_time_ms,
    };

    s_expr_module_t module;
    s_expr_tree_instance_t* tree = NULL;
    uint8_t init_err = s_engine_init_rom(&module, &register_dongle_v2_module_rom, alloc);
    (void)init_err;
    if (init_err == S_EXPR_ERR_OK) {
        s_expr_module_set_debug(&module, debug_packet_fn);
        tree = s_expr_tree_create_by_hash(&module, REGISTER_DONGLE_V2_HASH, 0);

        // Emit captured reset cause as first dbg_log frame. Sits in TX ring
        // until USB CDC enumerates; host sees on first attach. Grep-able
        // literal "[BOOT] rstsr=0xNN" — only RCAUSE low byte is meaningful.
        char buf[24];
        uint8_t rc = hal_get_reset_cause();
        static const char hex[] = "0123456789abcdef";
        buf[0]='['; buf[1]='B'; buf[2]='O'; buf[3]='O'; buf[4]='T'; buf[5]=']';
        buf[6]=' '; buf[7]='r'; buf[8]='s'; buf[9]='t'; buf[10]='s'; buf[11]='r';
        buf[12]='='; buf[13]='0'; buf[14]='x';
        buf[15] = hex[(rc >> 4) & 0xF];
        buf[16] = hex[(rc >> 0) & 0xF];
        buf[17] = '\0';
        debug_packet_fn(NULL, buf);

        // Companion line summarising interlock persistence + crash record.
        // Always emitted, even on cold boot (slots EMPTY, crash zeroed).
        char il_buf[96];
        (void)interlock_format_boot_line(il_buf, sizeof(il_buf));
        debug_packet_fn(NULL, il_buf);
    }

    // Engine tick cadence: 250 ms (chain expects 4 ticks/sec).
    //
    // Cold-boot OP_REGISTER delivery: Phase 2f moved emission to a retry
    // loop in the BOOT state's chain. Re-fires every ~1 sec until
    // OP_REGISTER_ACK arrives, so host attach timing doesn't matter.
    uint32_t next_tick_ms = 250;

    // Host-reattach detection: poll tud_cdc_connected() (DTR line state).
    // On false->true edge after a prior true->false drop, push
    // EV_HOST_REATTACH to the engine event queue. The chain's
    // handle_internal_events user fn responds by writing dongle_state =
    // BOOT, which se_state_machine sees on its tick within the same drain.
    // Polling (not the tud_cdc_line_state_cb callback) keeps it
    // race-free single-producer for the event queue.
    bool prev_cdc_connected = false;
    bool saw_disconnect     = false;

    for (;;) {
        tud_task();

        // Host-reattach edge detection.
        if (tree != NULL) {
            bool now_connected = tud_cdc_connected();
            if (!now_connected && prev_cdc_connected) {
                saw_disconnect = true;
                // DIAG: log the drop event so we can see DTR transitions.
                if (module.debug_fn) module.debug_fn(NULL, "[CDC] DTR dropped");
            } else if (now_connected && !prev_cdc_connected) {
                if (saw_disconnect) {
                    saw_disconnect = false;
                    s_expr_event_push(tree, SE_EVENT_TICK, EV_HOST_REATTACH, NULL);
                    if (module.debug_fn) module.debug_fn(NULL, "[CDC] DTR up after drop -> EV_HOST_REATTACH");
                } else {
                    if (module.debug_fn) module.debug_fn(NULL, "[CDC] DTR up (first attach)");
                }
            }
            prev_cdc_connected = now_connected;
        }

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

        // Deferred reboot for L0 commissioning. Wait until the requested time
        // has passed AND the TX ring is empty so the OP_COMMISSION_REPLY frame
        // (and any logs) actually leave the dongle before USB renegotiates.
        if (g_reboot_at_ms != 0 && (int32_t)(board_millis() - g_reboot_at_ms) >= 0) {
            tud_cdc_write_flush();
            NVIC_SystemReset();
            // not reached
        }
    }
}
