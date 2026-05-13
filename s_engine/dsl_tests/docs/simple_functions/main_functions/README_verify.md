```lua
function se_verify(pred_function, reset_flag, error_function)
    if type(pred_function) ~= "function" then
        error("se_verify: pred_function must be a function")
    end
    if type(reset_flag) ~= "boolean" then
        error("se_verify: reset_flag must be a boolean")
    end
    if type(error_function) ~= "function" then
        error("se_verify: error_function must be a function")
    end

    local c = m_call("SE_VERIFY")
        pred_function()
        int(reset_flag and 1 or 0)
        error_function()
    end_call(c)
end
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
        // Predicate passed
        return SE_PIPELINE_CONTINUE;
    }

    // Predicate failed - invoke error function at logical child 2
    s_expr_child_invoke_oneshot(inst, params, param_count, 2);

    if (reset_flag) {
        return SE_PIPELINE_RESET;
    } else {
        return SE_PIPELINE_TERMINATE;
    }
}
```

write a readme for the following function se_verify

