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
#include "samd21_hal_pin.h"  // hal_pin_check_consistency
#include "samd21_interlocks.h"
#include "samd21_rs485.h"    // RS-485 passthrough RX poll + OP_RS485_FRAME_RX

// Implemented in user_functions.c.
extern void     register_dongle_load_commissioning(void);
extern uint32_t g_pending_commission_instance_id;
extern bool     shell_pending_push(const uint8_t* payload, uint8_t len);
extern uint8_t  register_dongle_rs485_addr(void);
extern void     register_dongle_chip_uid(uint8_t out[16]);

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

// 1024 B sized to hold (a) the largest possible SHELL_REPLY (128 B payload +
// 8 B framing + SLIP-worst-case ≈ 280 B) plus (b) ~700 B of backlogged DBG_LOG
// / heartbeat / OP_EVENT bytes that may be queued during a long shell-handler
// run (cmd_adc_capture at 60 samples × 5 ms holds the loop for ~300 ms during
// which nothing drains). 256 B previously was tight enough that adc_capture
// replies were silently dropped — see samd21_adc_capture_bug_2026-05-28.
#define TX_RING_SIZE 1024u
static uint8_t       g_tx_ring_buf[TX_RING_SIZE];
frame_ring_t         g_tx_ring;

// ----------------------------------------------------------------------------
// Stack hardening (slice-4): paint .stack at boot with STACK_PAINT_VALUE,
// reserve the lowest 4 bytes at _sstack as a canary, then check the canary
// + scan for high-water mark after every chain pump tick. Overflow triggers
// a deferred reset so the diagnostic log line drains via TX ring first.
//
// Linker symbols (placed by seeeduino_xiao_noinit.ld):
//   _sstack — lowest address of the .stack region
//   _estack — highest address; initial SP value loaded by the vector table
//
// .noinit now sits ABOVE .stack (after slice-4 reorder), so SP dropping
// below _sstack writes into .bss/heap instead of corrupting g_interlock_persist.
// ----------------------------------------------------------------------------
// _sstack/_estack declared as `char[]` higher up in this file (for sysinfo).
#define STACK_PAINT_VALUE   0xDEADBEEFu
#define STACK_CANARY_VALUE  0xCAFEBABEu

volatile uint16_t g_stack_hwm_bytes      = 0;     // peak observed depth (bytes)
volatile uint16_t g_stack_size_bytes     = 0;     // total stack region size
volatile uint8_t  g_stack_canary_tripped = 0;     // 1 = overflow detected

// Slice 6 — feeds the IL_VIRT_T_SINCE_M2S virtual input. Updated every time
// rx_drain decodes a complete host frame; reset to board_millis() at boot so
// virtuals don't trip on the time-since-epoch right after a cold start.
volatile uint32_t g_last_m2s_rx_ms       = 0;

static void stack_paint_and_canary(void) {
    // Paint from _sstack up to (current SP - safety). Don't write above SP —
    // that would clobber our own stack frame.
    uint32_t sp;
    __asm__ volatile ("mov %0, sp" : "=r"(sp));
    uint32_t* p   = (uint32_t*)(uintptr_t)_sstack;
    uint32_t* end = (uint32_t*)(sp - 64);     // 64 B safety from current SP
    while (p < end) *p++ = STACK_PAINT_VALUE;
    // Canary at the LOWEST word — first thing overwritten by stack overflow.
    *(volatile uint32_t*)(uintptr_t)_sstack = STACK_CANARY_VALUE;
    g_stack_size_bytes = (uint16_t)((uintptr_t)_estack - (uintptr_t)_sstack);
}

// ----------------------------------------------------------------------------
// system_self_check (slice 5, defensive amendment D).
//
// Called at end of init, immediately before the for(;;) main loop. Validates
// every invariant the main loop assumes; any failure routes through panic()
// so the next boot prints [BOOT_IL] ... pn=N ... with the discriminator.
//
// Six invariants per slice-5 prep memory Q6 ratification:
//   1. Stack canary intact at _sstack
//   2. interlock_persist magic == INTERLOCK_MAGIC
//   3. interlock_persist version == INTERLOCK_PERSIST_VERSION
//   4. interlock_persist self_size == sizeof(interlock_persist_t)
//   5. HAL pin claim table has no duplicate non-shared owners
//   6. Crash record self-consistency (if last_pc != 0 then last_crashed_slot
//      must be 0xFF (no-slot) OR < INTERLOCK_MAX_SLOTS)
// ----------------------------------------------------------------------------

extern interlock_persist_t g_interlock_persist;   // declared in samd21_interlocks.h

static void system_self_check(void) {
    // 1. Stack canary
    if (*(volatile uint32_t*)(uintptr_t)_sstack != STACK_CANARY_VALUE) {
        panic(PANIC_INIT_CANARY_BAD, 0);
    }
    // 2. Persist magic
    if (g_interlock_persist.magic != INTERLOCK_MAGIC) {
        panic(PANIC_PERSIST_MAGIC_BAD, g_interlock_persist.magic);
    }
    // 3. Persist version
    if (g_interlock_persist.version != INTERLOCK_PERSIST_VERSION) {
        panic(PANIC_PERSIST_VERSION_BAD, g_interlock_persist.version);
    }
    // 4. Persist self_size
    if (g_interlock_persist.self_size != (uint16_t)sizeof(interlock_persist_t)) {
        panic(PANIC_PERSIST_SIZE_BAD, g_interlock_persist.self_size);
    }
    // 5. HAL pin claim table
    if (!hal_pin_check_consistency()) {
        panic(PANIC_HAL_PIN_DUPLICATE, 0);
    }
    // 6. Crash record consistency — if there's a recorded crash (last_pc != 0
    //    or panic_code != 0), the slot field must be either NONE (0xFF) or a
    //    valid slot index.
    {
        const interlock_crash_record_t* cr = &g_interlock_persist.crash;
        bool has_crash = (cr->last_pc != 0) || (cr->panic_code != PANIC_NONE);
        bool slot_ok   = (cr->last_crashed_slot == INTERLOCK_CRASHED_SLOT_NONE)
                      || (cr->last_crashed_slot < INTERLOCK_MAX_SLOTS);
        if (has_crash && !slot_ok) {
            panic(PANIC_CRASH_RECORD_BAD, cr->last_crashed_slot);
        }
    }
}

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

static uint8_t g_poll_reply_seq = 0;

// OP_POLL is handled inline (not through the chain) so it works in any
// dongle state — useful for diagnostics during BOOT/L1_DONE. Reply is the
// 64 B il_status_buffer_t pre-built by interlock_tick_all().
static void handle_op_poll_inline(void) {
    const il_status_buffer_t* sb = interlock_get_status_buffer();
    frame_meta_t meta = {
        .addr        = 1,
        .cmd         = OP_POLL_REPLY,
        .seq         = g_poll_reply_seq++,
        .ack_seq     = 0,
        .ack_status  = 0,
        .payload_len = (uint8_t)IL_STATUS_BUFFER_SIZE,
    };
    (void)frame_encode_s2m(&meta, (const uint8_t*)sb, &g_tx_ring);
}

// ----------------------------------------------------------------------------
// Drain reassembled RS-485 frames and push each one to the host as
// OP_RS485_FRAME_RX [from_addr:u8][payload bytes]. Called every main-loop
// iteration alongside the CDC RX drain. Bounded per call (drain a few frames)
// so a flooded bus can't starve the rest of the loop; remaining frames are
// picked up next iteration.
// ----------------------------------------------------------------------------

static uint8_t g_rs485_rx_seq = 0;

static void rs485_drain_to_host(void) {
    uint8_t addr;
    uint8_t payload[RS485_PAYLOAD_MAX];
    uint8_t len;
    for (uint8_t guard = 0; guard < 4; guard++) {
        if (!rs485_poll_frame(&addr, payload, &len)) return;
        uint8_t frame[1 + RS485_PAYLOAD_MAX];
        frame[0] = addr;
        for (uint8_t i = 0; i < len; i++) frame[1 + i] = payload[i];
        frame_meta_t meta = {
            .addr        = 1,
            .cmd         = OP_RS485_FRAME_RX,
            .seq         = g_rs485_rx_seq++,
            .ack_seq     = 0,
            .ack_status  = 0,
            .payload_len = (uint8_t)(1u + len),
        };
        (void)frame_encode_s2m(&meta, frame, &g_tx_ring);
    }
}

#if defined(ROLE_SLAVE)
// ----------------------------------------------------------------------------
// RS-485 slave responder (Phase-1 day-1 scope). Frames are accepted only when
// addressed to our rs485_addr (set at boot via rs485_config). The one thing it
// does: answer a "PING" request frame with [chip_uid(16)][rs485_addr(1)],
// sourced from our own address. Runs in any engine state — the responder is
// independent of the USB-CDC sync ladder (USB is commissioning-only on a
// slave). Richer RS-485 opcodes layer on top later.
// ----------------------------------------------------------------------------
static void rs485_slave_poll(void) {
    uint8_t addr;
    uint8_t payload[RS485_PAYLOAD_MAX];
    uint8_t len;
    for (uint8_t guard = 0; guard < 4; guard++) {
        if (!rs485_poll_frame(&addr, payload, &len)) return;
        if (len == 4 && payload[0] == 'P' && payload[1] == 'I'
                     && payload[2] == 'N' && payload[3] == 'G') {
            uint8_t resp[17];
            register_dongle_chip_uid(resp);                 // resp[0..15]
            resp[16] = register_dongle_rs485_addr();
            rs485_send_frame(register_dongle_rs485_addr(), resp, sizeof(resp));
        }
        // Other payloads: ignored in day-1 scope.
    }
}
#endif

static void rx_drain_to_event_queue(s_expr_tree_instance_t* tree) {
    if (!tud_cdc_connected() || !tud_cdc_available()) return;
    uint32_t n = tud_cdc_read(g_rx_buf, sizeof(g_rx_buf));
    for (uint32_t i = 0; i < n; i++) {
        frame_meta_t meta;
        frame_decode_result_t r =
            frame_decoder_feed(&g_rx_decoder, g_rx_buf[i], &meta, g_rx_payload);
        if (r == FRAME_DECODE_FRAME_READY) {
            // Slice 6 — feed the IL_VIRT_T_SINCE_M2S virtual.
            g_last_m2s_rx_ms = board_millis();
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
            // OP_POLL: inline-handled (skips chain); replies with status buffer.
            if (meta.cmd == OP_POLL) {
                handle_op_poll_inline();
                continue;
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

    // Paint stack + install canary. Must run AFTER reset cause capture
    // (which uses minimal stack) and BEFORE any deeper call chain that
    // would consume more stack than our 64 B paint-safety margin allows.
    stack_paint_and_canary();

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

    // Re-parse + re-claim pins for any DSL-defined interlock slots that
    // survived the warm boot. Slots that fail re-parse / re-claim are
    // marked POISONED. Idempotent on cold boot (no ARMED slots).
    interlock_warm_restore();

    tusb_rhport_init_t const rhport_init = {
        .role  = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO,
    };
    tusb_init(BOARD_TUD_RHPORT, &rhport_init);

    frame_ring_init(&g_tx_ring, g_tx_ring_buf, TX_RING_SIZE);
    frame_decoder_init(&g_rx_decoder, FRAME_DIR_M2S);

    // Slice 6: seed the m2s timestamp at "now" so _t_since_m2s starts at 0,
    // not at uptime. Host hasn't sent anything yet but we don't want the
    // virtual to trip an arming-immediately watch before the first frame.
    g_last_m2s_rx_ms = board_millis();

    // L0: read commissioning blob from flash before engine starts.
    // Factory-fresh dongles read nothing; defaults to UNCOMMISSIONED.
    register_dongle_load_commissioning();

#if defined(ROLE_SLAVE)
    // Slave listens only for frames addressed to its rs485_addr (= low byte of
    // instance_id). Must run after commissioning is loaded. 0 if uncommissioned.
    rs485_config(0, register_dongle_rs485_addr(), 0);
#endif

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

    // Defensive amendment D — gate entry to the main loop on invariants.
    // Any failure panics with a discriminated code; next boot prints it.
    system_self_check();

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

        // RS-485 service. On a slave build, answer addressed PING frames; on
        // dongle/bus_controller, push received frames up to the host.
#if defined(ROLE_SLAVE)
        rs485_slave_poll();
#else
        rs485_drain_to_host();
#endif

        uint32_t now = board_millis();
        if (tree != NULL && (int32_t)(now - next_tick_ms) >= 0) {
            next_tick_ms += 250;
            tick_and_drain(tree);
            // After the chain pump, run the interlock tick. Sequential with
            // the chain (single-core M0+) means no concurrency to worry
            // about; the chain's just-drained event queue is now empty so a
            // freshly-arrived CMD_INTERLOCK_SET won't race with the tick.
            interlock_tick_all();

            // Amendment A — pre-overflow SP check. Catches a near-overflow
            // BEFORE any deeper call chain can write below _sstack. 256 B
            // margin sized to cover the deepest IRQ chain (~500 B for
            // TinyUSB CDC) with half the margin reserved for IRQ overhead
            // between this check and the overflow point.
            {
                uint32_t sp;
                __asm__ volatile ("mov %0, sp" : "=r"(sp));
                if (sp < ((uintptr_t)_sstack + 256u)) {
                    panic(PANIC_STACK_NEAR_OVERFLOW, sp);
                }
            }

            // Stack hardening: verify canary + update HWM after every
            // chain pump cycle. Canary trip → panic (immediate reset) so
            // the post-mortem code surfaces in the next boot's [BOOT_IL]
            // line via panic_code = INIT_CANARY_BAD or equivalent.
            if (*(volatile uint32_t*)(uintptr_t)_sstack != STACK_CANARY_VALUE) {
                g_stack_canary_tripped = 1;
                if (module.debug_fn) {
                    module.debug_fn(NULL, "[STACK_CANARY_TRIPPED] panicking");
                }
                panic(PANIC_INIT_CANARY_BAD, 0);
            } else {
                uint32_t* p   = (uint32_t*)((uintptr_t)_sstack + 4);
                uint32_t* end = (uint32_t*)(uintptr_t)_estack;
                while (p < end && *p == STACK_PAINT_VALUE) p++;
                uint16_t used = (uint16_t)((uintptr_t)end - (uintptr_t)p);
                if (used > g_stack_hwm_bytes) g_stack_hwm_bytes = used;
            }
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
