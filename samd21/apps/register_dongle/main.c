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
// NOTE (6b): the reactive bridge waits for a synchronous DATA reply in the same
// window — incompatible with the 6b async slave (which ACKs then replies on a
// later poll). It is superseded by the always-on sweep (bus_cmd_enqueue path) and
// kept only for the poll-disabled fallback; opcode is passed through so the wire
// frame is well-formed.
static void bridge_start(uint16_t opcode, uint8_t slave, const uint8_t* exec, uint8_t exec_len) {
    // Mutually exclusive with autonomous polling (Stage 3a): while polling owns
    // the bus, refuse the reactive --target bridge. 3c queues it onto a poll slot.
    if (bus_poll_cfg()->enabled) return;
    if (g_bridge_pending) return;          // one-in-flight; drop (host will retry)
    if (exec_len > (uint8_t)(RS485_PAYLOAD_MAX - 2u)) exec_len = RS485_PAYLOAD_MAX - 2u;
    uint8_t buf[RS485_PAYLOAD_MAX];
    buf[0] = (uint8_t)(opcode & 0xFFu);
    buf[1] = (uint8_t)(opcode >> 8);
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

// --- Stage 3c / 6b: one command injected into the sweep (single in flight) --
// While polling owns the bus, a Pi frame addressed to a slave is queued here and
// sent on the next slot as DATA (instead of a routine POLL).
//
// 6b — ACK-frees-the-bus: the command slot now waits only for the slave's ISR
// ACK/NAK (µs), NOT the execution reply. On ACK the bus is freed immediately and
// the slot returns to routine polling; the slave executes off the bus and its
// reply arrives as a DATA frame on a LATER routine poll (collected below). ACK /
// NAK and the async reply are all relayed to the Pi defer-never-drop. (The L2
// command tracker owns the real per-slave queue + retry now.)
#define CMD_ACK_TIMEOUT_MS  40u   // wait for the slave's ACK/NAK (a couple poll slots)

typedef enum { CMD_IDLE = 0, CMD_QUEUED, CMD_SENT } cmd_state_t;
static cmd_state_t   g_cmd_state = CMD_IDLE;
static uint8_t       g_cmd_slave = 0;
static uint8_t       g_cmd_buf[RS485_PAYLOAD_MAX];   // [opcode:u16][exec body]
static uint8_t       g_cmd_len   = 0;

static rs485_frame_t g_cmd_reply;                    // async slave reply awaiting USB relay
static bool          g_async_reply_fresh = false;    // a reply (from a poll) awaits relay

static bool          g_cmd_ack_fresh   = false;      // an ACK/NAK awaits relay
static bool          g_cmd_ack_is_nak  = false;
static uint8_t       g_cmd_ack_addr    = 0;
static uint16_t      g_cmd_ack_reqid   = 0;

// Pi -> slave command enqueue (from rx_drain while polling is enabled). Drops a
// second enqueue while one is in flight (the L2 tracker queues + retries properly).
// The DATA frame to the slave is [opcode:u16][body]: opcode passes through from the
// Pi frame — OP_BUS_EXEC (6b-ii, body carries exec_timeout) or legacy OP_SHELL_EXEC.
static void bus_cmd_enqueue(uint16_t opcode, uint8_t slave, const uint8_t* exec, uint8_t exec_len) {
    if (g_cmd_state != CMD_IDLE) return;
    if (exec_len > (uint8_t)(RS485_PAYLOAD_MAX - 2u)) exec_len = RS485_PAYLOAD_MAX - 2u;
    g_cmd_buf[0] = (uint8_t)(opcode & 0xFFu);
    g_cmd_buf[1] = (uint8_t)(opcode >> 8);
    for (uint8_t i = 0; i < exec_len; i++) g_cmd_buf[2u + i] = exec[i];
    g_cmd_len   = (uint8_t)(2u + exec_len);
    g_cmd_slave = slave;
    g_cmd_state = CMD_QUEUED;
}

// Relay an async command reply (a DATA frame the slave returned on a routine poll
// once it finished executing) to the Pi, retrying while the TX ring is full
// (defer-never-drop). Reply DATA payload = [opcode:u16][body] -> s2m frame
// addr=replying-slave, cmd=opcode. The Pi demux correlates by request_id (inside
// the body), so the slave that answered is the authoritative addr. Called every
// main loop.
static void bus_drain_cmd_reply(void) {
    if (!g_async_reply_fresh) return;
    uint16_t opcode = (uint16_t)g_cmd_reply.payload[0]
                    | ((uint16_t)g_cmd_reply.payload[1] << 8);
    frame_meta_t meta = { .addr = g_cmd_reply.src, .cmd = opcode, .seq = g_bridge_seq,
                          .ack_seq = 0, .ack_status = 0,
                          .payload_len = (uint8_t)(g_cmd_reply.len - 2u) };
    if (frame_encode_s2m(&meta, &g_cmd_reply.payload[2], &g_tx_ring) == 0) {
        g_bridge_seq++;
        g_async_reply_fresh = false;
    }
    // else: ring full, retry next loop.
}

// Relay a command ACK/NAK to the Pi (defer-never-drop). OP_BUS_CMD_ACK/NAK body =
// [addr:u8][req_id:u16] -> the L2 tracker advances SENT->INFLIGHT (ACK) or resends
// (NAK). Called every main loop.
static void bus_drain_cmd_ack(void) {
    if (!g_cmd_ack_fresh) return;
    uint8_t body[3] = { g_cmd_ack_addr,
                        (uint8_t)(g_cmd_ack_reqid & 0xFFu),
                        (uint8_t)(g_cmd_ack_reqid >> 8) };
    frame_meta_t meta = { .addr = g_cmd_ack_addr,
                          .cmd = g_cmd_ack_is_nak ? OP_BUS_CMD_NAK : OP_BUS_CMD_ACK,
                          .seq = g_bridge_seq, .ack_seq = 0, .ack_status = 0,
                          .payload_len = sizeof(body) };
    if (frame_encode_s2m(&meta, body, &g_tx_ring) == 0) {
        g_bridge_seq++;
        g_cmd_ack_fresh = false;
    }
}

// Emit one pending summary-bit edge (OP_BUS_SLAVE_FLAGGED [addr][flags]) per
// call, defer-never-drop (same discipline as bus_drain_events). The roster's
// (summary != announced_summary) IS the pending-edge flag. The flags byte bit0
// = an armed interlock on that slave is tripped; L2 reads detail + advises.
static void bus_drain_flagged(void) {
    uint8_t n = bus_roster_count();
    for (uint8_t i = 0; i < n; i++) {
        const bus_slave_t* cs = bus_roster_at(i);
        if (cs == NULL || cs->summary == cs->announced_summary) continue;
        bus_slave_t* s = bus_roster_find(cs->addr);
        if (s == NULL) continue;
        uint8_t body[2] = { s->addr, s->summary };
        frame_meta_t meta = { .addr = s->addr, .cmd = OP_BUS_SLAVE_FLAGGED, .seq = g_bridge_seq,
                              .ack_seq = 0, .ack_status = 0, .payload_len = sizeof(body) };
        if (frame_encode_s2m(&meta, body, &g_tx_ring) == 0) {
            g_bridge_seq++;
            s->announced_summary = s->summary;
        }
        return;   // one emit per call; ring may be full — retry next loop
    }
}

// 7b piece 2 — the BC->Pi status report: periodically re-assert EVERY slave's
// CURRENT summary (the bit the BC already holds from the sweep) to the Pi as
// OP_BUS_STATUS_REPORT, UNCONDITIONAL (not edge-gated). This is the reliable status
// INDEX the Pi reconciles its received messages against: it heals a lost summary
// edge and establishes the confirmed-ok baseline for a never-tripped slave. Travels
// BC->Pi over USB only — ZERO RS-485 traffic. One emit per call, cursor walks the
// roster, defer-never-drop (same discipline as the other drains).
#define BUS_STATUS_INTERVAL_MS 3000u

static uint32_t g_status_next_ms = 0;   // when the next snapshot is due
static uint8_t  g_status_cursor  = 0;   // roster index within the active snapshot
static bool     g_status_active  = false;

static void bus_drain_status_report(void) {
    if (!bus_poll_cfg()->enabled) { g_status_active = false; return; }
    uint32_t now = board_millis();
    if (!g_status_active && (int32_t)(now - g_status_next_ms) >= 0) {
        g_status_active  = true;             // start a fresh roster snapshot
        g_status_cursor  = 0;
        g_status_next_ms = now + BUS_STATUS_INTERVAL_MS;
    }
    if (!g_status_active) return;

    uint8_t n = bus_roster_count();
    if (g_status_cursor >= n) { g_status_active = false; return; }
    const bus_slave_t* s = bus_roster_at(g_status_cursor);
    if (s == NULL) { g_status_cursor++; return; }
    uint8_t body[2] = { s->addr, s->summary };
    frame_meta_t meta = { .addr = s->addr, .cmd = OP_BUS_STATUS_REPORT, .seq = g_bridge_seq,
                          .ack_seq = 0, .ack_status = 0, .payload_len = sizeof(body) };
    if (frame_encode_s2m(&meta, body, &g_tx_ring) == 0) {
        g_bridge_seq++;
        g_status_cursor++;   // advance only on a successful emit (defer-never-drop)
    }
    // else: ring full — retry this same slave next loop.
}

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
        rs485_rx_flush();                                    // drop stale before listening
        if (g_cmd_state == CMD_QUEUED) {
            // Inject the queued command into this slot instead of a routine POLL.
            // Its reply (a DATA frame) is captured in POLL_WAIT and relayed.
            // Non-blocking TX (4b-ii step 1): the BC transmits via the interrupt
            // driver. If the engine is still draining a prior frame, retry next
            // pass (shouldn't happen at the poll cadence). rx_flush already ran.
            if (!rs485_tx_async_start(g_cmd_slave, BUS_CONTROLLER_LOCAL_ADDR,
                                      RS485_FT_DATA, ++g_poll_seq, g_cmd_buf, g_cmd_len))
                return;
            g_poll_cur_addr = g_cmd_slave;
            g_cmd_state     = CMD_SENT;
            g_poll_deadline = now + CMD_ACK_TIMEOUT_MS;   // 6b: wait for the ACK, not the reply
            g_poll_state    = POLL_WAIT;
            return;
        }
        const bus_slave_t* s = bus_roster_next_enabled(&g_poll_cursor);
        if (s == NULL) { g_poll_next_ms = now + cfg->poll_period_ms; return; }
        if (!rs485_tx_async_start(s->addr, BUS_CONTROLLER_LOCAL_ADDR,
                                  RS485_FT_POLL, ++g_poll_seq, NULL, 0))
            return;
        g_poll_cur_addr = s->addr;
        g_poll_deadline = now + POLL_SLOT_TIMEOUT_MS;
        g_poll_state    = POLL_WAIT;
        return;
    }

    // POLL_WAIT: any valid frame from the polled slave = alive; else timeout = miss.
    // 6b dispatch by frame class:
    //   * CMD_SENT slot: expect ACK/NAK (bus freed on either) — relay it.
    //   * DATA on a routine poll: an async command reply the slave finished — relay.
    //   * NO_MESSAGE: capture the interlock summary-bit.
    // POLL_WAIT only stages relays + roster state; the bus_drain_* helpers emit.
    rs485_frame_t f;
    if (rs485_recv(&f) && f.src == g_poll_cur_addr) {
        bus_mark_alive(g_poll_cur_addr, now);
        uint8_t cls = (uint8_t)(f.type & RS485_FT_MASK);
        uint16_t rid = (f.len >= 2u)
                     ? (uint16_t)((uint16_t)f.payload[0] | ((uint16_t)f.payload[1] << 8))
                     : (uint16_t)0;
        if (g_cmd_state == CMD_SENT) {
            // The just-injected command is ACK'd (claimed) or NAK'd (slave busy).
            if (cls == RS485_FT_ACK || cls == RS485_FT_NAK) {
                if (!g_cmd_ack_fresh) {           // stage the relay (defer-never-drop)
                    g_cmd_ack_addr   = g_cmd_slave;
                    g_cmd_ack_reqid  = rid;       // ACK/NAK payload = [req_id:u16]
                    g_cmd_ack_is_nak = (cls == RS485_FT_NAK);
                    g_cmd_ack_fresh  = true;
                }
            }
            g_cmd_state = CMD_IDLE;               // bus freed; reply (if any) comes later
        } else if (cls == RS485_FT_DATA && f.len >= 2u) {
            // Async command reply on a routine poll -> relay defer-never-drop.
            if (!g_async_reply_fresh) { g_cmd_reply = f; g_async_reply_fresh = true; }
            // else: a prior reply is still draining; the slave re-offers on its next
            // poll (one-in-flight per slave makes a pile-up impossible at cadence).
        } else {
            // NO_MESSAGE [summary]: capture the interlock summary-bit.
            bus_slave_t* s = bus_roster_find(g_poll_cur_addr);
            if (s != NULL) s->summary = (f.len >= 1u) ? f.payload[0] : 0u;
        }
        g_poll_state   = POLL_IDLE;
        g_poll_next_ms = now + cfg->poll_period_ms;
    } else if ((int32_t)(now - g_poll_deadline) >= 0) {
        bus_mark_miss(g_poll_cur_addr, cfg->max_misses);
        if (g_cmd_state == CMD_SENT) g_cmd_state = CMD_IDLE;   // no ACK; L2 ack-timeout resends
        g_poll_state   = POLL_IDLE;
        g_poll_next_ms = now + cfg->poll_period_ms;
    }
}
#endif  // ROLE_BUS_CONTROLLER

#if defined(ROLE_SLAVE)
// ----------------------------------------------------------------------------
// RS-485 slave, ISR-driven response (Layer-1 / 4b-i). The RX-complete ISR
// assembles each frame addressed to us and dispatches it via slave_rx_isr (IN
// ISR CONTEXT):
//   * POLL -> answer immediately with NO_MESSAGE carrying the PRIMED interlock
//             summary byte (bit0 = an armed interlock tripped). Served straight
//             from the interrupt, zero main-loop involvement.
//   * DATA -> stash into the one-in-flight input buffer for the main loop.
// The main loop (rs485_slave_poll) keeps the summary primed, then processes a
// stashed DATA (an OP_SHELL_EXEC body) through the SAME shell layer used over USB
// and queues the OP_SHELL_REPLY via the non-blocking TX engine. Replies >
// RS485_PAYLOAD_MAX-2 are clamped (fragmentation is later).
//
// NOTE (4b-ii boundary): today the BC waits for the command reply inside its poll
// window, so the slave sends it from the main loop here. When the BC sweep moves
// into its own ISR, the reply becomes a second "fresh" buffer the slave ISR emits
// on a poll alongside the interlock buffer (the full two-buffer model).
// ----------------------------------------------------------------------------
#define RS485_SHELL_EXEC_HEADER_LEN 4u     // request_id u16 + command_id u16
#define SLAVE_ABORT_MARGIN_MS       1000u  // 6b-ii: BUSY-watchdog slack over exec_timeout

static volatile uint8_t g_slave_summary;        // primed interlock summary (ISR reads)

// 6b — ACK-frees-the-bus, single-in-flight. The RX ISR ACKs a command on receipt
// (claim -> BUSY) or NAKs if already busy; the main loop executes it and arms the
// reply, which the ISR emits on the NEXT poll. The bus is freed the instant the ACK
// shifts out — a slow command runs entirely off the bus.
//
// 6b-ii — the command DATA carries a per-command exec_timeout: an OP_BUS_EXEC frame
// is [opcode][exec_timeout:u16][req_id][cmd][args] (hdr 4); a legacy OP_SHELL_EXEC
// frame is [opcode][req_id][cmd][args] (hdr 2, no timeout). A BUSY-watchdog frees
// the slot if a claimed command overruns exec_timeout+margin (wedged command, or a
// master that vanished before collecting the reply) so the node stays commandable.
static volatile bool     g_slave_busy;        // claimed a command; held until its reply ships
static volatile bool     g_slave_work_fresh;  // a claimed command awaits main-loop execution
static rs485_frame_t     g_slave_in;          // ISR -> main loop (the claimed command)
static volatile bool     g_slave_reply_ready; // main loop armed a reply; ISR emits it on a poll
static uint32_t          g_slave_claim_ms;    // when the in-flight command was picked up
static uint16_t          g_slave_exec_to;     // its exec_timeout (0 = watchdog disabled)

static uint8_t g_slave_reply[RS485_PAYLOAD_MAX];  // buffer 1: [OP_SHELL_REPLY:u16][req_id][status][result]
static uint8_t g_slave_reply_len;

// 7b piece 1 — buffer 2: the async interlock MESSAGE. On a summary CHANGE (trip or
// recover edge) the main loop fills this with [OP_BUS_INTERLOCK_MSG:u16][v2 status]
// and marks it fresh; the ISR pushes it on the next poll (best-effort/UDP-style —
// a lost push is healed by the BC-status reconciliation, piece 3). The two-buffer
// model: buffer 1 = command reply, buffer 2 = interlock message.
static uint8_t       g_slave_il_msg[RS485_PAYLOAD_MAX];
static uint8_t       g_slave_il_msg_len;
static volatile bool g_slave_il_msg_fresh;
static uint8_t       g_slave_prev_summary;   // edge detector for the fill

// Locate the shell-exec body inside a bus DATA command + extract exec_timeout.
// Returns the header length (offset to [req_id][cmd][args]), or 0 if not a command.
static uint8_t slave_cmd_parse(const rs485_frame_t* f, uint16_t* exec_timeout_ms) {
    if (f->len < 2u) return 0u;
    uint16_t opcode = (uint16_t)f->payload[0] | ((uint16_t)f->payload[1] << 8);
    if (opcode == OP_BUS_EXEC) {
        if (f->len < (uint8_t)(4u + RS485_SHELL_EXEC_HEADER_LEN)) return 0u;  // opcode+to+rid+cmd
        *exec_timeout_ms = (uint16_t)f->payload[2] | ((uint16_t)f->payload[3] << 8);
        return 4u;   // opcode(2) + exec_timeout(2)
    }
    if (opcode == OP_SHELL_EXEC) {
        if (f->len < (uint8_t)(2u + RS485_SHELL_EXEC_HEADER_LEN)) return 0u;
        *exec_timeout_ms = 0u;
        return 2u;   // opcode(2)
    }
    return 0u;
}

// ISR context:
//   POLL -> if a reply is armed, ship it on this window (frees BUSY); else
//           NO_MESSAGE + the primed interlock summary byte.
//   DATA(SHELL_EXEC) -> ACK + claim (IDLE), or NAK (already BUSY). The ACK/NAK
//           echoes the command's request_id so the BC/L2 correlate it.
static void slave_rx_isr(const rs485_frame_t* f) {
    uint8_t cls = (uint8_t)(f->type & RS485_FT_MASK);
    if (cls == RS485_FT_POLL) {
        // Priority: the safety interlock message (buffer 2) > the command reply
        // (buffer 1) > NO_MESSAGE+summary. Each poll ships one; both buffers drain
        // over successive polls ("the ISR sends both").
        if (g_slave_il_msg_fresh) {
            if (rs485_tx_async_start(RS485_ADDR_MASTER, register_dongle_rs485_addr(),
                                     RS485_FT_DATA, f->seq,
                                     g_slave_il_msg, g_slave_il_msg_len))
                g_slave_il_msg_fresh = false;
            // TX busy this round -> retry on the next poll (message stays fresh).
        } else if (g_slave_reply_ready) {
            // Deliver the finished command's reply on this poll (req_id is inside it).
            if (rs485_tx_async_start(RS485_ADDR_MASTER, register_dongle_rs485_addr(),
                                     RS485_FT_DATA, f->seq,
                                     g_slave_reply, g_slave_reply_len)) {
                g_slave_reply_ready = false;
                g_slave_busy        = false;   // done; ready for the next command
            }
            // TX busy this round -> retry on the next poll (reply stays armed).
        } else {
            rs485_tx_async_start(RS485_ADDR_MASTER, register_dongle_rs485_addr(),
                                 RS485_FT_NO_MESSAGE, f->seq,
                                 (const uint8_t*)&g_slave_summary, 1);
        }
    } else if (cls == RS485_FT_DATA) {
        // OP_BUS_EXEC (6b-ii, with exec_timeout) or legacy OP_SHELL_EXEC; req_id
        // sits right after the header so it can be echoed in the ACK/NAK.
        uint16_t to;
        uint8_t hdr = slave_cmd_parse(f, &to);
        if (hdr == 0u) return;                                 // not a recognised command
        uint8_t rid[2] = { f->payload[hdr], f->payload[hdr + 1u] };
        if (!g_slave_busy) {
            g_slave_in         = *f;
            g_slave_busy       = true;
            g_slave_work_fresh = true;
            rs485_tx_async_start(RS485_ADDR_MASTER, register_dongle_rs485_addr(),
                                 RS485_FT_ACK, f->seq, rid, 2);
        } else {
            rs485_tx_async_start(RS485_ADDR_MASTER, register_dongle_rs485_addr(),
                                 RS485_FT_NAK, f->seq, rid, 2);
        }
    }
}

// Main loop: BUSY-watchdog, keep the summary primed, then execute a claimed command
// and arm its reply for the ISR to ship on the next poll. Does NOT transmit (the ISR
// owns the wire), so execution never holds the bus.
static void rs485_slave_poll(void) {
    uint8_t summ = interlock_summary_flags();
    g_slave_summary = summ;                        // 1 byte = atomic on M0+

    // 7b piece 1 — buffer 2: on a summary CHANGE (trip or recover edge), fill the
    // async interlock message; the ISR pushes it on the next poll. 7b piece 3 — the
    // dumb re-push: the Pi (reconciliation) pokes CMD_INTERLOCK_REPUSH to fill a gap,
    // which re-emits the current message the same way. A latest-state-wins overwrite
    // is fine (best-effort).
    bool edge   = (summ != g_slave_prev_summary);
    if (edge) g_slave_prev_summary = summ;
    bool repush = interlock_take_repush();   // one-shot; evaluated even when edge fires
    if (edge || repush) {
        g_slave_il_msg[0] = (uint8_t)(OP_BUS_INTERLOCK_MSG & 0xFFu);
        g_slave_il_msg[1] = (uint8_t)(OP_BUS_INTERLOCK_MSG >> 8);
        uint16_t n = interlock_build_status_v2(&g_slave_il_msg[2]);
        if (n > (uint16_t)(RS485_PAYLOAD_MAX - 2u)) n = RS485_PAYLOAD_MAX - 2u;
        g_slave_il_msg_len   = (uint8_t)(2u + n);
        g_slave_il_msg_fresh = true;
    }

    // 6b-ii BUSY-watchdog: a claimed command that overruns exec_timeout+margin (or
    // whose reply never ships because the master vanished) frees the slot so the
    // node stays commandable; the L2 exec-deadline reports the failure.
    if (g_slave_busy && g_slave_exec_to > 0u &&
        (uint32_t)(board_millis() - g_slave_claim_ms) >
            (uint32_t)g_slave_exec_to + SLAVE_ABORT_MARGIN_MS) {
        g_slave_busy        = false;
        g_slave_work_fresh  = false;
        g_slave_reply_ready = false;
        g_slave_exec_to     = 0u;
    }

    if (!g_slave_work_fresh) return;
    g_slave_work_fresh = false;
    rs485_frame_t f = g_slave_in;      // stable while BUSY (ISR won't reclaim until reply ships)

    uint16_t to;
    uint8_t hdr = slave_cmd_parse(&f, &to);
    if (hdr == 0u) { g_slave_busy = false; return; }   // malformed (ISR already filtered)
    g_slave_claim_ms = board_millis();
    g_slave_exec_to  = to;
    const uint8_t* exec     = &f.payload[hdr];
    uint16_t       exec_len = (uint16_t)(f.len - hdr);

    uint8_t  reply_body[COMM_PAYLOAD_MAX];
    uint16_t reply_len = shell_dispatch_payload(exec, exec_len, reply_body);
    if (reply_len > (uint16_t)(RS485_PAYLOAD_MAX - 2u)) reply_len = RS485_PAYLOAD_MAX - 2u;
    g_slave_reply[0] = (uint8_t)(OP_SHELL_REPLY & 0xFFu);
    g_slave_reply[1] = (uint8_t)(OP_SHELL_REPLY >> 8);
    for (uint16_t i = 0; i < reply_len; i++) g_slave_reply[2u + i] = reply_body[i];
    g_slave_reply_len   = (uint8_t)(2u + reply_len);
    g_slave_reply_ready = true;        // ISR ships it on the next poll
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
            // for an RS-485 slave. While the sweep owns the bus, inject it into a
            // poll slot (Stage 3c); otherwise use the reactive one-shot bridge.
            if (meta.addr != BUS_CONTROLLER_LOCAL_ADDR) {
                if (bus_poll_cfg()->enabled)
                    bus_cmd_enqueue(meta.cmd, meta.addr, g_rx_payload, meta.payload_len);
                else
                    bridge_start(meta.cmd, meta.addr, g_rx_payload, meta.payload_len);
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
    // Layer-1 / 4b-i: answer POLLs straight from the RX-complete ISR; DATA is
    // deferred to the main loop via the one-in-flight input buffer.
    rs485_set_isr_dispatch(slave_rx_isr);
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

#ifdef I2C_CLIENT
        i2c_store_service();   // M2b: do any pending config-store flash commit (~ms, off the I2C ISR)
#endif

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
            bus_drain_events();      // emit pending DOWN/UP; full ring defers, never drops
            bus_drain_flagged();     // emit pending summary-bit edges (interlock tripped)
            bus_drain_cmd_ack();     // relay a command ACK/NAK (6b); same discipline
            bus_drain_cmd_reply();   // relay an async command reply; same discipline
            bus_drain_status_report(); // 7b-2: periodic per-slave status index (USB-only)
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
