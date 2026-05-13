// ============================================================================
// user_functions.c — register_dongle (SAMD21 Phase-2 merge)
//
// Real implementations of the chain's three user-defined functions.
// The Lua chain (register_dongle.lua) wires:
//   io_call(send_register)  -> oneshot, fires once on first INIT
//   o_call(send_heartbeat)  -> oneshot, fires every chain_flow cycle (~1 Hz)
//   m_call(toggle_led)      -> main, fires every tick (bare leaf inside fork)
//
// Frame work uses the vendored libcomm slice (SLIP + CRC-8/AUTOSAR). All
// emitted frames go through the shared g_tx_ring, which main.c drains to
// USB-CDC every loop iteration.
// ============================================================================

#include <stdint.h>
#include <string.h>

#include "bsp/board_api.h"
#include "samd21.h"          // CMSIS: PORT register block

#include "s_engine_types.h"
#include "s_engine_module.h"

#include "frame.h"
#include "opcodes.h"

// ----------------------------------------------------------------------------
// Shared TX ring (defined in main.c). Every user function that emits a frame
// stages SLIP-encoded bytes here; main.c's loop drains the ring to CDC.
// ----------------------------------------------------------------------------
extern frame_ring_t g_tx_ring;

// ----------------------------------------------------------------------------
// M-port stack shims.
// s_engine_node.c / s_engine_module.c call these unconditionally, but the
// M-port decision (#11) is "no stack" — inst->stack stays NULL so the
// bodies are dead at runtime. Empty defs here let us drop s_engine_stack.c
// from the build.
// ----------------------------------------------------------------------------
void s_expr_tree_reset_stack(s_expr_tree_instance_t* inst) { (void)inst; }
void s_expr_tree_free_stack (s_expr_tree_instance_t* inst) { (void)inst; }

// ----------------------------------------------------------------------------
// SAMD21 chip UID (datasheet §10.3.3): four 32-bit words at fixed addresses.
// Word 0 lives at 0x0080A00C; words 1..3 live at 0x0080A040 / +0x44 / +0x48.
// ----------------------------------------------------------------------------
#define SAMD21_UID_WORD0_ADDR  0x0080A00CU
#define SAMD21_UID_WORD1_ADDR  0x0080A040U
#define SAMD21_UID_WORD2_ADDR  0x0080A044U
#define SAMD21_UID_WORD3_ADDR  0x0080A048U

static void samd21_read_uid(uint8_t out[16]) {
    uint32_t w0 = *(volatile uint32_t*)SAMD21_UID_WORD0_ADDR;
    uint32_t w1 = *(volatile uint32_t*)SAMD21_UID_WORD1_ADDR;
    uint32_t w2 = *(volatile uint32_t*)SAMD21_UID_WORD2_ADDR;
    uint32_t w3 = *(volatile uint32_t*)SAMD21_UID_WORD3_ADDR;
    // Little-endian byte order in the payload (matches the rest of libcomm wire format).
    out[ 0] = (uint8_t)(w0 >>  0); out[ 1] = (uint8_t)(w0 >>  8);
    out[ 2] = (uint8_t)(w0 >> 16); out[ 3] = (uint8_t)(w0 >> 24);
    out[ 4] = (uint8_t)(w1 >>  0); out[ 5] = (uint8_t)(w1 >>  8);
    out[ 6] = (uint8_t)(w1 >> 16); out[ 7] = (uint8_t)(w1 >> 24);
    out[ 8] = (uint8_t)(w2 >>  0); out[ 9] = (uint8_t)(w2 >>  8);
    out[10] = (uint8_t)(w2 >> 16); out[11] = (uint8_t)(w2 >> 24);
    out[12] = (uint8_t)(w3 >>  0); out[13] = (uint8_t)(w3 >>  8);
    out[14] = (uint8_t)(w3 >> 16); out[15] = (uint8_t)(w3 >> 24);
}

// ----------------------------------------------------------------------------
// send_register — oneshot (io_call). Fires once on first INIT.
// Payload (24 B, little-endian):
//   [ 0..15]  chip_uid (16 B)
//   [16..17]  vid 0x2886
//   [18..19]  pid 0x802F
//   [20..23]  fw_version 0x00000001
// ----------------------------------------------------------------------------
#define REGISTER_PAYLOAD_LEN  24u
#define REGISTER_VID          0x2886U
#define REGISTER_PID          0x802FU
#define REGISTER_FW_VERSION   0x00000001U

void send_register(s_expr_tree_instance_t* inst,
                   const s_expr_param_t*  params,
                   uint16_t               param_count,
                   s_expr_event_type_t    event_type,
                   uint16_t               event_id,
                   void*                  event_data) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;

    uint8_t payload[REGISTER_PAYLOAD_LEN];
    samd21_read_uid(&payload[0]);
    payload[16] = (uint8_t)(REGISTER_VID >> 0);
    payload[17] = (uint8_t)(REGISTER_VID >> 8);
    payload[18] = (uint8_t)(REGISTER_PID >> 0);
    payload[19] = (uint8_t)(REGISTER_PID >> 8);
    payload[20] = (uint8_t)(REGISTER_FW_VERSION >>  0);
    payload[21] = (uint8_t)(REGISTER_FW_VERSION >>  8);
    payload[22] = (uint8_t)(REGISTER_FW_VERSION >> 16);
    payload[23] = (uint8_t)(REGISTER_FW_VERSION >> 24);

    frame_meta_t meta = {
        .addr        = 1,
        .cmd         = OP_REGISTER,
        .seq         = 0,
        .ack_seq     = 0,
        .ack_status  = 0,
        .payload_len = REGISTER_PAYLOAD_LEN,
    };
    (void)frame_encode_s2m(&meta, payload, &g_tx_ring);
}

// ----------------------------------------------------------------------------
// send_heartbeat — oneshot (o_call). Fires every chain_flow cycle.
// Payload (8 B, little-endian):
//   [0..3]  uptime_ms = board_millis() truncated to uint32
//   [4..7]  seq_counter (static, increments each invocation)
// ----------------------------------------------------------------------------
#define HEARTBEAT_PAYLOAD_LEN  8u

static uint32_t g_heartbeat_seq = 0;

void send_heartbeat(s_expr_tree_instance_t* inst,
                    const s_expr_param_t*   params,
                    uint16_t                param_count,
                    s_expr_event_type_t     event_type,
                    uint16_t                event_id,
                    void*                   event_data) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;

    uint32_t uptime_ms = (uint32_t)board_millis();
    uint32_t seq       = g_heartbeat_seq++;

    uint8_t payload[HEARTBEAT_PAYLOAD_LEN];
    payload[0] = (uint8_t)(uptime_ms >>  0);
    payload[1] = (uint8_t)(uptime_ms >>  8);
    payload[2] = (uint8_t)(uptime_ms >> 16);
    payload[3] = (uint8_t)(uptime_ms >> 24);
    payload[4] = (uint8_t)(seq       >>  0);
    payload[5] = (uint8_t)(seq       >>  8);
    payload[6] = (uint8_t)(seq       >> 16);
    payload[7] = (uint8_t)(seq       >> 24);

    frame_meta_t meta = {
        .addr        = 1,
        .cmd         = OP_HEARTBEAT,
        .seq         = (uint8_t)(seq & 0xFFu),
        .ack_seq     = 0,
        .ack_status  = 0,
        .payload_len = HEARTBEAT_PAYLOAD_LEN,
    };
    (void)frame_encode_s2m(&meta, payload, &g_tx_ring);
}

// ----------------------------------------------------------------------------
// send_pong — oneshot (o_call). Fires once per OP_PING event dispatch.
// Payload (8 B, little-endian): uptime_ms + monotonic pong_seq counter.
// Mirrors send_heartbeat's shape; cmd is OP_PONG (0x0005) on s2m.
// ----------------------------------------------------------------------------
#define PONG_PAYLOAD_LEN  8u

static uint32_t g_pong_seq = 0;

void send_pong(s_expr_tree_instance_t* inst,
               const s_expr_param_t*   params,
               uint16_t                param_count,
               s_expr_event_type_t     event_type,
               uint16_t                event_id,
               void*                   event_data) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;

    uint32_t uptime_ms = (uint32_t)board_millis();
    uint32_t seq       = g_pong_seq++;

    uint8_t payload[PONG_PAYLOAD_LEN];
    payload[0] = (uint8_t)(uptime_ms >>  0);
    payload[1] = (uint8_t)(uptime_ms >>  8);
    payload[2] = (uint8_t)(uptime_ms >> 16);
    payload[3] = (uint8_t)(uptime_ms >> 24);
    payload[4] = (uint8_t)(seq       >>  0);
    payload[5] = (uint8_t)(seq       >>  8);
    payload[6] = (uint8_t)(seq       >> 16);
    payload[7] = (uint8_t)(seq       >> 24);

    frame_meta_t meta = {
        .addr        = 1,
        .cmd         = OP_PONG,
        .seq         = (uint8_t)(seq & 0xFFu),
        .ack_seq     = 0,
        .ack_status  = 0,
        .payload_len = PONG_PAYLOAD_LEN,
    };
    (void)frame_encode_s2m(&meta, payload, &g_tx_ring);
}

// ----------------------------------------------------------------------------
// toggle_led — main (m_call). Fires every tick (bare leaf under se_fork).
// PA17 is the Xiao user LED. Must return SE_PIPELINE_CONTINUE so the fork
// keeps the branch alive across ticks.
// ----------------------------------------------------------------------------
#define REGISTER_DONGLE_LED_PIN 17u

s_expr_result_t toggle_led(s_expr_tree_instance_t* inst,
                           const s_expr_param_t*   params,
                           uint16_t                param_count,
                           s_expr_event_type_t     event_type,
                           uint16_t                event_id,
                           void*                   event_data) {
    (void)inst; (void)params; (void)param_count;
    (void)event_id; (void)event_data;

    if (event_type == SE_EVENT_INIT || event_type == SE_EVENT_TERMINATE) {
        return SE_PIPELINE_CONTINUE;
    }

    PORT->Group[0].OUTTGL.reg = (1u << REGISTER_DONGLE_LED_PIN);
    return SE_PIPELINE_CONTINUE;
}
