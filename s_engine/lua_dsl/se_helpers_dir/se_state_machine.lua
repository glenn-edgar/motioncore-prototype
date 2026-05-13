--============================================================================
-- se_state_machine.lua
-- State machine dispatch, event dispatch, case helpers
--============================================================================
register_builtin("SE_FIELD_DISPATCH")
register_builtin("SE_STATE_MACHINE")
register_builtin("SE_EVENT_DISPATCH")


local dispatch_case_values = {}
local in_dispatch = false

function se_case(case_val, action_fn)
    local int_val
    
    if case_val == "default" then
        int_val = -1
    elseif type(case_val) == "number" and math.floor(case_val) == case_val then
        int_val = case_val
    else
        error("se_case: first parameter must be integer or 'default', got: " .. tostring(case_val))
    end
    
    if in_dispatch then
        if dispatch_case_values[int_val] then
            local label = (int_val == -1) and "default" or tostring(int_val)
            error("se_case: duplicate case value: " .. label)
        end
        dispatch_case_values[int_val] = true
    end
    
    int(int_val)
    action_fn()
end

local function dispatch_body(cases_fn, func_name)
    if type(cases_fn) == "function" then
        cases_fn()
    elseif type(cases_fn) == "table" then
        for _, case_fn in ipairs(cases_fn) do
            case_fn()
        end
    else
        error(func_name .. ": cases must be function or table")
    end
end

function se_field_dispatch(state_field, cases_fn)
    dispatch_case_values = {}
    in_dispatch = true
    
    local success, err = pcall(function()
        local c = m_call("SE_FIELD_DISPATCH")
            field_ref(state_field)
            dispatch_body(cases_fn, "se_field_dispatch")
        end_call(c)
    end)
    
    in_dispatch = false
    dispatch_case_values = {}
    
    if not success then
        error(err)
    end
end

function se_state_machine(state_field, cases_fn)
    dispatch_case_values = {}
    in_dispatch = true
    
    local success, err = pcall(function()
        local c = m_call("SE_STATE_MACHINE")
            field_ref(state_field)
            dispatch_body(cases_fn, "se_state_machine")
        end_call(c)
    end)
    
    in_dispatch = false
    dispatch_case_values = {}
    
    if not success then
        error(err)
    end
end

--============================================================================
-- EVENT DISPATCH
--============================================================================

function se_event_case(event_val, action_fn)
    local int_val
    
    if event_val == "default" then
        int_val = -1
    elseif type(event_val) == "number" and math.floor(event_val) == event_val then
        int_val = event_val
    else
        error("se_event_case: event must be integer or 'default', got: " .. tostring(event_val))
    end
    
    int(int_val)
    action_fn()
end

function se_event_dispatch(cases)
    local c = m_call("SE_EVENT_DISPATCH")
        if type(cases) == "function" then
            cases()
        elseif type(cases) == "table" then
            for _, case_fn in ipairs(cases) do
                case_fn()
            end
        else
            error("se_event_dispatch: cases must be function or table")
        end
    end_call(c)
end