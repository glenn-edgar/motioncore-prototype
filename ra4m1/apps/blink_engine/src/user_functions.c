// ============================================================================
// user_functions.c — blink_engine (Seeed XIAO RA4M1)
//
// The chain's single node calls toggle_led(). On the SAMD21 this toggled the
// user LED; here it increments g_engine_calls so main can report — over CDC —
// that the engine actually dispatched the reused chain ROM's node on the
// RA4M1. (The XIAO RA4M1's LED pin is not yet verified against the copied
// uno_r4 board def, so a counter is the reliable observable.)
//
// The symbol name stays `toggle_led`: the chain ROM (blink_engine_module_rom.c,
// reused byte-for-byte from the SAMD21) binds to it by that name.
// ============================================================================

#include "s_engine_types.h"

// ----------------------------------------------------------------------------
// M-port stack shims.
// s_engine_node.c / s_engine_module.c call these unconditionally, but the
// M-port carries no stack (inst->stack stays NULL) so the bodies are dead.
// Empty defs satisfy the linker without dragging in s_engine_stack.c.
// ----------------------------------------------------------------------------

void s_expr_tree_reset_stack(s_expr_tree_instance_t* inst) {
    (void)inst;
}

void s_expr_tree_free_stack(s_expr_tree_instance_t* inst) {
    (void)inst;
}

// Incremented on every node dispatch; read by main.c over CDC.
volatile uint32_t g_engine_calls = 0;

s_expr_result_t toggle_led(s_expr_tree_instance_t* inst,
                           const s_expr_param_t* params,
                           uint16_t param_count,
                           s_expr_event_type_t event_type,
                           uint16_t event_id,
                           void* event_data) {
    (void)inst; (void)params; (void)param_count;
    (void)event_id; (void)event_data;

    if (event_type == SE_EVENT_INIT || event_type == SE_EVENT_TERMINATE) {
        return SE_CONTINUE;
    }

    g_engine_calls++;
    return SE_CONTINUE;
}
