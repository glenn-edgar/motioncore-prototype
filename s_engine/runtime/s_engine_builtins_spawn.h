// ============================================================================
// spawn_and_tick_tree
// Creates a child tree, stores in pointer slot, forwards ticks with event queue
// params[0]: ptr_idx (index_to_pointer for child storage)
// params[1]: tree name hash
// params[2]: ct_node_id
// params[3]: stack_size (0 = no stack)
// ============================================================================

static bool result_is_complete(s_expr_result_t result) {
    return result != SE_PIPELINE_CONTINUE && 
           result != SE_PIPELINE_DISABLE;
}


static s_expr_result_t tick_with_event_queue(s_expr_tree_instance_t* child,
                                              uint16_t event_id,
                                              void* event_data)
{
    s_expr_result_t result = s_expr_node_tick(child, event_id, event_data);
    
    uint16_t event_count = s_expr_event_queue_count(child);
    while (event_count > 0 && !result_is_complete(result)) {
        uint16_t tick_type;
        uint16_t ev_id;
        void* ev_data;
        
        s_expr_event_pop(child, &tick_type, &ev_id, &ev_data);
        
        uint16_t saved_tick_type = child->tick_type;
        child->tick_type = tick_type;
        
        s_expr_result_t event_result = s_expr_node_tick(child, ev_id, ev_data);
        
        child->tick_type = saved_tick_type;
        
        if (result_is_complete(event_result)) {
            result = event_result;
            break;
        }
        
        event_count = s_expr_event_queue_count(child);
    }
    
    return result;
}

static s_expr_result_t se_spawn_and_tick_tree(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t*   params,
    uint16_t                param_count,
    s_expr_event_type_t     event_type,
    uint16_t                event_id,
    void*                   event_data
) {
    if (param_count < 3) {
        EXCEPTION("spawn_and_tick_tree: expected 3 params (ptr, tree_hash, stack_size)");
        return SE_PIPELINE_TERMINATE;
    }
    
    uint8_t state = s_expr_get_state(inst);
    
    uint8_t ptr_idx = params[0].index_to_pointer;
    s_expr_slot_t* slot = &inst->pointer_array[ptr_idx];
    
    if (event_type == SE_EVENT_INIT || state == 0) {
        s_expr_hash_t tree_hash = s_expr_param_str_hash(&params[1]);
        uint16_t stack_size = (uint16_t)s_expr_param_uint(&params[2]);
        
        s_expr_tree_instance_t* child = s_expr_tree_create_by_hash(
            inst->module, tree_hash, inst->ct_node_id
        );
        if (!child) {
            EXCEPTION("spawn_and_tick_tree: failed to create child tree");
            return SE_PIPELINE_TERMINATE;
        }
        
        if (stack_size > 0) {
            s_expr_tree_create_stack(child, stack_size);
        }
        
        slot->ptr = child;
        inst->slot_flags[ptr_idx] = S_EXPR_SLOT_FLAG_ALLOCATED;
        
        tick_with_event_queue(child, 0, NULL);
        s_expr_set_state(inst, 1);
        return SE_PIPELINE_CONTINUE;
    }
    
    if (event_type == SE_EVENT_TERMINATE) {
        s_expr_tree_instance_t* child = (s_expr_tree_instance_t*)slot->ptr;
        if (child) {
            s_expr_tree_free(child);
            slot->ptr = NULL;
            inst->slot_flags[ptr_idx] = S_EXPR_SLOT_FLAG_NONE;
        }
        return SE_PIPELINE_CONTINUE;
    }
    
    s_expr_tree_instance_t* child = (s_expr_tree_instance_t*)slot->ptr;
    if (!child) {
        EXCEPTION("spawn_and_tick_tree: child tree not found");
        return SE_PIPELINE_TERMINATE;
    }
    
    s_expr_result_t result = tick_with_event_queue(child, event_id, event_data);
    
    uint16_t event_count = s_expr_event_queue_count(inst);
    while (event_count > 0 && !result_is_complete(result)) {
        uint16_t tick_type;
        uint16_t ev_id;
        void* ev_data;
        
        s_expr_event_pop(inst, &tick_type, &ev_id, &ev_data);
        
        uint16_t saved_tick_type = child->tick_type;
        child->tick_type = tick_type;
        
        result = tick_with_event_queue(child, ev_id, ev_data);
        
        child->tick_type = saved_tick_type;
        
        event_count = s_expr_event_queue_count(inst);
    }
    
    if (result_is_complete(result)) {
        s_expr_tree_free(child);
        slot->ptr = NULL;
        inst->slot_flags[ptr_idx] = S_EXPR_SLOT_FLAG_NONE;
    }
    
    return result;
}


s_expr_result_t se_exec_fn(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t*   params,
    uint16_t                param_count,
    s_expr_event_type_t     event_type,
    uint16_t                event_id,
    void*                   event_data
) {
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (param_count < 1) {
        EXCEPTION("se_exec_fn: expected 1 param (field_ref)");
        return SE_PIPELINE_TERMINATE;
    }
    
    if (event_type == SE_EVENT_INIT) {
        if (!inst->blackboard) {
            EXCEPTION("se_exec_fn: no blackboard bound");
            return SE_PIPELINE_TERMINATE;
        }
        
        const s_expr_param_t** fn_ptr = S_EXPR_GET_FIELD(inst, &params[0], const s_expr_param_t*);
        if (!fn_ptr || !*fn_ptr) {
            EXCEPTION("se_exec_fn: NULL s-expression pointer in blackboard");
            return SE_PIPELINE_TERMINATE;
        }
        
        s_expr_set_user_u64(inst, (uint64_t)(uintptr_t)*fn_ptr);
        return SE_PIPELINE_CONTINUE;
    }
    
    if (event_type == SE_EVENT_TERMINATE) {
        return SE_PIPELINE_CONTINUE;
    }
    
    const s_expr_param_t* sexpr = (const s_expr_param_t*)(uintptr_t)s_expr_get_user_u64(inst);
    if (!sexpr) {
        EXCEPTION("se_exec_fn: NULL cached s-expression");
        return SE_PIPELINE_TERMINATE;
    }
    
    // Reset callable nodes before invoking
    uint8_t opcode = sexpr->type & S_EXPR_OPCODE_MASK;
    if (opcode == S_EXPR_PARAM_OPEN_CALL) {
        s_expr_reset_recursive_at(inst, sexpr, 0);
    }
    
    s_expr_result_t result = s_expr_invoke_any(inst, sexpr, 0);
    
    if (result == SE_PIPELINE_DISABLE) {
        result = SE_PIPELINE_CONTINUE;
    }
    
    return result;
}


s_expr_result_t se_exec_dict_internal(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t*   params,
    uint16_t                param_count,
    s_expr_event_type_t     event_type,
    uint16_t                event_id,
    void*                   event_data
) {
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (param_count < 1) {
        EXCEPTION("se_exec_dict_internal: expected 1 param (key_hash)");
        return SE_PIPELINE_TERMINATE;
    }
    
    if (event_type == SE_EVENT_INIT || event_type == SE_EVENT_TERMINATE) {
        return SE_PIPELINE_CONTINUE;
    }
    
    if (!inst->current_dict) {
        EXCEPTION("se_exec_dict_internal: no dictionary context");
        return SE_PIPELINE_TERMINATE;
    }
    
    s_expr_hash_t key_hash = s_expr_param_str_hash(&params[0]);
    const s_expr_param_t* value = se_dicth_find(inst->current_dict, key_hash);
    if (!value) {
        EXCEPTION("se_exec_dict_internal: key not found");
        return SE_PIPELINE_TERMINATE;
    }
    
    // Scan forward to find CLOSE_KEY
    uint16_t content_count = 0;
    const s_expr_param_t* p = value;
    while (true) {
        uint8_t opcode = p->type & S_EXPR_OPCODE_MASK;
        if (opcode == S_EXPR_PARAM_CLOSE_KEY) {
            break;
        }
        p = value + s_expr_skip_param(value, content_count);
        content_count = (uint16_t)(p - value);
    }
    
    if (content_count == 0) {
        EXCEPTION("se_exec_dict_internal: empty key");
        return SE_PIPELINE_TERMINATE;
    }
    
    // Reset all callable nodes within the key before invoking
    // This allows dictionary entries to be called multiple times
    for (uint16_t idx = 0; idx < content_count; ) {
        uint8_t opcode = value[idx].type & S_EXPR_OPCODE_MASK;
        if (opcode == S_EXPR_PARAM_OPEN_CALL) {
            s_expr_reset_recursive_at(inst, value, idx);
        }
        idx = s_expr_skip_param(value, idx);
    }
    
    // Invoke all children
    s_expr_result_t result = SE_PIPELINE_CONTINUE;
    
    for (uint16_t idx = 0; idx < content_count; ) {
        uint8_t opcode = value[idx].type & S_EXPR_OPCODE_MASK;
        
        if (opcode == S_EXPR_PARAM_OPEN_CALL) {
            uint8_t func_type = value[idx + 1].type & S_EXPR_OPCODE_MASK;
            
            if (func_type == S_EXPR_PARAM_ONESHOT || func_type == S_EXPR_PARAM_PRED) {
                s_expr_invoke_any(inst, value, idx);
            } else {
                result = s_expr_invoke_any(inst, value, idx);
                if (result != SE_PIPELINE_CONTINUE && result != SE_PIPELINE_DISABLE) {
                    break;
                }
            }
        }
        
        idx = s_expr_skip_param(value, idx);
    }
    
    if (result == SE_PIPELINE_DISABLE) {
        result = SE_PIPELINE_CONTINUE;
    }
    
    return result;
}
s_expr_result_t se_exec_dict_dispatch(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t*   params,
    uint16_t                param_count,
    s_expr_event_type_t     event_type,
    uint16_t                event_id,
    void*                   event_data
) {
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (param_count < 2) {
        EXCEPTION("se_exec_dict_dispatch: expected 2 params (field_ref, key_hash)");
        return SE_PIPELINE_TERMINATE;
    }
    
    if (event_type == SE_EVENT_INIT) {
        if (!inst->blackboard) {
            EXCEPTION("se_exec_dict_dispatch: no blackboard bound");
            return SE_PIPELINE_TERMINATE;
        }
        
        const s_expr_param_t** dict_ptr = S_EXPR_GET_FIELD(inst, &params[0], const s_expr_param_t*);
        if (!dict_ptr || !*dict_ptr) {
            EXCEPTION("se_exec_dict_dispatch: NULL dictionary pointer");
            return SE_PIPELINE_TERMINATE;
        }
        
        const s_expr_param_t* dict = *dict_ptr;
        
        if (!se_dicth_is_dict(dict)) {
            EXCEPTION("se_exec_dict_dispatch: not a dictionary");
            return SE_PIPELINE_TERMINATE;
        }
        
        inst->current_dict = (void*)dict;
        return SE_PIPELINE_CONTINUE;
    }
    
    if (event_type == SE_EVENT_TERMINATE) {
        inst->current_dict = NULL;
        return SE_PIPELINE_CONTINUE;
    }
    
    if (!inst->current_dict) {
        EXCEPTION("se_exec_dict_dispatch: no dictionary context");
        return SE_PIPELINE_TERMINATE;
    }
    
    s_expr_hash_t key_hash = s_expr_param_str_hash(&params[1]);
    const s_expr_param_t* value = se_dicth_find(inst->current_dict, key_hash);
    if (!value) {
        EXCEPTION("se_exec_dict_dispatch: key not found");
        return SE_PIPELINE_TERMINATE;
    }
    
    // Scan forward to find CLOSE_KEY
    uint16_t content_count = 0;
    const s_expr_param_t* p = value;
    while (true) {
        uint8_t opcode = p->type & S_EXPR_OPCODE_MASK;
        if (opcode == S_EXPR_PARAM_CLOSE_KEY) {
            break;
        }
        p = value + s_expr_skip_param(value, content_count);
        content_count = (uint16_t)(p - value);
    }
    
    if (content_count == 0) {
        EXCEPTION("se_exec_dict_dispatch: empty key");
        return SE_PIPELINE_TERMINATE;
    }
    
    // Reset all callable nodes before invoking
    for (uint16_t idx = 0; idx < content_count; ) {
        uint8_t opcode = value[idx].type & S_EXPR_OPCODE_MASK;
        if (opcode == S_EXPR_PARAM_OPEN_CALL) {
            s_expr_reset_recursive_at(inst, value, idx);
        }
        idx = s_expr_skip_param(value, idx);
    }
    
    // Invoke all children
    s_expr_result_t result = SE_PIPELINE_CONTINUE;
    
    for (uint16_t idx = 0; idx < content_count; ) {
        uint8_t opcode = value[idx].type & S_EXPR_OPCODE_MASK;
        
        if (opcode == S_EXPR_PARAM_OPEN_CALL) {
            uint8_t func_type = value[idx + 1].type & S_EXPR_OPCODE_MASK;
            
            if (func_type == S_EXPR_PARAM_ONESHOT || func_type == S_EXPR_PARAM_PRED) {
                s_expr_invoke_any(inst, value, idx);
            } else {
                result = s_expr_invoke_any(inst, value, idx);
                if (result != SE_PIPELINE_CONTINUE && result != SE_PIPELINE_DISABLE) {
                    break;
                }
            }
        }
        
        idx = s_expr_skip_param(value, idx);
    }
    
    if (result == SE_PIPELINE_DISABLE) {
        result = SE_PIPELINE_CONTINUE;
    }
    
    return result;
}

s_expr_result_t se_exec_dict_fn_ptr(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t*   params,
    uint16_t                param_count,
    s_expr_event_type_t     event_type,
    uint16_t                event_id,
    void*                   event_data
) {
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (param_count < 2) {
        EXCEPTION("se_exec_dict_fn_ptr: expected 2 params (dict_field, hash_field)");
        return SE_PIPELINE_TERMINATE;
    }
    
    if (event_type == SE_EVENT_INIT) {
        if (!inst->blackboard) {
            EXCEPTION("se_exec_dict_fn_ptr: no blackboard bound");
            return SE_PIPELINE_TERMINATE;
        }
        
        const s_expr_param_t** dict_ptr = S_EXPR_GET_FIELD(inst, &params[0], const s_expr_param_t*);
        if (!dict_ptr || !*dict_ptr) {
            EXCEPTION("se_exec_dict_fn_ptr: NULL dictionary pointer");
            return SE_PIPELINE_TERMINATE;
        }
        
        const s_expr_param_t* dict = *dict_ptr;
        
        if (!se_dicth_is_dict(dict)) {
            EXCEPTION("se_exec_dict_fn_ptr: not a dictionary");
            return SE_PIPELINE_TERMINATE;
        }
        
        inst->current_dict = (void*)dict;
        return SE_PIPELINE_CONTINUE;
    }
    
    if (event_type == SE_EVENT_TERMINATE) {
        inst->current_dict = NULL;
        return SE_PIPELINE_CONTINUE;
    }
    
    if (!inst->current_dict) {
        EXCEPTION("se_exec_dict_fn_ptr: no dictionary context");
        return SE_PIPELINE_TERMINATE;
    }
    
    // Key hash from blackboard field instead of compile-time constant
    const uint32_t* hash_ptr = S_EXPR_GET_FIELD(inst, &params[1], uint32_t);
    if (!hash_ptr) {
        EXCEPTION("se_exec_dict_fn_ptr: cannot read hash field");
        return SE_PIPELINE_TERMINATE;
    }
    s_expr_hash_t key_hash = *hash_ptr;
    
    const s_expr_param_t* value = se_dicth_find(inst->current_dict, key_hash);
    if (!value) {
        EXCEPTION("se_exec_dict_fn_ptr: key not found");
        return SE_PIPELINE_TERMINATE;
    }
    
    // Scan forward to find CLOSE_KEY
    uint16_t content_count = 0;
    const s_expr_param_t* p = value;
    while (true) {
        uint8_t opcode = p->type & S_EXPR_OPCODE_MASK;
        if (opcode == S_EXPR_PARAM_CLOSE_KEY) {
            break;
        }
        p = value + s_expr_skip_param(value, content_count);
        content_count = (uint16_t)(p - value);
    }
    
    if (content_count == 0) {
        EXCEPTION("se_exec_dict_fn_ptr: empty key");
        return SE_PIPELINE_TERMINATE;
    }
    
    // Reset all callable nodes before invoking
    for (uint16_t idx = 0; idx < content_count; ) {
        uint8_t opcode = value[idx].type & S_EXPR_OPCODE_MASK;
        if (opcode == S_EXPR_PARAM_OPEN_CALL) {
            s_expr_reset_recursive_at(inst, value, idx);
        }
        idx = s_expr_skip_param(value, idx);
    }
    
    // Invoke all children
    s_expr_result_t result = SE_PIPELINE_CONTINUE;
    
    for (uint16_t idx = 0; idx < content_count; ) {
        uint8_t opcode = value[idx].type & S_EXPR_OPCODE_MASK;
        
        if (opcode == S_EXPR_PARAM_OPEN_CALL) {
            uint8_t func_type = value[idx + 1].type & S_EXPR_OPCODE_MASK;
            
            if (func_type == S_EXPR_PARAM_ONESHOT || func_type == S_EXPR_PARAM_PRED) {
                s_expr_invoke_any(inst, value, idx);
            } else {
                result = s_expr_invoke_any(inst, value, idx);
                if (result != SE_PIPELINE_CONTINUE && result != SE_PIPELINE_DISABLE) {
                    break;
                }
            }
        }
        
        idx = s_expr_skip_param(value, idx);
    }
    
    if (result == SE_PIPELINE_DISABLE) {
        result = SE_PIPELINE_CONTINUE;
    }
    
    return result;
}






s_expr_result_t se_spawn_tree(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (event_type == SE_EVENT_INIT) {
        if (param_count < 3) {
            EXCEPTION("se_spawn_tree: expected 3 params (tree_pointer, tree_name_hash, stack_size)");
            return SE_PIPELINE_TERMINATE;
        }
        if (!inst || !inst->blackboard || !inst->module) {
            EXCEPTION("se_spawn_tree: NULL instance, blackboard, or module");
            return SE_PIPELINE_TERMINATE;
        }
        
        const s_expr_param_t* ptr_param = &params[0];
        if ((ptr_param->type & S_EXPR_OPCODE_MASK) != S_EXPR_PARAM_FIELD) {
            EXCEPTION("se_spawn_tree: param[0] is not a FIELD");
            return SE_PIPELINE_TERMINATE;
        }
        
        const s_expr_param_t* hash_param = &params[1];
        if ((hash_param->type & S_EXPR_OPCODE_MASK) != S_EXPR_PARAM_STR_HASH) {
            EXCEPTION("se_spawn_tree: param[1] is not a STR_HASH");
            return SE_PIPELINE_TERMINATE;
        }
        
        const s_expr_param_t* size_param = &params[2];
        if ((size_param->type & S_EXPR_OPCODE_MASK) != S_EXPR_PARAM_UINT) {
            EXCEPTION("se_spawn_tree: param[2] is not a UINT");
            return SE_PIPELINE_TERMINATE;
        }
        
        s_expr_hash_t tree_hash = hash_param->str_hash;
        uint16_t stack_size = (uint16_t)size_param->uint_val;
        
        s_expr_tree_instance_t* child = s_expr_tree_create_by_hash(
            inst->module, tree_hash, 0);
        if (!child) {
            EXCEPTION("se_spawn_tree: failed to create tree instance");
            return SE_PIPELINE_TERMINATE;
        }
        
        if (stack_size > 0) {
            if (!s_expr_tree_create_stack(child, stack_size)) {
                EXCEPTION("se_spawn_tree: failed to create stack");
                s_expr_tree_free(child);
                return SE_PIPELINE_TERMINATE;
            }
        }
        
        s_expr_node_init_states(child);
        
        // Store in pt_m_call's own 64-bit slot
        s_expr_set_user_ptr(inst, child);
        
        // Store in blackboard field
        uint8_t* bb = (uint8_t*)inst->blackboard;
        uint64_t* ptr_field = (uint64_t*)(bb + ptr_param->field_offset);
        *ptr_field = (uint64_t)(uintptr_t)child;
        
        return SE_PIPELINE_CONTINUE;
    }
    
    if (event_type == SE_EVENT_TERMINATE) {
        s_expr_tree_instance_t* child = (s_expr_tree_instance_t*)s_expr_get_user_ptr(inst);
        if (child) {
            s_expr_node_terminate(child);
            s_expr_tree_free(child);
            s_expr_set_user_ptr(inst, NULL);
            
            // Clear blackboard field
            if (inst->blackboard && param_count >= 1) {
                const s_expr_param_t* ptr_param = &params[0];
                if ((ptr_param->type & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_FIELD) {
                    uint8_t* bb = (uint8_t*)inst->blackboard;
                    uint64_t* ptr_field = (uint64_t*)(bb + ptr_param->field_offset);
                    *ptr_field = 0;
                }
            }
        }
        return SE_PIPELINE_CONTINUE;
    }
    
    return SE_PIPELINE_CONTINUE;
}


s_expr_result_t se_tick_tree(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    if (param_count < 1) {
        EXCEPTION("se_tick_tree: expected 1 param (tree_pointer)");
        return SE_PIPELINE_TERMINATE;
    }
    if (!inst || !inst->blackboard) {
        EXCEPTION("se_tick_tree: NULL instance or blackboard");
        return SE_PIPELINE_TERMINATE;
    }
    
    const s_expr_param_t* ptr_param = &params[0];
    if ((ptr_param->type & S_EXPR_OPCODE_MASK) != S_EXPR_PARAM_FIELD) {
        EXCEPTION("se_tick_tree: param[0] is not a FIELD");
        return SE_PIPELINE_TERMINATE;
    }
    
    uint8_t* bb = (uint8_t*)inst->blackboard;
    uint64_t raw_ptr = *(uint64_t*)(bb + ptr_param->field_offset);
    s_expr_tree_instance_t* child = (s_expr_tree_instance_t*)(uintptr_t)raw_ptr;
    
    if (!child) {
        EXCEPTION("se_tick_tree: child tree instance is NULL");
        return SE_PIPELINE_TERMINATE;
    }
    
    if (event_type == SE_EVENT_INIT) {
        s_expr_node_full_reset(child);
        return SE_PIPELINE_CONTINUE;
    }
    
    if (event_type == SE_EVENT_TERMINATE) {
        s_expr_node_terminate(child);
        return SE_PIPELINE_CONTINUE;
    }
    
    // Tick with the current event
    s_expr_result_t result = s_expr_node_tick(child, event_id, event_data);
    
    // Drain the child's event queue
    while (s_expr_event_queue_count(child) > 0) {
        uint16_t q_tick_type;
        uint16_t q_event_id;
        void* q_event_data;
        
        s_expr_event_pop(child, &q_tick_type, &q_event_id, &q_event_data);
        result = s_expr_node_tick(child, q_event_id, q_event_data);
    }
    
    return result;
}