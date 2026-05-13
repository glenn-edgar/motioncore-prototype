static s_expr_result_t se_frame_allocate(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (!inst || !inst->stack || param_count < 4) {
        EXCEPTION("SE_FRAME_ALLOCATE: invalid parameters");
        return SE_PIPELINE_TERMINATE;
    }
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
    uint16_t num_params = (uint16_t)params[0].uint_val;
    uint16_t num_locals = (uint16_t)params[1].uint_val;
    uint16_t scratch_depth = (uint16_t)params[2].uint_val;
    
    // Push stack frame BEFORE executing children
    if (!s_expr_stack_push_frame(inst->stack, num_params, num_locals)) {
        EXCEPTION("SE_FRAME_ALLOCATE: stack push failed");
        return SE_PIPELINE_TERMINATE;
    }
    for (uint16_t i = 0; i < scratch_depth; i++) {
        s_expr_stack_push_int(inst->stack, 0);
    }
    for (uint16_t i = 3; i < count; i++) {
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
            s_expr_stack_pop_frame(inst->stack);
            return SE_PIPELINE_HALT;
        }
        if (r < SE_PIPELINE_CONTINUE) {
            s_expr_stack_pop_frame(inst->stack);
            return r;
        }
        
        // PIPELINE codes (12-17) - handle internally
        switch (r) {
            case SE_PIPELINE_CONTINUE:
                active_count++;
                continue;
                
            case SE_PIPELINE_HALT:
                s_expr_stack_pop_frame(inst->stack);
                return SE_PIPELINE_CONTINUE;
                
            case SE_PIPELINE_DISABLE:
                s_expr_child_terminate(inst, params, param_count, i);
                continue;

            case SE_PIPELINE_TERMINATE:
                s_expr_children_terminate_all(inst, params, param_count);
                s_expr_stack_pop_frame(inst->stack);
                return SE_PIPELINE_TERMINATE;
                
            case SE_PIPELINE_RESET:
                s_expr_children_terminate_all(inst, params, param_count);
                s_expr_children_reset_all(inst, params, param_count);
                s_expr_stack_pop_frame(inst->stack);
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
        s_expr_stack_pop_frame(inst->stack);
        return SE_PIPELINE_DISABLE;
    }
    s_expr_stack_pop_frame(inst->stack);
    return SE_PIPELINE_CONTINUE;
}

static s_expr_result_t se_frame_free(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    
    UNUSED(event_id);
    UNUSED(params);
    UNUSED(param_count);
    UNUSED(event_data);
    
    if (event_type != SE_EVENT_INIT) {
        
        return SE_PIPELINE_CONTINUE;
    }
    if(event_type == SE_EVENT_TERMINATE) {
        return SE_PIPELINE_CONTINUE;
    }
    
    
    s_expr_stack_pop_frame(inst->stack);
    
    
    return SE_PIPELINE_CONTINUE;
}

static void se_log_stack(s_expr_tree_instance_t* inst, const s_expr_param_t* params, uint16_t param_count, s_expr_event_type_t event_type, uint16_t event_id, void* event_data) {
    UNUSED(event_id);
    UNUSED(event_data);
    UNUSED(params);
    UNUSED(param_count);
    UNUSED(event_type);
    
    
    printf("SE_LOG_STACK: stack capacity = %d\n", inst->stack->capacity);
    printf("SE_LOG_STACK: stack free space = %u\n", inst->stack->capacity-inst->stack->sp);
    printf("SE_LOG_STACK: stack stack pointer = %u\n", inst->stack->sp);
    
    printf("SE_LOG_STACK: stack frame count = %d\n", inst->stack->frame_count);
    
    if(inst->stack->frame_count > 0) {
        printf("SE_LOG_STACK: stack frame base ptr = %u\n", inst->stack->frames[inst->stack->frame_count - 1].base_ptr);
        printf("SE_LOG_STACK: stack frame num params = %d\n", inst->stack->frames[inst->stack->frame_count - 1].num_params);
        printf("SE_LOG_STACK: stack frame num locals = %d\n", inst->stack->frames[inst->stack->frame_count - 1].num_locals);
        printf("SE_LOG_STACK: stack frame scratch base = %u\n", inst->stack->frames[inst->stack->frame_count - 1].scratch_base);
    }
   
}