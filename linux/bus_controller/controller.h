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

// 1 once a REGISTER has been parsed since the last link-up; 0 otherwise
// (identity is cleared on link-down and re-learned on reconnect).
int                      controller_has_identity(const controller_t *c);
const dongle_identity_t *controller_identity(const controller_t *c);
dongle_role_t            controller_role(const controller_t *c);

#ifdef __cplusplus
}
#endif
