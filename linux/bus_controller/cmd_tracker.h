// cmd_tracker.{c,h} — Plan 1 Step 6a/6b. The Layer-2 command tracker: per-slave
// availability state + a bounded command queue, sitting ABOVE the demux's
// request_id correlation.
//
//   6a: per-slave queue (depth CMD_QUEUE_DEPTH), one command in flight per slave
//       (L2-enforced), monotonic completion handle, backpressure.
//   6b (now): ACK-frees-the-bus. The slot SM grows IDLE -> SENT (awaiting the
//       slave's ISR ACK) -> INFLIGHT (ACK'd, slave executing; bus is free) ->
//       IDLE (reply arrived on a later poll). Two L2 timeouts:
//         * ACK timeout  — submit -> ACK; no ACK -> bounded resend -> unreachable.
//         * exec deadline — ACK -> reply; = exec_timeout_ms + margin; overrun ->
//           the slave is stuck. (exec_timeout_ms is the per-command value the API
//           already carries; 6b uses it as the L2 deadline. 6b-ii puts it ON THE
//           WIRE so the slave self-aborts too.)
//       NAK (slave busy) -> bounded resend. ACK/NAK are fed in from the BC's
//       OP_BUS_CMD_ACK / OP_BUS_CMD_NAK events.
//   7a: CMD_SLOT_FAULTED gates the queue when a slave's interlock trips.
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

// Synthetic completion status for a NAK whose bounded resends were exhausted
// (sits above the firmware shell range and the DEMUX_STATUS_* codes).
#define CMD_STATUS_BUSY   0xFDu

// Per-slave availability — observable by schedulers + clients.
typedef enum {
    CMD_SLOT_IDLE     = 0,   // nothing in flight; queue may hold waiting commands
    CMD_SLOT_SENT     = 1,   // command on the wire, awaiting the slave's ACK/NAK
    CMD_SLOT_INFLIGHT = 2,   // ACK'd; slave executing (bus is FREE); awaiting reply
    CMD_SLOT_FAULTED  = 3,   // reserved for 7a: interlock-tripped gating (unused now)
} cmd_slot_state_t;

// Completion callback. status: 0..5 firmware shell status, CMD_STATUS_BUSY, or
// DEMUX_STATUS_TIMEOUT / DEMUX_STATUS_LINK_DOWN. result/result_len valid in-call.
typedef void (*cmd_done_cb)(void *user, uint32_t handle, uint8_t addr,
                            uint8_t status, const uint8_t *result, uint16_t result_len);

typedef struct cmd_tracker cmd_tracker_t;

// Create a tracker that sends through `dx`. No slots until cmd_tracker_add_slave.
cmd_tracker_t *cmd_tracker_create(demux_t *dx);
void           cmd_tracker_destroy(cmd_tracker_t *t);

// Drop the whole slot set (in-flight + queued commands are completed with
// `abort_status` so callers are never orphaned). Call on (re)provisioning.
void cmd_tracker_reset(cmd_tracker_t *t, uint8_t abort_status);

// Register a slave addr as a schedulable slot. Idempotent. 0 ok / -1 if full.
int  cmd_tracker_add_slave(cmd_tracker_t *t, uint8_t addr);

// Submit a command to a slave: enqueued (depth CMD_QUEUE_DEPTH), one in flight per
// slave, completed via on_done. Returns a monotonic handle (>0), or 0 on:
// unknown addr / args_len > CMD_ARG_MAX / queue full (backpressure).
uint32_t cmd_tracker_submit(cmd_tracker_t *t, uint8_t addr, uint16_t command_id,
                            const uint8_t *args, uint16_t args_len,
                            uint32_t exec_timeout_ms,
                            cmd_done_cb on_done, void *user);

// Feed the BC's command-ACK / command-NAK events (OP_BUS_CMD_ACK/NAK [addr][rid]).
// ACK: SENT -> INFLIGHT (bus freed, exec clock starts). NAK: bounded resend.
void cmd_tracker_on_ack(cmd_tracker_t *t, uint8_t addr, uint16_t req_id);
void cmd_tracker_on_nak(cmd_tracker_t *t, uint8_t addr, uint16_t req_id);

// Pump every slot: send queued work on idle slots, and sweep the ACK-timeout /
// exec-deadline timers. `now_ms` is a monotonic millisecond clock. Call often.
void cmd_tracker_poll(cmd_tracker_t *t, uint64_t now_ms);

// Observation.
cmd_slot_state_t cmd_tracker_state(const cmd_tracker_t *t, uint8_t addr);
uint8_t          cmd_tracker_qdepth(const cmd_tracker_t *t, uint8_t addr);
uint32_t         cmd_tracker_total_acks(const cmd_tracker_t *t);  // ACKs seen (bus-free events)

#ifdef __cplusplus
}
#endif
