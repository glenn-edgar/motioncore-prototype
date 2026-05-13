// ============================================================================
// s_engine_eval.h
// S-Expression Evaluator API
// ============================================================================

#ifndef S_ENGINE_EVAL_H
#define S_ENGINE_EVAL_H

#include "s_engine_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// TREE LIFECYCLE
// ============================================================================



// Reset tree (preserves EVER_INIT flags)
void s_expr_tree_reset(s_expr_tree_instance_t* inst);

// Terminate tree (sends TERMINATE to all initialized nodes)
void s_expr_tree_terminate(s_expr_tree_instance_t* inst);

// Full reset (terminate + reinitialize)
void s_expr_tree_full_reset(s_expr_tree_instance_t* inst);

// Initialize node states (sets all to ACTIVE)
void s_expr_tree_init_states(s_expr_tree_instance_t* inst);

// ============================================================================
// CALLABLE INVOCATION
// For use by composite main functions (state machines, dispatchers, etc.)
// ============================================================================

// Invoke a MAIN callable at params[idx]
s_expr_result_t s_expr_invoke_main(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t idx
);

// Invoke a ONESHOT callable at params[idx]
void s_expr_invoke_oneshot(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t idx
);

// Invoke a PRED callable at params[idx]
bool s_expr_invoke_pred(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t idx
);

// Invoke any callable type at params[idx]
s_expr_result_t s_expr_invoke_any(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t idx
);

// ============================================================================
// PARAMETER UTILITIES
// ============================================================================

// Count logical parameters (skipping nested structures)
uint16_t s_expr_count_params(const s_expr_param_t* params, uint16_t count);

// Find first parameter with given opcode
uint16_t s_expr_find_param(const s_expr_param_t* params, uint16_t count, uint8_t opcode);

// Callback for parameter iteration
typedef bool (*s_expr_param_iter_fn)(const s_expr_param_t* params, uint16_t idx, void* ctx);

// Iterate through logical parameters
void s_expr_iterate_params(
    const s_expr_param_t* params,
    uint16_t count,
    s_expr_param_iter_fn callback,
    void* ctx
);



// ============================================================================
// NODE STATE ACCESSORS (current node during callback)
// ============================================================================

// Get/set 8-bit state machine state
uint8_t s_expr_get_state(s_expr_tree_instance_t* inst);
void s_expr_set_state(s_expr_tree_instance_t* inst, uint8_t state);

// Get/set 16-bit user flags
uint16_t s_expr_get_user_flags(s_expr_tree_instance_t* inst);
void s_expr_set_user_flags(s_expr_tree_instance_t* inst, uint16_t flags);

// ============================================================================
// 64-BIT STORAGE ACCESSORS (for pt_m_call functions only)
// These use pointer_array indexed by pointer_base
// ============================================================================

uint64_t s_expr_get_user_u64(s_expr_tree_instance_t* inst);
void s_expr_set_user_u64(s_expr_tree_instance_t* inst, uint64_t value);

double s_expr_get_user_f64(s_expr_tree_instance_t* inst);
void s_expr_set_user_f64(s_expr_tree_instance_t* inst, double value);

int64_t s_expr_get_user_i64(s_expr_tree_instance_t* inst);
void s_expr_set_user_i64(s_expr_tree_instance_t* inst, int64_t value);

void* s_expr_get_user_ptr(s_expr_tree_instance_t* inst);
void s_expr_set_user_ptr(s_expr_tree_instance_t* inst, void* value);

#ifdef __cplusplus
}
#endif

#endif // S_ENGINE_EVAL_H