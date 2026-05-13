static s_expr_result_t se_tick_delay(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_id; (void)event_data;
    
    if (event_type == SE_EVENT_INIT) {
        uint32_t ticks = (param_count > 0) ? (uint32_t)params[0].uint_val : 0;
        ticks++;
        s_expr_set_u64(inst, (uint64_t)ticks);
        return SE_PIPELINE_CONTINUE;
    }
    
    if (event_type == SE_EVENT_TERMINATE) {
        return SE_PIPELINE_CONTINUE;
    }
    
    uint64_t remaining = s_expr_get_u64(inst);
   
    if (remaining > 0) {
        remaining--;
        s_expr_set_u64(inst, remaining);
        return SE_FUNCTION_HALT;
    }
    
    return SE_PIPELINE_DISABLE;
}

static s_expr_result_t se_time_delay(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)event_id; (void)event_data;
    
    s_expr_module_t* mod = inst->module;
    
    if (event_type == SE_EVENT_INIT) {
        double seconds = (param_count > 0) ? (double)params[0].float_val : 0.0;
        
        if (seconds <= 0.0) {
            return SE_PIPELINE_CONTINUE;
        }
        
        double now = 0.0;
        if (mod && mod->alloc.get_time) {
            now = mod->alloc.get_time(mod->alloc.ctx);
        }
        
        double target_time = now + seconds;
        s_expr_set_f64(inst, target_time);
        
        return SE_PIPELINE_CONTINUE;
    }
    
    if (event_type == SE_EVENT_TERMINATE) {
        return SE_PIPELINE_CONTINUE;
    }
    
    if (event_id != SE_EVENT_TICK) {
        return SE_FUNCTION_HALT;
    }
    
    double target_time = s_expr_get_f64(inst);
    
    double now = 0.0;
    if (mod && mod->alloc.get_time) {
        now = mod->alloc.get_time(mod->alloc.ctx);
    }
    
    if (now >= target_time) {
        return SE_PIPELINE_DISABLE;
    }
    
    return SE_FUNCTION_HALT;
}

// ============================================================================
// SE_WAIT_EVENT (main)
// Wait for a specific event to occur a specified number of times.
// Param layout: [INT/UINT target_event_id] [INT/UINT count]
// Returns SE_PIPELINE_HALT while waiting
// Returns SE_PIPELINE_DISABLE when count reached
// ============================================================================

static s_expr_result_t se_wait_event(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_data);

    // =========================================================================
    // TERMINATE EVENT
    // =========================================================================
    if (event_type == SE_EVENT_TERMINATE) {
        return SE_PIPELINE_CONTINUE;
    }

    // =========================================================================
    // INIT EVENT
    // =========================================================================
    if (event_type == SE_EVENT_INIT) {
        if (param_count < 2) {
            EXCEPTION("se_wait_event: requires 2 parameters (target_event, count)");
            return SE_PIPELINE_DISABLE;
        }

        uint32_t target_event = (uint32_t)params[0].int_val;
        uint32_t count = (uint32_t)params[1].int_val;

        // Pack target_event and count into 64-bit state
        uint64_t state = ((uint64_t)target_event << 32) | count;
        s_expr_set_user_u64(inst, state);

        return SE_PIPELINE_CONTINUE;
    }

    // =========================================================================
    // TICK EVENT
    // =========================================================================
    uint64_t state = s_expr_get_user_u64(inst);
    uint32_t target_event = (uint32_t)(state >> 32);
    uint32_t remaining = (uint32_t)(state & 0xFFFFFFFF);

    // Already complete
    if (remaining == 0) {
        return SE_PIPELINE_DISABLE;
    }

    // Check if this is the event we're waiting for
    if (event_id == target_event) {
        remaining--;
        state = ((uint64_t)target_event << 32) | remaining;
        s_expr_set_user_u64(inst, state);

        if (remaining == 0) {
            return SE_PIPELINE_DISABLE;
        }
    }

    return SE_PIPELINE_HALT;
}
static s_expr_result_t se_nop(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    (void)inst; (void)params; (void)param_count;
    (void)event_type; (void)event_id; (void)event_data;
    return SE_DISABLE;
}


static s_expr_result_t se_wait(
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
        return SE_PIPELINE_CONTINUE;
    }

    // =========================================================================
    // INIT EVENT
    // =========================================================================
    if (event_type == SE_EVENT_INIT) {
        if (param_count < 1) {
            EXCEPTION("se_wait: requires 1 parameter");
            return SE_PIPELINE_DISABLE;
        }

        return SE_PIPELINE_CONTINUE;
    }

    // =========================================================================
    // TICK EVENT
    // =========================================================================
    bool pred_result = s_expr_child_invoke_pred(inst, params, param_count, 0);

    if (pred_result) {
        return SE_PIPELINE_DISABLE;
    }

    return SE_PIPELINE_HALT;
}

// ============================================================================
// SE_WAIT_TIMEOUT (main, pt_m_call)
// Wait for a predicate to become true, with timeout protection.
// Param layout: [PRED pred_function] [FLOAT timeout] [INT reset_flag] [ONESHOT error_function]
//
// Logical children:
//   0: OPEN_CALL(PRED) pred_function
//   1: FLOAT timeout
//   2: INT reset_flag
//   3: OPEN_CALL(ONESHOT) error_function
//
// On INIT: store start time in f64 slot
// On TICK (SE_EVENT_TICK only):
//   - If predicate true: return SE_PIPELINE_DISABLE (complete)
//   - If elapsed > timeout: invoke error_function, return RESET or TERMINATE
//   - Else: return SE_PIPELINE_HALT (waiting)
// ============================================================================

static s_expr_result_t se_wait_timeout(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_data);

    // =========================================================================
    // TERMINATE EVENT
    // =========================================================================
    if (event_type == SE_EVENT_TERMINATE) {
        return SE_PIPELINE_CONTINUE;
    }

    // =========================================================================
    // INIT EVENT
    // =========================================================================
    if (event_type == SE_EVENT_INIT) {
        if (param_count < 4) {
            EXCEPTION("se_wait_timeout: requires 4 parameters");
            return SE_PIPELINE_DISABLE;
        }

        // Store start time in pointer slot
        double start_time = 0.0;
        s_expr_module_t* mod = inst->module;
        if (mod && mod->alloc.get_time) {
            start_time = mod->alloc.get_time(mod->alloc.ctx);
        }
        s_expr_set_user_f64(inst, start_time);

        return SE_PIPELINE_CONTINUE;
    }

    // =========================================================================
    // TICK EVENT - only process SE_EVENT_TICK
    // =========================================================================
    if (event_id != SE_EVENT_TICK) {
        return SE_PIPELINE_HALT;
    }

    // Evaluate predicate at logical child 0
    bool pred_result = s_expr_child_invoke_pred(inst, params, param_count, 0);

    if (pred_result) {
        return SE_PIPELINE_DISABLE;
    }

    // Get timeout at logical child 1
    uint16_t timeout_idx = s_expr_child_index(params, param_count, 1);
    if (timeout_idx == UINT16_MAX) {
        EXCEPTION("se_wait_timeout: timeout not found");
        return SE_PIPELINE_DISABLE;
    }
    ct_float_t timeout = params[timeout_idx].float_val;

    // Get reset_flag at logical child 2
    uint16_t reset_flag_idx = s_expr_child_index(params, param_count, 2);
    if (reset_flag_idx == UINT16_MAX) {
        EXCEPTION("se_wait_timeout: reset_flag not found");
        return SE_PIPELINE_DISABLE;
    }
    bool reset_flag = (params[reset_flag_idx].int_val != 0);

    // Check elapsed time
    double start_time = s_expr_get_user_f64(inst);
    double current_time = 0.0;
    s_expr_module_t* mod = inst->module;
    if (mod && mod->alloc.get_time) {
        current_time = mod->alloc.get_time(mod->alloc.ctx);
    }

    double elapsed = current_time - start_time;

    if (elapsed > (double)timeout) {
        // Reset and invoke error function at logical child 3
        s_expr_child_reset(inst, params, param_count, 3);
        s_expr_child_invoke_oneshot(inst, params, param_count, 3);

        if (reset_flag) {
            return SE_PIPELINE_RESET;
        } else {
            return SE_PIPELINE_TERMINATE;
        }
    }

    return SE_PIPELINE_HALT;
}


