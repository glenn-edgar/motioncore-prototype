-- ============================================================================
-- s_expr_dsl.lua
-- S-Expression Engine DSL Core Library - Version 5.3
-- 
-- This is the main DSL library that provides:
--   1. DSL functions for defining modules, records, trees, etc.
--   2. C header generation (via s_expr_generators.lua)
--   3. Binary module generation (via s_expr_generators.lua)
--   4. Debug output generation (via s_expr_debug.lua)
--
-- VERSION 5.3 CHANGES:
--   - Added tree validation: exactly one top-level node required
--   - Better error messages for tree structure violations
--
-- VERSION 5.2 CHANGES:
--   - Split into 3 files for easier maintenance
--   - Added array structures (array_start/array_end)
--   - Added tuple structures (tuple_start/tuple_end)
--   - Added str_hash() for emitting pre-computed hashes
--   - Added key()/key_end() as aliases for dict_key()/end_dict_key()
--   - Added key_hash() for integer hash keys
--
-- Usage: This file is loaded by s_compile.lua and sets up global DSL functions
-- ============================================================================

local ffi = require("ffi")
local bit = require("bit")
jit.off()

local M = {}

-- ============================================================================
-- FNV-1a 32-bit HASH
-- ============================================================================

local hash_module = {}

function hash_module.fnv1a_32(str)
    local hash = 0x811c9dc5

    for i = 1, #str do
        hash = bit.bxor(hash, str:byte(i))
        
        local lo = bit.band(hash, 0xFFFF)
        local hi = bit.band(bit.rshift(hash, 16), 0xFFFF)
        
        local prime = 0x01000193
        local lo_prod = lo * prime
        local hi_prod = hi * prime
        
        hash = lo_prod + bit.lshift(bit.band(hi_prod, 0xFFFF), 16)
        hash = bit.tobit(hash)
    end

    local u32 = ffi.new("uint32_t", hash)
    return tonumber(u32)
end

function hash_module.fmt_hash(h)
    local u32 = ffi.new("uint32_t", h)
    return string.format("0x%08X", tonumber(u32))
end

-- JIT warmup
for i = 1, 10 do
    hash_module.fnv1a_32("warmup_string_" .. i)
end

M.fnv1a_32 = hash_module.fnv1a_32
M.fmt_hash = hash_module.fmt_hash
_G.fnv1a_32 = hash_module.fnv1a_32

-- ============================================================================
-- TYPE SYSTEM
-- ============================================================================

local types_module = {}

types_module.type_info = {
    int8    = { size = 1, align = 1, ctype = "int8_t",   tag = 0x01 },
    int16   = { size = 2, align = 2, ctype = "int16_t",  tag = 0x02 },
    int32   = { size = 4, align = 4, ctype = "int32_t",  tag = 0x03 },
    int64   = { size = 8, align = 8, ctype = "int64_t",  tag = 0x04 },
    uint8   = { size = 1, align = 1, ctype = "uint8_t",  tag = 0x05 },
    uint16  = { size = 2, align = 2, ctype = "uint16_t", tag = 0x06 },
    uint32  = { size = 4, align = 4, ctype = "uint32_t", tag = 0x07 },
    uint64  = { size = 8, align = 8, ctype = "uint64_t", tag = 0x08 },
    float   = { size = 4, align = 4, ctype = "float",    tag = 0x09 },
    double  = { size = 8, align = 8, ctype = "double",   tag = 0x0A },
    bool    = { size = 1, align = 1, ctype = "bool",     tag = 0x0B },
    char    = { size = 1, align = 1, ctype = "char",     tag = 0x0C },
}

types_module.S_EXPR_PARAM = {
    INT         = 0x00,
    UINT        = 0x01,
    FLOAT       = 0x02,
    STR_HASH    = 0x03,
    SLOT        = 0x04,
    OPEN        = 0x05,
    CLOSE       = 0x06,
    OPEN_CALL   = 0x07,
    ONESHOT     = 0x08,
    MAIN        = 0x09,
    PRED        = 0x0A,
    FIELD       = 0x0B,
    RESULT      = 0x0C,
    STR_IDX     = 0x0D,
    CONST_REF   = 0x0E,
    RESERVED_0F = 0x0F,
    OPEN_DICT   = 0x10,
    CLOSE_DICT  = 0x11,
    OPEN_KEY    = 0x12,
    CLOSE_KEY   = 0x13,
    OPEN_ARRAY  = 0x14,
    CLOSE_ARRAY = 0x15,
    OPEN_TUPLE  = 0x16,
    CLOSE_TUPLE = 0x17,
    STACK_TOS   = 0x18,
    STACK_LOCAL = 0x19,
    NULL_PARAM  = 0x1A,
    STACK_PUSH  = 0x1B,
    STACK_POP   = 0x1C,
}

types_module.S_EXPR_FLAG_SURVIVES_RESET = 0x40
types_module.S_EXPR_FLAG_POINTER        = 0x80

M.type_info = types_module.type_info
M.S_EXPR_PARAM = types_module.S_EXPR_PARAM

-- ============================================================================
-- BUILTIN FUNCTION LIST
-- ============================================================================

local builtins_module = {}

builtins_module.BUILTIN_FUNCTIONS = {}
builtins_module.BUILTIN_SET = {}

function builtins_module.register(name)
    if not builtins_module.BUILTIN_SET[name] then
        builtins_module.BUILTIN_SET[name] = true
        table.insert(builtins_module.BUILTIN_FUNCTIONS, name)
    end
end

function builtins_module.is_builtin(name)
    return builtins_module.BUILTIN_SET[name] == true
end

_G.register_builtin = builtins_module.register

M.BUILTIN_FUNCTIONS = builtins_module.BUILTIN_FUNCTIONS
M.BUILTIN_SET = builtins_module.BUILTIN_SET
M.is_builtin = builtins_module.is_builtin
-- ============================================================================
-- MODULE STATE
-- ============================================================================

local current_module = nil
local current_record = nil
local current_tree = nil
local current_const = nil
local current_call_stack = {}
local order_stack = {0}
local in_composite_block = false
local debug_mode = false
_G.frame_stack = {}

-- ============================================================================
-- BRACE STACK VALIDATION
-- ============================================================================

local brace_stack = {}
local MAX_BRACE_STACK = 64
local gensym_counter = 0

local function gensym(prefix)
    gensym_counter = gensym_counter + 1
    return string.format("__%s_%d", prefix or "anon", gensym_counter)
end

local function push_brace(name, brace_type)
    if #brace_stack >= MAX_BRACE_STACK then
        error("[DSL Error] Brace stack overflow (max " .. MAX_BRACE_STACK .. ")", 3)
    end
    local marker = {
        name = name or gensym(brace_type),
        brace_type = brace_type,
        line = debug.getinfo(3, "l").currentline or 0
    }
    table.insert(brace_stack, marker)
    return marker
end

local function pop_brace(marker, expected_type)
    if #brace_stack == 0 then
        error(string.format("[DSL Error] Unmatched %s close", expected_type), 3)
    end
    local top = brace_stack[#brace_stack]
    if top ~= marker then
        error(string.format("[DSL Error] Brace mismatch: expected '%s' (%s line %d), got '%s'",
            top.name, top.brace_type, top.line, marker and marker.name or "nil"), 3)
    end
    if top.brace_type ~= expected_type then
        error(string.format("[DSL Error] Type mismatch: '%s' opened as %s, closed as %s",
            top.name, top.brace_type, expected_type), 3)
    end
    table.remove(brace_stack)
end

local function check_brace_balance()
    if #brace_stack > 0 then
        local unclosed = {}
        for _, b in ipairs(brace_stack) do
            table.insert(unclosed, string.format("'%s' (%s line %d)", b.name, b.brace_type, b.line))
        end
        error("[DSL Error] Unclosed braces: " .. table.concat(unclosed, ", "), 3)
    end
end

local function reset_brace_stack()
    brace_stack = {}
    gensym_counter = 0
end

-- ============================================================================
-- DSL ERROR HANDLING
-- ============================================================================

local function dsl_error(msg)
    error("[DSL Error] " .. msg, 3)
end

_G.dsl_error = dsl_error

-- ============================================================================
-- MODULE FUNCTIONS
-- ============================================================================

function _G.start_module(name)
    if current_module then
        dsl_error("Module already started: " .. current_module.name)
    end
    
    current_module = {
        name = name,
        name_hash = hash_module.fnv1a_32(name),
        records = {},
        record_order = {},
        trees = {},
        tree_order = {},
        constants = {},
        const_order = {},
        oneshot_funcs = {},
        main_funcs = {},
        pred_funcs = {},
        string_table = {},
        string_index = {},
        events = {},
        event_names = {},
        pointer_size = _G._pointer_size or 4,
        debug = false,
    }
    
    return current_module
end

function _G.end_module(mod)
    if not current_module then
        dsl_error("No module started")
    end
    
    local result = current_module
    current_module = nil
    current_record = nil
    current_tree = nil
    current_const = nil
    current_call_stack = {}
    frame_stack = {}
    return result
end

function _G.use_32bit()
    if current_module then current_module.pointer_size = 4 end
    _G._pointer_size = 4
end

function _G.use_64bit()
    if current_module then current_module.pointer_size = 8 end
    _G._pointer_size = 8
end

function _G.set_debug(val)
    debug_mode = val
    if current_module then current_module.debug = val end
end

function _G.is_debug()
    return debug_mode
end

-- ============================================================================
-- EVENT FUNCTIONS
-- ============================================================================

function _G.EVENT(name, id)
    if not current_module then dsl_error("No module started") end
    if type(name) ~= "string" then dsl_error("Event name must be a string") end
    if type(id) ~= "number" or id < 0 or id > 0xFFFA then
        dsl_error("Event ID must be a number in range 0x0000-0xFFFA")
    end
    if current_module.event_names[name] then dsl_error("Event '" .. name .. "' already defined") end
    
    table.insert(current_module.events, { name = name, id = id })
    current_module.event_names[name] = id
    return id
end

function _G.EVENTS(event_table)
    if not current_module then dsl_error("No module started") end
    for name, id in pairs(event_table) do
        EVENT(name, id)
    end
end

function _G.EVENT_ID(name)
    if not current_module then dsl_error("No module started") end
    local id = current_module.event_names[name]
    if not id then dsl_error("Unknown event: " .. name) end
    return id
end

-- ============================================================================
-- RECORD FUNCTIONS
-- ============================================================================

function _G.RECORD(name)
    if not current_module then dsl_error("No module started") end
    if current_record then dsl_error("Record already open: " .. current_record.name) end
    if current_module.records[name] then dsl_error("Record already defined: " .. name) end
    
    current_record = {
        name = name,
        name_hash = hash_module.fnv1a_32(name),
        fields = {},
        size = 0,
        align = 1,
    }
end

function _G.FIELD(name, type_name)
    if not current_record then dsl_error("No record open") end
    
    -- Reject sub-32-bit types - 32-bit writes corrupt adjacent fields
    local unsafe_types = { int8=1, uint8=1, int16=1, uint16=1, bool=1, char=1 }
    if unsafe_types[type_name] then
        dsl_error("Type '" .. type_name .. "' not allowed in FIELD() - use int32/uint32 minimum. For strings use CHAR_ARRAY().")
    end
    
    local info = types_module.type_info[type_name]
    local field = {
        name = name,
        name_hash = hash_module.fnv1a_32(name),
        type = type_name,
        is_pointer = false,
        is_char_array = false,
        is_embedded = false,
    }
    
    if info then
        field.size = info.size
        field.align = info.align
        field.type_tag = info.tag
    else
        local embedded = current_module.records[type_name]
        if embedded then
            field.size = embedded.size
            field.align = embedded.align or 4
            field.is_embedded = true
            field.embedded_record = type_name
            field.type_tag = 0x0F
        else
            dsl_error("Unknown type: " .. type_name)
        end
    end
    
    local offset = current_record.size
    local padding = (field.align - (offset % field.align)) % field.align
    offset = offset + padding
    field.offset = offset
    
    current_record.size = offset + field.size
    if field.align > current_record.align then
        current_record.align = field.align
    end
    
    table.insert(current_record.fields, field)
end


function _G.INT32_ARRAY(name, length)
    if not current_record then dsl_error("No record open") end
    
    local field = {
        name = name,
        name_hash = hash_module.fnv1a_32(name),
        type = "int32_array",
        array_len = length,
        size = length * 4,
        align = 4,
        is_pointer = false,
        is_char_array = false,
        is_int32_array = true,
        is_embedded = false,
        type_tag = 0x13,  -- New type tag for int32 array
    }
    
    local offset = current_record.size
    local padding = (field.align - (offset % field.align)) % field.align
    offset = offset + padding
    field.offset = offset
    
    current_record.size = offset + field.size
    if field.align > current_record.align then
        current_record.align = field.align
    end
    
    table.insert(current_record.fields, field)
end

function _G.FLOAT32_ARRAY(name, length)
    if not current_record then dsl_error("No record open") end
    
    local field = {
        name = name,
        name_hash = hash_module.fnv1a_32(name),
        type = "float32_array",
        array_len = length,
        size = length * 4,
        align = 4,
        is_pointer = false,
        is_char_array = false,
        is_float32_array = true,
        is_embedded = false,
        type_tag = 0x14,  -- New type tag for float32 array
    }
    
    local offset = current_record.size
    local padding = (field.align - (offset % field.align)) % field.align
    offset = offset + padding
    field.offset = offset
    
    current_record.size = offset + field.size
    if field.align > current_record.align then
        current_record.align = field.align
    end
    
    table.insert(current_record.fields, field)
end
-- ============================================================================
-- RECORD FUNCTIONS (add after PTR_FIELD)
-- ============================================================================

function _G.PTR64_FIELD(name, target_type)
    if not current_record then dsl_error("No record open") end
    
    local field = {
        name = name,
        name_hash = hash_module.fnv1a_32(name),
        type = "ptr64",
        target_type = target_type,
        size = 8,
        align = 8,
        is_pointer = true,
        is_ptr64 = true,
        is_char_array = false,
        is_embedded = false,
        type_tag = 0x0E,
    }
    
    local offset = current_record.size
    local padding = (field.align - (offset % field.align)) % field.align
    offset = offset + padding
    field.offset = offset
    
    current_record.size = offset + field.size
    if field.align > current_record.align then
        current_record.align = field.align
    end
    
    table.insert(current_record.fields, field)
end
function _G.CHAR_ARRAY(name, length)
    if not current_record then dsl_error("No record open") end
    
    if length < 4 then
        dsl_error("CHAR_ARRAY length must be at least 4 bytes (32-bit write safety)")
    end
    
    local field = {
        name = name,
        name_hash = hash_module.fnv1a_32(name),
        type = "char_array",
        array_len = length,
        size = length,
        align = 1,
        is_pointer = false,
        is_char_array = true,
        is_embedded = false,
        type_tag = 0x0D,
    }
    
    field.offset = current_record.size
    current_record.size = current_record.size + field.size
    
    table.insert(current_record.fields, field)
end

function _G.END_RECORD()
    if not current_record then dsl_error("No record open") end
    
    local padding = (current_record.align - (current_record.size % current_record.align)) % current_record.align
    current_record.size = current_record.size + padding
    
    current_module.records[current_record.name] = current_record
    table.insert(current_module.record_order, current_record.name)
    
    current_record = nil
end

function _G.get_field_offset(record_name, field_name)
    if not current_module then dsl_error("No module started") end
    local rec = current_module.records[record_name]
    if not rec then dsl_error("Unknown record: " .. record_name) end
    
    for _, f in ipairs(rec.fields) do
        if f.name == field_name then
            return f.offset, f.size, f
        end
    end
    
    dsl_error("Field '" .. field_name .. "' not found in record '" .. record_name .. "'")
end
-- Add to s_expr_dsl.lua, modify emit_json_value:
-- ============================================================================
-- HELPER CONSTRUCTORS FOR EMBEDDED FUNCTIONS (Generic)
-- ============================================================================

-- Function reference (just a hash for runtime lookup)
function _G.func(name)
    return {_type = "func_ref", name = name}
end

-- Inline main call
function _G.main(name, ...)
    return {_type = "m_call", name = name, params = {...}}
end

-- Inline main call with pointer (survives reset)
function _G.main_pt(name, ...)
    return {_type = "pt_m_call", name = name, params = {...}}
end

-- Inline oneshot call
function _G.oneshot(name, ...)
    return {_type = "o_call", name = name, params = {...}}
end

-- Inline oneshot call (survives reset)
function _G.oneshot_i(name, ...)
    return {_type = "io_call", name = name, params = {...}}
end

-- Inline predicate call
function _G.pred(name, ...)
    return {_type = "p_call", name = name, params = {...}}
end

-- Inline composite predicate call
function _G.pred_c(name, ...)
    return {_type = "p_call_composite", name = name, params = {...}}
end

-- Field reference helper
function _G.field(name)
    return {_type = "field", name = name}
end

-- Nested field reference helper
function _G.nfield(path)
    return {_type = "nested_field", path = path}
end

-- Constant reference helper
function _G.const(name)
    return {_type = "const_ref", name = name}
end

-- String hash helper (for runtime lookup keys)
function _G.hash(str)
    return {_type = "str_hash", value = str}
end

-- Explicit list constructor
function _G.list(...)
    return {_type = "list", items = {...}}
end

-- Explicit tuple constructor
function _G.tuple(...)
    return {_type = "tuple", items = {...}}
end
local function emit_json_value(value, use_hash_keys)
    local vtype = type(value)
    
    if vtype == "nil" then
        -- nil emits 0
        int(0)
        
    elseif vtype == "number" then
        if math.floor(value) == value then
            int(value)
        else
            flt(value)
        end
        
    elseif vtype == "string" then
        str(value)
        
    elseif vtype == "boolean" then
        int(value and 1 or 0)
        
    elseif vtype == "function" then
        -- Call the function to get its specification
        local spec = value()
        if spec == nil then
            int(0)
        elseif type(spec) == "string" then
            -- Simple string returned = function name hash
            str_hash(spec)
        elseif type(spec) == "table" then
            -- Recurse to handle the spec table
            emit_json_value(spec, use_hash_keys)
        else
            dsl_error("Function must return spec table, string, or nil")
        end
        
    elseif vtype == "table" then
        -- Check for special marker tables first
        if value._type then
            local spec = value
            
            if spec._type == "m_call" then
                local n = m_call(spec.name)
                if spec.params then
                    for _, p in ipairs(spec.params) do
                        emit_json_value(p, use_hash_keys)
                    end
                end
                end_call(n)
                
            elseif spec._type == "pt_m_call" then
                local n = pt_m_call(spec.name)
                if spec.params then
                    for _, p in ipairs(spec.params) do
                        emit_json_value(p, use_hash_keys)
                    end
                end
                end_call(n)
                
            elseif spec._type == "o_call" then
                local n = o_call(spec.name)
                if spec.params then
                    for _, p in ipairs(spec.params) do
                        emit_json_value(p, use_hash_keys)
                    end
                end
                end_call(n)
                
            elseif spec._type == "io_call" then
                local n = io_call(spec.name)
                if spec.params then
                    for _, p in ipairs(spec.params) do
                        emit_json_value(p, use_hash_keys)
                    end
                end
                end_call(n)
                
            elseif spec._type == "p_call" then
                local n = p_call(spec.name)
                if spec.params then
                    for _, p in ipairs(spec.params) do
                        emit_json_value(p, use_hash_keys)
                    end
                end
                end_call(n)
                
            elseif spec._type == "p_call_composite" then
                local n = p_call_composite(spec.name)
                if spec.params then
                    for _, p in ipairs(spec.params) do
                        emit_json_value(p, use_hash_keys)
                    end
                end
                end_call(n)
                
            elseif spec._type == "func_ref" then
                -- Just emit a hash reference
                str_hash(spec.name)
                
            elseif spec._type == "field" then
                -- Field reference
                field_ref(spec.name)
                
            elseif spec._type == "nested_field" then
                -- Nested field reference
                nested_field_ref(spec.path)
                
            elseif spec._type == "const_ref" then
                -- Constant reference
                const_ref(spec.name)
                
            elseif spec._type == "str_hash" then
                -- String hash
                str_hash(spec.value)
                
            elseif spec._type == "result" then
                -- Result code
                add_param("result", spec.value)
                
            elseif spec._type == "list" then
                -- Explicit list
                local l = list_start()
                if spec.items then
                    for _, item in ipairs(spec.items) do
                        emit_json_value(item, use_hash_keys)
                    end
                end
                list_end(l)
                
            elseif spec._type == "tuple" then
                -- Explicit tuple
                local t = tuple_start()
                if spec.items then
                    for _, item in ipairs(spec.items) do
                        emit_json_value(item, use_hash_keys)
                    end
                end
                tuple_end(t)
                
            else
                dsl_error("Unknown spec type: " .. tostring(spec._type))
            end
            
            return
        end
        
        -- Check if array (sequential integer keys starting at 1)
        local is_array = true
        local max_idx = 0
        local count = 0
        
        for k, _ in pairs(value) do
            count = count + 1
            if type(k) ~= "number" or k ~= math.floor(k) or k < 1 then
                is_array = false
                break
            end
            if k > max_idx then max_idx = k end
        end
        
        if is_array and max_idx ~= count then
            is_array = false  -- Sparse array
        end
        
        if count == 0 then
            -- Empty table - emit as empty dict
            local d = dict_start()
            dict_end(d)
        elseif is_array then
            local a = array_start()
            for _, v in ipairs(value) do
                emit_json_value(v, use_hash_keys)
            end
            array_end(a)
        else
            local d = dict_start()
            -- Sort keys for deterministic output
            local keys = {}
            for k, _ in pairs(value) do
                table.insert(keys, k)
            end
            table.sort(keys, function(a, b)
                return tostring(a) < tostring(b)
            end)
            
            for _, k in ipairs(keys) do
                local v = value[k]
                local key_marker
                if use_hash_keys then
                    key_marker = key_hash(fnv1a_32(tostring(k)))
                else
                    key_marker = key(tostring(k))
                end
                emit_json_value(v, use_hash_keys)
                key_end(key_marker)
            end
            dict_end(d)
        end
    else
        dsl_error("Unsupported type in json(): " .. vtype)
    end
end

-- String keys (default)
function _G.json(tbl)
    emit_json_value(tbl, false)
end

-- Hash keys (smaller binary, faster lookup)
function _G.json_hash(tbl)
    emit_json_value(tbl, true)
end
-- ============================================================================
-- CONSTANT FUNCTIONS
-- ============================================================================

local function resolve_field_path(rec, path)
    local parts = {}
    for part in path:gmatch("[^.]+") do table.insert(parts, part) end
    
    local offset = 0
    local current_rec = rec
    local field = nil
    
    for i, part in ipairs(parts) do
        field = nil
        for _, f in ipairs(current_rec.fields) do
            if f.name == part then field = f; break end
        end
        if not field then return nil, nil, nil end
        offset = offset + field.offset
        if i < #parts and field.is_embedded then
            current_rec = current_module.records[field.embedded_record]
            if not current_rec then return nil, nil, nil end
        end
    end
    
    return offset, field.size, field
end

local function write_value_to_buffer(buf, offset, value, field)
    local ftype = field.type
    
    if ftype == "float" then
        local fbuf = ffi.new("float[1]", value)
        local bytes = ffi.cast("uint8_t*", fbuf)
        for i = 0, 3 do buf[offset + i + 1] = bytes[i] end
    elseif ftype == "double" then
        local dbuf = ffi.new("double[1]", value)
        local bytes = ffi.cast("uint8_t*", dbuf)
        for i = 0, 7 do buf[offset + i + 1] = bytes[i] end
    elseif ftype == "bool" then
        buf[offset + 1] = value and 1 or 0
    elseif ftype == "int8" or ftype == "uint8" or ftype == "char" then
        buf[offset + 1] = bit.band(value, 0xFF)
    elseif ftype == "int16" or ftype == "uint16" then
        buf[offset + 1] = bit.band(value, 0xFF)
        buf[offset + 2] = bit.band(bit.rshift(value, 8), 0xFF)
    elseif ftype == "int32" or ftype == "uint32" then
        if value < 0 then value = 0x100000000 + value end
        buf[offset + 1] = bit.band(value, 0xFF)
        buf[offset + 2] = bit.band(bit.rshift(value, 8), 0xFF)
        buf[offset + 3] = bit.band(bit.rshift(value, 16), 0xFF)
        buf[offset + 4] = bit.band(bit.rshift(value, 24), 0xFF)
    elseif ftype == "int64" or ftype == "uint64" then
        local lo = bit.band(value, 0xFFFFFFFF)
        local hi = math.floor(value / 0x100000000)
        for i = 0, 3 do buf[offset + i + 1] = bit.band(bit.rshift(lo, i * 8), 0xFF) end
        for i = 0, 3 do buf[offset + i + 5] = bit.band(bit.rshift(hi, i * 8), 0xFF) end
    end
end

function _G.CONST(name, record_type)
    if not current_module then dsl_error("No module started") end
    if current_const then dsl_error("Constant already open: " .. current_const.name) end
    
    local rec = current_module.records[record_type]
    if not rec then dsl_error("Unknown record type for constant: " .. record_type) end
    
    current_const = {
        name = name,
        name_hash = hash_module.fnv1a_32(name),
        record_type = record_type,
        values = {},
        data_bytes = nil,
    }
end

function _G.VALUE(field_path, value)
    if not current_const then dsl_error("No constant open") end
    table.insert(current_const.values, { path = field_path, value = value })
end

function _G.END_CONST()
    if not current_const then dsl_error("No constant open") end
    
    local rec = current_module.records[current_const.record_type]
    local data = {}
    for i = 1, rec.size do data[i] = 0 end
    
    for _, v in ipairs(current_const.values) do
        local offset, size, field = resolve_field_path(rec, v.path)
        if offset and field then
            write_value_to_buffer(data, offset, v.value, field)
        end
    end
    
    current_const.data_bytes = data
    current_module.constants[current_const.name] = current_const
    table.insert(current_module.const_order, current_const.name)
    current_const = nil
end

-- ============================================================================
-- TREE FUNCTIONS
-- ============================================================================

function _G.start_tree(name)
    if not current_module then dsl_error("No module started") end
    if current_tree then dsl_error("Tree already open: " .. current_tree.name) end
    
    current_tree = {
        name = name,
        name_hash = hash_module.fnv1a_32(name),
        record_name = nil,
        record_index = 0,
        nodes = {},
        defaults_name = nil,
        defaults_hash = 0,
        defaults_index = 0xFFFF,
        node_count = 0,
        pointer_count = 0,
    }
    
    current_call_stack = {}
    order_stack = {0}
    reset_brace_stack()
    frame_stack = {}
end

function _G.use_record(name)
    if not current_tree then dsl_error("No tree open") end
    current_tree.record_name = name
    for i, rname in ipairs(current_module.record_order) do
        if rname == name then current_tree.record_index = i - 1; break end
    end
end

function _G.use_defaults(const_name)
    if not current_tree then dsl_error("No tree open") end
    if not current_module.constants[const_name] then dsl_error("Unknown constant: " .. const_name) end
    
    local cnst = current_module.constants[const_name]
    if current_tree.record_name and cnst.record_type ~= current_tree.record_name then
        dsl_error(string.format("Constant '%s' is for record '%s', but tree uses '%s'",
            const_name, cnst.record_type, current_tree.record_name))
    end
    
    current_tree.defaults_name = const_name
    current_tree.defaults_hash = hash_module.fnv1a_32(const_name)
    for i, name in ipairs(current_module.const_order) do
        if name == const_name then current_tree.defaults_index = i - 1; break end
    end
end

function _G.end_tree(name)
    if not current_tree then dsl_error("No tree open") end
    if name and current_tree.name ~= name then
        dsl_error("Tree name mismatch: expected " .. current_tree.name .. ", got " .. name)
    end
    
    check_brace_balance()
    if #frame_stack > 0 then
        dsl_error("Tree '" .. current_tree.name .. "' has " .. #frame_stack ..
                  " unclosed stack frame(s)")
    end
    
    -- ========================================================================
    -- TREE STRUCTURE VALIDATION (v5.3)
    -- ========================================================================
    -- A tree must have exactly ONE top-level node. The runtime engine only
    -- processes the first element of the nodes array. Multiple top-level
    -- nodes indicate a structural error - they should be wrapped in a
    -- container like SE_SEQUENCE, SE_FORK, or SE_STATE_MACHINE.
    -- ========================================================================
    
    local top_level_count = #current_tree.nodes
    if top_level_count == 0 then
        dsl_error("Tree '" .. current_tree.name .. "' has no top-level function.\n" ..
                  "  A tree must have exactly one root node (e.g., m_call, o_call, etc.)")
    elseif top_level_count > 1 then
        -- Collect info about all top-level nodes for a helpful error message
        local node_names = {}
        for i, node in ipairs(current_tree.nodes) do
            table.insert(node_names, string.format("  %d. %s (%s)", i, node.func_name, node.call_type))
        end
        dsl_error("Tree '" .. current_tree.name .. "' has " .. top_level_count .. 
                  " top-level functions.\n" ..
                  "  The runtime engine only executes the FIRST root node.\n" ..
                  "  Top-level nodes found:\n" .. table.concat(node_names, "\n") ..
                  "\n\n  FIX: Wrap multiple functions in a container:\n" ..
                  "    - SE_SEQUENCE: Execute in order, stop on non-CONTINUE\n" ..
                  "    - SE_FORK: Execute all in parallel\n" ..
                  "    - SE_STATE_MACHINE: State-based execution\n" ..
                  "    - SE_CHAIN_FLOW: Pipeline processing")
    end
    
    current_module.trees[current_tree.name] = current_tree
    table.insert(current_module.tree_order, current_tree.name)
    
    current_tree = nil
    current_call_stack = {}
end

-- ============================================================================
-- CALL FUNCTIONS
-- ============================================================================

local function start_call(func_name, call_type)
    if not current_tree then dsl_error("No tree open") end
    
    local node = {
        func_name = func_name,
        func_hash = hash_module.fnv1a_32(func_name),
        call_type = call_type,
        params = {},
        children = {},
        param_count = 0,
        pointer_index = nil,
    }
    
    if call_type == "pt_m_call" then
        node.pointer_index = current_tree.pointer_count
        current_tree.pointer_count = current_tree.pointer_count + 1
    end
    
    local func_list = nil
    if call_type == "o_call" or call_type == "io_call" then
        func_list = current_module.oneshot_funcs
    elseif call_type == "m_call" or call_type == "pt_m_call" then
        func_list = current_module.main_funcs
    elseif call_type == "p_call" or call_type == "p_call_composite" then
        func_list = current_module.pred_funcs
    end
    
    if func_list then
        local found = false
        for _, n in ipairs(func_list) do
            if n == func_name then found = true; break end
        end
        if not found then table.insert(func_list, func_name) end
    end
    
    if #current_call_stack > 0 then
        local parent = current_call_stack[#current_call_stack]
        node.order = order_stack[#order_stack]
        order_stack[#order_stack] = order_stack[#order_stack] + 1
        table.insert(parent.children, node)
    else
        table.insert(current_tree.nodes, node)
    end
    
    table.insert(current_call_stack, node)
    table.insert(order_stack, 0)
    current_tree.node_count = current_tree.node_count + 1
    
    return node
end

function _G.o_call(func_name) return start_call(func_name, "o_call") end
function _G.m_call(func_name) return start_call(func_name, "m_call") end
function _G.p_call(func_name) return start_call(func_name, "p_call") end
function _G.pt_m_call(func_name) return start_call(func_name, "pt_m_call") end
function _G.io_call(func_name) return start_call(func_name, "io_call") end

function _G.p_call_composite(func_name)
    in_composite_block = true
    return start_call(func_name, "p_call_composite")
end

function _G.p_call_bit(func_name) return _G.p_call_composite(func_name) end

function _G.end_call(node)
    if #current_call_stack == 0 then dsl_error("No call to end") end
    
    local top = current_call_stack[#current_call_stack]
    if top.call_type == "p_call_composite" then in_composite_block = false end
    
    table.remove(current_call_stack)
    table.remove(order_stack)
    return top
end

function _G.check_composite_block_only(func_name)
    if not in_composite_block then
        dsl_error(func_name .. "() can only be used inside a composite predicate block")
    end
end

function _G.check_bit_block_only(func_name) _G.check_composite_block_only(func_name) end

-- ============================================================================
-- PARAMETER FUNCTIONS
-- ============================================================================

local function add_param(ptype, value)
    if #current_call_stack == 0 then dsl_error("No call open for parameter") end
    
    local node = current_call_stack[#current_call_stack]
    table.insert(node.params, { 
        type = ptype, 
        value = value,
        order = order_stack[#order_stack]
    })
    node.param_count = node.param_count + 1
    order_stack[#order_stack] = order_stack[#order_stack] + 1
end

function _G.int(value) add_param("int", value) end
function _G.uint(value) add_param("uint", value) end
function _G.flt(value) add_param("float", value) end

function _G.stack_push()
    add_param("stack_push", 0)
end

function _G.stack_pop()
    add_param("stack_pop", 0)
end

function _G.stack_tos(offset)
    offset = offset or 0
    if type(offset) ~= "number" or offset < 0 then
        dsl_error("stack_tos: offset must be a non-negative number")
    end
    offset = math.floor(offset)

    if #frame_stack == 0 then
        dsl_error("stack_tos: no active stack frame (use inside se_call)")
    end
    local frame = frame_stack[#frame_stack]
    if offset >= frame.scratch_depth then
        dsl_error("stack_tos(" .. offset .. "): out of range, scratch_depth = " ..
                  frame.scratch_depth .. " (valid: 0.." .. (frame.scratch_depth - 1) .. ")")
    end

    add_param("stack_tos", offset)
end

function _G.stack_local(index)
    if type(index) ~= "number" or index < 0 then
        dsl_error("stack_local: index must be a non-negative number")
    end
    index = math.floor(index)

    if #frame_stack == 0 then
        dsl_error("stack_local: no active stack frame (use inside se_call)")
    end
    local frame = frame_stack[#frame_stack]
    local max = frame.num_params + frame.num_locals
    if index >= max then
        dsl_error("stack_local(" .. index .. "): out of range, frame has " ..
                  frame.num_params .. " params + " .. frame.num_locals ..
                  " locals = " .. max .. " slots (valid: 0.." .. (max - 1) .. ")")
    end

    add_param("stack_local", index)
end

function _G.null_param()
    add_param("null_param", 0)
end

function _G.str(value)
    if not current_module.string_index[value] then
        current_module.string_index[value] = #current_module.string_table
        table.insert(current_module.string_table, value)
    end
    add_param("str_idx", value)
end

function _G.str_ptr(value)
    if not current_module.string_index[value] then
        current_module.string_index[value] = #current_module.string_table
        table.insert(current_module.string_table, value)
    end
    add_param("str_ptr", value)
end

function _G.str_hash(value)
    local h = hash_module.fnv1a_32(value)
    add_param("str_hash", { hash = h, str = value })
end

function _G.field_ref(name) add_param("field_ref", name) end
function _G.nested_field_ref(path) add_param("nested_field_ref", path) end
function _G.const_ref(name) add_param("const_ref", name) end

-- ============================================================================
-- STRUCTURE FUNCTIONS
-- ============================================================================

function _G.list_start(name)
    local marker = push_brace(name, "list")
    add_param("list_start", marker.name)
    return marker
end

function _G.list_end(marker)
    pop_brace(marker, "list")
    add_param("list_end", marker and marker.name or "")
end

function _G.dict_start(name)
    local marker = push_brace(name, "dict")
    add_param("dict_start", marker.name)
    return marker
end

function _G.dict_end(marker)
    pop_brace(marker, "dict")
    add_param("dict_end", marker and marker.name or "")
end

function _G.dict_key(key_str)
    if type(key_str) ~= "string" then dsl_error("dict_key requires a string argument") end
    local marker = push_brace(key_str, "dict_key")
    marker.key_hash = hash_module.fnv1a_32(key_str)
    add_param("dict_key", key_str)
    return marker
end

function _G.key(key_str) return _G.dict_key(key_str) end

function _G.key_hash(hash_value)
    if type(hash_value) ~= "number" then dsl_error("key_hash requires a number argument") end
    local marker = push_brace(tostring(hash_value), "dict_key")
    marker.key_hash = hash_value
    add_param("dict_key_hash", hash_value)
    return marker
end

function _G.end_dict_key(marker)
    pop_brace(marker, "dict_key")
    add_param("end_dict_key", marker and marker.name or "")
end

function _G.key_end(marker) return _G.end_dict_key(marker) end

function _G.array_start(name)
    local marker = push_brace(name, "array")
    add_param("array_start", marker.name)
    return marker
end

function _G.array_end(marker)
    pop_brace(marker, "array")
    add_param("array_end", marker and marker.name or "")
end

function _G.tuple_start(name)
    local marker = push_brace(name, "tuple")
    add_param("tuple_start", marker.name)
    return marker
end

function _G.tuple_end(marker)
    pop_brace(marker, "tuple")
    add_param("tuple_end", marker and marker.name or "")
end

-- ============================================================================
-- RESULT CODES
-- ============================================================================
-- APPLICATION RESULT CODES
-- APPLICATION RESULT CODES (0-5)
_G.SE_CONTINUE           = 0
_G.SE_HALT               = 1
_G.SE_TERMINATE          = 2
_G.SE_RESET              = 3
_G.SE_DISABLE            = 4
_G.SE_SKIP_CONTINUE      = 5

-- FUNCTION RESULT CODES (6-11)
_G.SE_FUNCTION_CONTINUE  = 6
_G.SE_FUNCTION_HALT      = 7
_G.SE_FUNCTION_TERMINATE = 8
_G.SE_FUNCTION_RESET     = 9
_G.SE_FUNCTION_DISABLE   = 10
_G.SE_FUNCTION_SKIP_CONTINUE = 11

-- PIPELINE RESULT CODES (12-17)
_G.SE_PIPELINE_CONTINUE  = 12
_G.SE_PIPELINE_HALT      = 13
_G.SE_PIPELINE_TERMINATE = 14
_G.SE_PIPELINE_RESET     = 15
_G.SE_PIPELINE_DISABLE   = 16
_G.SE_PIPELINE_SKIP_CONTINUE = 17

-- ============================================================================
-- LOAD GENERATORS AND DEBUG MODULES
-- ============================================================================

local script_dir = debug.getinfo(1, "S").source:match("@(.*/)")  or "./"

local generators = dofile(script_dir .. "s_expr_generators.lua")
generators._init(hash_module, types_module, builtins_module)

local debug_mod = dofile(script_dir .. "s_expr_debug.lua")
debug_mod._init(hash_module, types_module)

-- Attach to_debug_dump to BinaryModuleGenerator
local orig_new = generators.BinaryModuleGenerator.new
generators.BinaryModuleGenerator.new = function(module_data)
    local self = orig_new(module_data)
    self.to_debug_dump = function(self, base_name)
        return debug_mod.to_debug_dump(self, base_name)
    end
    return self
end

-- Export generators
M.ModuleGenerator = generators.ModuleGenerator
M.BinaryModuleGenerator = generators.BinaryModuleGenerator
M.BinaryEmitter = generators.BinaryEmitter
M.CSourceModuleGenerator = generators.CSourceModuleGenerator
M.generate_debug_header = debug_mod.generate_debug_header
M.write_debug_header = debug_mod.write_debug_header

return M