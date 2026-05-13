// ============================================================================
// s_engine_node.h
// Node Support Functions for Composite Nodes
// ============================================================================

#ifndef S_ENGINE_NODE_H
#define S_ENGINE_NODE_H



#ifdef __cplusplus
extern "C" {
#endif
#include "s_engine_types.h"
// ============================================================================
// TREE LIFECYCLE (node-controlled model)
// ============================================================================

// Tick tree - invokes root only
s_expr_result_t s_expr_node_tick(
    s_expr_tree_instance_t* inst,
    uint16_t event_id,
    void* event_data
);

// Reset tree - all nodes inactive except root
void s_expr_node_reset(s_expr_tree_instance_t* inst);

// Terminate tree - backward walk, TERMINATE to initialized nodes
void s_expr_node_terminate(s_expr_tree_instance_t* inst);

// Full reset - terminate then reset
void s_expr_node_full_reset(s_expr_tree_instance_t* inst);

// Initialize - set root active only
void s_expr_node_init_states(s_expr_tree_instance_t* inst);

// ============================================================================
// CHILD ENUMERATION
// ============================================================================

// Count logical children in param array
uint16_t s_expr_child_count(
    const s_expr_param_t* params,
    uint16_t param_count
);

// Get physical index of Nth logical child (0-based)
// Returns UINT16_MAX if N >= child count
uint16_t s_expr_child_index(
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
);

// ============================================================================
// CHILD LIFECYCLE
// ============================================================================
void s_expr_reset_recursive_at(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t phys_idx
);
// Reset Nth child: clear flags so next invoke sends INIT
// Does not send TERMINATE, does not recurse
void s_expr_child_reset(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
);
// Recursively reset Nth child and all its descendants
// Clears flags, sets ACTIVE, recurses into nested callables
void s_expr_child_reset_recursive(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
);
// Terminate Nth child: send TERMINATE if initialized
// Child is responsible for propagating to its children
void s_expr_child_terminate(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
);

// Terminate all children in reverse order
// Sends TERMINATE to each initialized MAIN child, back to front
void s_expr_children_terminate_all(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count
);

// Reset all children (clear flags, no TERMINATE sent)
void s_expr_children_reset_all(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count
);

// ============================================================================
// CHILD INVOCATION
// Sends INIT automatically if not yet initialized
// Handles SE_DISABLE by terminating child and returning SE_CONTINUE
// ============================================================================

// Invoke Nth child, auto-detect type
// This is the primary invocation function
s_expr_result_t s_expr_child_invoke(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
);

// Invoke Nth child specifically as MAIN
s_expr_result_t s_expr_child_invoke_main(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
);

// Invoke Nth child as PRED, returns bool
bool s_expr_child_invoke_pred(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
);

// Invoke Nth child as ONESHOT
void s_expr_child_invoke_oneshot(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
);

// ============================================================================
// CHILD TYPE INSPECTION
// ============================================================================

// Check if Nth child is a callable (OPEN_CALL)
bool s_expr_child_is_callable(
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
);

// Get function type of Nth child (S_EXPR_PARAM_MAIN, PRED, ONESHOT)
// Returns 0 if not a callable
uint8_t s_expr_child_func_type(
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
);

// Check if Nth child is currently active
bool s_expr_child_is_active(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
);

// Check if Nth child has been initialized
bool s_expr_child_is_initialized(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index
);

// ============================================================================
// S-EXPRESSION PAYLOAD STRUCTURE
// Standard structure for passing S-expressions via event_data
// ============================================================================

typedef struct {
    const s_expr_param_t* params;
    uint16_t param_count;
    void* context;  // Optional additional context
} s_expr_payload_t;

// ============================================================================
// EXTENDED CHILD INVOCATION
// Explicit event context on caller's stack
// Saves/restores instance event state internally
// ============================================================================

// Invoke Nth child with explicit event, auto-detect type
s_expr_result_t s_expr_child_invoke_ex(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index,
    uint16_t event_id,
    void* event_data
);

// Invoke Nth child as MAIN with explicit event
s_expr_result_t s_expr_child_invoke_main_ex(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index,
    uint16_t event_id,
    void* event_data
);

// Invoke Nth child as PRED with explicit event
bool s_expr_child_invoke_pred_ex(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index,
    uint16_t event_id,
    void* event_data
);

// Invoke Nth child as ONESHOT with explicit event
void s_expr_child_invoke_oneshot_ex(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t logical_index,
    uint16_t event_id,
    void* event_data
);

// ============================================================================
// EXTENDED BULK OPERATIONS
// ============================================================================

// Broadcast explicit event to all children
// Returns last non-CONTINUE result, or CONTINUE if all children return CONTINUE
// Stops early on SE_TERMINATE
s_expr_result_t s_expr_children_broadcast_ex(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t event_id,
    void* event_data
);

// ============================================================================
// EXECUTE PASSED S-EXPRESSION
// Invoke S-expression received via event_data
// Uses same instance/state - no isolation
// Finds first callable in params and invokes it
// ============================================================================

s_expr_result_t s_expr_invoke_params(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    uint16_t event_id,
    void* event_data
);

// ============================================================================
// CONVENIENCE MACROS
// ============================================================================

// Create payload from params
#define S_EXPR_PAYLOAD(p, c) \
    (s_expr_payload_t){ .params = (p), .param_count = (c), .context = NULL }

// Create payload with context
#define S_EXPR_PAYLOAD_CTX(p, c, ctx) \
    (s_expr_payload_t){ .params = (p), .param_count = (c), .context = (ctx) }
#ifdef __cplusplus
}
#endif

#endif // S_ENGINE_NODE_H