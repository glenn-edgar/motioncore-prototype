write a readme for the following function 

```lua
function se_verify_and_check_elapsed_time(timeout, reset_flag, error_function)
    if type(timeout) ~= "number" then
        error("se_verify_and_check_elapsed_time: timeout must be a number")
    end
    if type(reset_flag) ~= "boolean" then
        error("se_verify_and_check_elapsed_time: reset_flag must be a boolean")
    end
    if type(error_function) ~= "function" then
        error("se_verify_and_check_elapsed_time: error_function must be a function")
    end

    local c = pt_m_call("SE_VERIFY_AND_CHECK_ELAPSED_TIME")
        flt(timeout)
        int(reset_flag and 1 or 0)
        error_function()
    end_call(c)
end
```


```c
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
        // Timeout - invoke error function at logical child 2
        s_expr_child_invoke_oneshot(inst, params, param_count, 2);

        // Return based on reset_flag
        if (reset_flag) {
            return SE_PIPELINE_RESET;
        } else {
            return SE_PIPELINE_TERMINATE;
        }
    }

    return SE_PIPELINE_CONTINUE;
}
```
