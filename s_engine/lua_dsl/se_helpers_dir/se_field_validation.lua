--============================================================================
-- se_field_validation.lua
-- Compile-time field validation helpers
-- Used by se_dictionary.lua and se_function_dict.lua
--============================================================================

function validate_field_exists(field_name, func_name)
    if not current_tree or not current_tree.record_name then
        return nil
    end
    
    local rec = current_module.records[current_tree.record_name]
    if not rec then
        return nil
    end
    
    for _, f in ipairs(rec.fields) do
        if f.name == field_name then
            return f
        end
    end
    
    dsl_error(string.format(
        "%s: field '%s' not found in record '%s'",
        func_name, field_name, current_tree.record_name))
end

function validate_field_is_ptr64(field_name, func_name)
    local f = validate_field_exists(field_name, func_name)
    if not f then return end
    
    if not f.is_ptr64 then
        dsl_error(string.format(
            "%s: field '%s' must be a PTR64_FIELD (got type='%s', size=%d)\n" ..
            "  Hint: Use PTR64_FIELD(\"%s\", \"void\") in record '%s'",
            func_name,
            field_name, 
            f.type or "unknown",
            f.size or 0,
            field_name,
            current_tree.record_name))
    end
    
    return f
end

function validate_field_is_numeric(field_name, func_name)
    local f = validate_field_exists(field_name, func_name)
    if not f then return end
    
    local numeric_types = {
        int32 = true, uint32 = true,
        int64 = true, uint64 = true,
        float = true, double = true
    }
    
    if not numeric_types[f.type] then
        dsl_error(string.format(
            "%s: field '%s' must be a numeric type (got '%s')",
            func_name, field_name, f.type or "unknown"))
    end
    
    return f
end

function validate_field_type(field_name, expected_type, func_name)
    local f = validate_field_exists(field_name, func_name)
    if not f then return end
    
    if f.type ~= expected_type then
        dsl_error(string.format(
            "%s: field '%s' must be type '%s' (got '%s')",
            func_name, field_name, expected_type, f.type or "unknown"))
    end
    
    return f
end