// register_dongle opcodes.
// Locally defined for the dongle-registration milestone (Phase 2 merge of
// blink_engine + blink_frame + register_dongle chain). Lives alongside the
// vendored libcomm slice and is included only by user_functions.c.
//
// Opcodes here are app-private; the canonical libcomm catalogue
// (comm.h::comm_link_cmd_t) is reserved for link-control. These OP_*
// values are payload commands carried in frame_meta_t.cmd.
//
// Allocation rule (locked 2026-05-13):
//   * s2m opcodes (outgoing; only used as frame_meta_t.cmd, never dispatched
//     on by the engine):  0x0001-0x00FF
//   * m2s opcodes (incoming; pushed to engine event_queue and matched against
//     by se_event_dispatch — MUST avoid SE_EVENT_TICK=4 / SE_EVENT_INIT=0xfffe
//     / SE_EVENT_TERMINATE=0xfffd):  0x0100+

#pragma once

#include <stdint.h>

// ----- s2m (dongle -> host) -----
#define OP_REGISTER       ((uint16_t)0x0001)  // dongle boot announcement
#define OP_HEARTBEAT      ((uint16_t)0x0002)  // periodic alive ping
#define OP_PONG           ((uint16_t)0x0005)  // response to host's OP_PING
#define OP_DBG_LOG        ((uint16_t)0x0010)  // se_log output (UTF-8 text payload)

// ----- m2s (host -> dongle) -----
#define OP_REGISTER_ACK   ((uint16_t)0x0103)  // host acknowledges OP_REGISTER
#define OP_PING           ((uint16_t)0x0104)  // host pings dongle

// ----- engine-internal events (never appear on the wire) -----
// Range 0xFE00-0xFEFF. main.c (or other firmware-internal code) pushes
// these to the engine event_queue; chains dispatch on them. They MUST be
// disjoint from any cmd value that comes in over libcomm so the chain
// can't be tricked by a malicious host into faking an internal event.
#define EV_HOST_REATTACH  ((uint16_t)0xFE00)  // host closed and reopened CDC
