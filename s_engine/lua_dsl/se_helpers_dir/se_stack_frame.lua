--============================================================================
-- se_stack_frame.lua
-- Stack frame management: instance, call wrapper, frame allocate
--============================================================================
register_builtin("SE_STACK_FRAME_INSTANCE")
register_builtin("SE_FRAME_ALLOCATE")

function se_stack_frame_instance(num_params, num_locals, scratch_depth, return_vars)
    if type(num_params) ~= "number" or num_params < 0 then
        dsl_error("se_stack_frame_instance: num_params must be non-negative number")
    end
    if type(num_locals) ~= "number" or num_locals < 0 then
        dsl_error("se_stack_frame_instance: num_locals must be non-negative number")
    end
    if type(scratch_depth) ~= "number" or scratch_depth < 0 then
        dsl_error("se_stack_frame_instance: scratch_depth must be non-negative number")
    end
    if type(return_vars) ~= "table" then
        dsl_error("se_stack_frame_instance: return_vars must be a table")
    end

    local max_local = num_params + num_locals
    for i, idx in ipairs(return_vars) do
        if type(idx) ~= "number" or idx < 0 or idx >= max_local then
            dsl_error("se_stack_frame_instance: return_vars[" .. i .. "] = " .. tostring(idx) ..
                      " out of range (valid: 0.." .. (max_local - 1) .. ")")
        end
    end

    num_params    = math.floor(num_params)
    num_locals    = math.floor(num_locals)
    scratch_depth = math.floor(scratch_depth)

    local c = pt_m_call("SE_STACK_FRAME_INSTANCE")
        uint(num_params)
        uint(num_locals)
        uint(scratch_depth)
        local l = list_start()
            for _, idx in ipairs(return_vars) do
                uint(math.floor(idx))
            end
        list_end(l)
    end_call(c)
end

function se_call(num_params, num_locals, scratch_depth, return_vars, body_fns)
    if type(body_fns) ~= "table" or #body_fns == 0 then
        dsl_error("se_call: body_fns must be a non-empty list of functions")
    end
    for i, fn in ipairs(body_fns) do
        if type(fn) ~= "function" then
            dsl_error("se_call: body_fns[" .. i .. "] must be a function")
        end
    end

    local max_local = num_params + num_locals
    for i, idx in ipairs(return_vars) do
        if type(idx) ~= "number" or idx < 0 or idx >= max_local then
            dsl_error("se_call: return_vars[" .. i .. "] = " .. tostring(idx) ..
                      " out of range (valid: 0.." .. (max_local - 1) .. ")")
        end
    end

    -- Push compile-time frame for bounds checking stack_local/stack_tos
    table.insert(frame_stack, {
        num_params    = num_params,
        num_locals    = num_locals,
        scratch_depth = scratch_depth,
    })

    se_sequence_once(
        function()
            se_push_stack(function() uint(num_params) end)
            se_stack_frame_instance(num_params, num_locals, scratch_depth, return_vars)
            for _, fn in ipairs(body_fns) do
                fn()
            end
        end
    )

    -- Pop compile-time frame
    table.remove(frame_stack)
end

function se_frame_allocate(num_params, num_locals, scratch_depth, ...)
    local children = {...}
    
    table.insert(frame_stack, {
        num_params = num_params,
        num_locals = num_locals,
        scratch_depth = scratch_depth,
    })
    
    local f = m_call("SE_FRAME_ALLOCATE")
        uint(num_params)
        uint(num_locals)
        uint(scratch_depth)
        for _, child in ipairs(children) do
            if type(child) == "function" then
                child()
            end
        end
    end_call(f)
    
    table.remove(frame_stack)
end