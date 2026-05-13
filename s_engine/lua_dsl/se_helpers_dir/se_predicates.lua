--============================================================================
-- se_predicates.lua
-- Predicate builder (pred_begin/pred_end), composite and leaf predicates
--============================================================================

--============================================================================
-- Predicate Builder - Recursive tree builder
--============================================================================

local pred_builder_active = false
local pred_id_counter = 0
local pred_current_children = nil
local pred_parent_stack = {}

register_builtin("SE_PRED_OR")
register_builtin("SE_PRED_AND")
register_builtin("SE_PRED_NOR")
register_builtin("SE_PRED_NAND")
register_builtin("SE_PRED_XOR")
register_builtin("SE_PRED_NOT")
register_builtin("SE_TRUE")
register_builtin("SE_FALSE")
register_builtin("SE_CHECK_EVENT")
register_builtin("SE_FIELD_EQ")
register_builtin("SE_FIELD_NE")
register_builtin("SE_FIELD_GT")
register_builtin("SE_FIELD_GE")
register_builtin("SE_FIELD_LT")
register_builtin("SE_FIELD_LE")
register_builtin("SE_FIELD_IN_RANGE")
register_builtin("SE_FIELD_INCREMENT_AND_TEST")
register_builtin("SE_STATE_INCREMENT_AND_TEST")

function pred_begin()
    if pred_builder_active then
        error("pred_begin: already in predicate builder")
    end
    pred_builder_active = true
    pred_id_counter = 0
    pred_current_children = {}
    pred_parent_stack = {}
end

function pred_end()
    if not pred_builder_active then
        error("pred_end: not in predicate builder")
    end
    if #pred_parent_stack > 0 then
        error("pred_end: unclosed composite predicate")
    end
    if #pred_current_children == 0 then
        error("pred_end: empty predicate")
    end

    pred_builder_active = false

    local ops = {}
    for i, op in ipairs(pred_current_children) do
        ops[i] = op
    end
    pred_current_children = nil
    pred_parent_stack = {}

    return function()
        for _, op in ipairs(ops) do
            op()
        end
    end
end

local function next_pred_id()
    pred_id_counter = pred_id_counter + 1
    return pred_id_counter
end

local function pred_push_leaf(emit_fn)
    if pred_builder_active then
        table.insert(pred_current_children, emit_fn)
        return nil
    else
        return emit_fn
    end
end

local function pred_open_composite(name)
    if not pred_builder_active then
        error(name .. ": must be inside pred_begin/pred_end")
    end

    local id = next_pred_id()

    table.insert(pred_parent_stack, { name = name, id = id, children = pred_current_children })
    pred_current_children = {}

    return id
end

function pred_close(id)
    if not pred_builder_active then
        error("pred_close: not in predicate builder")
    end
    if type(id) ~= "number" then
        error("pred_close: expected numeric id, got " .. type(id))
    end
    if #pred_parent_stack == 0 then
        error("pred_close: no open composite")
    end

    local top = pred_parent_stack[#pred_parent_stack]
    if top.id ~= id then
        error("pred_close: expected id=" .. top.id .. " (" .. top.name .. "), got id=" .. id)
    end

    table.remove(pred_parent_stack)

    local name = top.name
    local children = pred_current_children

    if #children == 0 then
        error("pred_close: composite " .. name .. " (id=" .. id .. ") has no children")
    end

    pred_current_children = top.children

    table.insert(pred_current_children, function()
        local c = p_call_composite(name)
            for _, child_fn in ipairs(children) do
                child_fn()
            end
        end_call(c)
    end)
end

--============================================================================
-- Composite Predicates (only inside pred_begin/pred_end)
--============================================================================

function se_pred_or()
    return pred_open_composite("SE_PRED_OR")
end

function se_pred_and()
    return pred_open_composite("SE_PRED_AND")
end

function se_pred_nor()
    return pred_open_composite("SE_PRED_NOR")
end

function se_pred_nand()
    return pred_open_composite("SE_PRED_NAND")
end

function se_pred_xor()
    return pred_open_composite("SE_PRED_XOR")
end

function se_pred_not()
    return pred_open_composite("SE_PRED_NOT")
end

--============================================================================
-- Leaf Predicates
--============================================================================

-- Helper used by field predicates and oneshot field setters
function emit_typed_value(value)
    local t = type(value)
    if t == "number" then
        if math.floor(value) == value then
            if value < 0 then
                int(value)
            else
                uint(value)
            end
        else
            flt(value)
        end
    elseif t == "string" then
        str_hash(value)
    elseif t == "boolean" then
        uint(value and 1 or 0)
    else
        dsl_error("emit_typed_value: unsupported type: " .. t)
    end
end

function se_pred(name)
    return pred_push_leaf(function()
        local c = p_call(name)
        end_call(c)
    end)
end

function se_pred_with(name, param_fn)
    return pred_push_leaf(function()
        local c = p_call(name)
            param_fn()
        end_call(c)
    end)
end

function se_true()
    return pred_push_leaf(function()
        local c = p_call("SE_TRUE")
        end_call(c)
    end)
end

function se_false()
    return pred_push_leaf(function()
        local c = p_call("SE_FALSE")
        end_call(c)
    end)
end

function se_check_event(...)
    local event_ids = {...}
    return pred_push_leaf(function()
        local c = p_call("SE_CHECK_EVENT")
            for _, id in ipairs(event_ids) do
                int(id)
            end
        end_call(c)
    end)
end

function se_field_eq(field_name, value)
    return pred_push_leaf(function()
        local c = p_call("SE_FIELD_EQ")
            field_ref(field_name)
            emit_typed_value(value)
        end_call(c)
    end)
end

function se_field_ne(field_name, value)
    return pred_push_leaf(function()
        local c = p_call("SE_FIELD_NE")
            field_ref(field_name)
            emit_typed_value(value)
        end_call(c)
    end)
end

function se_field_gt(field_name, value)
    return pred_push_leaf(function()
        local c = p_call("SE_FIELD_GT")
            field_ref(field_name)
            emit_typed_value(value)
        end_call(c)
    end)
end

function se_field_ge(field_name, value)
    return pred_push_leaf(function()
        local c = p_call("SE_FIELD_GE")
            field_ref(field_name)
            emit_typed_value(value)
        end_call(c)
    end)
end

function se_field_lt(field_name, value)
    return pred_push_leaf(function()
        local c = p_call("SE_FIELD_LT")
            field_ref(field_name)
            emit_typed_value(value)
        end_call(c)
    end)
end

function se_field_le(field_name, value)
    return pred_push_leaf(function()
        local c = p_call("SE_FIELD_LE")
            field_ref(field_name)
            emit_typed_value(value)
        end_call(c)
    end)
end

function se_field_in_range(field_name, min, max)
    return pred_push_leaf(function()
        local c = p_call("SE_FIELD_IN_RANGE")
            field_ref(field_name)
            emit_typed_value(min)
            emit_typed_value(max)
        end_call(c)
    end)
end

function se_field_increment_and_test(field_name, increment_value, value_to_test)
    return pred_push_leaf(function()
        local c = p_call("SE_FIELD_INCREMENT_AND_TEST")
            field_ref(field_name)
            field_ref(increment_value)
            field_ref(value_to_test)
        end_call(c)
    end)
end

function se_state_increment_and_test(increment_value, value_to_test)
    increment_value = math.floor(increment_value)
    value_to_test = math.floor(value_to_test)
    if increment_value <= 0 or increment_value > 0xFFFF then
        error("se_state_increment_and_test: increment_value must be 0-0xFFFF, got: " .. tostring(increment_value))
    end
    if value_to_test < 0 or value_to_test > 0xFFFF then
        error("se_state_increment_and_test: value_to_test must be 0-0xFFFF, got: " .. tostring(value_to_test))
    end
    
    return pred_push_leaf(function()
        local c = p_call("SE_STATE_INCREMENT_AND_TEST")
            uint(increment_value)
            uint(value_to_test)
        end_call(c)
    end)
end