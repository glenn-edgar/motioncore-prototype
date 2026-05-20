// libcomm/bus_config.h
// Centralised tunable sizes for libcomm and the dongle decomposition.
// Per-target builds override these by passing -D<NAME>=<value> on the
// compile line; the defaults below match the Linux waypoint as of
// 2026-04-28.
//
// Rules:
//   - Every static array size lives here, never as a literal in code.
//   - Wire-protocol constants (frame markers, address space, NAK reasons,
//     handle bit layout) are NOT tunables — they remain in comm.h /
//     frame.h.
//   - Adding a new tunable: pick a name, document the unit, give it an
//     #ifndef guard so external builds can override.
//   - Changing a default: update the comment with the rationale; embedded
//     targets often need smaller values.
//
// Forward-looking entries (LOGICAL_ROBOT_MAX, MSGQ_DEPTH_DEFAULT,
// BUS_THREAD_STACK_BYTES) are wired up by the bus_kernel.h port (Track A.2);
// they live here today so the audit is complete and the override surface
// is one file.

#pragma once

// ============ libcomm core ============

#ifndef COMM_PAYLOAD_MAX
#define COMM_PAYLOAD_MAX             128   // max payload bytes per frame
#endif

#ifndef COMM_HANDLES_MAX
#define COMM_HANDLES_MAX              32   // simultaneous in-flight requests
#endif

#ifndef COMM_DONGLES_MAX
#define COMM_DONGLES_MAX               4   // virtual host + up to 3 real dongles
#endif

#ifndef COMM_BUSES_MAX
#define COMM_BUSES_MAX                 8   // logical buses across all dongles
#endif

#ifndef COMM_SLAVES_MAX
#define COMM_SLAVES_MAX               64   // declarable slaves total
#endif

// ============ chain-tree integration ============

#ifndef CT_COMM_RX_PERIOD_MS
#define CT_COMM_RX_PERIOD_MS          20   // chain-tree heartbeat for comm_rx; floor for tick_period_ms
#endif

// ============ transport: in-process (virtual host dongle) ============

#ifndef TRANSPORT_INPROC_MAX_ENDPOINTS
#define TRANSPORT_INPROC_MAX_ENDPOINTS 32
#endif

#ifndef TRANSPORT_INPROC_RING_SIZE
#define TRANSPORT_INPROC_RING_SIZE    512  // power of 2; per-direction per-endpoint
#endif

// ============ transport: UART/pty (Linux waypoint + future dongle) ============

#ifndef TRANSPORT_UART_TX_SIZE
#define TRANSPORT_UART_TX_SIZE       1024  // power of 2; staged m2s bytes
#endif

#ifndef TRANSPORT_UART_RX_SIZE
#define TRANSPORT_UART_RX_SIZE       2048  // power of 2; absorbs s2m bursts
#endif

#ifndef TRANSPORT_UART_PATH_MAX
#define TRANSPORT_UART_PATH_MAX        64  // max strlen+NUL of /dev/pts/N or /dev/ttyUSBn
#endif

// ============ dongle decomposition (Track A.2 forward-looking) ============
// Used by bus_kernel.h, bus_msg.h, and the future logical_robot pool.

#ifndef LOGICAL_ROBOT_MAX
#define LOGICAL_ROBOT_MAX              8   // ceiling per dongle (Track C Q5)
#endif

#ifndef LOGICAL_ROBOT_PRIMARY_MAX
#define LOGICAL_ROBOT_PRIMARY_MAX      2   // ext_bus-addressable robots
#endif

#ifndef LOGICAL_ROBOT_AUX_MAX
#define LOGICAL_ROBOT_AUX_MAX          6   // event-emitting / passive robots
#endif

#ifndef MSGQ_DEPTH_DEFAULT
#define MSGQ_DEPTH_DEFAULT            16   // bus_msgq slots (per-queue default)
#endif

#ifndef BUS_MSGQ_DEFAULT_DEPTH
#define BUS_MSGQ_DEFAULT_DEPTH        16   // per-robot inbox depth (Track C Q1)
#endif

#ifndef BUS_MSG_INLINE_PAYLOAD_MAX
#define BUS_MSG_INLINE_PAYLOAD_MAX    32   // bus_msg_t payload tail (Track C Q1)
#endif

#ifndef BUS_THREAD_STACK_BYTES
#define BUS_THREAD_STACK_BYTES      1536   // per-thread stack on RTOS targets; ignored on pthreads
#endif

// ============ bus_msg_t sentinel constants (Track C Q2) ============
// Reserved bus_msg_t shapes used by logical_robot_entry to deliver
// internal events (timer ticks, cooperative shutdown). External-bus
// frames never carry dst_robot=0xFF, so these can never collide with
// real traffic. See libcomm/bus_msg.h for the full layout.

#define BUS_MSG_DST_SENTINEL          0xFFu  // dst_robot = sentinel marker
#define BUS_MSG_SENTINEL_TICK         0x00u  // cmd_lo when dst=0xFF
#define BUS_MSG_SENTINEL_SHUTDOWN     0x01u  // cmd_lo when dst=0xFF
