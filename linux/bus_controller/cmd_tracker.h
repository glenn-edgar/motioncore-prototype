// cmd_tracker.{c,h} — Plan 1 Step 6a. The Layer-2 command tracker: per-slave
// availability state + a bounded command queue, sitting ABOVE the demux's
// request_id correlation.
//
// This is the foundation the rest of Step 6 / the interlock model rides on:
//   6a (now): per-slave queue (depth CMD_QUEUE_DEPTH), one command in flight per
//             slave (L2-enforced), monotonic completion handle, backpressure.
//             Completion = the demux reply (or its timeout / link-down). The
//             per-command exec_timeout_ms is carried through the API and stored,
//             but NOT yet the controlling deadline — the demux global timeout is
//             the backstop here.
//   6b:       slave ISR ACK/NAK frees the bus; exec_timeout_ms goes ON THE WIRE
//             (slave self-abort) and becomes the L2 execution deadline (the two-
//             timeout model). The tracker's SLOT states gain SENT->ACK->BUSY.
//   7a:       CMD_SLOT_FAULTED gates the queue when a slave's interlock trips.
//
// Pure portable C: depends only on demux.h, so it lifts to an M33/M7 unchanged.

#pragma once

#include <stdint.h>
#include "demux.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CMD_QUEUE_DEPTH   5     // per-slave queued commands (excl. the in-flight one)
#define CMD_ARG_MAX       96    // max args bytes carried per queued command

// Per-slave availability — observable by schedulers + clients.
typedef enum {
    CMD_SLOT_IDLE     = 0,   // nothing in flight; queue may hold waiting commands
    CMD_SLOT_INFLIGHT = 1,   // one command sent, awaiting its reply (6a: reply=done)
    CMD_SLOT_FAULTED  = 2,   // reserved for 7a: interlock-tripped gating (unused in 6a)
} cmd_slot_state_t;

// Completion callback. status: 0..5 firmware shell status, or DEMUX_STATUS_TIMEOUT
// / DEMUX_STATUS_LINK_DOWN. result/result_len are valid only during the call.
typedef void (*cmd_done_cb)(void *user, uint32_t handle, uint8_t addr,
                            uint8_t status, const uint8_t *result, uint16_t result_len);

typedef struct cmd_tracker cmd_tracker_t;

// Create a tracker that sends through `dx`. No slots until cmd_tracker_add_slave.
// Returns NULL on alloc failure.
cmd_tracker_t *cmd_tracker_create(demux_t *dx);
void           cmd_tracker_destroy(cmd_tracker_t *t);

// Drop the whole slot set (in-flight + queued commands are completed with
// `abort_status` so callers are never orphaned). Call before re-adding slaves on
// (re)provisioning / roster change.
void cmd_tracker_reset(cmd_tracker_t *t, uint8_t abort_status);

// Register a slave addr as a schedulable slot. Idempotent. Returns 0, or -1 if the
// slot table is full.
int  cmd_tracker_add_slave(cmd_tracker_t *t, uint8_t addr);

// Submit a command to a slave: enqueued (depth CMD_QUEUE_DEPTH), one in flight per
// slave, completed via on_done. Returns a monotonic handle (>0), or 0 on:
// unknown addr / args_len > CMD_ARG_MAX / queue full (backpressure).
uint32_t cmd_tracker_submit(cmd_tracker_t *t, uint8_t addr, uint16_t command_id,
                            const uint8_t *args, uint16_t args_len,
                            uint32_t exec_timeout_ms,
                            cmd_done_cb on_done, void *user);

// Pump every slot: if IDLE with a non-empty queue, send the next command. Submit
// auto-pumps too, so this mainly retries sends that failed transiently (link down /
// pending table full) and advances slots freed by a completion. Call often.
void cmd_tracker_poll(cmd_tracker_t *t);

// Observation.
cmd_slot_state_t cmd_tracker_state(const cmd_tracker_t *t, uint8_t addr);
uint8_t          cmd_tracker_qdepth(const cmd_tracker_t *t, uint8_t addr);

#ifdef __cplusplus
}
#endif
