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
#if defined(ROLE_BUS_CONTROLLER)
#include "bus_roster.h"      // BC slave roster + poll config (Stage 2/3)
#endif

// Implemented in user_functions.c.
extern void     register_dongle_load_commissioning(void);
extern uint32_t g_pending_commission_instance_id;
extern bool     shell_pending_push(const uint8_t* payload, uint8_t len);
extern uint8_t  register_dongle_rs485_addr(void);
extern void     register_dongle_chip_uid(uint8_t out[16]);
extern uint16_t shell_dispatch_payload(const uint8_t* exec, uint16_t exec_len, uint8_t* reply);

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
    rs485_frame_t f;
    for (uint8_t guard = 0; guard < 4; guard++) {
        if (!rs485_recv(&f)) return;
        // [from_addr:u8][type:u8][seq:u8][payload bytes] — sniffer diagnostics.
        uint8_t frame[3 + RS485_PAYLOAD_MAX];
        frame[0] = f.src;
        frame[1] = f.type;
        frame[2] = f.seq;
        for (uint8_t i = 0; i < f.len; i++) frame[3 + i] = f.payload[i];
        frame_meta_t meta = {
            .addr        = 1,
            .cmd         = OP_RS485_FRAME_RX,
            .seq         = g_rs485_rx_seq++,
            .ack_seq     = 0,
            .ack_status  = 0,
            .payload_len = (uint8_t)(3u + f.len),
        };
        (void)frame_encode_s2m(&meta, frame, &g_tx_ring);
    }
}

#if defined(ROLE_BUS_CONTROLLER)
// ----------------------------------------------------------------------------
// Bus-controller bridge (BC-1b). The bus_controller is its own libcomm node at
// BUS_CONTROLLER_LOCAL_ADDR; frames from the Pi carrying any OTHER addr are
// destined for that RS-485 slave. We forward the frame payload (an
// OP_SHELL_EXEC body) over the bus, then non-blockingly wait for the slave's
// reply and relay it back to the Pi as an OP_SHELL_REPLY tagged with the slave
// addr. One transaction in flight (matches libcomm's one-in-flight rule). The
// bounded deadline IS dead-slave detection — on timeout we NAK the Pi. RX
// stays in sniffer mode (rs485_init default my_addr=0xFF) so any slave's reply
// is accepted.
// ----------------------------------------------------------------------------
// The host (dongle_console) sends m2s frames with addr=0x00, so 0x00 = "for the
// bus_controller itself" (sync ladder, its own shell). Any other addr = route to
// that RS-485 slave. Slaves therefore must not use rs485_addr 0 (which is also
// the RS-485 broadcast address — consistent).
#define BUS_CONTROLLER_LOCAL_ADDR 0u
#define BRIDGE_TIMEOUT_MS         1500u   // covers slow HIL (adc_capture ~300ms); < WDT 4s

static bool          g_bridge_pending    = false;
static uint8_t       g_bridge_slave      = 0;
static uint32_t      g_bridge_deadline   = 0;
static uint8_t       g_bridge_seq        = 0;   // s2m seq toward the Pi
static uint8_t       g_bridge_req_seq    = 0;   // RS-485 seq toward the slave
static rs485_frame_t g_bridge_reply;            // buffered slave reply awaiting USB
static bool          g_bridge_have_reply = false;

// Called from rx_drain when a Pi frame addressed to a slave arrives. `exec` is
// the OP_SHELL_EXEC body; we wrap it as a DATA frame payload [opcode][body] and
// send it to the slave. UDP DATA for now — the reply is the response; the TCP
// ack-table + retransmit lands with the BC-3 poll engine.
static void bridge_start(uint8_t slave, const uint8_t* exec, uint8_t exec_len) {
    // Mutually exclusive with autonomous polling (Stage 3a): while polling owns
    // the bus, refuse the reactive --target bridge. 3c queues it onto a poll slot.
    if (bus_poll_cfg()->enabled) return;
    if (g_bridge_pending) return;          // one-in-flight; drop (host will retry)
    if (exec_len > (uint8_t)(RS485_PAYLOAD_MAX - 2u)) exec_len = RS485_PAYLOAD_MAX - 2u;
    uint8_t buf[RS485_PAYLOAD_MAX];
    buf[0] = (uint8_t)(OP_SHELL_EXEC & 0xFFu);
    buf[1] = (uint8_t)(OP_SHELL_EXEC >> 8);
    for (uint8_t i = 0; i < exec_len; i++) buf[2u + i] = exec[i];
    rs485_send(slave, BUS_CONTROLLER_LOCAL_ADDR, RS485_FT_DATA, ++g_bridge_req_seq,
               buf, (uint8_t)(2u + exec_len));
    g_bridge_slave    = slave;
    g_bridge_deadline = board_millis() + BRIDGE_TIMEOUT_MS;
    g_bridge_pending  = true;
}

// Called every main-loop iteration. Relays the slave reply, or NAKs on timeout.
static void rs485_bridge_poll(void) {
    if (!g_bridge_pending) return;

    // A reply is received but its USB frame didn't fit the TX ring yet — retry
    // until the ring drains. frame_encode_s2m rolls back fully on a full ring,
    // so a large relay frame is otherwise silently dropped (small ones fit, the
    // ~70 B interlock_status reply doesn't when chain logs partly fill the ring).
    if (g_bridge_have_reply) {
        uint16_t opcode = (uint16_t)g_bridge_reply.payload[0]
                        | ((uint16_t)g_bridge_reply.payload[1] << 8);
        frame_meta_t meta = {
            .addr        = g_bridge_slave,
            .cmd         = opcode,
            .seq         = g_bridge_seq,
            .ack_seq     = 0,
            .ack_status  = 0,
            .payload_len = (uint8_t)(g_bridge_reply.len - 2u),
        };
        if (frame_encode_s2m(&meta, &g_bridge_reply.payload[2], &g_tx_ring) == 0) {
            g_bridge_seq++;
            g_bridge_have_reply = false;
            g_bridge_pending    = false;
        } else if ((int32_t)(board_millis() - g_bridge_deadline) >= 0) {
            g_bridge_have_reply = false;   // ring stuck full past deadline — drop
            g_bridge_pending    = false;
        }
        return;
    }

    rs485_frame_t f;
    if (rs485_recv(&f)
        && (f.type & RS485_FT_MASK) == RS485_FT_DATA
        && f.src == g_bridge_slave
        && f.len >= 2u) {
        // DATA payload = [opcode:u16-LE][body]; buffer it; the block above relays
        // it to the Pi, retrying while the USB ring is momentarily full.
        g_bridge_reply      = f;
        g_bridge_have_reply = true;
    } else if ((int32_t)(board_millis() - g_bridge_deadline) >= 0) {
        // Dead/slow slave — relay OP_NAK { reason, rejected_cmd } to the Pi.
        uint8_t nak[3] = {
            (uint8_t)NAK_ERR_NO_RESOURCES,
            (uint8_t)(OP_SHELL_EXEC & 0xFFu),
            (uint8_t)(OP_SHELL_EXEC >> 8),
        };
        frame_meta_t meta = {
            .addr        = g_bridge_slave,
            .cmd         = OP_NAK,
            .seq         = g_bridge_seq++,
            .ack_seq     = 0,
            .ack_status  = 0,
            .payload_len = sizeof(nak),
        };
        (void)frame_encode_s2m(&meta, nak, &g_tx_ring);
        g_bridge_pending = false;
    }
}

// ----------------------------------------------------------------------------
// Bus-controller autonomous poll engine (BC-3 / Stage 3a-i). When the Pi has
// enabled polling (CMD_BUS_POLL_ENABLE), the BC round-robins the ENABLED roster
// at the configured cadence: send POLL(UDP) to one slave, open a bounded window
// for its reply, and update liveness. A slave that misses `max_misses` polls in
// a row is marked DEAD and OP_BUS_SLAVE_DOWN pushed to the Pi; its first good
// frame after that marks it ALIVE and pushes OP_BUS_SLAVE_UP.
//
// 3a-i: the slave's window is just NO_MESSAGE (any valid frame from it = alive).
// Slave->Pi DATA forwarding (events) is 3b; TCP ack-table + queued Pi->slave
// commands are 3c. While polling is enabled the --target bridge is refused
// (bridge_start no-ops) — the two own the bus mutually exclusively until 3c.
//
// Non-blocking state machine: one POLL per (re)entry, every wait deadline-
// guarded, so the loop can never wedge the engine. The bus-progress WDT that
// would catch a *logic* wedge (engine ticks but no bus bytes) is 3a-ii.
// ----------------------------------------------------------------------------
#define POLL_SLOT_TIMEOUT_MS 20u   // per-slave reply window (> a slave's worst RX->TX turnaround)

typedef enum { POLL_IDLE = 0, POLL_WAIT = 1 } poll_state_t;
static poll_state_t g_poll_state    = POLL_IDLE;
static uint8_t      g_poll_cursor   = 0;   // round-robin slot cursor (opaque)
static uint8_t      g_poll_cur_addr = 0;   // addr currently polled
static uint8_t      g_poll_seq      = 0;   // POLL frame seq
static uint32_t     g_poll_next_ms  = 0;   // next slot start (cadence gate)
static uint32_t     g_poll_deadline = 0;   // current window deadline

// Slave answered: clear misses, stamp last_seen, set ALIVE. (Event push is
// handled by bus_reconcile_events so a momentarily-full TX ring can't drop it.)
static void bus_mark_alive(uint8_t addr, uint32_t now) {
    bus_slave_t* s = bus_roster_find(addr);
    if (s == NULL) return;
    s->consecutive_misses = 0;
    s->last_seen_ms       = now;
    s->state              = BUS_STATE_ALIVE;
}

// Slave missed its window: bump misses; cross max_misses once -> DEAD.
static void bus_mark_miss(uint8_t addr, uint8_t max_misses) {
    bus_slave_t* s = bus_roster_find(addr);
    if (s == NULL) return;
    if (s->consecutive_misses < 0xFFu) s->consecutive_misses++;
    if (s->state != BUS_STATE_DEAD && s->consecutive_misses >= max_misses) {
        s->state = BUS_STATE_DEAD;
    }
}

// Reconcile the WHOLE roster's announced state with real state, emitting one
// pending edge event (DOWN / UP) per call. Called every main-loop iteration
// (not from the poll engine) so a momentarily-full TX ring just defers the emit
// to the next loop — the same drain discipline as the bridge reply. The roster's
// (state != announced_state) IS the pending-event flag: an event can never be
// lost, only delayed until the ring drains. One emit per call keeps each loop
// cheap; multiple pending events drain over successive loops (sub-ms apart).
// UNKNOWN->ALIVE is silent (only a recovery from DEAD pushes UP, per the spec).
static void bus_drain_events(void) {
    uint8_t n = bus_roster_count();
    for (uint8_t i = 0; i < n; i++) {
        const bus_slave_t* cs = bus_roster_at(i);
        if (cs == NULL || cs->state == cs->announced_state) continue;
        bus_slave_t* s = bus_roster_find(cs->addr);   // mutable handle
        if (s == NULL) continue;

        if (s->state == BUS_STATE_DEAD) {
            uint8_t body[1] = { s->addr };
            frame_meta_t meta = { .addr = s->addr, .cmd = OP_BUS_SLAVE_DOWN, .seq = g_bridge_seq,
                                  .ack_seq = 0, .ack_status = 0, .payload_len = sizeof(body) };
            if (frame_encode_s2m(&meta, body, &g_tx_ring) == 0) {
                g_bridge_seq++;
                s->announced_state = BUS_STATE_DEAD;
            }
            return;   // one emit per call; ring may be full — retry next loop
        } else if (s->state == BUS_STATE_ALIVE) {
            if (s->announced_state == BUS_STATE_DEAD) {
                uint8_t body[5];
                body[0] = s->addr;
                body[1] = (uint8_t)(s->class_id      );
                body[2] = (uint8_t)(s->class_id >>  8);
                body[3] = (uint8_t)(s->class_id >> 16);
                body[4] = (uint8_t)(s->class_id >> 24);
                frame_meta_t meta = { .addr = s->addr, .cmd = OP_BUS_SLAVE_UP, .seq = g_bridge_seq,
                                      .ack_seq = 0, .ack_status = 0, .payload_len = sizeof(body) };
                if (frame_encode_s2m(&meta, body, &g_tx_ring) == 0) {
                    g_bridge_seq++;
                    s->announced_state = BUS_STATE_ALIVE;
                }
                return;   // one emit per call
            } else {
                s->announced_state = BUS_STATE_ALIVE;   // UNKNOWN->ALIVE: no event, no frame
            }
        }
    }
}

static void bus_poll_engine(void) {
    bus_poll_cfg_t* cfg = bus_poll_cfg();
    if (!cfg->enabled) { g_poll_state = POLL_IDLE; return; }
    uint32_t now = board_millis();

    if (g_poll_state == POLL_IDLE) {
        if ((int32_t)(now - g_poll_next_ms) < 0) return;     // hold cadence
        const bus_slave_t* s = bus_roster_next_enabled(&g_poll_cursor);
        if (s == NULL) { g_poll_next_ms = now + cfg->poll_period_ms; return; }
        g_poll_cur_addr = s->addr;
        rs485_rx_flush();                                    // drop stale before listening
        rs485_send(s->addr, BUS_CONTROLLER_LOCAL_ADDR, RS485_FT_POLL, ++g_poll_seq, NULL, 0);
        g_poll_deadline = now + POLL_SLOT_TIMEOUT_MS;
        g_poll_state    = POLL_WAIT;
        return;
    }

    // POLL_WAIT: any valid frame from the polled slave = alive; else timeout = miss.
    // Reconcile after resolving so the DOWN/UP edge event is (re)tried; a drop on
    // a full ring is retried on this slave's next round-robin visit.
    // POLL_WAIT only updates roster state; bus_drain_events() (main loop) emits
    // the DOWN/UP edge so a full ring defers rather than drops the event.
    rs485_frame_t f;
    if (rs485_recv(&f) && f.src == g_poll_cur_addr) {
        bus_mark_alive(g_poll_cur_addr, now);
        g_poll_state   = POLL_IDLE;
        g_poll_next_ms = now + cfg->poll_period_ms;
    } else if ((int32_t)(now - g_poll_deadline) >= 0) {
        bus_mark_miss(g_poll_cur_addr, cfg->max_misses);
        g_poll_state   = POLL_IDLE;
        g_poll_next_ms = now + cfg->poll_period_ms;
    }
}
#endif  // ROLE_BUS_CONTROLLER

#if defined(ROLE_SLAVE)
// ----------------------------------------------------------------------------
// RS-485 slave shell tunnel (BC-1a, synchronous). Frames are accepted only when
// addressed to our rs485_addr (set at boot via rs485_config). The payload of an
// inbound frame IS an OP_SHELL_EXEC body [request_id u16][command_id u16][args];
// we dispatch it through the SAME shell layer used over USB and send the
// OP_SHELL_REPLY body back over RS-485, sourced from our own address. This makes
// the full workbench/HIL command suite runnable on the slave node over the bus
// with no new command code. Synchronous + main-loop (USB is commissioning-only
// on a slave); the master waits for the reply. Replies > RS485_PAYLOAD_MAX-2 are
// clamped (fragmentation is a later BC slice). A DATA frame's payload is
// [opcode:u16-LE][body]; we handle OP_SHELL_EXEC and reply OP_SHELL_REPLY,
// echoing the request seq for forward-compat with the future ack-table.
// ----------------------------------------------------------------------------
#define RS485_SHELL_EXEC_HEADER_LEN 4u   // request_id u16 + command_id u16

static void rs485_slave_poll(void) {
    rs485_frame_t f;
    for (uint8_t guard = 0; guard < 4; guard++) {
        if (!rs485_recv(&f)) return;
        // POLL (BC-3 / Stage 3a): "your window is open." We have no outbound queue
        // yet (events = 3b), so terminate the window immediately with NO_MESSAGE.
        // Sourced from our own addr so the BC's liveness check (f.src == polled)
        // passes. NO_MESSAGE carries no payload.
        if ((f.type & RS485_FT_MASK) == RS485_FT_POLL) {
            rs485_send(RS485_ADDR_MASTER, register_dongle_rs485_addr(),
                       RS485_FT_NO_MESSAGE, f.seq, NULL, 0);
            continue;
        }
        if ((f.type & RS485_FT_MASK) != RS485_FT_DATA) continue;  // ACK/NAK: later
        if (f.len < 2u) continue;
        uint16_t opcode = (uint16_t)f.payload[0] | ((uint16_t)f.payload[1] << 8);
        if (opcode != OP_SHELL_EXEC) continue;          // only shell-exec this step
        const uint8_t* exec     = &f.payload[2];
        uint16_t       exec_len = (uint16_t)(f.len - 2u);
        if (exec_len < RS485_SHELL_EXEC_HEADER_LEN) continue;

        uint8_t  reply_body[COMM_PAYLOAD_MAX];
        uint16_t reply_len = shell_dispatch_payload(exec, exec_len, reply_body);
        if (reply_len > (uint16_t)(RS485_PAYLOAD_MAX - 2u)) {
            reply_len = RS485_PAYLOAD_MAX - 2u;         // clamp; frag is later
        }
        uint8_t out[RS485_PAYLOAD_MAX];
        out[0] = (uint8_t)(OP_SHELL_REPLY & 0xFFu);
        out[1] = (uint8_t)(OP_SHELL_REPLY >> 8);
        for (uint16_t i = 0; i < reply_len; i++) out[2u + i] = reply_body[i];
        rs485_send(RS485_ADDR_MASTER, register_dongle_rs485_addr(),
                   RS485_FT_DATA, f.seq, out, (uint8_t)(2u + reply_len));
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
#if defined(ROLE_BUS_CONTROLLER)
            // Address routing: a frame for any addr other than ours is destined
            // for an RS-485 slave — bridge it instead of dispatching locally.
            if (meta.addr != BUS_CONTROLLER_LOCAL_ADDR) {
                bridge_start(meta.addr, g_rx_payload, meta.payload_len);
                continue;
            }
#endif
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

    // RS-485 transport config. flags=0 (no self-echo discard) is correct on BOTH
    // bare-TTL cross-wire AND a shared transceiver bus: address/src filtering
    // already rejects a node's own echo, and the ISR + 256-word ring absorb it.
    // RS485_FLAG_SHARED_BUS is an optional cleanliness optimisation for the
    // shared bus; enable it at runtime via CMD_RS485_CONFIG once transceivers are
    // in. It MUST stay off for a D6->D7 loopback self-test (echo IS the signal).
#if defined(ROLE_SLAVE)
    // Slave listens only for frames addressed to its rs485_addr (= low byte of
    // instance_id). Must run after commissioning is loaded. 0 if uncommissioned.
    rs485_config(0, register_dongle_rs485_addr(), 0);
#elif defined(ROLE_BUS_CONTROLLER)
    // Master stays a sniffer (accept every slave reply).
    rs485_config(0, RS485_ADDR_SNIFFER, 0);
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

    // Engine tick cadence: 50 ms (20 ticks/sec) for snappy command latency — a
    // shell-exec event waits at most one tick before the chain dispatches it.
    // The chain's only tick-counted timers (heartbeat, BOOT REGISTER retry) are
    // rate-limited to wall-clock in their handlers, so they don't scale with the
    // faster tick. Interlock eval is decoupled onto its own 1 ms gate below.
    uint32_t next_tick_ms = 25;
    uint32_t next_il_ms   = 1;

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

        // RS-485 service, by role: slave answers tunneled shell-exec frames;
        // bus_controller relays the in-flight bridge reply; dongle sniffs frames
        // up to the host (passthrough diagnostics).
#if defined(ROLE_SLAVE)
        rs485_slave_poll();
#elif defined(ROLE_BUS_CONTROLLER)
        // Polling enabled -> autonomous master owns the bus; else the reactive
        // --target bridge stays available for bench HIL on a single slave.
        if (bus_poll_cfg()->enabled) {
            bus_poll_engine();
            bus_drain_events();   // emit pending DOWN/UP; full ring defers, never drops
        } else {
            rs485_bridge_poll();
        }
#else
        rs485_drain_to_host();
#endif

        uint32_t now = board_millis();

        // Interlock eval on a fast ~1 kHz cadence, decoupled from the slow chain
        // tick — a bounded safety response instead of the chain's command
        // cadence. Single-core: this runs sequentially with the chain pump, so a
        // CMD_INTERLOCK_SET dispatched on a chain tick can't race the eval (no
        // partial-config read; the SET completes fully before the next eval).
        if (tree != NULL && (int32_t)(now - next_il_ms) >= 0) {
            next_il_ms += 1;
            interlock_tick_all();
        }

        if (tree != NULL && (int32_t)(now - next_tick_ms) >= 0) {
            next_tick_ms += 25;
            tick_and_drain(tree);

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

        // Drain the TX ring to CDC every loop, but ONLY as many bytes as the CDC
        // FIFO will accept this instant. Draining a fixed 64 B and ignoring
        // tud_cdc_write()'s return silently drops the overflow (the bytes are
        // already gone from g_tx_ring) — which shreds a large frame that spans a
        // partly-full FIFO while small frames fit. Pulling exactly write_available
        // bytes keeps every frame intact; the rest stays queued for next loop.
        if (tud_cdc_connected()) {
            uint32_t avail = tud_cdc_write_available();
            if (avail > 0) {
                uint8_t buf[64];
                if (avail > sizeof(buf)) avail = sizeof(buf);
                uint32_t n = frame_ring_read_drain(&g_tx_ring, buf, avail);
                if (n > 0) {
                    tud_cdc_write(buf, n);   // n <= avail, fully accepted
                    tud_cdc_write_flush();
                }
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
