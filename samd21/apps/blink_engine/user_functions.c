// ============================================================================
// user_functions.c - blink_engine
// Single user main function: toggles Xiao user LED (PA17, active-low) and
// returns SE_CONTINUE so the engine re-enters every tick.
// ============================================================================

#include "s_engine_types.h"
#include "samd21.h"   // CMSIS: PORT register block

// ----------------------------------------------------------------------------
// M-port stack shims.
// s_engine_node.c and s_engine_module.c call s_expr_tree_reset_stack /
// s_expr_tree_free_stack unconditionally, but the M-port decision (#11) is
// "no stack". inst->stack stays NULL, so the bodies are dead at runtime; we
// provide empty definitions purely to satisfy the linker without dragging in
// s_engine_stack.c (which depends on heap-style alloc/free + frame ops).
// ----------------------------------------------------------------------------

void s_expr_tree_reset_stack(s_expr_tree_instance_t* inst) {
    (void)inst;
}

void s_expr_tree_free_stack(s_expr_tree_instance_t* inst) {
    (void)inst;
}

// LED is PA17 on the Seeeduino Xiao SAMD21 (LED_PIN=17 in the TinyUSB BSP).
// The BSP's board_init() has already configured it as output and driven it
// inactive. We toggle the OUT register directly for minimum overhead.
#define BLINK_ENGINE_LED_PIN 17u

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

    PORT->Group[0].OUTTGL.reg = (1u << BLINK_ENGINE_LED_PIN);
    return SE_CONTINUE;
}
