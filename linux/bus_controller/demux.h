// demux.h — Plan 1 Step 0b. The reply-vs-async demultiplexer that sits on top of
// the link endpoint.
//
// One RX path. Every inbound s2m frame is dispatched by opcode:
//   * OP_SHELL_REPLY  -> correlated to a pending request by its request_id and
//                        resolved (reply callback fires once, then the slot frees)
//   * everything else -> the async/event router (REGISTER, HEARTBEAT, DBG_LOG,
//                        EVENT, NAK, MANIFEST_REPLY, BUS_SLAVE_*, ...)
//   * a reply whose request_id matches no pending request -> dropped + logged
//                        (this is what kills the id-reuse mis-correlation class
//                        of bug: a stale reply can never latch onto a live req)
//
// request_ids are allocated MONOTONICALLY and never reused.

#pragma once

#include <stdint.h>
#include "link_endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

// Synthetic statuses delivered through demux_reply_cb for non-firmware outcomes.
// Firmware shell statuses are 0..5 (shell_status_t); these sit above that range.
#define DEMUX_STATUS_TIMEOUT    0xFFu   // no reply within the sweep timeout
#define DEMUX_STATUS_LINK_DOWN  0xFEu   // link went down with the request in flight

typedef struct demux demux_t;

// Resolution of a correlated shell command. result/result_len are valid only for
// the duration of the call. For TIMEOUT / LINK_DOWN, result is NULL / len 0.
typedef void (*demux_reply_cb)(void *user, uint16_t request_id, uint8_t status,
                               const uint8_t *result, uint16_t result_len);

// Any non-reply inbound frame (async stream). meta/payload valid only in-call.
typedef void (*demux_event_cb)(void *user,
                               const frame_meta_t *meta, const uint8_t *payload);

// Link up/down passthrough. The demux clears in-flight pendings on DOWN
// (resolving each with DEMUX_STATUS_LINK_DOWN) before invoking this.
typedef void (*demux_state_cb)(void *user, link_state_t state);

// Bind a demux onto an existing link endpoint. The demux installs itself as the
// endpoint's frame+state handler, so create it AFTER the endpoint exists and do
// not set the endpoint's handlers elsewhere. Returns NULL on alloc failure.
demux_t *demux_create(link_endpoint_t *ep,
                      demux_event_cb on_event,
                      demux_state_cb on_state,
                      void *user);
void     demux_destroy(demux_t *d);

// Pump: services the link endpoint (RX/TX/reconnect) and sweeps request timeouts.
// timeout_ms == 0 disables timeout sweeping. Call often.
void     demux_poll(demux_t *d, uint32_t timeout_ms);

// Send a bare m2s opcode with optional payload (sync ladder, ping, etc.). No
// request_id, no pending entry. Returns 0 on success, -1 if the link can't take it.
int      demux_send_raw(demux_t *d, uint16_t opcode,
                        const uint8_t *payload, uint16_t len);

// Send OP_SHELL_EXEC: allocates a monotonic request_id, registers a pending
// entry resolved by on_reply, and frames [request_id u16][command_id u16][args].
// Returns the allocated request_id, or 0xFFFF on error (link down / table full).
// demux_send_shell targets the dongle itself (frame addr 0); demux_send_shell_to
// targets an RS-485 slave by addr (the BC bridges/injects it onto the bus). The
// reply correlates by request_id regardless of which node answered.
uint16_t demux_send_shell(demux_t *d, uint16_t command_id,
                          const uint8_t *args, uint16_t args_len,
                          demux_reply_cb on_reply, void *reply_user);
uint16_t demux_send_shell_to(demux_t *d, uint8_t dest_addr, uint16_t command_id,
                             const uint8_t *args, uint16_t args_len,
                             demux_reply_cb on_reply, void *reply_user);

// 6b-ii: like demux_send_shell_to but as an OP_BUS_EXEC bus command carrying a
// per-command exec_timeout_ms on the wire (the slave self-aborts on overrun).
// payload = [exec_timeout_ms u16][request_id u16][command_id u16][args]. The reply
// is still OP_SHELL_REPLY correlated by request_id. Bus-only — never sent to the
// dongle itself (addr 0), so the USB shell schema is unchanged.
uint16_t demux_send_bus_exec(demux_t *d, uint8_t dest_addr, uint16_t command_id,
                             uint16_t exec_timeout_ms,
                             const uint8_t *args, uint16_t args_len,
                             demux_reply_cb on_reply, void *reply_user);

#ifdef __cplusplus
}
#endif
