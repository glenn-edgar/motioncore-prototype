// ============================================================================
// SE_VERIFY_AND_CHECK_ELAPSED_TIME (main, pt_m_call)
// Monitors elapsed time and invokes error handler if timeout exceeded.
// Param layout: [FLOAT timeout] [INT reset_flag] [ONESHOT error_function]
//
// On INIT: stores current time in pointer slot
// On TICK (SE_EVENT_TICK only): checks if elapsed time > timeout
//          If timeout: invokes error_function, returns RESET or TERMINATE
//
// reset_flag = true:  returns SE_PIPELINE_RESET on timeout
// reset_flag = false: returns SE_PIPELINE_TERMINATE on timeout
//
// Ignores all events except SE_EVENT_TICK
// ============================================================================

static s_expr_result_t se_verify_and_check_elapsed_time(
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
        if (param_count < 3) {
            EXCEPTION("se_verify_and_check_elapsed_time: requires 3 parameters");
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
        return SE_PIPELINE_CONTINUE;
    }

    ct_float_t timeout = params[0].float_val;
    bool reset_flag = (params[1].int_val != 0);

    double start_time = s_expr_get_user_f64(inst);
    double current_time = 0.0;
    s_expr_module_t* mod = inst->module;
    if (mod && mod->alloc.get_time) {
        current_time = mod->alloc.get_time(mod->alloc.ctx);
    }

    double elapsed = current_time - start_time;

    if (elapsed > (double)timeout) {
        // Reset and invoke error function at logical child 2
        s_expr_child_reset(inst, params, param_count, 2);
        s_expr_child_invoke_oneshot(inst, params, param_count, 2);

        if (reset_flag) {
            return SE_PIPELINE_RESET;
        } else {
            return SE_PIPELINE_TERMINATE;
        }
    }

    return SE_PIPELINE_CONTINUE;
}
// ============================================================================
// SE_VERIFY_AND_CHECK_ELAPSED_EVENTS (main, pt_m_call)
// Monitors for a specific event and invokes error handler if count exceeded.
// Param layout: [UINT event_id] [UINT count] [INT reset_flag] [ONESHOT error_function]
//
// On INIT: stores 0 in pointer slot (event counter)
// On TICK: if event_id matches, increment counter
//          If counter > count: invokes error_function, returns RESET or TERMINATE
//
// reset_flag = true:  returns SE_PIPELINE_RESET on count exceeded
// reset_flag = false: returns SE_PIPELINE_TERMINATE on count exceeded
//
// Only counts events matching the target event_id
// ============================================================================

static s_expr_result_t se_verify_and_check_elapsed_events(
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
            EXCEPTION("se_verify_and_check_elapsed_events: requires 4 parameters");
            return SE_PIPELINE_DISABLE;
        }

        // Store initial count (0) in pointer slot
        s_expr_set_user_u64(inst, 0);

        return SE_PIPELINE_CONTINUE;
    }

    // =========================================================================
    // TICK EVENT
    // =========================================================================
    uint32_t target_event = (uint32_t)params[0].uint_val;
    uint32_t max_count = (uint32_t)params[1].uint_val;
    bool reset_flag = (params[2].int_val != 0);

    // Only count matching events
    if (event_id != target_event) {
        return SE_PIPELINE_CONTINUE;
    }

    // Increment counter
    uint64_t current_count = s_expr_get_user_u64(inst);
    current_count++;
    s_expr_set_user_u64(inst, current_count);

    // Check if count exceeded
    if (current_count > max_count) {
        // Reset and invoke error function at logical child 3
        s_expr_child_reset(inst, params, param_count, 3);
        s_expr_child_invoke_oneshot(inst, params, param_count, 3);

        if (reset_flag) {
            return SE_PIPELINE_RESET;
        } else {
            return SE_PIPELINE_TERMINATE;
        }
    }

    return SE_PIPELINE_CONTINUE;
}

// ============================================================================
// SE_VERIFY (main, m_call)
// Evaluates a predicate and invokes error handler if predicate returns false.
// Param layout: [PRED pred_function] [INT reset_flag] [ONESHOT error_function]
//
// On INIT: return SE_PIPELINE_CONTINUE
// On TICK (SE_EVENT_TICK only): evaluate predicate
//          If predicate returns false: invoke error_function, return RESET or TERMINATE
//          If predicate returns true: return SE_PIPELINE_CONTINUE
//
// reset_flag = true:  returns SE_PIPELINE_RESET on failure
// reset_flag = false: returns SE_PIPELINE_TERMINATE on failure
//
// Ignores all events except SE_EVENT_TICK
// ============================================================================

static s_expr_result_t se_verify(
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
        if (param_count < 3) {
            EXCEPTION("se_verify: requires 3 parameters");
            return SE_PIPELINE_DISABLE;
        }

        return SE_PIPELINE_CONTINUE;
    }

    // =========================================================================
    // TICK EVENT - only process SE_EVENT_TICK
    // =========================================================================
    if (event_id != SE_EVENT_TICK) {
        return SE_PIPELINE_CONTINUE;
    }

    // Get reset_flag at logical child 1
    uint16_t reset_flag_idx = s_expr_child_index(params, param_count, 1);
    if (reset_flag_idx == UINT16_MAX) {
        EXCEPTION("se_verify: reset_flag not found");
        return SE_PIPELINE_DISABLE;
    }
    bool reset_flag = (params[reset_flag_idx].int_val != 0);

    // Evaluate predicate at logical child 0
    bool pred_result = s_expr_child_invoke_pred(inst, params, param_count, 0);

    if (pred_result) {
        return SE_PIPELINE_CONTINUE;
    }

    // Predicate failed - reset and invoke error function at logical child 2
    s_expr_child_reset(inst, params, param_count, 2);
    s_expr_child_invoke_oneshot(inst, params, param_count, 2);

    if (reset_flag) {
        return SE_PIPELINE_RESET;
    } else {
        return SE_PIPELINE_TERMINATE;
    }
}
