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


