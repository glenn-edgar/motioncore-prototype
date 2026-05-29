// ============================================================================
// user_functions.c — register_dongle (SAMD21 Phase-2 merge)
//
// Real implementations of the chain's three user-defined functions.
// The Lua chain (register_dongle.lua) wires:
//   io_call(send_register)  -> oneshot, fires once on first INIT
//   o_call(send_heartbeat)  -> oneshot, fires every chain_flow cycle (~1 Hz)
//   m_call(toggle_led)      -> main, fires every tick (bare leaf inside fork)
//
// Frame work uses the vendored libcomm slice (SLIP + CRC-8/AUTOSAR). All
// emitted frames go through the shared g_tx_ring, which main.c drains to
// USB-CDC every loop iteration.
// ============================================================================

#include <stdint.h>
#include <string.h>

#include "bsp/board_api.h"
#include "samd21.h"          // CMSIS: PORT register block + NVIC_SystemReset

// Implemented in main.c.
extern void firmware_request_reboot(uint32_t delay_ms);

#include "s_engine_types.h"
#include "s_engine_module.h"

#include "frame.h"
#include "opcodes.h"
#include "flash_storage.h"
#include "shell_commands.h"

// ----------------------------------------------------------------------------
// Shared TX ring (defined in main.c). Every user function that emits a frame
// stages SLIP-encoded bytes here; main.c's loop drains the ring to CDC.
// ----------------------------------------------------------------------------
extern frame_ring_t g_tx_ring;

// ----------------------------------------------------------------------------
// M-port stack shims.
// s_engine_node.c / s_engine_module.c call these unconditionally, but the
// M-port decision (#11) is "no stack" — inst->stack stays NULL so the
// bodies are dead at runtime. Empty defs here let us drop s_engine_stack.c
// from the build.
// ----------------------------------------------------------------------------
void s_expr_tree_reset_stack(s_expr_tree_instance_t* inst) { (void)inst; }
void s_expr_tree_free_stack (s_expr_tree_instance_t* inst) { (void)inst; }

// ----------------------------------------------------------------------------
// SAMD21 chip UID (datasheet §10.3.3): four 32-bit words at fixed addresses.
// Word 0 lives at 0x0080A00C; words 1..3 live at 0x0080A040 / +0x44 / +0x48.
// ----------------------------------------------------------------------------
#define SAMD21_UID_WORD0_ADDR  0x0080A00CU
#define SAMD21_UID_WORD1_ADDR  0x0080A040U
#define SAMD21_UID_WORD2_ADDR  0x0080A044U
#define SAMD21_UID_WORD3_ADDR  0x0080A048U

static void samd21_read_uid(uint8_t out[16]) {
    uint32_t w0 = *(volatile uint32_t*)SAMD21_UID_WORD0_ADDR;
    uint32_t w1 = *(volatile uint32_t*)SAMD21_UID_WORD1_ADDR;
    uint32_t w2 = *(volatile uint32_t*)SAMD21_UID_WORD2_ADDR;
    uint32_t w3 = *(volatile uint32_t*)SAMD21_UID_WORD3_ADDR;
    // Little-endian byte order in the payload (matches the rest of libcomm wire format).
    out[ 0] = (uint8_t)(w0 >>  0); out[ 1] = (uint8_t)(w0 >>  8);
    out[ 2] = (uint8_t)(w0 >> 16); out[ 3] = (uint8_t)(w0 >> 24);
    out[ 4] = (uint8_t)(w1 >>  0); out[ 5] = (uint8_t)(w1 >>  8);
    out[ 6] = (uint8_t)(w1 >> 16); out[ 7] = (uint8_t)(w1 >> 24);
    out[ 8] = (uint8_t)(w2 >>  0); out[ 9] = (uint8_t)(w2 >>  8);
    out[10] = (uint8_t)(w2 >> 16); out[11] = (uint8_t)(w2 >> 24);
    out[12] = (uint8_t)(w3 >>  0); out[13] = (uint8_t)(w3 >>  8);
    out[14] = (uint8_t)(w3 >> 16); out[15] = (uint8_t)(w3 >> 24);
}

// ----------------------------------------------------------------------------
// send_register — oneshot (io_call). Fires once per chain_flow retry-cycle
// while in BOOT (Phase 2f loop) and again on EV_HOST_REATTACH.
//
// Payload v2 (38 bytes, little-endian) per dongle_class_identity_2026-05-13 §:
//   [0]       version              = 2
//   [1..4]    class_id             firmware build-time constant (FNV-1a)
//   [5..8]    instance_id          0 if uncommissioned
//   [9]       commissioning_state  0=UNCOMMISSIONED, 1=COMMISSIONED
//   [10..25]  chip_uid             SAMD21 16-byte factory UID
//   [26..27]  vid                  0x2886
//   [28..29]  pid                  0x802F
//   [30..33]  fw_version           (major<<16) | (minor<<8) | patch
//   [34..37]  build_date           packed YYYYMMDD as u32 decimal
//
// class_id below is an INTERIM value — FNV-1a-32 of the class-name string
//       "motioncore.dongle.register.samd21.v1" — unique vs the RA4M1 dongle.
// TODO: replace with #include "class_ids.h" + CLASS_SAMD21_SHELL_V1 once
//       kb_build delivers the authoritative catalog/codegen (see
//       dongle_class_identity_2026-05-13 cross-repo handoff).
// TODO: g_instance_id / g_commissioning_state read from NVMCTRL flash storage
//       once L0 commissioning lands. They're declared here so the future
//       commissioning handler has a single source of truth to update.
// ----------------------------------------------------------------------------
#define REGISTER_PAYLOAD_LEN     38u
#define REGISTER_PAYLOAD_VERSION 2u
#define REGISTER_VID             0x2886U
#define REGISTER_PID             0x802FU
#define REGISTER_FW_VERSION      0x00010000U   // v1.0.0 — matches manifest
#define REGISTER_BUILD_DATE      20260519U     // 2026-05-19; bump on each build

// Role-specific class_id. ROLE_DONGLE / ROLE_BUS_CONTROLLER is set by the
// Makefile via -DROLE_xxx=1. Default (no ROLE on the make command line) is
// dongle, matching every previously-flashed bench unit.
#if defined(ROLE_BUS_CONTROLLER)
  #define REGISTER_CLASS_ID_STUB   0x5E589000U  // sequential to dongle; real FNV-1a "motioncore.bus_controller.samd21.v1" TBD
#elif defined(ROLE_SLAVE)
  #define REGISTER_CLASS_ID_STUB   0x5E589100U  // RS-485 slave; real FNV-1a "motioncore.slave.samd21.v1" TBD
#elif defined(ROLE_DONGLE)
  #define REGISTER_CLASS_ID_STUB   0x5E588873U  // FNV-1a "motioncore.dongle.register.samd21.v1"
#else
  #error "ROLE must be defined: pass ROLE=dongle, bus_controller or slave to make"
#endif

// Mutable identity — initialized at boot via register_dongle_load_commissioning(),
// updated by handle_commission_set / handle_commission_clear (which reboot
// afterward, so the in-RAM update is mostly cosmetic; the flash write is what
// persists).
static uint32_t g_class_id            = REGISTER_CLASS_ID_STUB;
static uint32_t g_instance_id         = 0;
static uint8_t  g_commissioning_state = COMMISSIONING_UNCOMMISSIONED;

// Called once from main() before engine init. Reads flash; if no valid blob
// found (factory-fresh dongle), leaves the globals at their UNCOMMISSIONED
// defaults.
void register_dongle_load_commissioning(void) {
    commission_blob_t blob;
    if (flash_storage_read(&blob)) {
        g_instance_id         = blob.instance_id;
        g_commissioning_state = blob.commissioning_state;
    }
}

// True if the chain should treat this boot as "uncommissioned" — used by
// handle_register_ack to decide whether OP_REGISTER_ACK advances or NAKs.
bool register_dongle_is_uncommissioned(void) {
    return g_commissioning_state == COMMISSIONING_UNCOMMISSIONED;
}

// RS-485 slave address = low byte of the commissioned instance_id (single
// source of truth per phase plan decision #2). 0 when uncommissioned.
uint8_t register_dongle_rs485_addr(void) {
    return (uint8_t)(g_instance_id & 0xFFu);
}

// Public accessor for the 16-byte chip UID (used by the slave PING responder).
void register_dongle_chip_uid(uint8_t out[16]) {
    samd21_read_uid(out);
}

void send_register(s_expr_tree_instance_t* inst,
                   const s_expr_param_t*  params,
                   uint16_t               param_count,
                   s_expr_event_type_t    event_type,
                   uint16_t               event_id,
                   void*                  event_data) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;

    uint8_t payload[REGISTER_PAYLOAD_LEN];
    payload[ 0] = (uint8_t)REGISTER_PAYLOAD_VERSION;
    payload[ 1] = (uint8_t)(g_class_id            >>  0);
    payload[ 2] = (uint8_t)(g_class_id            >>  8);
    payload[ 3] = (uint8_t)(g_class_id            >> 16);
    payload[ 4] = (uint8_t)(g_class_id            >> 24);
    payload[ 5] = (uint8_t)(g_instance_id         >>  0);
    payload[ 6] = (uint8_t)(g_instance_id         >>  8);
    payload[ 7] = (uint8_t)(g_instance_id         >> 16);
    payload[ 8] = (uint8_t)(g_instance_id         >> 24);
    payload[ 9] = g_commissioning_state;
    samd21_read_uid(&payload[10]);
    payload[26] = (uint8_t)(REGISTER_VID          >>  0);
    payload[27] = (uint8_t)(REGISTER_VID          >>  8);
    payload[28] = (uint8_t)(REGISTER_PID          >>  0);
    payload[29] = (uint8_t)(REGISTER_PID          >>  8);
    payload[30] = (uint8_t)(REGISTER_FW_VERSION   >>  0);
    payload[31] = (uint8_t)(REGISTER_FW_VERSION   >>  8);
    payload[32] = (uint8_t)(REGISTER_FW_VERSION   >> 16);
    payload[33] = (uint8_t)(REGISTER_FW_VERSION   >> 24);
    payload[34] = (uint8_t)(REGISTER_BUILD_DATE   >>  0);
    payload[35] = (uint8_t)(REGISTER_BUILD_DATE   >>  8);
    payload[36] = (uint8_t)(REGISTER_BUILD_DATE   >> 16);
    payload[37] = (uint8_t)(REGISTER_BUILD_DATE   >> 24);

    frame_meta_t meta = {
        .addr        = 1,
        .cmd         = OP_REGISTER,
        .seq         = 0,
        .ack_seq     = 0,
        .ack_status  = 0,
        .payload_len = REGISTER_PAYLOAD_LEN,
    };
    (void)frame_encode_s2m(&meta, payload, &g_tx_ring);
}

// ----------------------------------------------------------------------------
// send_heartbeat — oneshot (o_call). Fires every chain_flow cycle.
// Payload (8 B, little-endian):
//   [0..3]  uptime_ms = board_millis() truncated to uint32
//   [4..7]  seq_counter (static, increments each invocation)
// ----------------------------------------------------------------------------
#define HEARTBEAT_PAYLOAD_LEN  8u

static uint32_t g_heartbeat_seq = 0;

void send_heartbeat(s_expr_tree_instance_t* inst,
                    const s_expr_param_t*   params,
                    uint16_t                param_count,
                    s_expr_event_type_t     event_type,
                    uint16_t                event_id,
                    void*                   event_data) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;

    uint32_t uptime_ms = (uint32_t)board_millis();
    uint32_t seq       = g_heartbeat_seq++;

    uint8_t payload[HEARTBEAT_PAYLOAD_LEN];
    payload[0] = (uint8_t)(uptime_ms >>  0);
    payload[1] = (uint8_t)(uptime_ms >>  8);
    payload[2] = (uint8_t)(uptime_ms >> 16);
    payload[3] = (uint8_t)(uptime_ms >> 24);
    payload[4] = (uint8_t)(seq       >>  0);
    payload[5] = (uint8_t)(seq       >>  8);
    payload[6] = (uint8_t)(seq       >> 16);
    payload[7] = (uint8_t)(seq       >> 24);

    frame_meta_t meta = {
        .addr        = 1,
        .cmd         = OP_HEARTBEAT,
        .seq         = (uint8_t)(seq & 0xFFu),
        .ack_seq     = 0,
        .ack_status  = 0,
        .payload_len = HEARTBEAT_PAYLOAD_LEN,
    };
    (void)frame_encode_s2m(&meta, payload, &g_tx_ring);
}

// ----------------------------------------------------------------------------
// send_manifest_reply — oneshot (o_call). Fires once per OP_GET_MANIFEST.
//
// Wire payload (19 bytes for register_dongle's current 5-entry m2s set):
//   [0..3]   schema_hash       FNV-1a 32-bit over MANIFEST_SCHEMA_STR
//   [4..7]   firmware_version  (major<<16) | (minor<<8) | patch  — v1.0.0
//   [8]      m2s_opcode_count  = 5
//   [9..10]  OP_REGISTER_ACK       0x0103
//   [11..12] OP_PING               0x0104
//   [13..14] OP_GET_MANIFEST       0x0107
//   [15..16] OP_OPERATIONAL_BEGIN  0x0108
//   [17..18] OP_SHELL_EXEC         0x0109
//
// OP_COMMISSION_SET/CLEAR (0x0105/0x0106) are intentionally absent from this
// list — they're L0 opcodes used only by the standalone commissioning tool,
// not by operational stacks, and so don't belong in the "operational m2s
// surface" the manifest advertises.
//
// schema_hash is the FNV-1a over the layout-description string. Adding new
// opcodes to the *list* doesn't change the *layout*, so the string and hash
// are stable. Bump the version prefix only when the manifest's structure
// changes (e.g., adding new top-level fields).
// ----------------------------------------------------------------------------

// FNV-1a 32-bit input — must match host's matching constant in dongle_console
// or any future stack-side decoder. Changing this string bumps the hash, which
// signals host that its schema dictionary is stale.
static const char MANIFEST_SCHEMA_STR[] =
    "manifest_v1:schema_hash:u32,firmware_version:u32,m2s_count:u8,m2s_ops:u16[]";

#define MANIFEST_FW_VERSION   0x00010000U   // v1.0.0 — (major<<16)|(minor<<8)|patch
#define MANIFEST_M2S_COUNT    5u
#define MANIFEST_PAYLOAD_LEN  (4u + 4u + 1u + 2u * MANIFEST_M2S_COUNT)  // = 19

static uint32_t manifest_schema_hash_cached(void) {
    static uint32_t cached = 0;
    static uint8_t  computed = 0;
    if (!computed) {
        uint32_t h = 0x811C9DC5U;
        for (const char* p = MANIFEST_SCHEMA_STR; *p; p++) {
            h ^= (uint32_t)(uint8_t)*p;
            h *= 0x01000193U;
        }
        cached = h;
        computed = 1;
    }
    return cached;
}

void send_manifest_reply(s_expr_tree_instance_t* inst,
                         const s_expr_param_t*   params,
                         uint16_t                param_count,
                         s_expr_event_type_t     event_type,
                         uint16_t                event_id,
                         void*                   event_data) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;

    uint32_t schema_hash = manifest_schema_hash_cached();

    uint8_t payload[MANIFEST_PAYLOAD_LEN];
    payload[ 0] = (uint8_t)(schema_hash         >>  0);
    payload[ 1] = (uint8_t)(schema_hash         >>  8);
    payload[ 2] = (uint8_t)(schema_hash         >> 16);
    payload[ 3] = (uint8_t)(schema_hash         >> 24);
    payload[ 4] = (uint8_t)(MANIFEST_FW_VERSION >>  0);
    payload[ 5] = (uint8_t)(MANIFEST_FW_VERSION >>  8);
    payload[ 6] = (uint8_t)(MANIFEST_FW_VERSION >> 16);
    payload[ 7] = (uint8_t)(MANIFEST_FW_VERSION >> 24);
    payload[ 8] = (uint8_t)MANIFEST_M2S_COUNT;
    payload[ 9] = (uint8_t)(OP_REGISTER_ACK      & 0xFFu);
    payload[10] = (uint8_t)(OP_REGISTER_ACK      >> 8);
    payload[11] = (uint8_t)(OP_PING              & 0xFFu);
    payload[12] = (uint8_t)(OP_PING              >> 8);
    payload[13] = (uint8_t)(OP_GET_MANIFEST      & 0xFFu);
    payload[14] = (uint8_t)(OP_GET_MANIFEST      >> 8);
    payload[15] = (uint8_t)(OP_OPERATIONAL_BEGIN & 0xFFu);
    payload[16] = (uint8_t)(OP_OPERATIONAL_BEGIN >> 8);
    payload[17] = (uint8_t)(OP_SHELL_EXEC        & 0xFFu);
    payload[18] = (uint8_t)(OP_SHELL_EXEC        >> 8);

    frame_meta_t meta = {
        .addr        = 1,
        .cmd         = OP_MANIFEST_REPLY,
        .seq         = 0,
        .ack_seq     = 0,
        .ack_status  = 0,
        .payload_len = MANIFEST_PAYLOAD_LEN,
    };
    (void)frame_encode_s2m(&meta, payload, &g_tx_ring);
}

// ----------------------------------------------------------------------------
// emit_nak — internal helper. Builds and stages a 3-byte OP_NAK frame.
// Used by both send_nak (default-case handler) and handle_register_ack
// (commissioning gate for OP_REGISTER_ACK while UNCOMMISSIONED).
// ----------------------------------------------------------------------------

#define OP_NAK_PAYLOAD_LEN 3u

static uint32_t g_nak_seq = 0;

static void emit_nak(uint8_t reason, uint16_t rejected_cmd) {
    uint8_t payload[OP_NAK_PAYLOAD_LEN];
    payload[0] = reason;
    payload[1] = (uint8_t)(rejected_cmd & 0xFFu);
    payload[2] = (uint8_t)(rejected_cmd >> 8);
    frame_meta_t meta = {
        .addr        = 1,
        .cmd         = OP_NAK,
        .seq         = (uint8_t)(g_nak_seq++ & 0xFFu),
        .ack_seq     = 0,
        .ack_status  = 0,
        .payload_len = OP_NAK_PAYLOAD_LEN,
    };
    (void)frame_encode_s2m(&meta, payload, &g_tx_ring);
}

// ----------------------------------------------------------------------------
// send_nak — oneshot (o_call). Wired as the `default` action of every
// se_event_dispatch in the chain. Emits OP_NAK err_state with rejected_cmd =
// event_id.
//
// Silent (no emit) for:
//   * SE_EVENT_TICK (id 4)               — regular per-tick activations
//   * s2m opcodes (0x0001..0x00FF)       — dongle doesn't receive these
//   * engine-internal events (0xFE00+)   — EV_HOST_REATTACH etc.; also SE_EVENT_INIT/TERMINATE
//
// COMMISSION_SET/CLEAR have explicit chain cases now (BOOT for SET, all
// states for CLEAR); they don't reach this default. If they do (e.g. host
// sends SET in L1_DONE), err_state is the correct response — they're legal
// opcodes but illegal in the current state.
// ----------------------------------------------------------------------------

void send_nak(s_expr_tree_instance_t* inst,
              const s_expr_param_t*   params,
              uint16_t                param_count,
              s_expr_event_type_t     event_type,
              uint16_t                event_id,
              void*                   event_data) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_data;

    if (event_id < 0x0100u) return;          // SE_EVENT_TICK + s2m range
    if (event_id >= 0xFE00u) return;         // internal events + SE_EVENT_INIT/TERMINATE

    emit_nak((uint8_t)NAK_ERR_STATE, event_id);
}

// ----------------------------------------------------------------------------
// L0 commissioning handlers (oneshot — o_call).
//
// handle_register_ack — gates BOOT → L1_DONE transition on commissioning
//   state. If UNCOMMISSIONED, NAKs with err_state (spec rule: only
//   UNCOMMISSIONED accepts OP_COMMISSION_SET; ACK is illegal). If
//   COMMISSIONED, writes DONGLE_L1_DONE (=1) into the blackboard.
//
// handle_commission_set — if UNCOMMISSIONED, persists the new instance_id
//   to flash, emits OP_COMMISSION_REPLY, schedules reboot. If COMMISSIONED,
//   NAKs with err_state (re-commissioning is two-step: CLEAR then SET).
//   Payload (4-byte u32 new_instance_id) is staged into
//   g_pending_commission_instance_id by main.c's rx_drain before the chain
//   tick fires.
//
// handle_commission_clear — clears flash, emits OP_COMMISSION_REPLY,
//   schedules reboot. Legal in all states.
//
// Both *_set and *_clear use ~200 ms reboot delay so the OP_COMMISSION_REPLY
// frame and any in-flight TX-ring contents have time to reach the host
// before NVIC_SystemReset() fires.
// ----------------------------------------------------------------------------

#define COMMISSION_REPLY_PAYLOAD_LEN 5u
#define COMMISSION_REBOOT_DELAY_MS   200u

// Set by main.c's rx_drain when OP_COMMISSION_SET arrives. Single-producer
// (rx_drain), single-consumer (handle_commission_set) — race-free under
// libcomm's one-in-flight rule.
uint32_t g_pending_commission_instance_id = 0;

static uint32_t g_commission_reply_seq = 0;

static void emit_commission_reply(uint32_t stored_instance_id, uint8_t status) {
    uint8_t payload[COMMISSION_REPLY_PAYLOAD_LEN];
    payload[0] = (uint8_t)(stored_instance_id >>  0);
    payload[1] = (uint8_t)(stored_instance_id >>  8);
    payload[2] = (uint8_t)(stored_instance_id >> 16);
    payload[3] = (uint8_t)(stored_instance_id >> 24);
    payload[4] = status;
    frame_meta_t meta = {
        .addr        = 1,
        .cmd         = OP_COMMISSION_REPLY,
        .seq         = (uint8_t)(g_commission_reply_seq++ & 0xFFu),
        .ack_seq     = 0,
        .ack_status  = 0,
        .payload_len = COMMISSION_REPLY_PAYLOAD_LEN,
    };
    (void)frame_encode_s2m(&meta, payload, &g_tx_ring);
}

void handle_register_ack(s_expr_tree_instance_t* inst,
                         const s_expr_param_t*   params,
                         uint16_t                param_count,
                         s_expr_event_type_t     event_type,
                         uint16_t                event_id,
                         void*                   event_data) {
    (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;

    if (g_commissioning_state == COMMISSIONING_COMMISSIONED) {
        // Advance BOOT → L1_DONE. dongle_state is field 0 in dongle_record
        // (single-field record); future record growth means switching to
        // s_expr_blackboard_set_int_by_string().
        if (inst && inst->blackboard) {
            *(int32_t*)inst->blackboard = 1;   // DONGLE_L1_DONE
        }
        s_engine_log(inst, "BOOT: received OP_REGISTER_ACK -> L1_DONE");
    } else {
        emit_nak((uint8_t)NAK_ERR_STATE, OP_REGISTER_ACK);
        s_engine_log(inst, "BOOT: REGISTER_ACK rejected (UNCOMMISSIONED)");
    }
}

void handle_commission_set(s_expr_tree_instance_t* inst,
                           const s_expr_param_t*   params,
                           uint16_t                param_count,
                           s_expr_event_type_t     event_type,
                           uint16_t                event_id,
                           void*                   event_data) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;

    if (g_commissioning_state != COMMISSIONING_UNCOMMISSIONED) {
        emit_nak((uint8_t)NAK_ERR_STATE, OP_COMMISSION_SET);
        s_engine_log(inst, "BOOT: COMMISSION_SET rejected (already COMMISSIONED)");
        return;
    }

    uint32_t new_instance_id = g_pending_commission_instance_id;
    if (new_instance_id == 0) {
        // instance_id=0 is reserved for "uncommissioned" — reject as bad args.
        emit_nak((uint8_t)NAK_ERR_ARGS, OP_COMMISSION_SET);
        s_engine_log(inst, "COMMISSION_SET rejected (instance_id=0 reserved)");
        return;
    }

    bool ok = flash_storage_write(new_instance_id, (uint8_t)COMMISSIONING_COMMISSIONED);
    uint8_t status = ok ? 0u : 1u;   // 0=ok, 1=flash_write_failed

    // Update in-RAM identity (cosmetic — we'll reboot in 200 ms and reload
    // from flash anyway, but keeps things coherent if the reboot is delayed).
    if (ok) {
        g_instance_id         = new_instance_id;
        g_commissioning_state = COMMISSIONING_COMMISSIONED;
    }

    emit_commission_reply(new_instance_id, status);
    s_engine_log(inst, ok ? "COMMISSION_SET stored; rebooting..."
                          : "COMMISSION_SET flash write FAILED; rebooting anyway");
    firmware_request_reboot(COMMISSION_REBOOT_DELAY_MS);
}

// ----------------------------------------------------------------------------
// OP_SHELL_EXEC payload queue.
//
// The original single-slot staging (g_pending_shell_payload) lost frames
// when multiple OP_SHELL_EXEC arrived between engine ticks — rx_drain ran in
// every main-loop iteration, but tick_and_drain only every 250 ms, so up to
// 5 host frames at 50 ms intervals could overwrite the slot before any were
// handled. All in-flight handlers then read the last-written payload, so
// reply request_ids clustered to whatever arrived last.
//
// Fix: a small circular queue. rx_drain pushes to the tail when it decodes
// OP_SHELL_EXEC; handle_shell_exec pops from the head. Depth 8 covers the
// realistic worst case (~5 frames per tick window for the loopback test).
//
// Single-producer (rx_drain) / single-consumer (handle_shell_exec), both
// run in the main loop in the same thread — no locking needed.
// ----------------------------------------------------------------------------

#define SHELL_PENDING_DEPTH 8u

typedef struct {
    uint8_t payload[COMM_PAYLOAD_MAX];
    uint8_t len;
} shell_pending_slot_t;

static shell_pending_slot_t g_shell_pending[SHELL_PENDING_DEPTH];
static uint8_t              g_shell_head = 0;
static uint8_t              g_shell_tail = 0;

// Called from main.c rx_drain when an OP_SHELL_EXEC frame is decoded.
// Returns true if the payload was queued, false if the queue is full (caller
// should drop the frame — DON'T push the engine event without a queued slot,
// or handle_shell_exec will replay an old payload).
bool shell_pending_push(const uint8_t* payload, uint8_t len) {
    uint8_t next_tail = (uint8_t)((g_shell_tail + 1u) % SHELL_PENDING_DEPTH);
    if (next_tail == g_shell_head) return false;   // full
    if (len > COMM_PAYLOAD_MAX)    return false;
    memcpy(g_shell_pending[g_shell_tail].payload, payload, len);
    g_shell_pending[g_shell_tail].len = len;
    g_shell_tail = next_tail;
    return true;
}

// Pop the head slot into *out. Returns false if the queue is empty.
static bool shell_pending_pop(shell_pending_slot_t* out) {
    if (g_shell_head == g_shell_tail) return false;   // empty
    *out = g_shell_pending[g_shell_head];
    g_shell_head = (uint8_t)((g_shell_head + 1u) % SHELL_PENDING_DEPTH);
    return true;
}

static uint32_t g_shell_reply_seq = 0;

// ----------------------------------------------------------------------------
// handle_shell_exec — oneshot (o_call). Wired as the OPERATIONAL state's
// OP_SHELL_EXEC dispatch case.
//
// Wire payload (from main.c via g_pending_shell_payload):
//   [0..1]   request_id (u16, LE)
//   [2..3]   command_id (u16, LE)
//   [4..]    args_message (command-specific binary message)
//
// Reply (OP_SHELL_REPLY):
//   [0..1]   request_id  (echo)
//   [2]      status      (SHELL_STATUS_*)
//   [3..]    result_message (command-specific binary message, may be empty)
// ----------------------------------------------------------------------------

#define SHELL_REPLY_HEADER_LEN 3u
#define SHELL_EXEC_HEADER_LEN  4u
#define SHELL_RESULT_MAX       (COMM_PAYLOAD_MAX - SHELL_REPLY_HEADER_LEN)

// shell_dispatch_payload — transport-agnostic core of the app shell. Takes an
// OP_SHELL_EXEC body [request_id u16][command_id u16][args...] and writes the
// OP_SHELL_REPLY body [request_id u16][status u8][result...] into `reply`
// (must have capacity >= COMM_PAYLOAD_MAX). Returns the reply length.
//
// Reused by both transports: the USB OP_SHELL_EXEC handler below and the
// RS-485 slave tunnel in main.c (workbench HIL commands over the bus). Callers
// guarantee exec_len >= SHELL_EXEC_HEADER_LEN.
uint16_t shell_dispatch_payload(const uint8_t* exec, uint16_t exec_len,
                                uint8_t* reply) {
    const uint16_t request_id =
        (uint16_t)exec[0] | ((uint16_t)exec[1] << 8);
    const uint16_t command_id =
        (uint16_t)exec[2] | ((uint16_t)exec[3] << 8);
    const uint8_t* args     = &exec[SHELL_EXEC_HEADER_LEN];
    const uint16_t args_len = (uint16_t)(exec_len - SHELL_EXEC_HEADER_LEN);

    uint8_t  result_buf[SHELL_RESULT_MAX];
    uint16_t result_len = 0;
    uint8_t  status     = SHELL_STATUS_OK;

    const shell_cmd_entry_t* cmd = shell_find_cmd(command_id);
    if (cmd == NULL) {
        status = SHELL_STATUS_UNKNOWN_CMD;
    } else {
        shell_reader_t r;
        shell_writer_t w;
        sr_init(&r, args, args_len);
        sw_init(&w, result_buf, sizeof(result_buf));
        status = cmd->fn(&r, &w);
        if (status == SHELL_STATUS_OK) {
            if (r.overflow)      status = SHELL_STATUS_BAD_ARGS;
            else if (w.overflow) status = SHELL_STATUS_RESULT_TOO_BIG;
        }
        // Capture any bytes the handler wrote regardless of status — failure
        // paths may attach a diagnostic payload (e.g., interlock-set parse-
        // error detail). Writer-overflow already promoted to RESULT_TOO_BIG.
        if (!w.overflow) result_len = sw_len(&w);
    }

    reply[0] = (uint8_t)(request_id & 0xFFu);
    reply[1] = (uint8_t)(request_id >> 8);
    reply[2] = status;
    if (result_len > 0) {
        memcpy(&reply[SHELL_REPLY_HEADER_LEN], result_buf, result_len);
    }
    return (uint16_t)(SHELL_REPLY_HEADER_LEN + result_len);
}

void handle_shell_exec(s_expr_tree_instance_t* inst,
                       const s_expr_param_t*   params,
                       uint16_t                param_count,
                       s_expr_event_type_t     event_type,
                       uint16_t                event_id,
                       void*                   event_data) {
    (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;

    shell_pending_slot_t slot;
    if (!shell_pending_pop(&slot)) {
        // Engine pushed an OP_SHELL_EXEC event but the payload queue is empty.
        // Shouldn't happen — rx_drain only pushes the event after a successful
        // shell_pending_push. If it does, log it and NAK without request_id.
        s_engine_log(inst, "shell_exec: payload queue empty (race?)");
        emit_nak((uint8_t)NAK_ERR_NO_RESOURCES, OP_SHELL_EXEC);
        return;
    }

    if (slot.len < SHELL_EXEC_HEADER_LEN) {
        // Malformed wrapper — short of even the {request_id, command_id} header.
        // Can't echo request_id (we don't have one); just NAK.
        emit_nak((uint8_t)NAK_ERR_ARGS, OP_SHELL_EXEC);
        return;
    }

    uint8_t  reply[COMM_PAYLOAD_MAX];
    uint16_t reply_len = shell_dispatch_payload(slot.payload, slot.len, reply);

    frame_meta_t meta = {
        .addr        = 1,
        .cmd         = OP_SHELL_REPLY,
        .seq         = (uint8_t)(g_shell_reply_seq++ & 0xFFu),
        .ack_seq     = 0,
        .ack_status  = 0,
        .payload_len = (uint8_t)reply_len,
    };
    (void)frame_encode_s2m(&meta, reply, &g_tx_ring);
}

void handle_commission_clear(s_expr_tree_instance_t* inst,
                             const s_expr_param_t*   params,
                             uint16_t                param_count,
                             s_expr_event_type_t     event_type,
                             uint16_t                event_id,
                             void*                   event_data) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;

    bool ok = flash_storage_write(0, (uint8_t)COMMISSIONING_UNCOMMISSIONED);
    uint8_t status = ok ? 0u : 1u;

    if (ok) {
        g_instance_id         = 0;
        g_commissioning_state = COMMISSIONING_UNCOMMISSIONED;
    }

    emit_commission_reply(0, status);
    s_engine_log(inst, ok ? "COMMISSION_CLEAR done; rebooting..."
                          : "COMMISSION_CLEAR flash write FAILED; rebooting anyway");
    firmware_request_reboot(COMMISSION_REBOOT_DELAY_MS);
}

// ----------------------------------------------------------------------------
// send_pong — oneshot (o_call). Fires once per OP_PING event dispatch.
// Payload (8 B, little-endian): uptime_ms + monotonic pong_seq counter.
// Mirrors send_heartbeat's shape; cmd is OP_PONG (0x0005) on s2m.
// ----------------------------------------------------------------------------
#define PONG_PAYLOAD_LEN  8u

static uint32_t g_pong_seq = 0;

void send_pong(s_expr_tree_instance_t* inst,
               const s_expr_param_t*   params,
               uint16_t                param_count,
               s_expr_event_type_t     event_type,
               uint16_t                event_id,
               void*                   event_data) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;

    uint32_t uptime_ms = (uint32_t)board_millis();
    uint32_t seq       = g_pong_seq++;

    uint8_t payload[PONG_PAYLOAD_LEN];
    payload[0] = (uint8_t)(uptime_ms >>  0);
    payload[1] = (uint8_t)(uptime_ms >>  8);
    payload[2] = (uint8_t)(uptime_ms >> 16);
    payload[3] = (uint8_t)(uptime_ms >> 24);
    payload[4] = (uint8_t)(seq       >>  0);
    payload[5] = (uint8_t)(seq       >>  8);
    payload[6] = (uint8_t)(seq       >> 16);
    payload[7] = (uint8_t)(seq       >> 24);

    frame_meta_t meta = {
        .addr        = 1,
        .cmd         = OP_PONG,
        .seq         = (uint8_t)(seq & 0xFFu),
        .ack_seq     = 0,
        .ack_status  = 0,
        .payload_len = PONG_PAYLOAD_LEN,
    };
    (void)frame_encode_s2m(&meta, payload, &g_tx_ring);
}

// ----------------------------------------------------------------------------
// handle_internal_events — main (m_call). Top-level sibling above
// se_state_machine. Dispatches on engine-internal event ids (range
// 0xFE00+, never on the wire).
//
// Currently handles only EV_HOST_REATTACH: when main.c detects the host
// re-opened the CDC port (DTR transitioned low->high), it pushes this
// event. We respond by writing DONGLE_BOOT into the dongle_state
// blackboard field. se_state_machine, when invoked later in the same
// tick by function_interface, reads the freshly-written field and
// switches from OPERATIONAL back to BOOT — and BOOT's retry chain_flow
// resumes emitting OP_REGISTER until the new host session ACKs.
//
// Why an m_call instead of a DSL se_event_dispatch + se_chain_flow +
// se_set_field + se_log composition: collapses ~10 lines of DSL into
// ~5 lines of C, identical observable behavior. Atomic side-effect with
// no internal sequencing belongs in C; chain-level orchestration belongs
// in DSL. This is the former.
// ----------------------------------------------------------------------------
s_expr_result_t handle_internal_events(s_expr_tree_instance_t* inst,
                                       const s_expr_param_t*   params,
                                       uint16_t                param_count,
                                       s_expr_event_type_t     event_type,
                                       uint16_t                event_id,
                                       void*                   event_data) {
    (void)params; (void)param_count; (void)event_data;

    if (event_type == SE_EVENT_INIT || event_type == SE_EVENT_TERMINATE) {
        return SE_PIPELINE_CONTINUE;
    }

    if (event_id == EV_HOST_REATTACH) {
        // Reset dongle_state to BOOT (=0). dongle_state is the only field
        // in dongle_record so offset is 0; if the record grows, this
        // direct-pointer write becomes wrong — switch to
        // s_expr_blackboard_set_int_by_string().
        if (inst->blackboard) {
            *(int32_t*)inst->blackboard = 0;   // DONGLE_BOOT
        }
        s_engine_log(inst, "host reattach -> reset to BOOT");
    }

    // Stay active across ticks; lets function_interface advance to
    // se_state_machine in the same tick.
    return SE_PIPELINE_CONTINUE;
}

// ----------------------------------------------------------------------------
// toggle_led — main (m_call). Fires every tick (bare leaf under se_fork).
// PA17 is the Xiao user LED. Must return SE_PIPELINE_CONTINUE so the fork
// keeps the branch alive across ticks.
// ----------------------------------------------------------------------------
#define REGISTER_DONGLE_LED_PIN 17u

s_expr_result_t toggle_led(s_expr_tree_instance_t* inst,
                           const s_expr_param_t*   params,
                           uint16_t                param_count,
                           s_expr_event_type_t     event_type,
                           uint16_t                event_id,
                           void*                   event_data) {
    (void)inst; (void)params; (void)param_count;
    (void)event_id; (void)event_data;

    if (event_type == SE_EVENT_INIT || event_type == SE_EVENT_TERMINATE) {
        return SE_PIPELINE_CONTINUE;
    }

    PORT->Group[0].OUTTGL.reg = (1u << REGISTER_DONGLE_LED_PIN);
    return SE_PIPELINE_CONTINUE;
}
