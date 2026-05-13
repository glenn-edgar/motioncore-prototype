

// Helper to invoke and handle all PIPELINE result codes consistently
static s_expr_result_t invoke_and_handle_result(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t action_idx
) {
    s_expr_result_t r = s_expr_invoke_any(inst, params, action_idx);
    
    // -----------------------------------------------------------------
    // Non-PIPELINE codes (0-11) - propagate to caller
    // -----------------------------------------------------------------
    if (r < SE_PIPELINE_CONTINUE) {
        return r;
    }
    
    // -----------------------------------------------------------------
    // PIPELINE codes (12-17) - handle internally
    // -----------------------------------------------------------------
    switch (r) {
        case SE_PIPELINE_CONTINUE:
        case SE_PIPELINE_HALT:
            // Action still running
            return r;
            
        case SE_PIPELINE_DISABLE:
        case SE_PIPELINE_TERMINATE:
        case SE_PIPELINE_RESET:
            // Action completed - terminate and reset for next activation
            terminate_action_at_index(inst, params, action_idx);
            s_expr_reset_recursive_at(inst, params, action_idx);
            return SE_PIPELINE_CONTINUE;
            
        case SE_PIPELINE_SKIP_CONTINUE:
            return SE_PIPELINE_CONTINUE;
            
        default:
            EXCEPTION("se_event_dispatch: unknown result code");
            return SE_PIPELINE_CONTINUE;
    }
}

static s_expr_result_t se_event_dispatch(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_data);
    
    // =========================================================================
    // INIT/TERMINATE EVENTS
    // =========================================================================
    if (event_type == SE_EVENT_INIT || event_type == SE_EVENT_TERMINATE) {
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // TICK EVENT - Dispatch based on event_id
    // =========================================================================
    uint16_t idx = 0;
    uint16_t default_action_idx = 0;
    
    while (idx < param_count) {
        uint8_t opcode = params[idx].type & S_EXPR_OPCODE_MASK;
        
        if (opcode == S_EXPR_PARAM_INT || opcode == S_EXPR_PARAM_UINT) {
            int32_t case_event = (int32_t)params[idx].int_val;
            uint16_t action_idx = idx + 1;
            
            if (action_idx < param_count) {
                // Exact match - invoke immediately
                if (case_event == (int32_t)event_id) {
                    return invoke_and_handle_result(inst, params, action_idx);
                }
                
                // Track default case (-1)
                if (case_event == -1) {
                    default_action_idx = action_idx;
                }
            }
            
            idx = s_expr_skip_param(params, idx);      // Skip int
            idx = s_expr_skip_param(params, idx);      // Skip action
        } else {
            idx = s_expr_skip_param(params, idx);
        }
    }
    
    // No exact match - try default
    if (default_action_idx > 0) {
        return invoke_and_handle_result(inst, params, default_action_idx);
    }
    
    // No match and no default - crash (Erlang-style)
    EXCEPTION("se_event_dispatch: no matching event handler");
    return SE_PIPELINE_CONTINUE;
}


// SE_FIELD_DISPATCH - dispatch based on integer field value
// params: [field_ref] [int, action] pairs (flat structure)
// Stateful: tracks branch changes, handles INIT/TERMINATE
// Crashes if no matching case (Erlang-style)
// SE_FIELD_DISPATCH - dispatch based on integer field value
// params: [field_ref] [int, action] pairs (flat structure)
// Stateful: tracks branch changes, handles INIT/TERMINATE
// Crashes if no matching case (Erlang-style)
static s_expr_result_t se_state_machine(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_id);
    UNUSED(event_data);
    
    uint16_t prev_action_idx = s_expr_get_user_flags(inst);
    
    // =========================================================================
    // TERMINATE EVENT
    // =========================================================================
    if (event_type == SE_EVENT_TERMINATE) {
        if (prev_action_idx > 0 && prev_action_idx != 0xFFFF) {
            terminate_action_at_index(inst, params, prev_action_idx);
        }
        s_expr_set_user_flags(inst, 0xFFFF);
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // INIT EVENT
    // =========================================================================
    if (event_type == SE_EVENT_INIT) {
        if (param_count < 3) {
            EXCEPTION("se_state_machine: need field_ref and at least one case");
            return SE_PIPELINE_CONTINUE;
        }
        s_expr_set_user_flags(inst, 0xFFFF);
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // TICK EVENT
    // =========================================================================
   
    // Get integer value from field
    int32_t* val_ptr = S_EXPR_GET_FIELD(inst, &params[0], int32_t);
    if (!val_ptr) {
        EXCEPTION("se_state_machine: field not found");
        return SE_PIPELINE_CONTINUE;
    }
    
    int32_t val = *val_ptr;
   
    // Search for matching case in flat [int, action] pairs
    uint16_t idx = s_expr_skip_param(params, 0);  // Skip field_ref
    uint16_t action_idx = 0;
    uint16_t default_idx = 0;
    
    while (idx < param_count) {
        uint8_t opcode = params[idx].type & S_EXPR_OPCODE_MASK;
        
        if (opcode == S_EXPR_PARAM_INT || opcode == S_EXPR_PARAM_UINT) {
            int32_t case_val = (int32_t)params[idx].int_val;
            uint16_t this_action_idx = idx + 1;
            
            if (this_action_idx < param_count) {
                if (case_val == val) {
                    action_idx = this_action_idx;
                    break;
                }
                
                if (case_val == -1) {
                    default_idx = this_action_idx;
                }
            }
            
            // Skip [int, action] pair
            idx = s_expr_skip_param(params, idx);      // Skip int
            idx = s_expr_skip_param(params, idx);      // Skip action
        } else {
            idx = s_expr_skip_param(params, idx);
        }
    }
    
    // =========================================================================
    // Use default if no exact match
    // =========================================================================
    if (action_idx == 0) {
        action_idx = default_idx;
    }
    
    // =========================================================================
    // No match and no default - crash (Erlang-style)
    // =========================================================================
    if (action_idx == 0) {
        EXCEPTION("se_state_machine: no matching case");
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // Handle branch change: terminate old, reset new
    // =========================================================================
    if (action_idx != prev_action_idx) {
        if (prev_action_idx > 0 && prev_action_idx != 0xFFFF) {
            terminate_action_at_index(inst, params, prev_action_idx);
            reset_action_at_index(inst, params, prev_action_idx);
        }
        
        reset_action_at_index(inst, params, action_idx);
        s_expr_set_user_flags(inst, action_idx);
    }
    
    // =========================================================================
    // Invoke current action
    // =========================================================================
    s_expr_result_t r = s_expr_invoke_any(inst, params, action_idx);
    
    // -----------------------------------------------------------------
    // Non-PIPELINE codes (0-11) - propagate to caller
    // -----------------------------------------------------------------
    if (r == SE_FUNCTION_HALT){
        return SE_PIPELINE_HALT;
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
            // Action still running
            return r;
            
        case SE_PIPELINE_DISABLE:
        case SE_PIPELINE_TERMINATE:
        case SE_PIPELINE_RESET:
            // Action completed - terminate and reset for next activation
            terminate_action_at_index(inst, params, action_idx);
            reset_action_at_index(inst, params, action_idx);
            return SE_PIPELINE_CONTINUE;
            
        case SE_PIPELINE_SKIP_CONTINUE:
            return SE_PIPELINE_CONTINUE;
            
        default:
            EXCEPTION("se_state_machine: unknown result code");
            return SE_PIPELINE_CONTINUE;
    }
}

// SE_FIELD_DISPATCH - dispatch based on integer field value
// params: [field_ref] [int, action] pairs (flat structure)
// Stateful: tracks branch changes, handles INIT/TERMINATE
// Crashes if no matching case (Erlang-style)
// Supports "default" case with value -1
static s_expr_result_t se_field_dispatch(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_id; (void)event_data;
    
    uint16_t prev_action_idx = s_expr_get_user_flags(inst);
    
    // =========================================================================
    // TERMINATE: Clean up active branch
    // =========================================================================
    if (event_type == SE_EVENT_TERMINATE) {
        if (prev_action_idx > 0 && prev_action_idx != 0xFFFF) {
            terminate_action_at_index(inst, params, prev_action_idx);
        }
        s_expr_set_user_flags(inst, 0xFFFF);
        return SE_CONTINUE;
    }
    
    // =========================================================================
    // INIT: Validate and set sentinel
    // =========================================================================
    if (event_type == SE_EVENT_INIT) {
        if (param_count < 3) {
            EXCEPTION("se_field_dispatch: need field_ref and at least one case");
            return SE_PIPELINE_CONTINUE;
        }
        s_expr_set_user_flags(inst, 0xFFFF);
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // TICK: Dispatch based on field value
    // =========================================================================
    
    // Get integer value from field
    int32_t* val_ptr = S_EXPR_GET_FIELD(inst, &params[0], int32_t);
    if (!val_ptr) {
        EXCEPTION("se_field_dispatch: field not found");
        return SE_PIPELINE_CONTINUE;
    }
    
    int32_t val = *val_ptr;
    
    // Search for matching case in flat [int, action] pairs
    // Also track default case (-1) as fallback
    uint16_t idx = s_expr_skip_param(params, 0);  // Skip field_ref
    uint16_t action_idx = 0;
    uint16_t default_idx = 0;
    
    while (idx < param_count) {
        uint8_t opcode = params[idx].type & S_EXPR_OPCODE_MASK;
        
        if (opcode == S_EXPR_PARAM_INT || opcode == S_EXPR_PARAM_UINT) {
            int32_t case_val = (int32_t)params[idx].int_val;
            uint16_t this_action_idx = idx + 1;
            
            if (this_action_idx < param_count) {
                if (case_val == val) {
                    action_idx = this_action_idx;
                    break;
                }
                
                if (case_val == -1) {
                    default_idx = this_action_idx;
                }
            }
            
            // Skip [int, action] pair
            idx = s_expr_skip_param(params, idx);      // Skip int
            idx = s_expr_skip_param(params, idx);      // Skip action
        } else {
            idx = s_expr_skip_param(params, idx);
        }
    }
    
    // =========================================================================
    // Use default if no exact match
    // =========================================================================
    if (action_idx == 0) {
        action_idx = default_idx;
    }
    
    // =========================================================================
    // No match and no default - crash (Erlang-style)
    // =========================================================================
    if (action_idx == 0) {
        EXCEPTION("se_field_dispatch: no matching case");
        return SE_CONTINUE;
    }
    
    // =========================================================================
    // Handle branch change: terminate old, reset new
    // =========================================================================
    if (action_idx != prev_action_idx) {
        if (prev_action_idx > 0 && prev_action_idx != 0xFFFF) {
            terminate_action_at_index(inst, params, prev_action_idx);
            reset_action_at_index(inst, params, prev_action_idx);
        }
        
        reset_action_at_index(inst, params, action_idx);
        s_expr_set_user_flags(inst, action_idx);
    }
    
    // =========================================================================
    // Invoke current action and handle pipeline reset
    // =========================================================================
    s_expr_result_t result = s_expr_invoke_any(inst, params, action_idx);

    if (result == SE_PIPELINE_RESET || result == SE_PIPELINE_DISABLE ||
        result == SE_PIPELINE_TERMINATE) {
        terminate_action_at_index(inst, params, action_idx);
        reset_action_at_index(inst, params, action_idx);
        s_expr_set_user_flags(inst, 0xFFFF);
        return SE_PIPELINE_CONTINUE;
    }

    return result;
}
