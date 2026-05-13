// ============================================================================
// user_functions.c — Linux prototype mocks for register_dongle
// Real implementations on SAMD21 build libcomm frames + flip GPIO.
// Here they just printf so we can verify chain shape correctness.
// ============================================================================

#include <stdio.h>
#include <stdint.h>
#include "s_engine_types.h"
#include "s_engine_module.h"

// Oneshot (io_call) — fires once on first INIT; never re-fires due to SURVIVES_RESET.
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

// Oneshot — fires once per chain_flow iteration via o_call.
// Auto-terminates after one fire; chain_flow advances to next child (tick_delay).
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

// Main — fires every tick (bare leaf inside se_fork). Must return SE_PIPELINE_CONTINUE
// so fork keeps the branch active across ticks.
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
    // Compact log: only print every 4th toggle to keep the terminal readable
    if ((g_led_toggles & 0x03) == 0) {
        printf("[LED] toggle #%u  state=%u\n", g_led_toggles, g_led_state);
        fflush(stdout);
    }
    return SE_PIPELINE_CONTINUE;
}

// Linux build: stack functions come from libs_s_engine.a (s_engine_stack.o).
// On SAMD21 (separate Makefile excluding s_engine_stack.c), add stubs here:
//   void s_expr_tree_reset_stack(s_expr_tree_instance_t* inst) { (void)inst; }
//   void s_expr_tree_free_stack(s_expr_tree_instance_t* inst) { (void)inst; }
