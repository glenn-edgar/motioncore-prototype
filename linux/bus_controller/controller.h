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

// 1 once a REGISTER has been parsed since the last link-up; 0 otherwise
// (identity is cleared on link-down and re-learned on reconnect).
int                      controller_has_identity(const controller_t *c);
const dongle_identity_t *controller_identity(const controller_t *c);
dongle_role_t            controller_role(const controller_t *c);

#ifdef __cplusplus
}
#endif
