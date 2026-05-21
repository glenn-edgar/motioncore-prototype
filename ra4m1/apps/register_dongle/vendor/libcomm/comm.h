// libcomm/comm.h
// Public C API for libcomm — the multi-bus, multi-dongle, request/response
// link layer between chain-tree and physics slaves. Phase 1: in-process
// transport only; UART transport stubbed for ABI symmetry with future dongle.
//
// Full design rationale lives in project memory
// (project_ros_planner_robot_pipe.md). This header is the wire-API contract
// chain-tree (via comm_ffi.lua) and the comm subtree both bind to.

#pragma once

#include <stdint.h>
#include <stddef.h>

#include "bus_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============ CONSTANTS ============
// Tunable sizes (COMM_PAYLOAD_MAX, COMM_HANDLES_MAX, COMM_DONGLES_MAX,
// COMM_BUSES_MAX, COMM_SLAVES_MAX, CT_COMM_RX_PERIOD_MS) live in
// bus_config.h so per-target builds can override them with -D.

// Handle layout: slot in low 5 bits, generation in high 27 bits.
#define COMM_HANDLE_SLOT_BITS          5
#define COMM_HANDLE_SLOT_MASK       0x1Fu
#define COMM_HANDLE_GEN_SHIFT          5

// Address space.
#define COMM_ADDR_MASTER             0x00
#define COMM_ADDR_SLAVE_MIN          0x01
#define COMM_ADDR_SLAVE_MAX          0xFC
#define COMM_ADDR_BROADCAST          0xFD
#define COMM_ADDR_DONGLE_SELF        0xFE   // also used for dongle handshake
#define COMM_ADDR_COMMISSIONING_ONLY 0xFF

// ack_status header-byte flags (s→m frames).
#define COMM_ACK_FLAG_URGENT         0x80

// Broadcast responder = no slave responds (fire-and-forget; ESTOP-class only).
#define COMM_RESPONDER_NONE          0x00

// Sentinel handle: never issued by comm_submit.
#define COMM_HANDLE_INVALID            0u

// NAK reasons libcomm itself emits when no slave handler is registered or
// the handler did not respond. Slave classes own the rest of the byte
// space (0x00..0xFC) for their own catalogue-defined reasons.
#define COMM_NAK_REASON_NO_HANDLER   0xFD
#define COMM_NAK_REASON_NO_RESPONSE  0xFE
#define COMM_NAK_REASON_UNKNOWN_CMD  0xFF

// ============ DONGLE IDENTITY ============
// Programmed externally by a TBD dongle-commissioning tool that runs
// off-robot. Output (dongles.json or equivalent) is baked into containers
// at build time; chain_tree reads it at startup and never writes it.
//
// Identity = (dongle_type, dongle_instance), both uint16. Packed into
// the existing manifest_dongle.dongle_uuid[16] field (bytes 0-1 = type
// little-endian, bytes 2-3 = instance little-endian, bytes 4-15 zero).
// Schema bump to a dedicated field deferred to a follow-up slice.
//
// dongle_type = role / capability class (drive_base, arm, lidar, …).
// dongle_instance = which of N peers of that type (1, 2, 3, …).

typedef enum {
    COMM_DONGLE_TYPE_RESERVED   = 0,   // never legal — catches blank dongles
    COMM_DONGLE_TYPE_DRIVE_BASE = 1,
    // future: ARM, LIDAR, CHARGING_STATION, …
} comm_dongle_type_t;

// Pack/unpack helpers. Operate on the manifest's 16-byte uuid field
// (or any equivalent buffer).
static inline uint16_t comm_dongle_get_type(const uint8_t *uuid) {
    return (uint16_t)(uuid[0]) | ((uint16_t)(uuid[1]) << 8);
}
static inline uint16_t comm_dongle_get_instance(const uint8_t *uuid) {
    return (uint16_t)(uuid[2]) | ((uint16_t)(uuid[3]) << 8);
}
static inline void comm_dongle_set_type(uint8_t *uuid, uint16_t t) {
    uuid[0] = (uint8_t)(t & 0xFF);
    uuid[1] = (uint8_t)((t >> 8) & 0xFF);
}
static inline void comm_dongle_set_instance(uint8_t *uuid, uint16_t i) {
    uuid[2] = (uint8_t)(i & 0xFF);
    uuid[3] = (uint8_t)((i >> 8) & 0xFF);
}

// Dongle attachment spec — what the orchestrator builds from dongles.json
// and hands to comm_init_with_dongles. One entry per dongle the chain_tree
// is responsible for.
typedef struct {
    const char *path;             // "/dev/pts/N" (sim) or "/dev/ttyUSBn" (prod)
    uint16_t    dongle_type;
    uint16_t    dongle_instance;
} comm_dongle_attach_t;

// ============ TYPES ============

typedef uint32_t comm_handle_t;
typedef uint16_t comm_cmd_t;

typedef enum {
    COMM_OK                     =   0,
    COMM_ERR_INIT               =  -1,  // not initialised / not implemented yet
    COMM_ERR_BUS                =  -2,  // submit while previous outstanding to this node
    COMM_ERR_BAD_MANIFEST       =  -3,  // manifest validation failed
    COMM_ERR_DONGLE_MISSING     =  -4,  // declared dongle not enumerated
    COMM_ERR_DONGLE_UNEXPECTED  =  -5,  // enumerated dongle not declared
    COMM_ERR_BAD_HANDLE         =  -6,  // stale or never-issued handle (gen mismatch)
    COMM_ERR_NO_DATA            =  -7,  // status queried before completion
    COMM_ERR_TIMEOUT            =  -8,  // master-side timeout fired
    COMM_ERR_NAK                =  -9,  // slave NAK'd
    COMM_ERR_FAULT              = -10,  // fault raised on this slave
    COMM_ERR_OVERFLOW           = -11,  // poll buffer too small / urgent ring full
    COMM_ERR_BAD_ARG            = -12,  // invalid argument
} comm_result_t;

typedef enum {
    COMM_NODE_UNKNOWN = 0,   // never seen / not declared
    COMM_NODE_PENDING = 1,   // joining (JOIN_REQ → JOIN_ACK in flight)
    COMM_NODE_LIVE    = 2,   // joined, accepting traffic
    COMM_NODE_FAULTED = 3,   // hard-stopped per feedback_no_soft_faults
} comm_node_state_t;

// Class 0x00xx link-control codes. Higher classes (0x01xx uplink,
// 0x02xx downlink, 0x03xx immediate) are deferred to a later catalogue.
typedef enum {
    COMM_CMD_NULL          = 0x0000,
    COMM_CMD_PING          = 0x0001,
    COMM_CMD_ACK_BARE      = 0x0002,   // s→m only; m→s is malformed
    COMM_CMD_NAK           = 0x0003,   // s→m only; payload {reason u8}
    COMM_CMD_DRAIN         = 0x0004,   // m→s after ACK_FLAG_URGENT
    COMM_CMD_JOIN_REQ      = 0x0005,
    COMM_CMD_JOIN_ACK      = 0x0006,
    COMM_CMD_JOIN_CONFIRM  = 0x0007,   // payload includes physics_model_id
    COMM_CMD_TIME_SYNC     = 0x0008,   // m→s broadcast, payload {master_us u64}
    COMM_CMD_HALT          = 0x0009,   // m→s broadcast
    COMM_CMD_RESUME        = 0x000A,   // m→s broadcast
    COMM_CMD_GO            = 0x000B,   // m→s broadcast
    COMM_CMD_ESTOP         = 0x000C,   // m→s broadcast
    COMM_CMD_DONGLE_HELLO  = 0x000D,   // host → addr=0xFE, payload {host_epoch u32}
    COMM_CMD_DONGLE_IDENT  = 0x000E,   // addr=0xFE → host, payload {uuid[16] fw_ver bus_count bus_local_ids[8] caps}
} comm_link_cmd_t;

// One completed request/response surfaced by comm_poll.
typedef struct {
    comm_handle_t handle;
    comm_cmd_t    cmd;
    uint8_t       mcu;
    uint8_t       ack_status;            // includes COMM_ACK_FLAG_URGENT
    uint16_t      seq;
    uint16_t      payload_len;
    uint32_t      elapsed_ms;
    uint8_t       payload[COMM_PAYLOAD_MAX];
} comm_event_t;

typedef void (*comm_logger_fn)(int level, const char* msg);

// Slave-class handler. Called by libcomm from comm_poll's slave loop
// when an m2s frame finishes decoding for a registered mcu.
//
// Inside the handler, the slave-class adapter must do EXACTLY ONE of:
//   - call comm_slave_respond(mcu, in_seq, ...)  emit a custom response
//   - call comm_slave_ack_bare(mcu, in_seq)      shorthand for ACK_BARE
//   - call comm_slave_nak(mcu, in_seq, reason)   shorthand for NAK
// If the handler returns without responding, libcomm auto-emits a NAK
// with reason = COMM_NAK_REASON_NO_RESPONSE so the master never hangs.
//
// Re-entrancy: the handler runs synchronously inside comm_poll. The
// handler MUST NOT call comm_poll, comm_submit, or comm_shutdown.
// Calling comm_slave_respond/ack_bare/nak from inside the handler is
// the only legal libcomm interaction.
typedef void (*comm_slave_handler_fn)(uint8_t        mcu,
                                       comm_cmd_t     cmd,
                                       const uint8_t *payload,
                                       uint16_t       payload_len,
                                       uint8_t        in_seq,
                                       void          *ctx);

// ============ HANDLE LAYOUT HELPERS ============

static inline uint8_t  comm_handle_slot(comm_handle_t h) {
    return (uint8_t)(h & COMM_HANDLE_SLOT_MASK);
}
static inline uint32_t comm_handle_gen (comm_handle_t h) {
    return h >> COMM_HANDLE_GEN_SHIFT;
}

// ============ LIFECYCLE ============

// Validates manifest blob, builds router table, opens transports.
// Returns COMM_OK on success, specific error otherwise.
comm_result_t comm_init    (const uint8_t* manifest_blob, size_t blob_len);
void          comm_shutdown(void);

// Phase B entry point: chain_tree opens N pre-existing pty/tty paths
// (each created by an external owner — robot_sim in sim, the kernel's
// USB serial driver in prod). Each spec carries the path plus the
// expected (dongle_type, dongle_instance) for the dongle behind that
// path. comm_init_with_dongles:
//   1. Validates the manifest blob.
//   2. For each spec, opens the path with cfmakeraw + flock(LOCK_EX).
//      Open or flock failure → hard fault (COMM_ERR_BAD_ARG / _BUS).
//   3. Sends CMD_DONGLE_HELLO and waits up to manifest.tunables.join_timeout_ms
//      for a CMD_DONGLE_IDENT response carrying the matching (type, instance).
//      Wrong identity, no response, or malformed reply → hard fault
//      (COMM_ERR_DONGLE_UNEXPECTED / _MISSING).
//   4. Binds the dongle to the corresponding manifest dongle_idx (matched
//      by (type, instance) declared in manifest_dongle.dongle_uuid[]).
// On any failure the partial state is torn down before return.
//
// dongles.json (the orchestrator's input — generated by the TBD dongle
// commissioning tool, baked into containers, never written at runtime)
// is the source of the spec list. Order of specs[] does not have to
// match manifest dongle order; matching is by (type, instance).
comm_result_t comm_init_with_dongles(const uint8_t              *manifest_blob,
                                     size_t                      blob_len,
                                     const comm_dongle_attach_t *specs,
                                     size_t                      n_specs);


// Bind a Lua-side physics_core handle to an in-process slave declared
// in the manifest. Only legal for dongle_idx pointing at the virtual
// host dongle (UUID = all zeros). physics_handle is opaque to libcomm;
// it gets handed back to the slave-tick callback when frames arrive.
comm_result_t comm_attach_internal(uint8_t dongle_idx,
                                   uint8_t bus_id,
                                   uint8_t addr,
                                   void*   physics_handle);

// ============ ASYNC SUBMIT / CLAIM / CANCEL ============

// Returns a fresh handle on success and writes COMM_OK to *err if non-NULL.
// On failure returns COMM_HANDLE_INVALID and writes the cause to *err.
// Depth-1 per node: a second submit while one is outstanding to the same
// slave returns COMM_ERR_BUS.
comm_handle_t comm_submit          (uint8_t mcu,
                                    comm_cmd_t cmd,
                                    const uint8_t* payload, uint16_t payload_len,
                                    comm_result_t* err);

comm_handle_t comm_submit_broadcast(uint8_t bus_id,
                                    comm_cmd_t cmd,
                                    uint8_t responder,
                                    const uint8_t* payload, uint16_t payload_len,
                                    comm_result_t* err);

// Non-blocking poll of a handle's slot. Returns COMM_OK if claim is
// ready, COMM_ERR_NO_DATA if still pending, or a fault/timeout/nak code.
comm_result_t comm_status(comm_handle_t h);

// Consume the response, freeing the slot. Bumps the generation counter.
comm_result_t comm_claim (comm_handle_t h, comm_event_t* out);

// Tear-down: cancel outstanding request, free slot, mark gen.
comm_result_t comm_cancel(comm_handle_t h);

// ============ SERVICE TICK ============

// MUST be called every chain-tree tick before any comm_status calls.
// Drains all per-bus RX rings, advances link FSMs, fires timeouts,
// reaps stale DONE/TIMEOUT/FAULT/NAK slots after grace. Writes up to
// `max` completed events to `out`; returns count written, or a negative
// comm_result_t value on error.
int comm_poll(comm_event_t* out, int max);

// ============ SLAVE HANDLER REGISTRATION (slice 2a) ============
// Register a slave-class handler for an mcu. fn = NULL clears the
// registration; the mcu then falls back to libcomm's built-in default
// (PING -> ACK_BARE; everything else -> NAK reason 0xFF).
// Returns COMM_OK or COMM_ERR_BAD_ARG (mcu unknown / not initialised).
comm_result_t comm_set_slave_handler(uint8_t                mcu,
                                     comm_slave_handler_fn  fn,
                                     void                  *ctx);

// Emit a master-response frame from inside a slave handler. `in_seq`
// MUST be the seq the handler was passed (this is what the master
// matches the response to). Returns COMM_OK on success, COMM_ERR_INIT
// if not in a handler context, COMM_ERR_BAD_ARG on misuse,
// COMM_ERR_OVERFLOW if the s2m ring is full.
comm_result_t comm_slave_respond(uint8_t        mcu,
                                 uint8_t        in_seq,
                                 comm_cmd_t     cmd,
                                 const uint8_t *payload,
                                 uint16_t       payload_len,
                                 uint8_t        ack_status);

comm_result_t comm_slave_ack_bare(uint8_t mcu, uint8_t in_seq);
comm_result_t comm_slave_nak     (uint8_t mcu, uint8_t in_seq, uint8_t reason);

// ============ DIAGNOSTICS ============

comm_node_state_t comm_node_state         (uint8_t mcu);
uint32_t          comm_node_physics_model (uint8_t mcu);
uint8_t           comm_node_miss_count    (uint8_t mcu);
uint32_t          comm_node_last_seen_ms  (uint8_t mcu);

// ============ COMMISSIONING ============
// No-op in this port (in-process slaves only). Defined for ABI symmetry
// with the future dongle path; runtime handlers are stubs.

int           comm_pending_commission(uint8_t out_uuid[16]);
comm_result_t comm_commission_assign (const uint8_t uuid[16],
                                      uint8_t dongle_idx,
                                      uint8_t bus_id,
                                      uint8_t addr);
comm_result_t comm_commission_clear  (const uint8_t uuid[16]);

// ============ LOGGING ============
// Levels: 0=ERR, 1=WARN, 2=INFO, 3=DEBUG. Default = NULL fn (silent).

void comm_set_logger(comm_logger_fn fn, int level);

// ============ CLOCK ============
// CLOCK_MONOTONIC milliseconds, truncated to 32 bits. Wraps every ~49 days
// — fine for elapsed-time math; subtraction across the wrap is correct.

uint32_t comm_now_ms(void);

#ifdef __cplusplus
}
#endif
