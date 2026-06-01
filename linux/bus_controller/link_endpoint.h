// link_endpoint.h — the tier-portability seam for the Layer-2 bus controller.
//
// Layer 2 (the portable controller routine) never knows what it runs on. It
// talks only to an abstract *link endpoint*: send a frame, get frames back,
// learn when the link goes up/down. Everything transport-specific lives BELOW
// this seam in a concrete implementation:
//
//   - Pi tier:      USB-CDC link manager (usb_link.c) — SLIP framing + termios
//   - M33/M7 tier:  a function-call / shared-memory shim (no SLIP)
//   - Zenoh tier:   a network shim
//
// The interface is IDENTICAL on every tier; only the impl below it changes.
// Because the seam deals in DECODED frames (frame_meta_t + payload), SLIP/CRC
// framing is an implementation detail of the USB impl and does not leak up.
//
// Direction note: a bus controller is always the MASTER on link A. It SENDS
// master->slave (m2s) frames and RECEIVES slave->master (s2m) frames. A concrete
// endpoint bakes that direction in.

#pragma once

#include "vendor/libcomm/frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LINK_DOWN = 0,   // no usable connection to the device right now
    LINK_UP   = 1,   // device open and readable/writable
} link_state_t;

typedef struct link_endpoint link_endpoint_t;

// Delivered once per fully-decoded inbound (s2m) frame. meta/payload are owned
// by the endpoint and valid only for the duration of the callback — copy if you
// need to keep them. payload may be NULL when meta->payload_len == 0.
typedef void (*link_frame_cb)(void *user,
                              const frame_meta_t *meta,
                              const uint8_t *payload);

// Delivered on every link-state transition (edge, not level). The controller
// re-runs its sync ladder on LINK_UP; nothing safety-critical lives here.
typedef void (*link_state_cb)(void *user, link_state_t state);

// ---- vtable: how a concrete endpoint plugs into the seam -------------------
struct link_vtable {
    // Encode+enqueue one m2s frame for transmission. Returns 0 on success,
    // -1 if the link is down or the outbound buffer is full (caller may retry).
    int  (*send)(link_endpoint_t *ep, const frame_meta_t *meta, const uint8_t *payload);
    // Pump the endpoint once: service RX, flush TX, run reconnect/backoff, and
    // fire callbacks. Single-threaded event-loop model — call it often.
    void (*poll)(link_endpoint_t *ep);
    // Tear down and free. Safe to call once; *ep is invalid afterwards.
    void (*close)(link_endpoint_t *ep);
};

// Common header every concrete endpoint embeds as its first member, so the
// generic inline wrappers below can dispatch and read state uniformly.
struct link_endpoint {
    const struct link_vtable *vt;
    link_frame_cb  on_frame;
    link_state_cb  on_state;
    void          *user;
    link_state_t   state;
};

// ---- generic seam API (tier-agnostic) --------------------------------------
static inline int link_send(link_endpoint_t *ep,
                            const frame_meta_t *meta,
                            const uint8_t *payload) {
    return ep->vt->send(ep, meta, payload);
}
static inline void link_poll(link_endpoint_t *ep)  { ep->vt->poll(ep); }
static inline void link_close(link_endpoint_t *ep) { ep->vt->close(ep); }
static inline link_state_t link_get_state(const link_endpoint_t *ep) { return ep->state; }

#ifdef __cplusplus
}
#endif
