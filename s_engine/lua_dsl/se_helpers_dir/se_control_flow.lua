--============================================================================
-- se_control_flow.lua
-- Core control flow: sequence, if/then/else, fork, while, cond, etc.
--============================================================================

register_builtin("SE_FUNCTION_INTERFACE")
register_builtin("SE_IF_THEN_ELSE")
register_builtin("SE_TRIGGER_ON_CHANGE")
register_builtin("SE_SEQUENCE")
register_builtin("SE_FORK")
register_builtin("SE_FORK_JOIN")
register_builtin("SE_CHAIN_FLOW")
register_builtin("SE_WHILE")
register_builtin("SE_SEQUENCE_ONCE")
register_builtin("SE_COND")

function se_function_interface(actions_fn)
    local c = m_call("SE_FUNCTION_INTERFACE")
        actions_fn()
    end_call(c)
end

function se_if_then_else(pred_fn, then_fn, else_fn)
    local c = m_call("SE_IF_THEN_ELSE")
        pred_fn()
        then_fn()
        else_fn()
    end_call(c)
end

function se_if_then(pred_fn, then_fn)
    se_if_then_else(pred_fn, then_fn, function()
        se_nop()
    end)
end

function se_trigger_on_change(initial_state, pred_fn, then_fn, else_fn)
    local c = m_call("SE_TRIGGER_ON_CHANGE")
        int(initial_state)
        pred_fn()
        then_fn()
        else_fn()
    end_call(c)
end

function se_on_rising_edge(pred_fn, action_fn)
    se_trigger_on_change(0, pred_fn, action_fn, function()
        se_nop()
    end)
end

function se_on_falling_edge(pred_fn, action_fn)
    se_trigger_on_change(1, pred_fn, function()
        se_nop()
    end, action_fn)
end

function se_sequence(...)
    local children = {...}
    local c = m_call("SE_SEQUENCE")
        for _, child_fn in ipairs(children) do
            child_fn()
        end
    end_call(c)
end

function se_fork(...)
    local children = {...}
    local f = m_call("SE_FORK")
    for _, child in ipairs(children) do
        if type(child) == "function" then
            child()
        end
    end
    end_call(f)
end

function se_fork_join(...)
    local children = {...}
    local f = m_call("SE_FORK_JOIN")
    for _, child in ipairs(children) do
        if type(child) == "function" then
            child()
        end
    end
    end_call(f)
end

function se_chain_flow(...)
    local children = {...}
    local f = m_call("SE_CHAIN_FLOW")
    for _, child in ipairs(children) do
        if type(child) == "function" then
            child()
        end
    end
    end_call(f)
end

function se_while(condition, ...)
    local children = {...}
    local w = m_call("SE_WHILE")
    condition()
    se_fork_join(unpack(children))
    end_call(w)
end

function se_sequence_once(...)
    local children = {...}
    if #children == 0 then
        dsl_error("se_sequence_once: requires at least one child function")
    end
    local c = m_call("SE_SEQUENCE_ONCE")
        for _, child_fn in ipairs(children) do
            if type(child_fn) ~= "function" then
                dsl_error("se_sequence_once: all arguments must be functions")
            end
            child_fn()
        end
    end_call(c)
end

--============================================================================
-- SE_COND - Lisp-style conditional dispatch
--============================================================================

local cond_case_count = 0
local cond_has_default = false
local in_cond = false

function se_cond(cases)
    cond_case_count = 0
    cond_has_default = false
    in_cond = true
    
    local success, err = pcall(function()
        local c = m_call("SE_COND")
            if type(cases) == "function" then
                cases()
            elseif type(cases) == "table" then
                for _, case_fn in ipairs(cases) do
                    case_fn()
                end
            else
                error("se_cond: cases must be function or table")
            end
        end_call(c)
    end)
    
    local case_count = cond_case_count
    local has_default = cond_has_default
    
    in_cond = false
    cond_case_count = 0
    cond_has_default = false
    
    if not success then
        error(err)
    end
    
    if case_count == 0 then
        error("se_cond: must have at least one case")
    end
    
    if not has_default then
        error("se_cond: must have a default case (use se_cond_default)")
    end
end

function se_cond_case(pred_fn, action_fn)
    return function()
        if not in_cond then
            error("se_cond_case: must be used inside se_cond")
        end
        if cond_has_default then
            error("se_cond_case: cannot add cases after se_cond_default (default must be last)")
        end
        cond_case_count = cond_case_count + 1
        pred_fn()
        action_fn()
    end
end

function se_cond_default(action_fn)
    return function()
        if not in_cond then
            error("se_cond_default: must be used inside se_cond")
        end
        if cond_has_default then
            error("se_cond_default: duplicate default case")
        end
        cond_has_default = true
        cond_case_count = cond_case_count + 1
        local pred = p_call("SE_TRUE")
        end_call(pred)
        action_fn()
    end
end