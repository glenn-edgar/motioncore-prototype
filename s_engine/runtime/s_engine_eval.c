// ============================================================================
// s_engine_eval.c
// S-Expression Evaluator Implementation
// Flat parameter walker
// ============================================================================

#include "s_engine_eval.h"
#include "s_engine_module.h"
#include "s_engine_exception.h"
#include <string.h>
#include <stdio.h>
// ============================================================================
// EXCEPTION MACRO (must be defined by application)
// ============================================================================




void s_expr_tree_reset(s_expr_tree_instance_t* inst);
void s_expr_tree_init_states(s_expr_tree_instance_t* inst);

// ============================================================================
// INTERNAL: Get node state by index
// ============================================================================

static inline s_expr_node_state_t* get_node_state(
    s_expr_tree_instance_t* inst,
    uint16_t node_index
) {
    if (!inst) {
        EXCEPTION("get_node_state: NULL instance");
        return NULL;
    }
    if (node_index >= inst->node_count) {
        EXCEPTION("get_node_state: node_index out of range");
        return NULL;
    }
    return &inst->node_states[node_index];
}

// ============================================================================
// INTERNAL: Dispatch oneshot function
// ============================================================================

static void dispatch_oneshot(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* func_param,
    const s_expr_param_t* args,
    uint16_t arg_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    if (!inst) {
        EXCEPTION("dispatch_oneshot: NULL instance");
        return;
    }
    if (!func_param) {
        EXCEPTION("dispatch_oneshot: NULL func_param");
        return;
    }
    
    s_expr_module_t* mod = inst->module;
    if (!mod) {
        EXCEPTION("dispatch_oneshot: NULL module");
        return;
    }
    
    uint16_t func_idx = func_param->func_index;
    uint16_t node_idx = func_param->node_index;
    bool survives_reset = (func_param->type & S_EXPR_FLAG_SURVIVES_RESET) != 0;
    
    s_expr_node_state_t* state = get_node_state(inst, node_idx);
    if (!state) return;
    
    uint8_t check_flag = survives_reset ? S_EXPR_NODE_FLAG_EVER_INIT : S_EXPR_NODE_FLAG_INITIALIZED;
    
    
    
    if (state->flags & check_flag) {
        
        return;
    }
    
    state->flags |= check_flag;
    
    uint16_t saved_node = inst->current_node_index;
    inst->current_node_index = node_idx;
    
    if (func_idx >= mod->def->oneshot_count) {
        EXCEPTION("dispatch_oneshot: func_index out of range");
        inst->current_node_index = saved_node;
        return;
    }
    
    if (mod->oneshot_fns[func_idx]) {
        mod->oneshot_fns[func_idx](inst, args, arg_count, event_type, event_id, event_data);
    } else {
        EXCEPTION("dispatch_oneshot: NULL function pointer");
    }
    
    inst->current_node_index = saved_node;
}
// ============================================================================
// INTERNAL: Dispatch predicate function
// ============================================================================

static bool dispatch_pred(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* func_param,
    const s_expr_param_t* args,
    uint16_t arg_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    if (!inst) {
        EXCEPTION("dispatch_pred: NULL instance");
        return false;
    }
    if (!func_param) {
        EXCEPTION("dispatch_pred: NULL func_param");
        return false;
    }
    
    s_expr_module_t* mod = inst->module;
    if (!mod) {
        EXCEPTION("dispatch_pred: NULL module");
        return false;
    }
    
    uint16_t func_idx = func_param->func_index;
    uint16_t node_idx = func_param->node_index;
    
    uint16_t saved_node = inst->current_node_index;
    inst->current_node_index = node_idx;
    
    bool result = false;
    
    if (func_idx >= mod->def->pred_count) {
        EXCEPTION("dispatch_pred: func_index out of range");
        inst->current_node_index = saved_node;
        return false;
    }
    
    if (mod->pred_fns[func_idx]) {
        result = mod->pred_fns[func_idx](inst, args, arg_count, event_type, event_id, event_data);
    } else {
        EXCEPTION("dispatch_pred: NULL function pointer");
    }
    
    inst->current_node_index = saved_node;
    return result;
}

// ============================================================================
// INTERNAL: Dispatch main function
// ============================================================================

static s_expr_result_t dispatch_main(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* func_param,
    const s_expr_param_t* args,
    uint16_t arg_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    if (!inst) {
        EXCEPTION("dispatch_main: NULL instance");
        return SE_TERMINATE;
    }
    if (!func_param) {
        EXCEPTION("dispatch_main: NULL func_param");
        return SE_TERMINATE;
    }
    
    s_expr_module_t* mod = inst->module;
    if (!mod) {
        EXCEPTION("dispatch_main: NULL module");
        return SE_TERMINATE;
    }
    
    uint16_t func_idx = func_param->func_index;
    uint16_t node_idx = func_param->node_index;
    bool is_pointer_call = (func_param->type & S_EXPR_FLAG_POINTER) != 0;
    uint8_t pointer_base = func_param->index_to_pointer;
   
    s_expr_node_state_t* state = get_node_state(inst, node_idx);
    if (!state) return SE_TERMINATE;
    
    if (!(state->flags & S_EXPR_NODE_FLAG_ACTIVE)) {
        return SE_PIPELINE_CONTINUE;
    }
    
    uint16_t saved_node = inst->current_node_index;
    bool saved_in_ptr = inst->in_pointer_call;
    uint8_t saved_ptr_base = inst->pointer_base;
    
    inst->current_node_index = node_idx;
    if (is_pointer_call) {
        inst->in_pointer_call = true;
        inst->pointer_base = pointer_base;
    }
    
    s_expr_result_t result = SE_PIPELINE_CONTINUE;
    
    if (func_idx >= mod->def->main_count) {
        EXCEPTION("dispatch_main: func_index out of range");
        inst->current_node_index = saved_node;
        inst->in_pointer_call = saved_in_ptr;
        inst->pointer_base = saved_ptr_base;
        return SE_TERMINATE;
    }
    
    s_expr_main_fn_t fn = mod->main_fns[func_idx];
    
    if (!fn) {
        EXCEPTION("dispatch_main: NULL function pointer");
        inst->current_node_index = saved_node;
        inst->in_pointer_call = saved_in_ptr;
        inst->pointer_base = saved_ptr_base;
        return SE_TERMINATE;
    }
    
    // =========================================================================
    // PHASE 1: INITIALIZATION (first call only)
    // Called with SE_EVENT_INIT on first dispatch to this node
    // =========================================================================
    if (!(state->flags & S_EXPR_NODE_FLAG_INITIALIZED)) {
        state->flags |= S_EXPR_NODE_FLAG_INITIALIZED;
        
        fn(inst, args, arg_count, SE_EVENT_INIT, event_id, event_data);
        
      
    }
    
    // =========================================================================
    // PHASE 2: REGULAR EVENT PROCESSING
    // Called with event_type (typically SE_EVENT_TICK or user event)
    // =========================================================================
    
    result = fn(inst, args, arg_count, event_type, event_id, event_data);
    
    // =========================================================================
    // PHASE 3b: TERMINATION (node completed or disabled)
    // Called with SE_EVENT_TERMINATE when result is SE_DISABLE
    // =========================================================================
    if (result == SE_PIPELINE_DISABLE) {
        fn(inst, args, arg_count, SE_EVENT_TERMINATE, event_id, event_data);
        state->flags &= ~S_EXPR_NODE_FLAG_ACTIVE;
    }
    
    inst->current_node_index = saved_node;
    inst->in_pointer_call = saved_in_ptr;
    inst->pointer_base = saved_ptr_base;
    
    return result;
}




// ============================================================================
// PUBLIC: Tree tick
// ============================================================================



// ============================================================================
// PUBLIC: Reset tree
// ============================================================================

void s_expr_tree_reset(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_tree_reset: NULL instance");
        return;
    }
    if (!inst->node_states && inst->node_count > 0) {
        EXCEPTION("s_expr_tree_reset: NULL node_states");
        return;
    }
    
    for (uint16_t i = 0; i < inst->node_count; i++) {
        uint8_t ever_init = inst->node_states[i].flags & S_EXPR_NODE_FLAG_EVER_INIT;
        inst->node_states[i].flags = S_EXPR_NODE_FLAG_ACTIVE | ever_init;
        inst->node_states[i].state = 0;
        inst->node_states[i].user_data = 0;
    }
}

// ============================================================================
// PUBLIC: Terminate tree
// ============================================================================

void s_expr_tree_terminate(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_tree_terminate: NULL instance");
        return;
    }
    if (!inst->tree) {
        EXCEPTION("s_expr_tree_terminate: NULL tree");
        return;
    }
    if (!inst->module) {
        EXCEPTION("s_expr_tree_terminate: NULL module");
        return;
    }
    if (!inst->node_states && inst->node_count > 0) {
        EXCEPTION("s_expr_tree_terminate: NULL node_states");
        return;
    }
    
    const s_expr_param_t* params = inst->tree->params;
    uint16_t count = inst->tree->param_count;
    
    for (uint16_t idx = 0; idx < count; ) {
        uint8_t opcode = params[idx].type & S_EXPR_OPCODE_MASK;
        
        if (opcode == S_EXPR_PARAM_OPEN_CALL) {
            const s_expr_param_t* func_param = &params[idx + 1];
            uint8_t func_opcode = func_param->type & S_EXPR_OPCODE_MASK;
            
            if (func_opcode == S_EXPR_PARAM_MAIN) {
                uint16_t node_idx = func_param->node_index;
                s_expr_node_state_t* state = get_node_state(inst, node_idx);
                
                if (state && (state->flags & S_EXPR_NODE_FLAG_INITIALIZED)) {
                    uint16_t func_idx = func_param->func_index;
                    bool is_pointer_call = (func_param->type & S_EXPR_FLAG_POINTER) != 0;
                    uint8_t pointer_base = func_param->index_to_pointer;
                    
                    if (func_idx < inst->module->def->main_count) {
                        s_expr_main_fn_t fn = inst->module->main_fns[func_idx];
                        if (fn) {
                            uint16_t close_idx = idx + params[idx].brace_idx;
                            uint16_t arg_count = (close_idx > idx + 2) ? (close_idx - idx - 2) : 0;
                            const s_expr_param_t* args = (arg_count > 0) ? &params[idx + 2] : NULL;
                            
                            inst->current_node_index = node_idx;
                            if (is_pointer_call) {
                                inst->in_pointer_call = true;
                                inst->pointer_base = pointer_base;
                            }
                            
                            fn(inst, args, arg_count, SE_EVENT_TERMINATE, 0, NULL);
                            
                            inst->in_pointer_call = false;
                        }
                    }
                }
            }
            
            idx += params[idx].brace_idx + 1;
        } else {
            idx++;
        }
    }
    
    for (uint16_t i = 0; i < inst->node_count; i++) {
        inst->node_states[i].flags = 0;
        inst->node_states[i].state = 0;
        inst->node_states[i].user_data = 0;
    }
}

// ============================================================================
// PUBLIC: Full reset
// ============================================================================

void s_expr_tree_full_reset(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_tree_full_reset: NULL instance");
        return;
    }
    s_expr_tree_terminate(inst);
    s_expr_tree_init_states(inst);
}

// ============================================================================
// PUBLIC: Initialize states
// ============================================================================

void s_expr_tree_init_states(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_tree_init_states: NULL instance");
        return;
    }
    if (!inst->node_states && inst->node_count > 0) {
        EXCEPTION("s_expr_tree_init_states: NULL node_states");
        return;
    }
    
    for (uint16_t i = 0; i < inst->node_count; i++) {
        inst->node_states[i].flags = S_EXPR_NODE_FLAG_ACTIVE;
        inst->node_states[i].state = 0;
        inst->node_states[i].user_data = 0;
    }
}

// ============================================================================
// PUBLIC: Invoke main callable
// ============================================================================

s_expr_result_t s_expr_invoke_main(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t idx
) {
    if (!inst) {
        EXCEPTION("s_expr_invoke_main: NULL instance");
        return SE_TERMINATE;
    }
    if (!params) {
        EXCEPTION("s_expr_invoke_main: NULL params");
        return SE_TERMINATE;
    }
    
    uint8_t opcode = params[idx].type & S_EXPR_OPCODE_MASK;
    
    if (opcode == S_EXPR_PARAM_OPEN_CALL) {
        uint16_t close_idx = idx + params[idx].brace_idx;
        const s_expr_param_t* func_param = &params[idx + 1];
        uint16_t arg_count = (close_idx > idx + 2) ? (close_idx - idx - 2) : 0;
        const s_expr_param_t* args = (arg_count > 0) ? &params[idx + 2] : NULL;
        
        return dispatch_main(inst, func_param, args, arg_count,
                            SE_EVENT_TICK, inst->current_event_id, inst->current_event_data);
    } else if (opcode == S_EXPR_PARAM_MAIN) {
        return dispatch_main(inst, &params[idx], NULL, 0,
                            SE_EVENT_TICK, inst->current_event_id, inst->current_event_data);
    }
    
    EXCEPTION("s_expr_invoke_main: param is not MAIN or OPEN_CALL");
    return SE_TERMINATE;
}

// ============================================================================
// PUBLIC: Invoke oneshot callable
// ============================================================================

void s_expr_invoke_oneshot(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t idx
) {
    if (!inst) {
        EXCEPTION("s_expr_invoke_oneshot: NULL instance");
        return;
    }
    if (!params) {
        EXCEPTION("s_expr_invoke_oneshot: NULL params");
        return;
    }
    
    uint8_t opcode = params[idx].type & S_EXPR_OPCODE_MASK;
    
    if (opcode == S_EXPR_PARAM_OPEN_CALL) {
        uint16_t close_idx = idx + params[idx].brace_idx;
        const s_expr_param_t* func_param = &params[idx + 1];
        uint16_t arg_count = (close_idx > idx + 2) ? (close_idx - idx - 2) : 0;
        const s_expr_param_t* args = (arg_count > 0) ? &params[idx + 2] : NULL;
        
        dispatch_oneshot(inst, func_param, args, arg_count,
                        SE_EVENT_TICK, inst->current_event_id, inst->current_event_data);
    } else if (opcode == S_EXPR_PARAM_ONESHOT) {
        dispatch_oneshot(inst, &params[idx], NULL, 0,
                        SE_EVENT_TICK, inst->current_event_id, inst->current_event_data);
    } else {
        EXCEPTION("s_expr_invoke_oneshot: param is not ONESHOT or OPEN_CALL");
    }
}

// ============================================================================
// PUBLIC: Invoke predicate callable
// ============================================================================

bool s_expr_invoke_pred(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t idx
) {
    if (!inst) {
        EXCEPTION("s_expr_invoke_pred: NULL instance");
        return false;
    }
    if (!params) {
        EXCEPTION("s_expr_invoke_pred: NULL params");
        return false;
    }
    
    uint8_t opcode = params[idx].type & S_EXPR_OPCODE_MASK;
    
    if (opcode == S_EXPR_PARAM_OPEN_CALL) {
        uint16_t close_idx = idx + params[idx].brace_idx;
        const s_expr_param_t* func_param = &params[idx + 1];
        uint16_t arg_count = (close_idx > idx + 2) ? (close_idx - idx - 2) : 0;
        const s_expr_param_t* args = (arg_count > 0) ? &params[idx + 2] : NULL;
        
        return dispatch_pred(inst, func_param, args, arg_count,
                            SE_EVENT_TICK, inst->current_event_id, inst->current_event_data);
    } else if (opcode == S_EXPR_PARAM_PRED) {
        return dispatch_pred(inst, &params[idx], NULL, 0,
                            SE_EVENT_TICK, inst->current_event_id, inst->current_event_data);
    }
    
    EXCEPTION("s_expr_invoke_pred: param is not PRED or OPEN_CALL");
    return false;
}

// ============================================================================
// PUBLIC: Invoke any callable
// ============================================================================

s_expr_result_t s_expr_invoke_any(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t idx
) {
    if (!inst) {
        EXCEPTION("s_expr_invoke_any: NULL instance");
        return SE_TERMINATE;
    }
    if (!params) {
        EXCEPTION("s_expr_invoke_any: NULL params");
        return SE_TERMINATE;
    }
    
    uint8_t opcode = params[idx].type & S_EXPR_OPCODE_MASK;
    
    if (opcode == S_EXPR_PARAM_OPEN_CALL) {
        const s_expr_param_t* func_param = &params[idx + 1];
        uint8_t func_opcode = func_param->type & S_EXPR_OPCODE_MASK;
        
        switch (func_opcode) {
            case S_EXPR_PARAM_MAIN:
                return s_expr_invoke_main(inst, params, idx);
            case S_EXPR_PARAM_ONESHOT:
                s_expr_invoke_oneshot(inst, params, idx);
                return SE_PIPELINE_CONTINUE;
            case S_EXPR_PARAM_PRED:
                return s_expr_invoke_pred(inst, params, idx) ? SE_PIPELINE_CONTINUE : SE_PIPELINE_HALT;
            default:
                EXCEPTION("s_expr_invoke_any: unknown function type in OPEN_CALL");
                return SE_TERMINATE;
        }
    }
    
    switch (opcode) {
        case S_EXPR_PARAM_MAIN:
            return s_expr_invoke_main(inst, params, idx);
        case S_EXPR_PARAM_ONESHOT:
            s_expr_invoke_oneshot(inst, params, idx);
            return SE_PIPELINE_CONTINUE;
        case S_EXPR_PARAM_PRED:
            return s_expr_invoke_pred(inst, params, idx) ? SE_PIPELINE_CONTINUE : SE_PIPELINE_HALT;
        default:
            EXCEPTION("s_expr_invoke_any: param is not a callable type");
            return SE_TERMINATE;
    }
}

// ============================================================================
// PUBLIC: Count logical parameters
// ============================================================================

uint16_t s_expr_count_params(const s_expr_param_t* params, uint16_t count) {
    if (!params) {
        if (count > 0) {
            EXCEPTION("s_expr_count_params: NULL params with non-zero count");
        }
        return 0;
    }
    
    uint16_t logical_count = 0;
    uint16_t idx = 0;
    
    while (idx < count) {
        logical_count++;
        idx = s_expr_skip_param(params, idx);
    }
    
    return logical_count;
}

// ============================================================================
// PUBLIC: Find parameter by opcode
// ============================================================================

uint16_t s_expr_find_param(const s_expr_param_t* params, uint16_t count, uint8_t opcode) {
    if (!params) {
        if (count > 0) {
            EXCEPTION("s_expr_find_param: NULL params with non-zero count");
        }
        return UINT16_MAX;
    }
    
    for (uint16_t idx = 0; idx < count; ) {
        if ((params[idx].type & S_EXPR_OPCODE_MASK) == opcode) {
            return idx;
        }
        idx = s_expr_skip_param(params, idx);
    }
    
    return UINT16_MAX;  // Not found is not an error
}

// ============================================================================
// PUBLIC: Iterate parameters
// ============================================================================

void s_expr_iterate_params(
    const s_expr_param_t* params,
    uint16_t count,
    s_expr_param_iter_fn callback,
    void* ctx
) {
    if (!callback) {
        EXCEPTION("s_expr_iterate_params: NULL callback");
        return;
    }
    if (!params) {
        if (count > 0) {
            EXCEPTION("s_expr_iterate_params: NULL params with non-zero count");
        }
        return;
    }
    
    uint16_t idx = 0;
    while (idx < count) {
        if (!callback(params, idx, ctx)) {
            break;
        }
        idx = s_expr_skip_param(params, idx);
    }
}

// ============================================================================
// ============================================================================
// 64-BIT STORAGE ACCESSORS
// Uses pointer_array indexed by pointer_base (set by pt_m_call dispatch)
// NOTE: Only valid when called from pt_m_call functions
// ============================================================================

uint64_t s_expr_get_user_u64(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_get_user_u64: NULL instance");
        return 0;
    }
    if (!inst->pointer_array) {
        EXCEPTION("s_expr_get_user_u64: NULL pointer_array");
        return 0;
    }
    if (!inst->in_pointer_call) {
        EXCEPTION("s_expr_get_user_u64: called outside pt_m_call context");
        return 0;
    }
    if (inst->pointer_base >= inst->pointer_count) {
        EXCEPTION("s_expr_get_user_u64: pointer_base out of range");
        return 0;
    }
    return inst->pointer_array[inst->pointer_base].u64;
}

void s_expr_set_user_u64(s_expr_tree_instance_t* inst, uint64_t value) {
    if (!inst) {
        EXCEPTION("s_expr_set_user_u64: NULL instance");
        return;
    }
    if (!inst->pointer_array) {
        EXCEPTION("s_expr_set_user_u64: NULL pointer_array");
        return;
    }
    if (!inst->in_pointer_call) {
        EXCEPTION("s_expr_set_user_u64: called outside pt_m_call context");
        return;
    }
    if (inst->pointer_base >= inst->pointer_count) {
        EXCEPTION("s_expr_set_user_u64: pointer_base out of range");
        return;
    }
    inst->pointer_array[inst->pointer_base].u64 = value;
}

double s_expr_get_user_f64(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_get_user_f64: NULL instance");
        return 0.0;
    }
    if (!inst->pointer_array) {
        EXCEPTION("s_expr_get_user_f64: NULL pointer_array");
        return 0.0;
    }
    if (!inst->in_pointer_call) {
        EXCEPTION("s_expr_get_user_f64: called outside pt_m_call context");
        return 0.0;
    }
    if (inst->pointer_base >= inst->pointer_count) {
        EXCEPTION("s_expr_get_user_f64: pointer_base out of range");
        return 0.0;
    }
    return inst->pointer_array[inst->pointer_base].f64;
}

void s_expr_set_user_f64(s_expr_tree_instance_t* inst, double value) {
    if (!inst) {
        EXCEPTION("s_expr_set_user_f64: NULL instance");
        return;
    }
    if (!inst->pointer_array) {
        EXCEPTION("s_expr_set_user_f64: NULL pointer_array");
        return;
    }
    if (!inst->in_pointer_call) {
        EXCEPTION("s_expr_set_user_f64: called outside pt_m_call context");
        return;
    }
    if (inst->pointer_base >= inst->pointer_count) {
        EXCEPTION("s_expr_set_user_f64: pointer_base out of range");
        return;
    }
    inst->pointer_array[inst->pointer_base].f64 = value;
}

int64_t s_expr_get_user_i64(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_get_user_i64: NULL instance");
        return 0;
    }
    if (!inst->pointer_array) {
        EXCEPTION("s_expr_get_user_i64: NULL pointer_array");
        return 0;
    }
    if (!inst->in_pointer_call) {
        EXCEPTION("s_expr_get_user_i64: called outside pt_m_call context");
        return 0;
    }
    if (inst->pointer_base >= inst->pointer_count) {
        EXCEPTION("s_expr_get_user_i64: pointer_base out of range");
        return 0;
    }
    return (int64_t)inst->pointer_array[inst->pointer_base].u64;
}

void s_expr_set_user_i64(s_expr_tree_instance_t* inst, int64_t value) {
    if (!inst) {
        EXCEPTION("s_expr_set_user_i64: NULL instance");
        return;
    }
    if (!inst->pointer_array) {
        EXCEPTION("s_expr_set_user_i64: NULL pointer_array");
        return;
    }
    if (!inst->in_pointer_call) {
        EXCEPTION("s_expr_set_user_i64: called outside pt_m_call context");
        return;
    }
    if (inst->pointer_base >= inst->pointer_count) {
        EXCEPTION("s_expr_set_user_i64: pointer_base out of range");
        return;
    }
    inst->pointer_array[inst->pointer_base].u64 = (uint64_t)value;
}

void* s_expr_get_user_ptr(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_get_user_ptr: NULL instance");
        return NULL;
    }
    if (!inst->pointer_array) {
        EXCEPTION("s_expr_get_user_ptr: NULL pointer_array");
        return NULL;
    }
    if (!inst->in_pointer_call) {
        EXCEPTION("s_expr_get_user_ptr: called outside pt_m_call context");
        return NULL;
    }
    if (inst->pointer_base >= inst->pointer_count) {
        EXCEPTION("s_expr_get_user_ptr: pointer_base out of range");
        return NULL;
    }
    return inst->pointer_array[inst->pointer_base].ptr;
}

void s_expr_set_user_ptr(s_expr_tree_instance_t* inst, void* value) {
    if (!inst) {
        EXCEPTION("s_expr_set_user_ptr: NULL instance");
        return;
    }
    if (!inst->pointer_array) {
        EXCEPTION("s_expr_set_user_ptr: NULL pointer_array");
        return;
    }
    if (!inst->in_pointer_call) {
        EXCEPTION("s_expr_set_user_ptr: called outside pt_m_call context");
        return;
    }
    if (inst->pointer_base >= inst->pointer_count) {
        EXCEPTION("s_expr_set_user_ptr: pointer_base out of range");
        return;
    }
    inst->pointer_array[inst->pointer_base].ptr = value;
}