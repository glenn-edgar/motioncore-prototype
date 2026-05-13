// register_dongle opcodes.
// Locally defined for the dongle-registration milestone (Phase 2 merge of
// blink_engine + blink_frame + register_dongle chain). Lives alongside the
// vendored libcomm slice and is included only by user_functions.c.
//
// Opcodes here are app-private; the canonical libcomm catalogue
// (comm.h::comm_link_cmd_t) is reserved for link-control. These OP_*
// values are payload commands carried in frame_meta_t.cmd on s2m frames.

#pragma once

#include <stdint.h>

#define OP_REGISTER   ((uint16_t)0x0001)  // s2m: dongle boot announcement
#define OP_HEARTBEAT  ((uint16_t)0x0002)  // s2m: periodic alive ping
