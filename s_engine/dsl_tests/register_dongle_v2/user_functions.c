// ============================================================================
// user_functions.c — Linux prototype mocks for register_dongle_v2
//
// All user fns just printf so we can verify the chain shape + state-machine
// traversal end-to-end. On SAMD21 these become real libcomm frame emitters
// and GPIO ops.
// ============================================================================

#include <stdio.h>
#include <stdint.h>
#include "s_engine_types.h"
#include "s_engine_module.h"

// ----------------------------------------------------------------------------
// io_call: fires once on first INIT, never re-fires due to SURVIVES_RESET
// ----------------------------------------------------------------------------
void send_register(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;
    printf("[REGISTER] chip_uid=0123456789ABCDEF0011223344556677  vid:pid=2886:802F  fw=0.0.1\n");
    fflush(stdout);
}

// ----------------------------------------------------------------------------
// o_call inside OPERATIONAL heartbeat chain_flow — fires once per cycle
// ----------------------------------------------------------------------------
static uint32_t g_heartbeat_seq = 0;
void send_heartbeat(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;
    printf("[HEARTBEAT] seq=%u\n", g_heartbeat_seq++);
    fflush(stdout);
}

// ----------------------------------------------------------------------------
// o_call inside OPERATIONAL event_dispatch — fires when OP_PING dispatched
// ----------------------------------------------------------------------------
static uint32_t g_pong_seq = 0;
void send_pong(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;
    printf("[PONG]      seq=%u  (responding to OP_PING)\n", g_pong_seq++);
    fflush(stdout);
}

// ----------------------------------------------------------------------------
// handle_internal_events — m_call sibling of se_state_machine. Dispatches
// on engine-internal event ids (range 0xFE00+, never on the wire).
// On Linux this mocks the SAMD21 behavior — write dongle_state=BOOT on
// EV_HOST_REATTACH so the state_machine can switch back to BOOT case.
// ----------------------------------------------------------------------------
#define EV_HOST_REATTACH 0xFE00

s_expr_result_t handle_internal_events(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)params; (void)param_count; (void)event_data;
    if (event_type == SE_EVENT_INIT || event_type == SE_EVENT_TERMINATE) {
        return SE_PIPELINE_CONTINUE;
    }
    if (event_id == EV_HOST_REATTACH) {
        if (inst->blackboard) {
            *(int32_t*)inst->blackboard = 0;   // DONGLE_BOOT
        }
        printf("[REATTACH] dongle_state -> BOOT (simulated host reset)\n");
        fflush(stdout);
    }
    return SE_PIPELINE_CONTINUE;
}

// ----------------------------------------------------------------------------
// m_call inside OPERATIONAL fork — fires every tick, must return CONTINUE
// ----------------------------------------------------------------------------
static uint8_t g_led_state = 0;
static uint32_t g_led_toggles = 0;
s_expr_result_t toggle_led(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)inst; (void)params; (void)param_count;
    (void)event_id; (void)event_data;
    if (event_type == SE_EVENT_INIT)      return SE_PIPELINE_CONTINUE;
    if (event_type == SE_EVENT_TERMINATE) return SE_PIPELINE_CONTINUE;
    g_led_state ^= 1;
    g_led_toggles++;
    // Print every 4th toggle to keep output readable
    if ((g_led_toggles & 0x03) == 0) {
        printf("[LED]       toggle #%u  state=%u\n", g_led_toggles, g_led_state);
        fflush(stdout);
    }
    return SE_PIPELINE_CONTINUE;
}

// Linux build: stack functions come from libs_s_engine.a (s_engine_stack.o).
// On SAMD21, add stubs:
//   void s_expr_tree_reset_stack(s_expr_tree_instance_t* inst) { (void)inst; }
//   void s_expr_tree_free_stack(s_expr_tree_instance_t* inst) { (void)inst; }
