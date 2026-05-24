// ============================================================================
// s_engine_node.c
// Node Support Functions Implementation
// ============================================================================

#include "s_engine_node.h"
#include "s_engine_module.h"
#include "s_engine_eval.h"
#include "s_engine_stack.h"
#include "s_engine_exception.h"

// Layer-2 hardware-watchdog hook (per wdt-layer2-pet-from-s-engine memory).
// Chip ports provide a strong override of s_engine_chip_wdt_pet() that calls
// their HAL pet. Linux and any chip port that hasn't added WDT yet inherits
// this weak no-op. Pet site is the top of s_expr_node_tick — "if chains
// stop progressing, the dongle is functionally dead, so the WDT bites."
__attribute__((weak)) void s_engine_chip_wdt_pet(void) { }

// ============================================================================
// INTERNAL: Skip one logical parameter
// ============================================================================

static uint16_t skip_logical_param(const s_expr_param_t* params, uint16_t idx) {
    uint8_t opcode = params[idx].type & S_EXPR_OPCODE_MASK;
    
    if (opcode == S_EXPR_PARAM_OPEN || 
        opcode == S_EXPR_PARAM_OPEN_CALL ||
        opcode == S_EXPR_PARAM_OPEN_DICT ||
        opcode == S_EXPR_PARAM_OPEN_ARRAY ||
        opcode == S_EXPR_PARAM_OPEN_TUPLE ||
        opcode == S_EXPR_PARAM_OPEN_KEY) {
        return idx + params[idx].brace_idx + 1;
    }
    return idx + 1;
}

// ============================================================================
// INTERNAL: Get node state for callable at physical index
// ============================================================================

static s_expr_node_state_t* get_callable_state(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t physical_idx
) {
    uint8_t opcode = params[physical_idx].type & S_EXPR_OPCODE_MASK;
    
    if (opcode != S_EXPR_PARAM_OPEN_CALL) {
        return NULL;
    }
    
    const s_expr_param_t* func_param = &params[physical_idx + 1];
    uint16_t node_idx = func_param->node_index;
    
    if (node_idx >= inst->node_count) {
        EXCEPTION("get_callable_state: node_index out of range");
        return NULL;
    }
    
    return &inst->node_states[node_idx];
}

// ============================================================================
// INTERNAL: Terminate node at physical index
// ============================================================================

static void terminate_at_physical_index(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t phys_idx
) {
    const s_expr_param_t* func_param = &params[phys_idx + 1];
    uint16_t func_idx = func_param->func_index;
    uint16_t node_idx = func_param->node_index;
    bool is_pointer_call = (func_param->type & S_EXPR_FLAG_POINTER) != 0;
    uint8_t pointer_base = func_param->index_to_pointer;
    
    if (func_idx >= inst->module->def->main_count) {
        EXCEPTION("terminate_at_physical_index: func_index out of range");
        return;
    }
    
    s_expr_main_fn_t fn = inst->module->main_fns[func_idx];
    if (!fn) {
        EXCEPTION("terminate_at_physical_index: NULL function pointer");
        return;
    }
    
    uint16_t close_idx = phys_idx + params[phys_idx].brace_idx;
    uint16_t arg_count = (close_idx > phys_idx + 2) ? (close_idx - phys_idx - 2) : 0;
    const s_expr_param_t* args = (arg_count > 0) ? &params[phys_idx + 2] : NULL;
    
    uint16_t saved_node = inst->current_node_index;
    bool saved_in_ptr = inst->in_pointer_call;
    uint8_t saved_ptr_base = inst->pointer_base;
    
    inst->current_node_index = node_idx;
    if (is_pointer_call) {
        inst->in_pointer_call = true;
        inst->pointer_base = pointer_base;
    }
    
    fn(inst, args, arg_count, SE_EVENT_TERMINATE, 0, NULL);
    
    inst->current_node_index = saved_node;
    inst->in_pointer_call = saved_in_ptr;
    inst->pointer_base = saved_ptr_base;
    
    // Clear state
    inst->node_states[node_idx].flags = 0;
    inst->node_states[node_idx].state = 0;
    inst->node_states[node_idx].user_data = 0;
}

// ============================================================================
// TREE LIFECYCLE
// ============================================================================

s_expr_result_t s_expr_node_tick(
    s_expr_tree_instance_t* inst,
    uint16_t event_id,
    void* event_data
) {
    // Layer-2 WDT pet — runs once per chain pump cycle. Weak no-op when no
    // chip has overridden it (Linux, unported chips).
    s_engine_chip_wdt_pet();

    if (!inst) {
        EXCEPTION("s_expr_node_tick: NULL instance");
        return SE_FUNCTION_TERMINATE;
    }
    if (!inst->tree) {
        EXCEPTION("s_expr_node_tick: NULL tree");
        return SE_FUNCTION_TERMINATE;
    }

    // Reset stack at start of each tick
    if (inst->stack) {
        s_expr_tree_reset_stack(inst);
    }
    
    const s_expr_param_t* params = inst->tree->params;
    uint16_t param_count = inst->tree->param_count;
    
    if (!params || param_count == 0) {
        EXCEPTION("s_expr_node_tick: empty tree");
        return SE_FUNCTION_TERMINATE;
    }
    
    // Verify first param is a callable
    uint8_t opcode = params[0].type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_OPEN_CALL) {
        EXCEPTION("s_expr_node_tick: root is not OPEN_CALL");
        return SE_FUNCTION_TERMINATE;
    }
    
    // Check root node is active
    const s_expr_param_t* func_param = &params[1];
    uint16_t node_idx = func_param->node_index;
    
    if (node_idx >= inst->node_count) {
        EXCEPTION("s_expr_node_tick: root node_index out of range");
        return SE_FUNCTION_TERMINATE;
    }
    
    if (!(inst->node_states[node_idx].flags & S_EXPR_NODE_FLAG_ACTIVE)) {
        return SE_FUNCTION_TERMINATE;  // Root inactive, tree complete
    }
    
    // Store event context for child invocations
    inst->current_event_id = event_id;
    inst->current_event_data = event_data;
    
    // Invoke root - it controls everything else
    s_expr_result_t result = s_expr_invoke_main(inst, params, 0);
   
    
    
    
    return result;
}

void s_expr_node_reset(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_node_reset: NULL instance");
        return;
    }
    if (!inst->node_states && inst->node_count > 0) {
        EXCEPTION("s_expr_node_reset: NULL node_states");
        return;
    }
    
    // All nodes inactive, preserve EVER_INIT
    for (uint16_t i = 0; i < inst->node_count; i++) {
        uint8_t ever_init = inst->node_states[i].flags & S_EXPR_NODE_FLAG_EVER_INIT;
        inst->node_states[i].flags = ever_init;  // Not active
        inst->node_states[i].state = 0;
        inst->node_states[i].user_data = 0;
    }
    
    // Activate root only
    if (!inst->tree || !inst->tree->params || inst->tree->param_count == 0) {
        return;
    }
    
    uint8_t opcode = inst->tree->params[0].type & S_EXPR_OPCODE_MASK;
    if (opcode == S_EXPR_PARAM_OPEN_CALL) {
        uint16_t root_node_idx = inst->tree->params[1].node_index;
        if (root_node_idx < inst->node_count) {
            inst->node_states[root_node_idx].flags |= S_EXPR_NODE_FLAG_ACTIVE;
        }
    }
}
// ============================================================================
// INTERNAL: Recursively reset all callables in params
// ============================================================================

static void reset_all_recursive(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count
) {
    uint16_t idx = 0;
    
    while (idx < param_count) {
        uint8_t opcode = params[idx].type & S_EXPR_OPCODE_MASK;
        
        if (opcode == S_EXPR_PARAM_OPEN_CALL) {
            // Reset this callable's state
            s_expr_node_state_t* state = get_callable_state(inst, params, idx);
            if (state) {
                uint8_t ever_init = state->flags & S_EXPR_NODE_FLAG_EVER_INIT;
                state->flags = S_EXPR_NODE_FLAG_ACTIVE | ever_init;
                state->state = 0;
                state->user_data = 0;
            }
            
            // Recurse into this callable's arguments
            uint16_t child_arg_count;
            const s_expr_param_t* child_args = s_expr_call_args(params, idx, &child_arg_count);
            if (child_args && child_arg_count > 0) {
                reset_all_recursive(inst, child_args, child_arg_count);
            }
        }
        
        idx = skip_logical_param(params, idx);
    }
}

// ============================================================================
// PUBLIC: Recursively reset Nth child and all its descendants
// ============================================================================

void s_expr_child_reset_recursive(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
) {
    if (!inst) {
        EXCEPTION("s_expr_child_reset_recursive: NULL instance");
        return;
    }
    
    uint16_t phys_idx = s_expr_child_index(params, param_count, logical_index);
    if (phys_idx == UINT16_MAX) {
        EXCEPTION("s_expr_child_reset_recursive: logical_index out of range");
        return;
    }
    
    uint8_t opcode = params[phys_idx].type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_OPEN_CALL) {
        return;  // Not a callable, nothing to reset
    }
    
    // Reset the child itself
    s_expr_node_state_t* state = get_callable_state(inst, params, phys_idx);
    if (state) {
        uint8_t ever_init = state->flags & S_EXPR_NODE_FLAG_EVER_INIT;
        state->flags = S_EXPR_NODE_FLAG_ACTIVE | ever_init;
        state->state = 0;
        state->user_data = 0;
    }
    
    // Recurse into child's arguments
    uint16_t child_arg_count;
    const s_expr_param_t* child_args = s_expr_call_args(params, phys_idx, &child_arg_count);
    if (child_args && child_arg_count > 0) {
        reset_all_recursive(inst, child_args, child_arg_count);
    }
}
void s_expr_reset_recursive_at(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t phys_idx
) {
    if (!inst) {
        EXCEPTION("s_expr_reset_recursive_at: NULL instance");
        return;
    }
    
    uint8_t opcode = params[phys_idx].type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_OPEN_CALL) {
        return;  // Not a callable, nothing to reset
    }
    
    // Reset the node itself
    s_expr_node_state_t* state = get_callable_state(inst, params, phys_idx);
    if (state) {
        uint8_t ever_init = state->flags & S_EXPR_NODE_FLAG_EVER_INIT;
        state->flags = S_EXPR_NODE_FLAG_ACTIVE | ever_init;
        state->state = 0;
        state->user_data = 0;
    }
    
    // Recurse into arguments
    uint16_t child_arg_count;
    const s_expr_param_t* child_args = s_expr_call_args(params, phys_idx, &child_arg_count);
    if (child_args && child_arg_count > 0) {
        reset_all_recursive(inst, child_args, child_arg_count);
    }
}

void s_expr_node_terminate(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_node_terminate: NULL instance");
        return;
    }
    if (!inst->tree) {
        EXCEPTION("s_expr_node_terminate: NULL tree");
        return;
    }
    if (!inst->module) {
        EXCEPTION("s_expr_node_terminate: NULL module");
        return;
    }
    
    const s_expr_param_t* params = inst->tree->params;
    uint16_t param_count = inst->tree->param_count;
    
    if (!params || param_count == 0) {
        return;
    }
    
    // Terminate all initialized MAINs in reverse order
    s_expr_children_terminate_all(inst, params, param_count);
    
    // Clear all states
    for (uint16_t i = 0; i < inst->node_count; i++) {
        inst->node_states[i].flags = 0;
        inst->node_states[i].state = 0;
        inst->node_states[i].user_data = 0;
    }
}

void s_expr_node_full_reset(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_node_full_reset: NULL instance");
        return;
    }
    s_expr_node_terminate(inst);
    s_expr_node_init_states(inst);
}

void s_expr_node_init_states(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_node_init_states: NULL instance");
        return;
    }
    if (!inst->node_states && inst->node_count > 0) {
        EXCEPTION("s_expr_node_init_states: NULL node_states");
        return;
    }
    
    // All nodes inactive
    for (uint16_t i = 0; i < inst->node_count; i++) {
        inst->node_states[i].flags = 0;
        inst->node_states[i].state = 0;
        inst->node_states[i].user_data = 0;
    }
    
    // Activate root only
    if (!inst->tree || !inst->tree->params || inst->tree->param_count == 0) {
        return;
    }
    
    uint8_t opcode = inst->tree->params[0].type & S_EXPR_OPCODE_MASK;
    if (opcode == S_EXPR_PARAM_OPEN_CALL) {
        uint16_t root_node_idx = inst->tree->params[1].node_index;
        if (root_node_idx < inst->node_count) {
            inst->node_states[root_node_idx].flags = S_EXPR_NODE_FLAG_ACTIVE;
        }
    }
}

// ============================================================================
// CHILD ENUMERATION
// ============================================================================

uint16_t s_expr_child_count(
    const s_expr_param_t* params,
    uint16_t param_count
) {
    if (!params) {
        if (param_count > 0) {
            EXCEPTION("s_expr_child_count: NULL params with non-zero count");
        }
        return 0;
    }
    
    uint16_t count = 0;
    uint16_t idx = 0;
    
    while (idx < param_count) {
        count++;
        idx = skip_logical_param(params, idx);
    }
    
    return count;
}

uint16_t s_expr_child_index(
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
) {
    if (!params) {
        if (param_count > 0) {
            EXCEPTION("s_expr_child_index: NULL params with non-zero count");
        }
        return UINT16_MAX;
    }
    
    uint16_t current = 0;
    uint16_t idx = 0;
    
    while (idx < param_count) {
        if (current == logical_index) {
            return idx;
        }
        current++;
        idx = skip_logical_param(params, idx);
    }
    
    return UINT16_MAX;
}

// ============================================================================
// CHILD LIFECYCLE
// ============================================================================

void s_expr_child_reset(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
) {
    if (!inst) {
        EXCEPTION("s_expr_child_reset: NULL instance");
        return;
    }
    
    uint16_t phys_idx = s_expr_child_index(params, param_count, logical_index);
    if (phys_idx == UINT16_MAX) {
        EXCEPTION("s_expr_child_reset: logical_index out of range");
        return;
    }
    
    s_expr_node_state_t* state = get_callable_state(inst, params, phys_idx);
    if (!state) {
        return;  // Not a callable, nothing to reset
    }
    
    uint8_t ever_init = state->flags & S_EXPR_NODE_FLAG_EVER_INIT;
    state->flags = S_EXPR_NODE_FLAG_ACTIVE | ever_init;
    state->state = 0;
    state->user_data = 0;
}

void s_expr_child_terminate(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
) {
    if (!inst) {
        EXCEPTION("s_expr_child_terminate: NULL instance");
        return;
    }
    if (!inst->module) {
        EXCEPTION("s_expr_child_terminate: NULL module");
        return;
    }
    
    uint16_t phys_idx = s_expr_child_index(params, param_count, logical_index);
    if (phys_idx == UINT16_MAX) {
        EXCEPTION("s_expr_child_terminate: logical_index out of range");
        return;
    }
    
    uint8_t opcode = params[phys_idx].type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_OPEN_CALL) {
        return;  // Not a callable
    }
    
    const s_expr_param_t* func_param = &params[phys_idx + 1];
    uint8_t func_opcode = func_param->type & S_EXPR_OPCODE_MASK;
    
    if (func_opcode != S_EXPR_PARAM_MAIN) {
        return;  // Only MAIN nodes receive TERMINATE
    }
    
    s_expr_node_state_t* state = get_callable_state(inst, params, phys_idx);
    if (!state) {
        return;
    }
    
    if (!(state->flags & S_EXPR_NODE_FLAG_INITIALIZED)) {
        return;  // Never initialized, no cleanup needed
    }
    
    terminate_at_physical_index(inst, params, phys_idx);
}

void s_expr_children_terminate_all(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count
) {
    if (!inst) {
        EXCEPTION("s_expr_children_terminate_all: NULL instance");
        return;
    }
    if (!params && param_count > 0) {
        EXCEPTION("s_expr_children_terminate_all: NULL params with non-zero count");
        return;
    }
    if (param_count == 0) {
        return;
    }
    
    // Count logical children to iterate backward
    uint16_t child_count = s_expr_child_count(params, param_count);
    if (child_count == 0) {
        return;
    }
    
    // Iterate backward through all children
    for (int16_t i = child_count - 1; i >= 0; i--) {
        uint16_t idx = s_expr_child_index(params, param_count, (uint16_t)i);
        if (idx == UINT16_MAX) {
            continue;
        }
        
        uint8_t opcode = params[idx].type & S_EXPR_OPCODE_MASK;
        if (opcode != S_EXPR_PARAM_OPEN_CALL) {
            continue;
        }
        
        const s_expr_param_t* func_param = &params[idx + 1];
        uint8_t func_opcode = func_param->type & S_EXPR_OPCODE_MASK;
        
        // Terminate MAINs if initialized
        if (func_opcode == S_EXPR_PARAM_MAIN) {
            uint16_t node_idx = func_param->node_index;
            if (node_idx < inst->node_count &&
                (inst->node_states[node_idx].flags & S_EXPR_NODE_FLAG_INITIALIZED)) {
                terminate_at_physical_index(inst, params, idx);
            }
        }
        
        // Reset ALL callable states (oneshots, predicates, MAINs)
        s_expr_node_state_t* state = get_callable_state(inst, params, idx);
        if (state) {
            uint8_t ever_init = state->flags & S_EXPR_NODE_FLAG_EVER_INIT;
            state->flags = S_EXPR_NODE_FLAG_ACTIVE | ever_init;
            state->state = 0;
            state->user_data = 0;
        }
    }
}

void s_expr_children_reset_all(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count
) {
    if (!inst) {
        EXCEPTION("s_expr_children_reset_all: NULL instance");
        return;
    }
    if (!params && param_count > 0) {
        EXCEPTION("s_expr_children_reset_all: NULL params with non-zero count");
        return;
    }
    
    uint16_t idx = 0;
    
    while (idx < param_count) {
        s_expr_node_state_t* state = get_callable_state(inst, params, idx);
        if (state) {
            uint8_t ever_init = state->flags & S_EXPR_NODE_FLAG_EVER_INIT;
            state->flags = S_EXPR_NODE_FLAG_ACTIVE | ever_init;
            state->state = 0;
            state->user_data = 0;
        }
        idx = skip_logical_param(params, idx);
    }
}

// ============================================================================
// CHILD INVOCATION
// ============================================================================

s_expr_result_t s_expr_child_invoke(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
) {
    if (!inst) {
        EXCEPTION("s_expr_child_invoke: NULL instance");
        return SE_TERMINATE;
    }
    
    uint16_t phys_idx = s_expr_child_index(params, param_count, logical_index);
    if (phys_idx == UINT16_MAX) {
        EXCEPTION("s_expr_child_invoke: logical_index out of range");
        return SE_TERMINATE;
    }
    
    s_expr_result_t result = s_expr_invoke_any(inst, params, phys_idx);
    
    if (result == SE_DISABLE) {
        // Child completed - terminate it
        s_expr_child_terminate(inst, params, param_count, logical_index);
        result = SE_CONTINUE;
    }
    
    return result;
}

// Invoke Nth child specifically as MAIN
s_expr_result_t s_expr_child_invoke_main(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
) {
    if (!inst) {
        EXCEPTION("s_expr_child_invoke_main: NULL instance");
        return SE_TERMINATE;
    }
    
    uint16_t phys_idx = s_expr_child_index(params, param_count, logical_index);
    if (phys_idx == UINT16_MAX) {
        EXCEPTION("s_expr_child_invoke_main: logical_index out of range");
        return SE_TERMINATE;
    }
    
    s_expr_result_t result = s_expr_invoke_main(inst, params, phys_idx);
    
    if (result == SE_PIPELINE_DISABLE) {
        // Child completed - terminate it
        s_expr_child_terminate(inst, params, param_count, logical_index);
        result = SE_PIPELINE_CONTINUE;
    }
    
    return result;
}

bool s_expr_child_invoke_pred(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
) {
    if (!inst) {
        EXCEPTION("s_expr_child_invoke_pred: NULL instance");
        return false;
    }
    
    uint16_t phys_idx = s_expr_child_index(params, param_count, logical_index);
    if (phys_idx == UINT16_MAX) {
        EXCEPTION("s_expr_child_invoke_pred: logical_index out of range");
        return false;
    }
    
    return s_expr_invoke_pred(inst, params, phys_idx);
}

void s_expr_child_invoke_oneshot(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
) {
    if (!inst) {
        EXCEPTION("s_expr_child_invoke_oneshot: NULL instance");
        return;
    }
    
    uint16_t phys_idx = s_expr_child_index(params, param_count, logical_index);
    if (phys_idx == UINT16_MAX) {
        EXCEPTION("s_expr_child_invoke_oneshot: logical_index out of range");
        return;
    }
    
    s_expr_invoke_oneshot(inst, params, phys_idx);
}

// ============================================================================
// CHILD TYPE INSPECTION
// ============================================================================

bool s_expr_child_is_callable(
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
) {
    uint16_t phys_idx = s_expr_child_index(params, param_count, logical_index);
    if (phys_idx == UINT16_MAX) {
        return false;
    }
    
    uint8_t opcode = params[phys_idx].type & S_EXPR_OPCODE_MASK;
    return opcode == S_EXPR_PARAM_OPEN_CALL;
}

uint8_t s_expr_child_func_type(
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
) {
    uint16_t phys_idx = s_expr_child_index(params, param_count, logical_index);
    if (phys_idx == UINT16_MAX) {
        return 0;
    }
    
    uint8_t opcode = params[phys_idx].type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_OPEN_CALL) {
        return 0;
    }
    
    return params[phys_idx + 1].type & S_EXPR_OPCODE_MASK;
}

bool s_expr_child_is_active(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
) {
    if (!inst) {
        EXCEPTION("s_expr_child_is_active: NULL instance");
        return false;
    }
    
    uint16_t phys_idx = s_expr_child_index(params, param_count, logical_index);
    if (phys_idx == UINT16_MAX) {
        return false;
    }
    
    s_expr_node_state_t* state = get_callable_state(inst, params, phys_idx);
    if (!state) {
        return false;
    }
    
    return (state->flags & S_EXPR_NODE_FLAG_ACTIVE) != 0;
}

bool s_expr_child_is_initialized(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
) {
    if (!inst) {
        EXCEPTION("s_expr_child_is_initialized: NULL instance");
        return false;
    }
    
    uint16_t phys_idx = s_expr_child_index(params, param_count, logical_index);
    if (phys_idx == UINT16_MAX) {
        return false;
    }
    
    s_expr_node_state_t* state = get_callable_state(inst, params, phys_idx);
    if (!state) {
        return false;
    }
    
    return (state->flags & S_EXPR_NODE_FLAG_INITIALIZED) != 0;
}

// ============================================================================
// s_engine_node_ex.c
// Extended Node API - Explicit Event Context
// ============================================================================

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

// Save current event context
static inline void save_event_context(
    s_expr_tree_instance_t* inst,
    uint16_t* saved_event_id,
    void** saved_event_data
) {
    *saved_event_id = inst->current_event_id;
    *saved_event_data = inst->current_event_data;
}

// Restore event context
static inline void restore_event_context(
    s_expr_tree_instance_t* inst,
    uint16_t saved_event_id,
    void* saved_event_data
) {
    inst->current_event_id = saved_event_id;
    inst->current_event_data = saved_event_data;
}

// Set new event context
static inline void set_event_context(
    s_expr_tree_instance_t* inst,
    uint16_t event_id,
    void* event_data
) {
    inst->current_event_id = event_id;
    inst->current_event_data = event_data;
}

// ============================================================================
// EXTENDED CHILD INVOCATION
// ============================================================================

s_expr_result_t s_expr_child_invoke_ex(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index,
    uint16_t event_id,
    void* event_data
) {
    if (!inst) {
        EXCEPTION("s_expr_child_invoke_ex: NULL instance");
        return SE_TERMINATE;
    }
    
    // Save current context
    uint16_t saved_event_id;
    void* saved_event_data;
    save_event_context(inst, &saved_event_id, &saved_event_data);
    
    // Set override
    set_event_context(inst, event_id, event_data);
    
    // Delegate to legacy API
    s_expr_result_t result = s_expr_child_invoke(inst, params, param_count, logical_index);
    
    // Restore
    restore_event_context(inst, saved_event_id, saved_event_data);
    
    return result;
}

s_expr_result_t s_expr_child_invoke_main_ex(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index,
    uint16_t event_id,
    void* event_data
) {
    if (!inst) {
        EXCEPTION("s_expr_child_invoke_main_ex: NULL instance");
        return SE_TERMINATE;
    }
    
    // Save current context
    uint16_t saved_event_id;
    void* saved_event_data;
    save_event_context(inst, &saved_event_id, &saved_event_data);
    
    // Set override
    set_event_context(inst, event_id, event_data);
    
    // Delegate to legacy API
    s_expr_result_t result = s_expr_child_invoke_main(inst, params, param_count, logical_index);
    
    // Restore
    restore_event_context(inst, saved_event_id, saved_event_data);
    
    return result;
}

bool s_expr_child_invoke_pred_ex(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index,
    uint16_t event_id,
    void* event_data
) {
    if (!inst) {
        EXCEPTION("s_expr_child_invoke_pred_ex: NULL instance");
        return false;
    }
    
    // Save current context
    uint16_t saved_event_id;
    void* saved_event_data;
    save_event_context(inst, &saved_event_id, &saved_event_data);
    
    // Set override
    set_event_context(inst, event_id, event_data);
    
    // Delegate to legacy API
    bool result = s_expr_child_invoke_pred(inst, params, param_count, logical_index);
    
    // Restore
    restore_event_context(inst, saved_event_id, saved_event_data);
    
    return result;
}

void s_expr_child_invoke_oneshot_ex(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index,
    uint16_t event_id,
    void* event_data
) {
    if (!inst) {
        EXCEPTION("s_expr_child_invoke_oneshot_ex: NULL instance");
        return;
    }
    
    // Save current context
    uint16_t saved_event_id;
    void* saved_event_data;
    save_event_context(inst, &saved_event_id, &saved_event_data);
    
    // Set override
    set_event_context(inst, event_id, event_data);
    
    // Delegate to legacy API
    s_expr_child_invoke_oneshot(inst, params, param_count, logical_index);
    
    // Restore
    restore_event_context(inst, saved_event_id, saved_event_data);
}

// ============================================================================
// EXTENDED BULK OPERATIONS
// ============================================================================
#if 0
s_expr_result_t s_expr_children_broadcast_ex(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t event_id,
    void* event_data
) {
    if (!inst) {
        EXCEPTION("s_expr_children_broadcast_ex: NULL instance");
        return SE_TERMINATE;
    }
    
    // Save current context
    uint16_t saved_event_id;
    void* saved_event_data;
    save_event_context(inst, &saved_event_id, &saved_event_data);
    
    // Set override
    set_event_context(inst, event_id, event_data);
    
    // Get child count and iterate
    uint16_t child_count = s_expr_child_count(params, param_count);
    s_expr_result_t result = SE_CONTINUE;
    
    for (uint16_t i = 0; i < child_count; i++) {
        s_expr_result_t child_result = s_expr_child_invoke(inst, params, param_count, i);
        
        // Capture non-CONTINUE results
        if (child_result != SE_CONTINUE) {
            result = child_result;
        }
        
        // Stop on TERMINATE
        if (child_result == SE_TERMINATE) {
            break;
        }
    }
    
    // Restore
    restore_event_context(inst, saved_event_id, saved_event_data);
    
    return result;
}
#endif
// ============================================================================
// EXECUTE PASSED S-EXPRESSION
// ============================================================================

s_expr_result_t s_expr_invoke_params(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t event_id,
    void* event_data
) {
    if (!inst) {
        EXCEPTION("s_expr_invoke_params: NULL instance");
        return SE_TERMINATE;
    }
    
    if (!params || param_count == 0) {
        EXCEPTION("s_expr_invoke_params: invalid params");
        return SE_TERMINATE;
    }
    
    // Find first callable in params
    for (uint16_t i = 0; i < param_count; i++) {
        if (s_expr_child_is_callable(params, param_count, i)) {
            // Save current context
            uint16_t saved_event_id;
            void* saved_event_data;
            save_event_context(inst, &saved_event_id, &saved_event_data);
            
            // Set new context
            set_event_context(inst, event_id, event_data);
            
            // Invoke the callable
            s_expr_result_t result = s_expr_invoke_any(inst, params, i);
            
            // Restore
            restore_event_context(inst, saved_event_id, saved_event_data);
            
            return result;
        }
    }
    
    EXCEPTION("s_expr_invoke_params: no callable found");
    return SE_TERMINATE;
}