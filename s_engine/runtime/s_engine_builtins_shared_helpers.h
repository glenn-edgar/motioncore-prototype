static void terminate_action_at_index(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t phys_idx
) {
    uint8_t opcode = params[phys_idx].type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_OPEN_CALL) {
        return;
    }
    
    const s_expr_param_t* func_param = &params[phys_idx + 1];
    uint8_t func_opcode = func_param->type & S_EXPR_OPCODE_MASK;
    
    if (func_opcode != S_EXPR_PARAM_MAIN) {
        return;  // Only MAIN nodes receive TERMINATE
    }
    
    uint16_t node_idx = func_param->node_index;
    if (node_idx >= inst->node_count) {
        return;
    }
    
    if (!(inst->node_states[node_idx].flags & S_EXPR_NODE_FLAG_INITIALIZED)) {
        return;  // Never initialized, no cleanup needed
    }
    
    uint16_t func_idx = func_param->func_index;
    if (func_idx >= inst->module->def->main_count) {
        return;
    }
    
    s_expr_main_fn_t fn = inst->module->main_fns[func_idx];
    if (!fn) {
        return;
    }
    
    // Calculate args
    uint16_t close_idx = phys_idx + params[phys_idx].brace_idx;
    uint16_t arg_count = (close_idx > phys_idx + 2) ? (close_idx - phys_idx - 2) : 0;
    const s_expr_param_t* args = (arg_count > 0) ? &params[phys_idx + 2] : NULL;
    
    // Save/restore context
    uint16_t saved_node = inst->current_node_index;
    bool saved_in_ptr = inst->in_pointer_call;
    uint8_t saved_ptr_base = inst->pointer_base;
    
    inst->current_node_index = node_idx;
    if (func_param->type & S_EXPR_FLAG_POINTER) {
        inst->in_pointer_call = true;
        inst->pointer_base = func_param->index_to_pointer;
    }
    
    // Send TERMINATE event
    fn(inst, args, arg_count, SE_EVENT_TERMINATE, 0, NULL);
    
    inst->current_node_index = saved_node;
    inst->in_pointer_call = saved_in_ptr;
    inst->pointer_base = saved_ptr_base;
    
    // Clear node state
    inst->node_states[node_idx].flags = 0;
    inst->node_states[node_idx].state = 0;
    inst->node_states[node_idx].user_data = 0;
}

static void reset_action_at_index(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t phys_idx
) {
    s_expr_reset_recursive_at(inst, params, phys_idx);
}