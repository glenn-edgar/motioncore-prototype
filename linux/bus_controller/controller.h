// controller.h — the portable Layer-2 bus controller core (the "C routine" the
// procedure shell wraps). It owns the demux (and through it the link endpoint),
// and accretes capability across Plan 1 steps:
//
//   Step 1 (now): capture the attached dongle's identity from its REGISTER
//                 announcement; track link up/down.
//   Step 2:       sync ladder + auto-resync on link-up.
//   Step 3:       roster load/recall + push sweep list to the BC.
//
// Nothing here is USB- or Linux-specific: it talks only to link_endpoint.h, so
// the same object compiles for an M33/M7 dongle or a Zenoh node.

#pragma once

#include "link_endpoint.h"
#include "identity.h"
#include "demux.h"
#include "roster.h"
#include "cmd_tracker.h"

#ifdef __cplusplus
extern "C" {
#endif

// Protocol sync state, mirroring the firmware's four-layer ladder as observed
// from the host side.
typedef enum {
    PROTO_UNKNOWN = 0,   // link just up / reset — nothing known yet
    PROTO_BOOT,          // dongle is announcing REGISTER; ACK driven
    PROTO_L1_ACKED,      // REGISTER_ACK sent + GET_MANIFEST issued; awaiting reply
    PROTO_MANIFEST_OK,   // MANIFEST_REPLY seen; OPERATIONAL_BEGIN sent; awaiting heartbeat
    PROTO_OPERATIONAL,   // heartbeat seen — fully synced
} proto_state_t;

// Captured from OP_MANIFEST_REPLY: schema_hash(u32) fw_version(u32) m2s_count(u8).
typedef struct {
    int      valid;
    uint32_t schema_hash;
    uint32_t fw_version;
    uint8_t  m2s_count;
} manifest_t;

typedef struct controller controller_t;

// --- FFI event seam (concern 3) --------------------------------------------
// The C controller delivers events via a DRAINABLE TYPED QUEUE for the LuaJIT
// wrapper (queue+poll — avoids FFI callbacks aborting JIT traces, keeps the one
// event loop in control of when it re-enters Lua, and makes deferred replies
// clean). Native C harnesses keep using the callbacks above; both coexist.
#define CTRL_EV_DATA_MAX  64u    // per-event payload copy (v2 status = 55 B; results small)

typedef enum {
    CTRL_EV_CMD_DONE  = 1,  // submitted command finished — handle, addr, status, data=result bytes
    CTRL_EV_FLAGGED   = 2,  // interlock summary edge — addr, aux=flags (bit0 tripped)
    CTRL_EV_INTERLOCK = 3,  // interlock message (buffer 2) — addr, data=v2 status snapshot
    CTRL_EV_LIVENESS  = 4,  // slave down/up — addr, status=is_up, aux=class_id
    CTRL_EV_LINK      = 5,  // link state change — aux=link_state
} ctrl_ev_kind_t;

typedef struct {
    uint8_t        kind;       // ctrl_ev_kind_t
    uint8_t        addr;       // slave addr (0 if N/A)
    uint8_t        status;     // CMD_DONE: shell status / DEMUX_*; LIVENESS: is_up
    uint8_t        _pad;
    uint32_t       handle;     // CMD_DONE: the submit handle
    uint32_t       aux;        // FLAGGED: flags; LIVENESS: class_id; LINK: state
    const uint8_t *data;       // CMD_DONE/INTERLOCK: bytes, valid until the next drain
    uint16_t       data_len;
} ctrl_event_t;

// Create a controller bound to an existing (down or up) link endpoint. The
// controller installs the demux onto the endpoint, so do not bind the endpoint's
// handlers elsewhere. Returns NULL on alloc failure.
controller_t *controller_create(link_endpoint_t *ep);
void          controller_destroy(controller_t *c);

// Pump once. Call often (single-threaded event loop).
void          controller_poll(controller_t *c);

// --- observation -----------------------------------------------------------
link_state_t  controller_link_state(const controller_t *c);

// Sync progress. The controller drives the ladder automatically (Step 2): it
// ACKs REGISTER, fetches the manifest, sends OPERATIONAL_BEGIN, and confirms
// OPERATIONAL on the first heartbeat. On link-down — or if the dongle resets and
// re-announces REGISTER mid-session — it resets and re-runs the ladder with no
// caller involvement.
proto_state_t controller_proto(const controller_t *c);
const char   *controller_proto_name(proto_state_t s);
int           controller_is_operational(const controller_t *c);
const manifest_t *controller_manifest(const controller_t *c);

// Optional: invoked on every proto-state transition (may be NULL).
typedef void (*controller_proto_cb)(void *user, proto_state_t from, proto_state_t to);
void controller_set_proto_cb(controller_t *c, controller_proto_cb cb, void *user);

// --- Step 3: roster recall + push ------------------------------------------
// Roster provisioning state (the controller pushes the L2 roster down to the BC
// as soon as it reaches OPERATIONAL, and re-pushes after any reset/resync).
typedef enum {
    PROV_IDLE = 0,    // no roster attached, or waiting for OPERATIONAL
    PROV_CLEAR,       // CMD_BUS_CLEAR_ROSTER sent
    PROV_REGISTER,    // CMD_BUS_REGISTER_SLAVE in flight (one per slave)
    PROV_SET_POLL,    // CMD_BUS_SET_POLL sent
    PROV_LIST,        // CMD_BUS_LIST_SLAVES sent (verification read-back)
    PROV_DONE,        // BC roster matches the L2 roster
    PROV_FAIL,        // a CMD_BUS_* rejected, or role is not bus_controller
} prov_state_t;

// Attach the authoritative roster. The controller copies it; pass NULL to detach.
// Provisioning runs automatically once OPERATIONAL on a bus_controller-role dongle.
void          controller_attach_roster(controller_t *c, const roster_t *r);
prov_state_t  controller_prov_state(const controller_t *c);
const char   *controller_prov_name(prov_state_t s);
// Read-back from CMD_BUS_LIST_SLAVES after a successful push: how many slaves the
// BC reports in its sweep roster (0 until PROV_DONE).
uint8_t       controller_bc_roster_total(const controller_t *c);

// --- Step 4: liveness from the BC sweep ------------------------------------
// The BC's autonomous poll sweep escalates per-slave liveness edges to L2 as
// OP_BUS_SLAVE_DOWN (slave missed max_misses) / OP_BUS_SLAVE_UP (recovered).
// is_up = 1 for UP (class_id valid), 0 for DOWN (class_id 0).
typedef void (*controller_liveness_cb)(void *user, uint8_t addr, int is_up, uint32_t class_id);
void controller_set_liveness_cb(controller_t *c, controller_liveness_cb cb, void *user);

// Summary-bit escalation from the BC sweep (OP_BUS_SLAVE_FLAGGED): a slave's
// poll-terminator summary changed. flags bit0 = an armed interlock is tripped.
// Advisory — the slave already acted locally; L2 may read detail + notify.
typedef void (*controller_flagged_cb)(void *user, uint8_t addr, uint8_t flags);
void controller_set_flagged_cb(controller_t *c, controller_flagged_cb cb, void *user);

// Start/stop the BC autonomous poll sweep (CMD_BUS_POLL_ENABLE). Reply ignored.
// Returns the request_id (0xFFFF on error).
uint16_t controller_set_poll_enable(controller_t *c, int on);

// --- shell command path ----------------------------------------------------
// Send an OP_SHELL_EXEC and resolve the reply via on_reply (correlated by
// request_id). controller_send_shell targets the dongle itself; ..._to targets
// an RS-485 slave by addr — while the sweep is enabled the BC injects it into a
// poll slot (Stage 3c), so liveness polling and command/reply coexist.
// Returns the request_id (0xFFFF on error).
uint16_t controller_send_shell(controller_t *c, uint16_t command_id,
                               const uint8_t *args, uint16_t args_len,
                               demux_reply_cb on_reply, void *reply_user);
uint16_t controller_send_shell_to(controller_t *c, uint8_t dest_addr, uint16_t command_id,
                                  const uint8_t *args, uint16_t args_len,
                                  demux_reply_cb on_reply, void *reply_user);

// --- Step 6a: L2 command tracker (per-slave queue + availability) -----------
// Submit a command to a roster slave through the L2 tracker: it is enqueued
// per-slave (depth CMD_QUEUE_DEPTH), at most one in flight per slave, and
// completed via on_done (correlated by the returned handle, not the wire id).
// Returns a monotonic handle (>0), or 0 on backpressure / unknown addr / args too
// big. The slot set is (re)built from the attached roster. exec_timeout_ms is
// carried through now; 6b puts it on the wire + makes it the execution deadline.
uint32_t controller_submit_command(controller_t *c, uint8_t addr, uint16_t command_id,
                                   const uint8_t *args, uint16_t args_len,
                                   uint32_t exec_timeout_ms,
                                   cmd_done_cb on_done, void *user);
// --- FFI event seam: submit-by-event + drain (the LuaJIT wrapper path) ------
// Submit a command WITHOUT a caller callback: the completion arrives as a
// CTRL_EV_CMD_DONE event keyed by the returned handle. Same queue/timeout/gating
// as controller_submit_command. Returns a monotonic handle (>0) or 0.
uint32_t controller_submit_command_ev(controller_t *c, uint8_t addr, uint16_t command_id,
                                      const uint8_t *args, uint16_t args_len,
                                      uint32_t exec_timeout_ms);
// Ungated variant (clear/safety/diagnostic lane): bypasses the per-slave queue +
// the FAULTED interlock gate, going straight to the demux (so it works even while
// the slave is tripped — that's how status-read + recovery reach a faulted slave).
// Completion is a CTRL_EV_CMD_DONE with the high bit set in the handle.
uint32_t controller_submit_command_ungated_ev(controller_t *c, uint8_t addr,
                                              uint16_t command_id,
                                              const uint8_t *args, uint16_t args_len);
// Drain one queued event into *out. Returns 1 (got one) or 0 (empty). out->data
// points into a controller-owned buffer valid only until the next drain — copy now.
int controller_drain(controller_t *c, ctrl_event_t *out);

// Per-slave availability + pending-queue depth (0 if addr is not a roster slave).
cmd_slot_state_t controller_slave_state(const controller_t *c, uint8_t addr);
uint8_t          controller_slave_qdepth(const controller_t *c, uint8_t addr);
// 6b: total command-ACKs seen (each = a slave freed the bus before executing).
uint32_t         controller_total_acks(const controller_t *c);

// 7a: read the L2 interlock buffer for a slave. Returns 1 tripped / 0 ok / -1
// unknown (no flagged update seen yet); *flags (if non-NULL) gets the raw summary
// flags. While tripped, the slave's command queue is held (state -> FAULTED); a
// clear/safety command can still be sent via controller_send_shell_to (ungated).
int              controller_interlock_state(const controller_t *c, uint8_t addr, uint8_t *flags);

// 7b-1: read the latest async interlock MESSAGE pushed by a slave (buffer 2 — the
// v2 status snapshot, same layout as a CMD_INTERLOCK_STATUS reply). Returns its
// length (0 if none); copies up to cap bytes into out; *count (if non-NULL) gets
// the total received from addr (so a client can tell a fresh push from a repeat).
uint16_t         controller_interlock_msg(const controller_t *c, uint8_t addr,
                                          uint8_t *out, uint16_t cap, uint32_t *count);

// 7b-2: total BC status-report snapshots received (the reliable index that heals
// lost edges + establishes the confirmed-ok baseline). >0 means the periodic
// refresh is flowing.
uint32_t         controller_total_status_reports(const controller_t *c);

// 7b-3: total reconciliation re-pushes sent (each = a detected gap — index says
// tripped but the matching buffer-2 message was missing — that the controller
// healed by poking the slave to re-emit). The reconciliation runs automatically in
// controller_poll; this is purely observation.
uint32_t         controller_total_repushes(const controller_t *c);

// 1 once a REGISTER has been parsed since the last link-up; 0 otherwise
// (identity is cleared on link-down and re-learned on reconnect).
int                      controller_has_identity(const controller_t *c);
const dongle_identity_t *controller_identity(const controller_t *c);
dongle_role_t            controller_role(const controller_t *c);

#ifdef __cplusplus
}
#endif
