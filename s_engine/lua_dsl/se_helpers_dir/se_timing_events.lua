--============================================================================
-- se_timing_events.lua
-- Tick/time delays, wait, verify, and event queueing
--============================================================================

register_builtin("SE_TICK_DELAY")
register_builtin("SE_TIME_DELAY")
register_builtin("SE_WAIT_EVENT")
register_builtin("SE_WAIT")
register_builtin("SE_WAIT_TIMEOUT")
register_builtin("SE_VERIFY_AND_CHECK_ELAPSED_TIME")
register_builtin("SE_VERIFY_AND_CHECK_ELAPSED_EVENTS")
register_builtin("SE_VERIFY")
register_builtin("SE_QUEUE_EVENT")

function se_tick_delay(tick_count)
    local c = pt_m_call("SE_TICK_DELAY")
        int(tick_count)
    end_call(c)
end

function se_time_delay(seconds)
    local c = pt_m_call("SE_TIME_DELAY")
        flt(seconds)
    end_call(c)
end

function se_wait_event(target_event, count)
    count = count or 1
    
    if type(target_event) ~= "number" then
        error("se_wait_event: target_event must be a number")
    end
    if type(count) ~= "number" or count < 1 then
        error("se_wait_event: count must be a positive integer")
    end
    
    count = math.floor(count)
    target_event = math.floor(target_event)
    
    local c = pt_m_call("SE_WAIT_EVENT")
        uint(target_event)
        uint(count)
    end_call(c)
end

function se_wait_event_once(event_id)
    se_wait_event(event_id, 1)
end

function se_wait(pred_function)
    if type(pred_function) ~= "function" then
        error("se_wait: pred_function must be a function")
    end

    local c = m_call("SE_WAIT")
        pred_function()
    end_call(c)
end

function se_wait_timeout(pred_function, timeout, reset_flag, error_function)
    if type(pred_function) ~= "function" then
        error("se_wait_timeout: pred_function must be a function")
    end
    if type(timeout) ~= "number" then
        error("se_wait_timeout: timeout must be a number")
    end
    if type(reset_flag) ~= "boolean" then
        error("se_wait_timeout: reset_flag must be a boolean")
    end
    if type(error_function) ~= "function" then
        error("se_wait_timeout: error_function must be a function")
    end

    local c = pt_m_call("SE_WAIT_TIMEOUT")
        pred_function()
        flt(timeout)
        int(reset_flag and 1 or 0)
        error_function()
    end_call(c)
end

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

function se_verify_and_check_elapsed_events(event_id, count, reset_flag, error_function)
    if type(event_id) ~= "number" then
        error("se_verify_and_check_elapsed_events: event_id must be a number")
    end
    if type(count) ~= "number" or count < 1 then
        error("se_verify_and_check_elapsed_events: count must be a positive number")
    end
    if type(reset_flag) ~= "boolean" then
        error("se_verify_and_check_elapsed_events: reset_flag must be a boolean")
    end
    if type(error_function) ~= "function" then
        error("se_verify_and_check_elapsed_events: error_function must be a function")
    end

    event_id = math.floor(event_id)
    count = math.floor(count)

    local c = pt_m_call("SE_VERIFY_AND_CHECK_ELAPSED_EVENTS")
        uint(event_id)
        uint(count)
        int(reset_flag and 1 or 0)
        error_function()
    end_call(c)
end

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

function se_queue_event(event_type, event_id, slot_name)
    if event_type > 0xFFFE then
        dsl_error("se_queue_event: event_type must be <= 0xFFFE")
    end
    if event_id > 0xFFFE then
        dsl_error("se_queue_event: event_id must be <= 0xFFFE")
    end

    local c = o_call("SE_QUEUE_EVENT")
        uint(event_type)
        uint(event_id)
        field_ref(slot_name)
    end_call(c)
end