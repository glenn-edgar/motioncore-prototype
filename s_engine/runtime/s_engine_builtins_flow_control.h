static s_expr_result_t se_if_then_else(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_id; 
    (void)event_data;
    
    // Expected structure: (if_then_else predicate then_action [else_action])
    // Child 0: predicate
    // Child 1: then branch
    // Child 2: else branch (optional)
    
    uint16_t count = s_expr_child_count(params, param_count);
    if (count < 2) {
        EXCEPTION("se_if_then_else: need at least predicate and then branch");
        return SE_PIPELINE_CONTINUE;
    }
    
    bool has_else = (count >= 3);
    
    const uint16_t PRED_CHILD = 0;
    const uint16_t THEN_CHILD = 1;
    const uint16_t ELSE_CHILD = 2;
    
    // =========================================================================
    // TERMINATE EVENT - pass through to all children
    // =========================================================================
    if (event_type == SE_EVENT_TERMINATE) {
        s_expr_children_terminate_all(inst, params, param_count);
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // INIT EVENT - pass through
    // =========================================================================
    if (event_type == SE_EVENT_INIT) {
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // TICK EVENT
    // =========================================================================
    
    // Evaluate predicate (child 0)
    uint16_t pred_phys_idx = s_expr_child_index(params, param_count, PRED_CHILD);
    bool condition = s_expr_invoke_pred(inst, params, pred_phys_idx);
    
    s_expr_result_t r;
    
    if (condition) {
        // Execute then branch (child 1)
        uint16_t phys_idx = s_expr_child_index(params, param_count, THEN_CHILD);
        r = s_expr_invoke_any(inst, params, phys_idx);
    } else if (has_else) {
        // Execute else branch (child 2)
        uint16_t phys_idx = s_expr_child_index(params, param_count, ELSE_CHILD);
        r = s_expr_invoke_any(inst, params, phys_idx);
    } else {
        // No else branch, condition false
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // RESULT HANDLING
    // =========================================================================
    
    // Non-PIPELINE codes (0-11): propagate to caller
    if (r < SE_PIPELINE_CONTINUE) {
        return r;
    }
    
    switch (r) {
        case SE_PIPELINE_CONTINUE:
        case SE_PIPELINE_HALT:
            return r;
            
        case SE_PIPELINE_RESET:
            // Recursive reset: terminate and reset all children
            s_expr_child_terminate(inst, params, param_count, THEN_CHILD);
            s_expr_child_reset(inst, params, param_count, THEN_CHILD);
            if (has_else) {
                s_expr_child_terminate(inst, params, param_count, ELSE_CHILD);
                s_expr_child_reset(inst, params, param_count, ELSE_CHILD);
            }
            return SE_PIPELINE_RESET;
            
        case SE_PIPELINE_DISABLE:
        case SE_PIPELINE_TERMINATE:
            s_expr_child_terminate(inst, params, param_count, THEN_CHILD);
            s_expr_child_reset(inst, params, param_count, THEN_CHILD);
            if (has_else) {
                s_expr_child_terminate(inst, params, param_count, ELSE_CHILD);
                s_expr_child_reset(inst, params, param_count, ELSE_CHILD);
            }
            return SE_PIPELINE_CONTINUE;
            
        case SE_PIPELINE_SKIP_CONTINUE:
            return SE_PIPELINE_CONTINUE;
            
        default:
            printf("se_if_then_else: unexpected result code %d\n", r);
            EXCEPTION("se_if_then_else: unexpected result code");
            return SE_PIPELINE_CONTINUE;
    }
}


static s_expr_result_t se_cond(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_id; (void)event_data;

    if (event_type == SE_EVENT_TERMINATE) {
        s_expr_children_terminate_all(inst, params, param_count);
        s_expr_set_user_flags(inst, 0xFFFF);
        return SE_PIPELINE_CONTINUE;
    }

    if (event_type == SE_EVENT_INIT) {
        s_expr_set_user_flags(inst, 0xFFFF);
        return SE_PIPELINE_CONTINUE;
    }

    // Parameter layout: [pred0] [action0] [pred1] [action1] ... [predN] [actionN]
    // Both predicates and actions are OPEN_CALL and counted as logical children.
    // Predicates are at even child indices: 0, 2, 4, ...
    // Actions are at odd child indices:    1, 3, 5, ...

    uint16_t active_child = s_expr_get_user_flags(inst);
    uint16_t matched_action = 0xFFFF;
    uint16_t child_index = 0;

    for (uint16_t i = 0; i < param_count; ) {
        if (s_expr_param_is_predicate(&params[i])) {
            bool result = s_expr_invoke_pred(inst, params, i);
            // Skip predicate
            i = s_expr_skip_param(params, i);
            child_index++;
            
            if (result && matched_action == 0xFFFF) {
                matched_action = child_index;  // action is next child
                break;
            }
            
            // Skip action
            i = s_expr_skip_param(params, i);
            child_index++;
        } else {
            i = s_expr_skip_param(params, i);
            child_index++;
        }
    }

    if (matched_action == 0xFFFF) {
        EXCEPTION("se_cond: no matching case (missing default)");
        return SE_PIPELINE_CONTINUE;
    }

    // Active child changed: terminate old, reset new
    if (matched_action != active_child) {
        if (active_child != 0xFFFF) {
            s_expr_child_terminate(inst, params, param_count, active_child);
            s_expr_child_reset_recursive(inst, params, param_count, active_child);
        }
        s_expr_child_terminate(inst, params, param_count, matched_action);
        s_expr_child_reset_recursive(inst, params, param_count, matched_action);
        s_expr_set_user_flags(inst, matched_action);
    }

    s_expr_result_t r = s_expr_child_invoke(inst, params, param_count, matched_action);

    // Non-PIPELINE codes (0-11): propagate to caller
    if (r < SE_PIPELINE_CONTINUE) {
        return r;
    }

    switch (r) {
        case SE_PIPELINE_CONTINUE:
        case SE_PIPELINE_HALT:
            return SE_PIPELINE_CONTINUE;
        case SE_PIPELINE_RESET:
            s_expr_child_terminate(inst, params, param_count, matched_action);
            s_expr_child_reset_recursive(inst, params, param_count, matched_action);
            return SE_PIPELINE_CONTINUE;
        case SE_PIPELINE_DISABLE:
        case SE_PIPELINE_TERMINATE:
        case SE_PIPELINE_SKIP_CONTINUE:
            return r;
        default:
            EXCEPTION("se_cond: unexpected result code");
            return SE_PIPELINE_CONTINUE;
    }
}


static s_expr_result_t se_trigger_on_change(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_id; 
    (void)event_data;
    
    // Expected structure: (trigger_on_change initial_state predicate rising_action [falling_action])
    // Child 0: initial state (INT 0 or 1)
    // Child 1: predicate
    // Child 2: rising action
    // Child 3: falling action (optional)
    
    uint16_t count = s_expr_child_count(params, param_count);
    if (count < 3) {
        EXCEPTION("se_trigger_on_change: need at least 3 children");
        return SE_PIPELINE_CONTINUE;
    }
    
    const uint16_t INIT_STATE_CHILD = 0;
    const uint16_t PRED_CHILD = 1;
    const uint16_t RISING_CHILD = 2;
    const uint16_t FALLING_CHILD = 3;
    bool has_falling = (count >= 4);
    
    // =========================================================================
    // TERMINATE EVENT
    // =========================================================================
    if (event_type == SE_EVENT_TERMINATE) {
        s_expr_children_terminate_all(inst, params, param_count);
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // INIT EVENT
    // =========================================================================
    if (event_type == SE_EVENT_INIT) {
        // Get initial state from child 0
        uint16_t init_phys_idx = s_expr_child_index(params, param_count, INIT_STATE_CHILD);
        uint8_t type0 = params[init_phys_idx].type & S_EXPR_OPCODE_MASK;
        
        if (type0 != S_EXPR_PARAM_INT && type0 != S_EXPR_PARAM_UINT) {
            EXCEPTION("se_trigger_on_change: child 0 must be INT or UINT");
            return SE_PIPELINE_CONTINUE;
        }
        
        int32_t initial_state = (int32_t)params[init_phys_idx].int_val;
        s_expr_set_state(inst, initial_state ? 1 : 0);
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // TICK EVENT
    // =========================================================================
    
    // Evaluate predicate and detect edges
    bool current = s_expr_child_invoke_pred(inst, params, param_count, PRED_CHILD);
    uint8_t prev = s_expr_get_state(inst);
    
    bool rising = (prev == 0 && current);
    bool falling = (prev != 0 && !current);
    
    s_expr_set_state(inst, current ? 1 : 0);
    
    if (rising) {
        // Terminate falling action (was running, now stopping)
        if (has_falling) {
            s_expr_child_terminate(inst, params, param_count, FALLING_CHILD);
            s_expr_child_reset(inst, params, param_count, FALLING_CHILD);
        }
        
        // Restart rising action: terminate, reset, invoke
        s_expr_child_terminate(inst, params, param_count, RISING_CHILD);
        s_expr_child_reset(inst, params, param_count, RISING_CHILD);
        
        uint16_t phys_idx = s_expr_child_index(params, param_count, RISING_CHILD);
        s_expr_result_t r = s_expr_invoke_any(inst, params, phys_idx);
        
        // Non-PIPELINE codes (0-11) - propagate to caller
        if (r < SE_PIPELINE_CONTINUE) {
            return r;
        }
        
        // PIPELINE codes (12-17) - handle internally
        switch (r) {
            case SE_PIPELINE_CONTINUE:
            case SE_PIPELINE_HALT:
                return SE_PIPELINE_CONTINUE;
                
            case SE_PIPELINE_DISABLE:
            case SE_PIPELINE_TERMINATE:
            case SE_PIPELINE_RESET:
                s_expr_child_terminate(inst, params, param_count, RISING_CHILD);
                s_expr_child_reset(inst, params, param_count, RISING_CHILD);
                return SE_PIPELINE_CONTINUE;
                
            case SE_PIPELINE_SKIP_CONTINUE:
                return SE_PIPELINE_CONTINUE;
                
            default:
                return SE_PIPELINE_CONTINUE;
        }
    } 
    else if (falling && has_falling) {
        // Terminate rising action (was running, now stopping)
        s_expr_child_terminate(inst, params, param_count, RISING_CHILD);
        s_expr_child_reset(inst, params, param_count, RISING_CHILD);
        
        // Restart falling action: terminate, reset, invoke
        s_expr_child_terminate(inst, params, param_count, FALLING_CHILD);
        s_expr_child_reset(inst, params, param_count, FALLING_CHILD);
        
        uint16_t phys_idx = s_expr_child_index(params, param_count, FALLING_CHILD);
        s_expr_result_t r = s_expr_invoke_any(inst, params, phys_idx);
        
        // Non-PIPELINE codes (0-11) - propagate to caller
        if (r < SE_PIPELINE_CONTINUE) {
            return r;
        }
        
        // PIPELINE codes (12-17) - handle internally
        switch (r) {
            case SE_PIPELINE_CONTINUE:
            case SE_PIPELINE_HALT:
                return SE_PIPELINE_CONTINUE;
                
            case SE_PIPELINE_DISABLE:
            case SE_PIPELINE_TERMINATE:
            case SE_PIPELINE_RESET:
                s_expr_child_terminate(inst, params, param_count, FALLING_CHILD);
                s_expr_child_reset(inst, params, param_count, FALLING_CHILD);
                return SE_PIPELINE_CONTINUE;
                
            case SE_PIPELINE_SKIP_CONTINUE:
                return SE_PIPELINE_CONTINUE;
                
            default:
                return SE_PIPELINE_CONTINUE;
        }
    }
    
    return SE_PIPELINE_CONTINUE;
}

// ============================================================================
// SE_SEQUENCE - Sequential Execution
// 
// Executes children one at a time in order. Advances to next child when
// current child completes. Sequence completes when all children finish.
//
// State: Current child index (0 to child_count-1)
//
// Child results:
//   PIPELINE_CONTINUE/HALT  -> Child running, pause sequence
//   PIPELINE_DISABLE/TERMINATE/RESET -> Child complete, advance to next
//   PIPELINE_SKIP_CONTINUE  -> Pause sequence this tick
//   Non-PIPELINE (0-11)     -> Propagate immediately to caller
// ============================================================================

static s_expr_result_t se_sequence(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_id);
    UNUSED(event_data);
    
    uint8_t state = s_expr_get_state(inst);
    uint16_t child_count = s_expr_child_count(params, param_count);
    
    // =========================================================================
    // TERMINATE EVENT
    // =========================================================================
    if (event_type == SE_EVENT_TERMINATE) {
        if (state < child_count) {
            if (s_expr_child_is_initialized(inst, params, param_count, state)) {
                s_expr_child_terminate(inst, params, param_count, state);
            }
        }
        s_expr_set_state(inst, 0);
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // INIT EVENT
    // =========================================================================
    if (event_type == SE_EVENT_INIT) {
        s_expr_set_state(inst, 0);
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // TICK EVENT
    // =========================================================================
    while (state < child_count) {
        // Skip non-callable parameters
        if (!s_expr_child_is_callable(params, param_count, state)) {
            state++;
            s_expr_set_state(inst, state);
            continue;
        }
        
        uint16_t phys_idx = s_expr_child_index(params, param_count, state);
        if (phys_idx == UINT16_MAX) {
            state++;
            s_expr_set_state(inst, state);
            continue;
        }
        
        uint8_t func_type = s_expr_child_func_type(params, param_count, state);
        
        // -----------------------------------------------------------------
        // ONESHOT - invoke and advance immediately
        // -----------------------------------------------------------------
        if (func_type == S_EXPR_PARAM_ONESHOT) {
            s_expr_invoke_any(inst, params, phys_idx);
            state++;
            s_expr_set_state(inst, state);
            continue;
        }
        
        // -----------------------------------------------------------------
        // PRED - invoke and advance immediately
        // -----------------------------------------------------------------
        if (func_type == S_EXPR_PARAM_PRED) {
            s_expr_invoke_any(inst, params, phys_idx);
            state++;
            s_expr_set_state(inst, state);
            continue;
        }
        
        // -----------------------------------------------------------------
        // MAIN - invoke and check result
        // -----------------------------------------------------------------
        s_expr_result_t r = s_expr_invoke_any(inst, params, phys_idx);
        
        // APPLICATION codes (0-5) - propagate immediately
        if (r <= SE_SKIP_CONTINUE) {
            return r;
        }
        
        // FUNCTION codes (6-11) - propagate, except HALT which converts
        if (r >= SE_FUNCTION_CONTINUE && r <= SE_FUNCTION_SKIP_CONTINUE) {
            if (r == SE_FUNCTION_HALT) {
                return SE_PIPELINE_HALT;
            }
            return r;
        }
        
        // PIPELINE codes (12-17) - handle internally
        switch (r) {
            case SE_PIPELINE_CONTINUE:
            case SE_PIPELINE_HALT:
                // Child still running - pause, resume next tick
                return SE_PIPELINE_CONTINUE;
                
            case SE_PIPELINE_DISABLE:
            case SE_PIPELINE_TERMINATE:
            case SE_PIPELINE_RESET:
                // Child complete - terminate and advance
                s_expr_child_terminate(inst, params, param_count, state);
                state++;
                s_expr_set_state(inst, state);
                continue;
                
            case SE_PIPELINE_SKIP_CONTINUE:
                return SE_PIPELINE_CONTINUE;
                
            default:
                EXCEPTION("se_sequence: unknown result code");
                return SE_PIPELINE_CONTINUE;
        }
    }
    
    // All children complete
    return SE_PIPELINE_DISABLE;
}



// ============================================================================
// SE_SEQUENCE_ONCE - Single-Tick Sequential Execution
// 
// Executes all children in order within a single tick. Each child gets
// exactly one tick, regardless of its return value. After all children
// execute once, returns PIPELINE_DISABLE.
//
// State: Current child index (0 to child_count-1)
//
// Child results:
//   APPLICATION codes (0-5) -> Propagate immediately to caller
//   FUNCTION codes (6-11)   -> Propagate immediately to caller
//   PIPELINE codes (12-17)  -> Ignored, continue to next child
//
// On TERMINATE: All initialized children are terminated.
// ============================================================================

static s_expr_result_t se_sequence_once(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_id);
    UNUSED(event_data);
    
    uint16_t child_count = s_expr_child_count(params, param_count);
    
    // =========================================================================
    // TERMINATE EVENT - terminate all initialized children
    // =========================================================================
    if (event_type == SE_EVENT_TERMINATE) {
        for (uint16_t i = 0; i < child_count; i++) {
            if (s_expr_child_is_initialized(inst, params, param_count, i)) {
                s_expr_child_terminate(inst, params, param_count, i);
            }
        }
        s_expr_set_state(inst, 0);
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // INIT EVENT
    // =========================================================================
    if (event_type == SE_EVENT_INIT) {
        s_expr_set_state(inst, 0);
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // TICK EVENT - execute all children once
    // =========================================================================
    for (uint16_t state = 0; state < child_count; state++) {
        // Skip non-callable parameters
        if (!s_expr_child_is_callable(params, param_count, state)) {
            continue;
        }
        
        uint16_t phys_idx = s_expr_child_index(params, param_count, state);
        if (phys_idx == UINT16_MAX) {
            continue;
        }
        
        uint8_t func_type = s_expr_child_func_type(params, param_count, state);
        
        // -----------------------------------------------------------------
        // ONESHOT - invoke and continue
        // -----------------------------------------------------------------
        if (func_type == S_EXPR_PARAM_ONESHOT) {
            s_expr_invoke_any(inst, params, phys_idx);
            continue;
        }
        
        // -----------------------------------------------------------------
        // PRED - invoke and continue
        // -----------------------------------------------------------------
        if (func_type == S_EXPR_PARAM_PRED) {
            s_expr_invoke_any(inst, params, phys_idx);
            continue;
        }
        s_expr_result_t r = s_expr_invoke_any(inst, params, phys_idx);
        //printf("se_sequence_once: child %d result=%d\n", state, r);
        if((r != SE_PIPELINE_CONTINUE) && (r != SE_PIPELINE_DISABLE  )) {
           break;
        }
    }
    
    // All children executed once - terminate all and disable
    for (uint16_t i = 0; i < child_count; i++) {
        if (s_expr_child_is_initialized(inst, params, param_count, i)) {
            s_expr_child_terminate(inst, params, param_count, i);
        }
    }
    
    return SE_PIPELINE_DISABLE;
}

#define FORK_STATE_INIT      0
#define FORK_STATE_RUNNING   1
#define FORK_STATE_COMPLETE  2



static s_expr_result_t se_function_interface(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_id);
    UNUSED(event_data);
    
    uint16_t child_count = s_expr_child_count(params, param_count);
    
    // =========================================================================
    // INIT EVENT
    // =========================================================================
    if (event_type == SE_EVENT_INIT) {
        s_expr_set_state(inst, FORK_STATE_RUNNING);
        s_expr_set_user_flags(inst, 0);
        
        for (uint16_t i = 0; i < child_count; i++) {
            if (s_expr_child_is_callable(params, param_count, i)) {
                s_expr_child_reset(inst, params, param_count, i);
            }
        }
        return SE_FUNCTION_CONTINUE;
    }
    
    // =========================================================================
    // TERMINATE EVENT
    // =========================================================================
    if (event_type == SE_EVENT_TERMINATE) {
        s_expr_children_terminate_all(inst, params, param_count);
        s_expr_set_state(inst, FORK_STATE_COMPLETE);
        return SE_FUNCTION_CONTINUE;
    }
    
    // =========================================================================
    // TICK EVENT
    // =========================================================================
    uint8_t state = s_expr_get_state(inst);
    
    if (state != FORK_STATE_RUNNING) {
        return SE_FUNCTION_DISABLE;
    }
    
    uint16_t active_count = 0;
    //printf("se_function_interface: child_count=%d\n", child_count);
    for (uint16_t i = 0; i < child_count; i++) {
        //printf("se_function_interface: child %d is_callable=%d is_active=%d\n", i, s_expr_child_is_callable(params, param_count, i), s_expr_child_is_active(inst, params, param_count, i));
        if (!s_expr_child_is_callable(params, param_count, i)) {
            continue;
        }
        
        if (!s_expr_child_is_active(inst, params, param_count, i)) {
            continue;
        }
        
        // Use raw invoke to get actual return code
        uint16_t phys_idx = s_expr_child_index(params, param_count, i);
        s_expr_result_t r = s_expr_invoke_any(inst, params, phys_idx);
        //printf("se_function_interface: child %d result=%d\n", i, r);
        // -----------------------------------------------------------------
        // Non-PIPELINE codes (0-11) - immediate exit, propagate to caller
        // -----------------------------------------------------------------
        
        if (r < SE_PIPELINE_CONTINUE) {
            //printf("se_function_interface: child %d result=%d propagating to caller\n", i, r);
            return r;
        }
        
        // -----------------------------------------------------------------
        // PIPELINE codes (12-17) - handle internally
        // -----------------------------------------------------------------
        //printf("se_function_interface: child %d result=%d\n", i, r);
        switch (r) {
            case SE_PIPELINE_CONTINUE:
            case SE_PIPELINE_HALT:
                active_count++;
                break;
                
            case SE_PIPELINE_DISABLE:
            case SE_PIPELINE_TERMINATE:
                s_expr_child_terminate(inst, params, param_count, i);
                break;
                
            case SE_PIPELINE_RESET:
                s_expr_child_terminate(inst, params, param_count, i);
                s_expr_child_reset(inst, params, param_count, i);
                active_count++;
                break;
                
            case SE_PIPELINE_SKIP_CONTINUE:
                active_count++;
                goto tick_complete;
                
            default:
                active_count++;
                break;
        }
    }
    
tick_complete:
    if (active_count == 0) {
        s_expr_set_state(inst, FORK_STATE_COMPLETE);
        return SE_FUNCTION_DISABLE;
    }
    //printf("se_function_interface: active_count=%d\n", active_count);
    return SE_FUNCTION_CONTINUE;
}

// ============================================================================
// se_fork.c
// Fork Composite - Executes all children in parallel each tick
// Only handles PIPELINE codes internally; others pass to outer node
// ============================================================================



s_expr_result_t se_fork(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t*   params,
    uint16_t                param_count,
    s_expr_event_type_t     event_type,
    uint16_t                event_id,
    void*                   event_data
) {
    UNUSED(event_id);
    UNUSED(event_data);
    
    uint16_t child_count = s_expr_child_count(params, param_count);
    
    // =========================================================================
    // TERMINATE EVENT
    // =========================================================================
    if (event_type == SE_EVENT_TERMINATE) {
        s_expr_children_terminate_all(inst, params, param_count);
        s_expr_set_state(inst, FORK_STATE_COMPLETE);
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // INIT EVENT
    // =========================================================================
    if (event_type == SE_EVENT_INIT) {
        s_expr_set_state(inst, FORK_STATE_RUNNING);
        s_expr_set_user_flags(inst, 0);
        
        for (uint16_t i = 0; i < child_count; i++) {
            if (s_expr_child_is_callable(params, param_count, i)) {
                s_expr_child_reset(inst, params, param_count, i);
            }
        }
        
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // TICK EVENT
    // =========================================================================
    uint8_t state = s_expr_get_state(inst);
    
    if (state != FORK_STATE_RUNNING) {
        return SE_PIPELINE_DISABLE;
    }
    
    for (uint16_t i = 0; i < child_count; i++) {
        if (!s_expr_child_is_callable(params, param_count, i)) {
            continue;
        }
        
        uint16_t phys_idx = s_expr_child_index(params, param_count, i);
        if (phys_idx == UINT16_MAX) {
            continue;
        }
        
        uint8_t func_type = s_expr_child_func_type(params, param_count, i);
        
        // -----------------------------------------------------------------
        // ONESHOT - fire once, skip if already initialized
        // -----------------------------------------------------------------
        if (func_type == S_EXPR_PARAM_ONESHOT) {
            if (!s_expr_child_is_initialized(inst, params, param_count, i)) {
                s_expr_invoke_any(inst, params, phys_idx);
            }
            continue;
        }
        
        // -----------------------------------------------------------------
        // PRED - evaluate once, skip if already initialized
        // -----------------------------------------------------------------
        if (func_type == S_EXPR_PARAM_PRED) {
            if (!s_expr_child_is_initialized(inst, params, param_count, i)) {
                s_expr_invoke_any(inst, params, phys_idx);
            }
            continue;
        }
        
        // -----------------------------------------------------------------
        // MAIN - only invoke if still active
        // -----------------------------------------------------------------
        if (!s_expr_child_is_active(inst, params, param_count, i)) {
            continue;
        }
        
        s_expr_result_t r = s_expr_invoke_any(inst, params, phys_idx);
        
        // -----------------------------------------------------------------
        // Regular (0-5) and FUNCTION (6-11) codes - pass to outer node
        // -----------------------------------------------------------------
        if (r == SE_FUNCTION_HALT) {
            r = SE_PIPELINE_HALT;
        }
        
        if (r < SE_PIPELINE_CONTINUE) {
            return r;
        }
        
        // -----------------------------------------------------------------
        // PIPELINE codes (12-17) - handle internally
        // -----------------------------------------------------------------
        switch (r) {
            case SE_PIPELINE_CONTINUE:
            case SE_PIPELINE_HALT:
                break;
                
            case SE_PIPELINE_DISABLE:
            case SE_PIPELINE_TERMINATE:
                s_expr_child_terminate(inst, params, param_count, i);
                break;
                
            case SE_PIPELINE_RESET:
                s_expr_child_terminate(inst, params, param_count, i);
                s_expr_child_reset_recursive(inst, params, param_count, i);
                break;
                
            case SE_PIPELINE_SKIP_CONTINUE:
                goto check_completion;
                
            default:
                break;
        }
    }
    
check_completion:
    ;  // empty stmt so C11 (arm-gcc 8.3.1) allows the decl below the label
    // Count active MAIN children only
    uint16_t active_main_count = 0;
    for (uint16_t i = 0; i < child_count; i++) {
        if (!s_expr_child_is_callable(params, param_count, i)) {
            continue;
        }
        
        uint8_t func_type = s_expr_child_func_type(params, param_count, i);
        
        if (func_type != S_EXPR_PARAM_MAIN) {
            continue;
        }
        
        if (s_expr_child_is_active(inst, params, param_count, i)) {
            active_main_count++;
        }
    }
    
    if (active_main_count == 0) {
        s_expr_set_state(inst, FORK_STATE_COMPLETE);
        return SE_PIPELINE_DISABLE;
    }
    
    return SE_PIPELINE_CONTINUE;
}
// ============================================================================
// SE_FORK_JOIN
// Execute all children in parallel, return SE_FUNCTION_HALT until all complete
// Returns SE_FUNCTION_HALT while working, SE_PIPELINE_DISABLE when all complete
// Fatal codes propagate immediately
// ============================================================================

static s_expr_result_t se_fork_join(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_id);
    UNUSED(event_data);
    
    // =========================================================================
    // TERMINATE EVENT
    // =========================================================================
    if (event_type == SE_EVENT_TERMINATE) {
        s_expr_children_terminate_all(inst, params, param_count);
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // INIT EVENT
    // =========================================================================
    if (event_type == SE_EVENT_INIT) {
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // TICK EVENT
    // =========================================================================
    uint16_t count = s_expr_child_count(params, param_count);
    
    for (uint16_t i = 0; i < count; i++) {
        if (!s_expr_child_is_callable(params, param_count, i)) {
            continue;
        }
        
        uint16_t phys_idx = s_expr_child_index(params, param_count, i);
        if (phys_idx == UINT16_MAX) {
            continue;
        }
        
        uint8_t func_type = s_expr_child_func_type(params, param_count, i);
        
        // -----------------------------------------------------------------
        // ONESHOT - fire once, skip if already initialized
        // -----------------------------------------------------------------
        if (func_type == S_EXPR_PARAM_ONESHOT) {
            if (!s_expr_child_is_initialized(inst, params, param_count, i)) {
                s_expr_invoke_any(inst, params, phys_idx);
            }
            continue;
        }
        
        // -----------------------------------------------------------------
        // PRED - evaluate once, skip if already initialized
        // -----------------------------------------------------------------
        if (func_type == S_EXPR_PARAM_PRED) {
            if (!s_expr_child_is_initialized(inst, params, param_count, i)) {
                s_expr_invoke_any(inst, params, phys_idx);
            }
            continue;
        }
        
        // -----------------------------------------------------------------
        // MAIN - only invoke if still active
        // -----------------------------------------------------------------
        if (!s_expr_child_is_active(inst, params, param_count, i)) {
            continue;
        }
        
        s_expr_result_t r = s_expr_invoke_any(inst, params, phys_idx);
        //printf("SE_FORK_JOIN: node %d child %d result=%d\n", inst->current_node_index, i, r);
        if (r == SE_FUNCTION_HALT) {
            r = SE_PIPELINE_HALT;
        }
        
        // Non-PIPELINE codes (0-11) - immediate exit, propagate to caller
        if (r < SE_PIPELINE_CONTINUE) {
            return r;
        }
        
        // PIPELINE codes (12-17) - handle internally
       
        switch (r) {
            
            case SE_PIPELINE_CONTINUE:
            case SE_PIPELINE_HALT:
                // Child still running
                break;
                
            case SE_PIPELINE_DISABLE:
            case SE_PIPELINE_TERMINATE:
                //printf("SE_FORK_JOIN: child %d result=%d terminating\n", i, r);
                terminate_action_at_index(inst, params, phys_idx);
                break;
                
            case SE_PIPELINE_RESET:
                // Child wants to restart
                terminate_action_at_index(inst, params, phys_idx);
                s_expr_reset_recursive_at(inst, params, phys_idx);
                break;
                
            case SE_PIPELINE_SKIP_CONTINUE:
                goto check_completion;
                
            default:
                EXCEPTION("se_fork_join: unknown result code");
                break;
        }
    }
    
check_completion:
    ;  // empty stmt so C11 (arm-gcc 8.3.1) allows the decl below the label
    // Count active MAIN children
    uint16_t active_main_count = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (!s_expr_child_is_callable(params, param_count, i)) {
            continue;
        }
        
        uint8_t func_type = s_expr_child_func_type(params, param_count, i);
        
        if (func_type != S_EXPR_PARAM_MAIN) {
            continue;
        }
        
        if (s_expr_child_is_active(inst, params, param_count, i)) {
            active_main_count++;
        }
    }
    
    if (active_main_count == 0) {
        return SE_PIPELINE_DISABLE;
    }
    
    return SE_FUNCTION_HALT;
}
// ============================================================================
// SE_CHAIN_FLOW
// Pipeline variant that processes events through children like ChainTree walker
// SE_FUNCTION_RESET: reset child, continue to next
// SE_FUNCTION_TERMINATE: terminate child, continue to next
// SE_CONTINUE: continue to next child
// All other results: return immediately
// ============================================================================

static s_expr_result_t se_chain_flow(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_id);
    UNUSED(event_data);
    
    // =========================================================================
    // TERMINATE EVENT
    // =========================================================================
    if (event_type == SE_EVENT_TERMINATE) {
        s_expr_children_terminate_all(inst, params, param_count);
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // INIT EVENT
    // =========================================================================
    if (event_type == SE_EVENT_INIT) {
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // TICK EVENT
    // =========================================================================
    uint16_t count = s_expr_child_count(params, param_count);
    uint16_t active_count = 0;

    for (uint16_t i = 0; i < count; i++) {
        if (!s_expr_child_is_callable(params, param_count, i)) {
            continue;
        }
        
        if (!s_expr_child_is_active(inst, params, param_count, i)) {
            continue;
        }
        
        uint16_t phys_idx = s_expr_child_index(params, param_count, i);
        if (phys_idx == UINT16_MAX) {
            continue;
        }
        
        uint8_t func_type = s_expr_child_func_type(params, param_count, i);
        
        // -----------------------------------------------------------------
        // ONESHOT - fire and mark inactive, don't count as active
        // -----------------------------------------------------------------
        if (func_type == S_EXPR_PARAM_ONESHOT) {
            s_expr_invoke_any(inst, params, phys_idx);
            s_expr_child_terminate(inst, params, param_count, i);
            continue;
        }
        
        // -----------------------------------------------------------------
        // PRED - evaluate and mark inactive, don't count as active
        // -----------------------------------------------------------------
        if (func_type == S_EXPR_PARAM_PRED) {
            s_expr_invoke_any(inst, params, phys_idx);
            s_expr_child_terminate(inst, params, param_count, i);
            continue;
        }
        
        // -----------------------------------------------------------------
        // MAIN - invoke and handle result
        // -----------------------------------------------------------------
        s_expr_result_t r = s_expr_invoke_any(inst, params, phys_idx);
        
        // Non-PIPELINE codes (0-11) - immediate exit, propagate to caller
        if (r == SE_FUNCTION_HALT) {
            return SE_PIPELINE_HALT;
        }
        if (r < SE_PIPELINE_CONTINUE) {
            return r;
        }
        
        // PIPELINE codes (12-17) - handle internally
        switch (r) {
            case SE_PIPELINE_CONTINUE:
                active_count++;
                continue;
                
            case SE_PIPELINE_HALT:
                return SE_PIPELINE_CONTINUE;
                
            case SE_PIPELINE_DISABLE:
                s_expr_child_terminate(inst, params, param_count, i);
                continue;

            case SE_PIPELINE_TERMINATE:
    
                s_expr_children_terminate_all(inst, params, param_count);
                return SE_PIPELINE_TERMINATE;
                
            case SE_PIPELINE_RESET:
                s_expr_children_terminate_all(inst, params, param_count);
                s_expr_children_reset_all(inst, params, param_count);
                return SE_PIPELINE_CONTINUE;
                
            case SE_PIPELINE_SKIP_CONTINUE:
                active_count++;
                goto tick_complete;
                
            default:
                active_count++;
                continue;
        }
    }
    
tick_complete:
    if (active_count == 0) {
        return SE_PIPELINE_DISABLE;
    }
    
    return SE_PIPELINE_CONTINUE;
}
// ============================================================================
// SE_WHILE
// Loop: evaluate predicate, if true run body (fork_join) to completion, repeat.
// Param layout: [PRED] [MAIN(se_fork_join)]
//   child 0 = predicate
//   child 1 = main body (typically se_fork_join)
//
// States:
//   0 = EVAL_PRED  - evaluate predicate, transition to RUN_BODY or DISABLE
//   1 = RUN_BODY   - tick body until it completes, then back to EVAL_PRED
//
// Returns SE_PIPELINE_HALT while looping
// Returns SE_PIPELINE_DISABLE when predicate returns false
// Fatal codes (0-11) propagate immediately from body
// ============================================================================

#define SE_WHILE_STATE_EVAL_PRED  0
#define SE_WHILE_STATE_RUN_BODY   1

static s_expr_result_t se_while(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_id);
    UNUSED(event_data);

    // =========================================================================
    // TERMINATE EVENT
    // =========================================================================
    if (event_type == SE_EVENT_TERMINATE) {
        // Terminate body child if it was running
        if (s_expr_child_is_initialized(inst, params, param_count, 1)) {
            s_expr_child_terminate(inst, params, param_count, 1);
        }
        return SE_PIPELINE_CONTINUE;
    }

    // =========================================================================
    // INIT EVENT
    // =========================================================================
    if (event_type == SE_EVENT_INIT) {
        s_expr_set_state(inst, SE_WHILE_STATE_EVAL_PRED);
       
        return SE_PIPELINE_CONTINUE;
    }

    // =========================================================================
    // TICK EVENT
    // =========================================================================
    uint8_t state = s_expr_get_state(inst);

    switch (state) {

        // -----------------------------------------------------------------
        // EVAL_PRED: invoke predicate, decide whether to enter body or exit
        // -----------------------------------------------------------------
        case SE_WHILE_STATE_EVAL_PRED: {
            bool cond = s_expr_child_invoke_pred(inst, params, param_count, 0);
           // printf("SE_WHILE: EVAL_PRED: cond = %d\n", cond);
            
            if (!cond) {
                return SE_PIPELINE_DISABLE;
            }

            // Predicate true - ensure body is reset and ready
            s_expr_child_reset_recursive(inst, params, param_count, 1);
            s_expr_set_state(inst, SE_WHILE_STATE_RUN_BODY);

            // Fall through to run body on same tick
        }
        /* FALLTHROUGH */

        // -----------------------------------------------------------------
        // RUN_BODY: tick the body (fork_join) until it completes
        // -----------------------------------------------------------------
        case SE_WHILE_STATE_RUN_BODY: {
            s_expr_result_t r = s_expr_child_invoke(inst, params, param_count, 1);
            //printf("SE_WHILE: RUN_BODY: r = %d\n", r);
        
           
            // Non-PIPELINE codes (0-11) - fatal, propagate immediately
            if (r < SE_PIPELINE_CONTINUE) {
                return r;
            }

            switch (r) {
                case SE_PIPELINE_CONTINUE:
                case SE_PIPELINE_HALT:
                case SE_PIPELINE_SKIP_CONTINUE:
                    //("SE_WHILE: RUN_BODY: body still running\n");
                    // Body still running
                    return SE_FUNCTION_HALT;

                case SE_PIPELINE_DISABLE:
                case SE_PIPELINE_TERMINATE:
                case SE_PIPELINE_RESET:
                    // Body completed - terminate it, loop back to predicate
                    s_expr_child_terminate(inst, params, param_count, 1);
                    s_expr_child_reset_recursive(inst, params, param_count, 1);
                    s_expr_set_state(inst, SE_WHILE_STATE_EVAL_PRED);
                    
                    return SE_PIPELINE_HALT;

                

                default:
                    EXCEPTION("se_while: unknown result code");
                    return SE_PIPELINE_DISABLE;
            }
        }

        default:
            EXCEPTION("se_while: invalid state");
            return SE_PIPELINE_DISABLE;
    }
}
