// ============================================================================
// register_dongle — Seeed XIAO RA4M1 (Renesas R7FA4M1AB, Cortex-M4)
// Step 3b — port of the SAMD21 register_dongle:
//   * s_engine M-port            (proven on RA4M1 by blink_engine)
//   * libcomm SLIP+CRC framing   (proven on RA4M1 by blink_frame)
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
//
// RA4M1-specific vs the SAMD21 reference: firmware_get_sysinfo (linker symbols
// + totals), the 1200-baud-touch DFU handler, and the chip header. The RX/TX
// path, the engine tick/drain, and host-reattach detection are chip-agnostic.
// ============================================================================

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"
#include "bsp_api.h"    // FSP/CMSIS: NVIC_SystemReset, SystemCoreClock

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
#include "mode.h"            // multi-mode foundation (VTOR reloc + mode table)
#include "ra4m1_hal.h"       // HIL peripheral drivers

// Implemented in user_functions.c.
extern void     register_dongle_load_commissioning(void);
extern uint32_t g_pending_commission_instance_id;
extern bool     shell_pending_push(const uint8_t* payload, uint8_t len);
extern void     workbench_analog_poll(void);   // ra4m1_commands.c — ADC sampler
extern void     spectral_pump(void);            // spectral.c — mode-2 FFT pump
extern void     goertzel_pump(void);            // goertzel.c — mode-4 block finalize

// ----------------------------------------------------------------------------
// Deferred-reboot plumbing. Two flavors:
//   * firmware_request_reboot()     — plain reset; the app relaunches. Used by
//     the L0 commissioning handlers (OP_COMMISSION_SET / CLEAR).
//   * firmware_request_dfu_reboot() — reset INTO the Seeed DFU bootloader so
//     `dfu-util` can reflash. Triggered by the 1200-baud touch (see
//     tud_cdc_line_coding_cb).
// Either way the main loop waits for TX to drain (~200 ms) before resetting,
// so the OP_COMMISSION_REPLY frame and any in-flight bytes reach the host.
//
// DFU entry: a plain NVIC_SystemReset() just relaunches the app — the Seeed
// bootloader needs a "stay in DFU" magic written to a known RAM location it
// checks after reset.
//
// TODO-verify (Pi): the magic value + RAM address the XIAO RA4M1 Seeed/Arduino
// bootloader checks. The values below are PLACEHOLDERS (the address is the top
// of the 32 KB SRAM; the value is the SAMD21 Arduino magic). Read the real
// values off the Seeed XIAO RA4M1 bootloader source, or confirm empirically.
// Until corrected, the 1200-baud touch resets but relaunches the app — flash
// via the BOOT-button + raflash route in the meantime. See README.md.
// ----------------------------------------------------------------------------
#define DFU_DOUBLE_TAP_ADDR   0x20007FF8U   // PLACEHOLDER — top of RA4M1 SRAM
#define DFU_DOUBLE_TAP_MAGIC  0x07738135U   // PLACEHOLDER — SAMD21 Arduino value

static volatile uint32_t g_reboot_at_ms  = 0;     // 0 = no reboot pending
static volatile bool     g_reboot_to_dfu = false;

void firmware_request_reboot(uint32_t delay_ms) {
    g_reboot_to_dfu = false;
    g_reboot_at_ms  = board_millis() + delay_ms;
    if (g_reboot_at_ms == 0) g_reboot_at_ms = 1;   // 0 reserved as sentinel
}

void firmware_request_dfu_reboot(void) {
    g_reboot_to_dfu = true;
    g_reboot_at_ms  = board_millis() + 50u;        // brief settle before reset
    if (g_reboot_at_ms == 0) g_reboot_at_ms = 1;
}

// ----------------------------------------------------------------------------
// tud_cdc_line_coding_cb — TinyUSB calls this whenever the host sets the CDC
// line coding. The 1200-baud "touch" is the Arduino convention for "reset
// into the bootloader to be reflashed": `stty -F /dev/ttyACM0 1200`. No normal
// host tool opens the port at 1200 bps, so it is a safe sentinel.
// ----------------------------------------------------------------------------
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const* coding) {
    (void)itf;
    if (coding != NULL && coding->bit_rate == 1200) {
        firmware_request_dfu_reboot();
    }
}

extern const s_engine_rom_t register_dongle_v2_module_rom;

// ----------------------------------------------------------------------------
// Bump allocator.
// Linux v2 chain peak: 520 B with one tree active. Cortex-M4 alignment is
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
// firmware_get_sysinfo — RA4M1 (R7FA4M1AB) implementation. Called by the
// CMD_SYSINFO shell handler. Linker symbols come from the FSP GCC linker
// script; the application is linked at flash 0x4000 (the low 16 KB is the
// Seeed DFU bootloader — see the RA4M1 bring-up memory).
//
// TODO-verify (Pi): the FSP linker-script symbol names below. The names used
// are the CMSIS/GCC-standard ones FSP's fsp.ld is expected to export; confirm
// against the vendored linker script and adjust if they differ. See README.md.
// ----------------------------------------------------------------------------
extern char __etext[];          // end of text+rodata in flash (.data init src)
extern char __data_start__[];   // start of .data in RAM
extern char __data_end__[];     // end   of .data in RAM
extern char __bss_start__[];    // start of .bss in RAM
extern char __bss_end__[];      // end   of .bss in RAM
extern char __StackTop[];       // top of stack
extern char __StackLimit[];     // bottom of stack region

#define RA4M1_FLASH_TOTAL_KB   256u
#define RA4M1_RAM_TOTAL_KB     32u
#define RA4M1_APP_FLASH_ORIGIN 0x4000u

void firmware_get_sysinfo(firmware_sysinfo_t* out) {
    out->flash_total_kb   = RA4M1_FLASH_TOTAL_KB;
    out->flash_text_b     = (uint32_t)((uintptr_t)__etext - RA4M1_APP_FLASH_ORIGIN);
    out->flash_data_b     = (uint32_t)((uintptr_t)__data_end__ - (uintptr_t)__data_start__);
    out->ram_total_kb     = RA4M1_RAM_TOTAL_KB;
    out->ram_bss_b        = (uint32_t)((uintptr_t)__bss_end__ - (uintptr_t)__bss_start__);
    out->ram_stack_b      = (uint32_t)((uintptr_t)__StackTop - (uintptr_t)__StackLimit);
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
    board_init();

    tusb_rhport_init_t const rhport_init = {
        .role  = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO,
    };
    tusb_init(BOARD_TUD_RHPORT, &rhport_init);

    // Multi-mode foundation: relocate the vector table to RAM (so mode
    // dispatcher handlers can be installed), prepare the mode periodic timer,
    // and enter MODE_WORKBENCH. After tusb_init() so the live USB vector
    // entries are captured into the relocated table.
    mode_init();

    frame_ring_init(&g_tx_ring, g_tx_ring_buf, TX_RING_SIZE);
    frame_decoder_init(&g_rx_decoder, FRAME_DIR_M2S);

    // L0: read commissioning blob from data flash before engine starts.
    // Factory-fresh dongles read nothing; defaults to UNCOMMISSIONED.
    register_dongle_load_commissioning();

    s_expr_allocator_t alloc = {
        .malloc      = bump_malloc,
        .free        = bump_free,
        .ctx         = NULL,
        .get_time    = NULL,    // skip double-precision path; see comment above
        .get_time_ms = engine_get_time_ms,
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

        // Workbench analog collection — self-gated ~1 kHz ADC sampler. No-op
        // unless ANALOG_START is active; runs in this main-loop context so it
        // never delays the DAC-waveform / PWM-dither ISRs.
        workbench_analog_poll();

        // Mode-2 spectral pump — picks up filled ADC capture buffers, runs
        // window + rfft + |X[k]|² accumulation. No-op unless SPECTRAL is the
        // active mode AND a frame has finished capturing. Foreground context
        // so the FFT does not preempt the sample-tick ISR.
        spectral_pump();
        goertzel_pump();

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

        // Deferred reboot. Wait until the requested time has passed AND the
        // TX ring is empty so the OP_COMMISSION_REPLY frame (and any logs)
        // actually leave the dongle before USB renegotiates.
        if (g_reboot_at_ms != 0 && (int32_t)(board_millis() - g_reboot_at_ms) >= 0) {
            tud_cdc_write_flush();
            if (g_reboot_to_dfu) {
                // Signal the Seeed bootloader to stay in DFU after this reset.
                // PLACEHOLDER constants — see the TODO-verify note at the top.
                *(volatile uint32_t*)DFU_DOUBLE_TAP_ADDR = DFU_DOUBLE_TAP_MAGIC;
            }
            NVIC_SystemReset();
            // not reached
        }
    }
}
