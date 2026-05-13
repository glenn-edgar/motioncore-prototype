--============================================================================
-- se_dictionary.lua
-- Dictionary/JSON loading, extraction (string-path and hash-path variants),
-- pointer storage, and batch extraction helpers
--============================================================================

-- ============================================================================
-- DICTIONARY LOADING
-- ============================================================================
register_builtin("SE_LOAD_DICTIONARY")
register_builtin("SE_DICT_EXTRACT_INT")
register_builtin("SE_DICT_EXTRACT_FLOAT")
register_builtin("SE_DICT_EXTRACT_UINT")
register_builtin("SE_DICT_EXTRACT_BOOL")
register_builtin("SE_DICT_EXTRACT_HASH")
register_builtin("SE_DICT_EXTRACT_INT_H")
register_builtin("SE_DICT_EXTRACT_FLOAT_H")
register_builtin("SE_DICT_EXTRACT_UINT_H")
register_builtin("SE_DICT_EXTRACT_BOOL_H")
register_builtin("SE_DICT_EXTRACT_HASH_H")
register_builtin("SE_DICT_STORE_PTR")
register_builtin("SE_DICT_STORE_PTR_H")


function se_load_dictionary(blackboard_field, json_expression)
    validate_field_is_ptr64(blackboard_field, "se_load_dictionary")
    
    local c = o_call("SE_LOAD_DICTIONARY")
        field_ref(blackboard_field)
        json(json_expression)
    end_call(c)
    return c
end

function se_load_dictionary_hash(blackboard_field, json_expression)
    validate_field_is_ptr64(blackboard_field, "se_load_dictionary_hash")
    
    local c = o_call("SE_LOAD_DICTIONARY")
        field_ref(blackboard_field)
        json_hash(json_expression)
    end_call(c)
    return c
end

-- ============================================================================
-- STRING PATH EXTRACTION (for json() dictionaries)
-- ============================================================================

function se_dict_extract_int(dict_field, path, dest_field)
    validate_field_is_ptr64(dict_field, "se_dict_extract_int")
    
    local c = o_call("SE_DICT_EXTRACT_INT")
        field_ref(dict_field)
        str(path)
        field_ref(dest_field)
    end_call(c)
    return c
end

function se_dict_extract_float(dict_field, path, dest_field)
    validate_field_is_ptr64(dict_field, "se_dict_extract_float")
    
    local c = o_call("SE_DICT_EXTRACT_FLOAT")
        field_ref(dict_field)
        str(path)
        field_ref(dest_field)
    end_call(c)
    return c
end

function se_dict_extract_uint(dict_field, path, dest_field)
    validate_field_is_ptr64(dict_field, "se_dict_extract_uint")
    
    local c = o_call("SE_DICT_EXTRACT_UINT")
        field_ref(dict_field)
        str(path)
        field_ref(dest_field)
    end_call(c)
    return c
end

function se_dict_extract_bool(dict_field, path, dest_field)
    validate_field_is_ptr64(dict_field, "se_dict_extract_bool")
    
    local c = o_call("SE_DICT_EXTRACT_BOOL")
        field_ref(dict_field)
        str(path)
        field_ref(dest_field)
    end_call(c)
    return c
end

function se_dict_extract_hash(dict_field, path, dest_field)
    validate_field_is_ptr64(dict_field, "se_dict_extract_hash")
    
    local c = o_call("SE_DICT_EXTRACT_HASH")
        field_ref(dict_field)
        str(path)
        field_ref(dest_field)
    end_call(c)
    return c
end

-- ============================================================================
-- HASH PATH EXTRACTION (for json_hash() dictionaries)
-- ============================================================================

function se_dict_extract_int_h(dict_field, path_keys, dest_field)
    validate_field_is_ptr64(dict_field, "se_dict_extract_int_h")
    
    if type(path_keys) ~= "table" or #path_keys == 0 then
        dsl_error("se_dict_extract_int_h: path_keys must be non-empty table")
    end
    
    local c = o_call("SE_DICT_EXTRACT_INT_H")
        field_ref(dict_field)
        for _, key in ipairs(path_keys) do
            str_hash(key)
        end
        field_ref(dest_field)
    end_call(c)
    return c
end

function se_dict_extract_float_h(dict_field, path_keys, dest_field)
    validate_field_is_ptr64(dict_field, "se_dict_extract_float_h")
    
    if type(path_keys) ~= "table" or #path_keys == 0 then
        dsl_error("se_dict_extract_float_h: path_keys must be non-empty table")
    end
    
    local c = o_call("SE_DICT_EXTRACT_FLOAT_H")
        field_ref(dict_field)
        for _, key in ipairs(path_keys) do
            str_hash(key)
        end
        field_ref(dest_field)
    end_call(c)
    return c
end

function se_dict_extract_uint_h(dict_field, path_keys, dest_field)
    validate_field_is_ptr64(dict_field, "se_dict_extract_uint_h")
    
    if type(path_keys) ~= "table" or #path_keys == 0 then
        dsl_error("se_dict_extract_uint_h: path_keys must be non-empty table")
    end
    
    local c = o_call("SE_DICT_EXTRACT_UINT_H")
        field_ref(dict_field)
        for _, key in ipairs(path_keys) do
            str_hash(key)
        end
        field_ref(dest_field)
    end_call(c)
    return c
end

function se_dict_extract_bool_h(dict_field, path_keys, dest_field)
    validate_field_is_ptr64(dict_field, "se_dict_extract_bool_h")
    
    if type(path_keys) ~= "table" or #path_keys == 0 then
        dsl_error("se_dict_extract_bool_h: path_keys must be non-empty table")
    end
    
    local c = o_call("SE_DICT_EXTRACT_BOOL_H")
        field_ref(dict_field)
        for _, key in ipairs(path_keys) do
            str_hash(key)
        end
        field_ref(dest_field)
    end_call(c)
    return c
end

function se_dict_extract_hash_h(dict_field, path_keys, dest_field)
    validate_field_is_ptr64(dict_field, "se_dict_extract_hash_h")
    
    if type(path_keys) ~= "table" or #path_keys == 0 then
        dsl_error("se_dict_extract_hash_h: path_keys must be non-empty table")
    end
    
    local c = o_call("SE_DICT_EXTRACT_HASH_H")
        field_ref(dict_field)
        for _, key in ipairs(path_keys) do
            str_hash(key)
        end
        field_ref(dest_field)
    end_call(c)
    return c
end

-- ============================================================================
-- POINTER STORAGE
-- ============================================================================

function se_dict_store_ptr(dict_field, path, dest_ptr_field)
    validate_field_is_ptr64(dict_field)
    validate_field_is_ptr64(dest_ptr_field)
    
    local call = o_call("SE_DICT_STORE_PTR")
        field_ref(dict_field)
        str(path)
        field_ref(dest_ptr_field)
    end_call(call)
end

function se_dict_store_ptr_h(dict_field, path_keys, dest_ptr_field)
    validate_field_is_ptr64(dict_field)
    validate_field_is_ptr64(dest_ptr_field)
    
    local call = o_call("SE_DICT_STORE_PTR_H")
        field_ref(dict_field)
        for _, key in ipairs(path_keys) do
            str_hash(key)
        end
        field_ref(dest_ptr_field)
    end_call(call)
end

-- ============================================================================
-- BATCH EXTRACTION HELPERS
-- ============================================================================

function se_dict_extract_all(dict_field, extractions)
    validate_field_is_ptr64(dict_field, "se_dict_extract_all")
    
    for _, ext in ipairs(extractions) do
        if not ext.path or not ext.dest then
            dsl_error("se_dict_extract_all: each extraction needs 'path' and 'dest'")
        end
        
        local typ = ext.type or "int"
        
        if typ == "int" then
            se_dict_extract_int(dict_field, ext.path, ext.dest)
        elseif typ == "float" then
            se_dict_extract_float(dict_field, ext.path, ext.dest)
        elseif typ == "uint" then
            se_dict_extract_uint(dict_field, ext.path, ext.dest)
        elseif typ == "bool" then
            se_dict_extract_bool(dict_field, ext.path, ext.dest)
        elseif typ == "hash" then
            se_dict_extract_hash(dict_field, ext.path, ext.dest)
        else
            dsl_error("se_dict_extract_all: unknown type '" .. typ .. "'")
        end
    end
end

function se_dict_extract_all_h(dict_field, extractions)
    validate_field_is_ptr64(dict_field, "se_dict_extract_all_h")
    
    for _, ext in ipairs(extractions) do
        if not ext.path or not ext.dest then
            dsl_error("se_dict_extract_all_h: each extraction needs 'path' and 'dest'")
        end
        
        local typ = ext.type or "int"
        
        if typ == "int" then
            se_dict_extract_int_h(dict_field, ext.path, ext.dest)
        elseif typ == "float" then
            se_dict_extract_float_h(dict_field, ext.path, ext.dest)
        elseif typ == "uint" then
            se_dict_extract_uint_h(dict_field, ext.path, ext.dest)
        elseif typ == "bool" then
            se_dict_extract_bool_h(dict_field, ext.path, ext.dest)
        elseif typ == "hash" then
            se_dict_extract_hash_h(dict_field, ext.path, ext.dest)
        else
            dsl_error("se_dict_extract_all_h: unknown type '" .. typ .. "'")
        end
    end
end