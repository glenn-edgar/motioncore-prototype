--============================================================================
-- se_function_dict.lua
-- Function dictionary loading/execution, internal dispatch,
-- tree spawning, function pointer load/exec
--============================================================================
register_builtin("SE_LOAD_FUNCTION_DICT")
register_builtin("SE_EXEC_DICT_INTERNAL")
register_builtin("SE_EXEC_DICT_DISPATCH")
register_builtin("SE_SPAWN_TREE")
register_builtin("SE_SET_EXTERNAL_FIELD")
register_builtin("SE_TICK_TREE")
register_builtin("SE_LOAD_FUNCTION")
register_builtin("SE_EXEC_FN")
register_builtin("SE_EXEC_DICT_FN_PTR")

function se_load_function_dict(blackboard_field, func_list)
    validate_field_is_ptr64(blackboard_field, "se_load_function_dict")

    if type(func_list) ~= "table" or #func_list == 0 then
        dsl_error("se_load_function_dict: func_list must not be empty")
    end

    local seen_keys = {}
    for i, entry in ipairs(func_list) do
        if type(entry) ~= "table" or #entry ~= 2 then
            dsl_error("se_load_function_dict: entry[" .. i .. 
                      "] must be {\"name\", function}")
        end
        local name, fn = entry[1], entry[2]
        if type(name) ~= "string" or name == "" then
            dsl_error("se_load_function_dict: entry[" .. i .. 
                      "] key must be non-empty string")
        end
        if type(fn) ~= "function" then
            dsl_error("se_load_function_dict: entry[" .. i .. 
                      "] value must be a function")
        end
        if seen_keys[name] then
            dsl_error("se_load_function_dict: duplicate key '" .. name .. "'")
        end
        seen_keys[name] = true
    end

    local c = o_call("SE_LOAD_FUNCTION_DICT")
        field_ref(blackboard_field)
        local d = dict_start("fn_dict")
            for _, entry in ipairs(func_list) do
                local name, fn = entry[1], entry[2]
                local k = dict_key(name)
                    fn()
                end_dict_key(k)
            end
        dict_end(d)
    end_call(c)
end

function se_exec_dict_internal(key_name)
    validate_field_is_ptr64(blackboard_field, "se_exec_dict_internal")
    if type(key_name) ~= "string" then
        dsl_error("se_exec_dict_internal: key_name must be non-empty string")
    end
    
    local c = pt_m_call("SE_EXEC_DICT_INTERNAL")
        str_hash(key_name)
    end_call(c)
end



function se_exec_dict_fn(blackboard_field, key_name)
    validate_field_is_ptr64(blackboard_field, "se_exec_dict_fn")
    if type(key_name) ~= "string" then
        dsl_error("se_exec_dict_fn: key_name must be a string")
    end
    local c = pt_m_call("SE_EXEC_DICT_DISPATCH")
        field_ref(blackboard_field)
        str_hash(key_name)
    end_call(c)
end

function se_exec_dict_fn_ptr(dict_field, hash_field)
    validate_field_is_ptr64(dict_field, "se_exec_dict_fn_ptr")
    local c = pt_m_call("SE_EXEC_DICT_FN_PTR")
        field_ref(dict_field)
        field_ref(hash_field)
    end_call(c)
end
function se_load_function(blackboard_field, fns)
    validate_field_is_ptr64(blackboard_field, "se_load_function")
   
    local c = io_call("SE_LOAD_FUNCTION")
        field_ref(blackboard_field)
        fns()
    end_call(c)
end

function se_exec_function(blackboard_field)
    validate_field_is_ptr64(blackboard_field, "se_exec_function")
    local c = pt_m_call("SE_EXEC_FN")
        field_ref(blackboard_field)
    end_call(c)
end


function se_spawn_tree(tree_pointer,  tree_name, stack_size)
    validate_field_is_ptr64(tree_pointer, "se_spawn_tree")
    if type(tree_name) ~= "string" then
        dsl_error("se_spawn_tree: tree_name must be a string")
    end
    if type(stack_size) ~= "number" then
        dsl_error("se_spawn_tree: stack_size must be a number")
    end
    local c = pt_m_call("SE_SPAWN_TREE")
        field_ref(tree_pointer)
        str_hash(tree_name)
        uint(stack_size)
    end_call(c)
end



function se_tick_tree(tree_pointer)
    validate_field_is_ptr64(tree_pointer, "se_tick_tree")
    
    stack_size = stack_size or 0
    local c = m_call("SE_TICK_TREE")
        field_ref(tree_pointer)
        
    end_call(c)
end


 