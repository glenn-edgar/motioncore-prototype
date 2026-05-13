--============================================================================
-- se_oneshot.lua
-- Oneshot operations: log, field set/inc/dec, push stack, log stack
--============================================================================
register_builtin("SE_LOG")
register_builtin("SE_LOG_INT")
register_builtin("SE_LOG_FLOAT")
register_builtin("SE_SET_FIELD")
register_builtin("SE_INC_FIELD")
register_builtin("SE_DEC_FIELD")
register_builtin("SE_PUSH_STACK")
register_builtin("SE_LOG_STACK")
register_builtin("SE_NOP")

function se_log(message)
    local c = o_call("SE_LOG")
        str_ptr(message)
    end_call(c)
end

function se_log_slot_integer(message, slot_name)
    if slot_name == nil or slot_name == "" then
        error("se_log_slot: slot_name cannot be nil or empty")
    end
    local c = o_call("SE_LOG_INT")
        str_ptr(message)
        field_ref(slot_name)
    end_call(c)
end

function se_log_slot_float(message, slot_name)
    if slot_name == nil or slot_name == "" then
        error("se_log_slot: slot_name cannot be nil or empty")
    end
    local c = o_call("SE_LOG_FLOAT")
        str_ptr(message)
        field_ref(slot_name)
    end_call(c)
end

function se_set_hash_field(target_field, value)
    if type(value) ~= "string" then
        error("se_set_field_hash: value must be a string")
    end
    local c = o_call("SE_SET_FIELD")
        field_ref(target_field)
        str_hash(value)
    end_call(c)
end

function se_nop()
    local c = o_call("SE_NOP")
    end_call(c)
end

function se_set_field(target_field, value)
    local c = o_call("SE_SET_FIELD")
        field_ref(target_field)
        emit_typed_value(value)
    end_call(c)
end

function se_i_set_field(target_field, value)
    local c = io_call("SE_SET_FIELD")
        field_ref(target_field)
        emit_typed_value(value)
    end_call(c)
end

function se_increment_field(target_field, increment_value)
    local c = o_call("SE_INC_FIELD")
        field_ref(target_field)
        uint(increment_value)
    end_call(c)
end

function se_decrement_field(target_field, decrement_value)
    local c = o_call("SE_DEC_FIELD")
        field_ref(target_field)
        uint(decrement_value)
    end_call(c)
end

function se_push_stack(value_fn)
    if type(value_fn) ~= "function" then
        dsl_error("se_push_stack: value must be a function emitting a parameter")
    end
    
    local c = o_call("SE_PUSH_STACK")
        value_fn()
    end_call(c)
end

function se_log_stack()
    local c = o_call("SE_LOG_STACK")
    end_call(c)
end


function se_set_external_field(value_field, tree_pointer, dictionary_offset)

    validate_field_is_ptr64(tree_pointer, "se_set_external_field")
    if type(dictionary_offset) ~= "number" then
        dsl_error("se_set_external_field: dictionary_offset must be a number")
    end
    local c = o_call("SE_SET_EXTERNAL_FIELD")
        field_ref(value_field)
        field_ref(tree_pointer)
        uint(dictionary_offset)
    end_call(c)
end