// register_dongle opcodes — four-layer-sync allocation per
// common/spec/four_layer_sync.md (2026-05-19).
//
// These OP_* values are payload commands carried in frame_meta_t.cmd. The
// canonical libcomm catalogue (comm.h::comm_link_cmd_t) is reserved for
// link-control and is not used here.
//
// Allocation rule:
//   * s2m opcodes (outgoing; only used as frame_meta_t.cmd, never dispatched
//     on by the engine):  0x0001 – 0x00FF
//   * m2s opcodes (incoming; pushed to engine event_queue and matched against
//     by se_event_dispatch — MUST avoid SE_EVENT_TICK=4 / SE_EVENT_INIT=0xfffe
//     / SE_EVENT_TERMINATE=0xfffd):  0x0100 – 0xFDFF
//   * engine-internal events (never on the wire):  0xFE00 – 0xFEFF

#pragma once

#include <stdint.h>

// ----- s2m (dongle -> host) -----
#define OP_REGISTER          ((uint16_t)0x0001)  // dongle boot announcement (REGISTER v2 payload)
#define OP_HEARTBEAT         ((uint16_t)0x0002)  // periodic alive ping
#define OP_PONG              ((uint16_t)0x0005)  // response to host's OP_PING
#define OP_COMMISSION_REPLY  ((uint16_t)0x0006)  // L0: confirms flash-write of instance_id
#define OP_NAK               ((uint16_t)0x0007)  // generic state/permission error (op_nak_t)
#define OP_MANIFEST_REPLY    ((uint16_t)0x0008)  // L2: schema_hash + fw_version + m2s opcode list
#define OP_DBG_LOG           ((uint16_t)0x0010)  // se_log output (UTF-8 text payload)
#define OP_SHELL_REPLY       ((uint16_t)0x0011)  // app-shell command result
#define OP_POLL_REPLY        ((uint16_t)0x0012)  // 64 B status buffer snapshot (slice 5)
#define OP_EVENT             ((uint16_t)0x0013)  // edge-trigger slot state/tf change (slice 5)
#define OP_RS485_FRAME_RX    ((uint16_t)0x0014)  // RS-485 frame received: [from_addr:u8][payload bytes]
#define OP_BUS_SLAVE_DOWN    ((uint16_t)0x0015)  // BC poll engine: slave missed max_misses — [addr:u8]
#define OP_BUS_SLAVE_UP      ((uint16_t)0x0016)  // BC poll engine: slave recovered — [addr:u8][class_id:u32]
#define OP_BUS_SLAVE_FLAGGED ((uint16_t)0x0017)  // BC sweep: slave summary-bit edge — [addr:u8][flags:u8] (bit0=interlock tripped)
#define OP_BUS_CMD_ACK       ((uint16_t)0x0018)  // 6b: slave ACK'd a bus command (bus freed) — [addr:u8][req_id:u16]
#define OP_BUS_CMD_NAK       ((uint16_t)0x0019)  // 6b: slave NAK'd a bus command (busy) — [addr:u8][req_id:u16]

// ----- m2s (host -> dongle) -----
#define OP_REGISTER_ACK      ((uint16_t)0x0103)  // L1: host acknowledges OP_REGISTER -> BOOT → L1_DONE
#define OP_PING              ((uint16_t)0x0104)  // OPERATIONAL: host probes dongle
#define OP_COMMISSION_SET    ((uint16_t)0x0105)  // L0: persist instance_id to flash (deferred)
#define OP_COMMISSION_CLEAR  ((uint16_t)0x0106)  // L0: factory-reset commissioning page (deferred)
#define OP_GET_MANIFEST      ((uint16_t)0x0107)  // L2: request OP_MANIFEST_REPLY
#define OP_OPERATIONAL_BEGIN ((uint16_t)0x0108)  // advance L1_DONE → OPERATIONAL
#define OP_SHELL_EXEC        ((uint16_t)0x0109)  // app-shell command invocation
#define OP_POLL              ((uint16_t)0x010A)  // request 64 B status buffer snapshot (slice 5)
#define OP_BUS_EXEC          ((uint16_t)0x010B)  // 6b-ii: bus command w/ exec_timeout — [exec_timeout:u16][req_id:u16][cmd:u16][args]; bus-only wrapper (USB shell schema untouched)

// ----- engine-internal events (never appear on the wire) -----
// main.c (or other firmware-internal code) pushes these to the engine
// event_queue; chains dispatch on them. They MUST be disjoint from any cmd
// value that comes in over libcomm so the chain can't be tricked by a
// malicious host into faking an internal event.
#define EV_HOST_REATTACH     ((uint16_t)0xFE00)  // host closed and reopened CDC

// ----- OP_NAK payload ------------------------------------------------------
// Wire: 3 bytes — { reason_code: u8, rejected_cmd: u16 little-endian }.

typedef enum {
    NAK_ERR_STATE           = 1,  // opcode not legal in current dongle state
    NAK_ERR_UNSUPPORTED_CMD = 2,  // opcode unknown or not implemented in this firmware
    NAK_ERR_NO_RESOURCES    = 3,  // bounded table full, RAM exhausted
    NAK_ERR_ARGS            = 4,  // payload parse error or argument out of range
} nak_reason_t;

// ----- OP_SHELL_REPLY status codes -----------------------------------------
// Wire: [request_id u16][status u8][result_message bytes...]
// status is one of these. UNKNOWN_CMD/BAD_ARGS/RESULT_TOO_BIG are the shell
// layer's own errors; CMD_FAILED is a catch-all the specific commands can
// return when their domain-specific operation fails (sensor not responding,
// etc.).

typedef enum {
    SHELL_STATUS_OK              = 0,
    SHELL_STATUS_UNKNOWN_CMD     = 1,  // command_id not in g_shell_cmds[]
    SHELL_STATUS_BAD_ARGS        = 2,  // args_message overflow / value out of range
    SHELL_STATUS_CMD_FAILED      = 3,  // runtime error inside the command
    SHELL_STATUS_RESULT_TOO_BIG  = 4,  // result_message would exceed COMM_PAYLOAD_MAX
    SHELL_STATUS_BUSY            = 5,  // resource owned by an active interlock; slice 2+ HIL gating
} shell_status_t;
