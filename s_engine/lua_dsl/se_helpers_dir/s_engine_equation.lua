-- ============================================================================
-- s_expr_compiler.lua
-- Expression Compiler for S-Expression Engine Quad Operations
-- Version 1.1
--
-- Compiles C-like expressions into sequences of quad and p_quad operations.
-- Runs at DSL compile time (in LuaJIT) to generate binary tree nodes.
--
-- Features:
--   - Arithmetic: +, -, *, /, %
--   - Bitwise: &, |, ^, ~, <<, >>
--   - Comparison: ==, !=, <, <=, >, >=
--   - Logical: &&, ||, !
--   - Unary: -, !, ~
--   - Parentheses for grouping
--   - Field references: @field_name (in expressions AND as destinations)
--   - Math functions: sqrt, sin, cos, abs, min, max, pow, etc.
--   - Compound assignment: +=, -=, *=, /=, etc.
--   - Multi-statement: semicolon separated
--   - Constant folding at compile time
--   - Type annotations for float/int dispatch
--   - Debug output of generated operations
--
-- VERSION 1.1 CHANGES:
--   - @field_name now works as assignment destination in quad_expr/quad_multi
--   - ref_for() handles @field prefix by delegating to field_val()
--   - quad_expr destination regex accepts @field.path syntax
--
-- Usage:
--   local v = frame_vars(
--       {"x:float", "y:float", "result:float", "count:int"},
--       {"t0:float", "t1:float", "t2:float"}
--   )
--   quad_expr("result = (x + 5.0) * y - 2.0", v, {"t0", "t1"})()
--   quad_expr("@blackboard_field = x + 1.0", v, {"t0"})()
--   quad_pred("x > 5.0 && y <= 10.0", v, {"t0", "t1"})()
--
-- Loaded by s_engine_helpers.lua via dofile()
-- ============================================================================

local bit = require("bit")

-- ============================================================================
-- TOKEN TYPES
-- ============================================================================

local TK = {
    NUM    = "num",
    IDENT  = "ident",
    FIELD  = "field",
    OP     = "op",
    LPAREN = "(",
    RPAREN = ")",
    COMMA  = ",",
    EOF    = "eof",
}

-- ============================================================================
-- OPERATOR PRECEDENCE AND INFO
-- ============================================================================

local op_info = {
    ["||"]  = {prec = 1,  assoc = "left", logic = true},
    ["&&"]  = {prec = 2,  assoc = "left", logic = true},
    ["|"]   = {prec = 3,  assoc = "left", bitwise = true},
    ["^"]   = {prec = 4,  assoc = "left", bitwise = true},
    ["&"]   = {prec = 5,  assoc = "left", bitwise = true},
    ["=="]  = {prec = 6,  assoc = "left", cmp = true},
    ["!="]  = {prec = 6,  assoc = "left", cmp = true},
    ["<"]   = {prec = 7,  assoc = "left", cmp = true},
    ["<="]  = {prec = 7,  assoc = "left", cmp = true},
    [">"]   = {prec = 7,  assoc = "left", cmp = true},
    [">="]  = {prec = 7,  assoc = "left", cmp = true},
    ["<<"]  = {prec = 8,  assoc = "left", bitwise = true},
    [">>"]  = {prec = 8,  assoc = "left", bitwise = true},
    ["+"]   = {prec = 9,  assoc = "left", arith = true},
    ["-"]   = {prec = 9,  assoc = "left", arith = true},
    ["*"]   = {prec = 10, assoc = "left", arith = true},
    ["/"]   = {prec = 10, assoc = "left", arith = true},
    ["%"]   = {prec = 10, assoc = "left", arith = true},
}

-- ============================================================================
-- BUILTIN MATH FUNCTIONS
-- ============================================================================

local builtin_funcs = {
    -- Unary float functions
    sqrt   = {arity = 1, fn_f = "quad_sqrt"},
    abs    = {arity = 1, fn_f = "quad_fabs",  fn_i = "quad_iabs"},
    sin    = {arity = 1, fn_f = "quad_sin"},
    cos    = {arity = 1, fn_f = "quad_cos"},
    tan    = {arity = 1, fn_f = "quad_tan"},
    asin   = {arity = 1, fn_f = "quad_asin"},
    acos   = {arity = 1, fn_f = "quad_acos"},
    atan   = {arity = 1, fn_f = "quad_atan"},
    exp    = {arity = 1, fn_f = "quad_exp"},
    log    = {arity = 1, fn_f = "quad_log"},
    log10  = {arity = 1, fn_f = "quad_log10"},
    log2   = {arity = 1, fn_f = "quad_log2"},
    sinh   = {arity = 1, fn_f = "quad_sinh"},
    cosh   = {arity = 1, fn_f = "quad_cosh"},
    tanh   = {arity = 1, fn_f = "quad_tanh"},
    neg    = {arity = 1, fn_f = "quad_fneg",  fn_i = "quad_ineg"},

    -- Binary functions
    min    = {arity = 2, fn_f = "quad_fmin",  fn_i = "quad_imin"},
    max    = {arity = 2, fn_f = "quad_fmax",  fn_i = "quad_imax"},
    pow    = {arity = 2, fn_f = "quad_pow"},
    atan2  = {arity = 2, fn_f = "quad_atan2"},
}

-- ============================================================================
-- QUAD FUNCTION LOOKUP TABLES
-- ============================================================================

-- Integer arithmetic operators
local int_arith_ops = {
    ["+"] = "quad_iadd", ["-"] = "quad_isub",
    ["*"] = "quad_imul", ["/"] = "quad_idiv", ["%"] = "quad_imod",
}

-- Float arithmetic operators
local float_arith_ops = {
    ["+"] = "quad_fadd", ["-"] = "quad_fsub",
    ["*"] = "quad_fmul", ["/"] = "quad_fdiv", ["%"] = "quad_fmod",
}

-- Bitwise operators (integer only)
local bitwise_ops = {
    ["&"]  = "quad_and",  ["|"]  = "quad_or",  ["^"] = "quad_xor",
    ["<<"] = "quad_shl",  [">>"] = "quad_shr",
}

-- Integer comparison operators (oneshot quad)
local int_cmp_ops = {
    ["=="] = "quad_ieq", ["!="] = "quad_ine",
    ["<"]  = "quad_ilt", ["<="] = "quad_ile",
    [">"]  = "quad_igt", [">="] = "quad_ige",
}

-- Float comparison operators (oneshot quad)
local float_cmp_ops = {
    ["=="] = "quad_feq", ["!="] = "quad_fne",
    ["<"]  = "quad_flt", ["<="] = "quad_fle",
    [">"]  = "quad_fgt", [">="] = "quad_fge",
}

-- Logical operators (oneshot quad)
local logic_ops = {
    ["&&"] = "quad_log_and", ["||"] = "quad_log_or",
}

-- Integer comparison operators (predicate p_quad)
local int_p_cmp_ops = {
    ["=="] = "p_icmp_eq", ["!="] = "p_icmp_ne",
    ["<"]  = "p_icmp_lt", ["<="] = "p_icmp_le",
    [">"]  = "p_icmp_gt", [">="] = "p_icmp_ge",
}

-- Float comparison operators (predicate p_quad)
local float_p_cmp_ops = {
    ["=="] = "p_fcmp_eq", ["!="] = "p_fcmp_ne",
    ["<"]  = "p_fcmp_lt", ["<="] = "p_fcmp_le",
    [">"]  = "p_fcmp_gt", [">="] = "p_fcmp_ge",
}

-- Logical operators (predicate p_quad)
local p_logic_ops = {
    ["&&"] = "p_log_and", ["||"] = "p_log_or",
}

-- Integer comparison + accumulate (predicate p_quad)
local int_p_cmp_acc_ops = {
    ["=="] = "p_icmp_eq_acc", ["!="] = "p_icmp_ne_acc",
    ["<"]  = "p_icmp_lt_acc", ["<="] = "p_icmp_le_acc",
    [">"]  = "p_icmp_gt_acc", [">="] = "p_icmp_ge_acc",
}

-- Float comparison + accumulate (predicate p_quad)
local float_p_cmp_acc_ops = {
    ["=="] = "p_fcmp_eq_acc", ["!="] = "p_fcmp_ne_acc",
    ["<"]  = "p_fcmp_lt_acc", ["<="] = "p_fcmp_le_acc",
    [">"]  = "p_fcmp_gt_acc", [">="] = "p_fcmp_ge_acc",
}

-- ============================================================================
-- AST NODE CONSTRUCTORS
-- ============================================================================

local function num_node(val, is_float)
    return {tag = "num", value = val, is_float = is_float}
end

local function var_node(name)
    return {tag = "var", name = name}
end

local function field_node(name)
    return {tag = "field", name = name}
end

local function binop_node(op, left, right)
    return {tag = "binop", op = op, left = left, right = right}
end

local function unop_node(op, operand)
    return {tag = "unop", op = op, operand = operand}
end

local function call_node(name, args)
    return {tag = "call", name = name, args = args}
end

-- ============================================================================
-- TOKENIZER
-- ============================================================================

local function tokenize(str)
    local tokens = {}
    local pos = 1
    local len = #str

    while pos <= len do
        local ch = str:sub(pos, pos)

        -- Skip whitespace
        if ch:match("%s") then
            pos = pos + 1

        -- Field reference: @identifier
        elseif ch == "@" then
            pos = pos + 1
            local start = pos
            while pos <= len and str:sub(pos, pos):match("[%w_.]") do
                pos = pos + 1
            end
            if pos == start then
                dsl_error("expr: expected field name after '@' at position " .. (pos - 1))
            end
            table.insert(tokens, {type = TK.FIELD, value = str:sub(start, pos - 1)})

        -- Numbers (integer and float, including negative literals after operators)
        elseif ch:match("[%d.]") or
               (ch == "-" and pos + 1 <= len and str:sub(pos + 1, pos + 1):match("[%d.]") and
                (#tokens == 0 or tokens[#tokens].type == TK.OP or
                 tokens[#tokens].type == TK.LPAREN or tokens[#tokens].type == TK.COMMA)) then
            local start = pos
            if ch == "-" then pos = pos + 1 end
            local has_dot = false
            while pos <= len do
                local c = str:sub(pos, pos)
                if c == "." and not has_dot then
                    has_dot = true
                    pos = pos + 1
                elseif c:match("[%d]") then
                    pos = pos + 1
                else
                    break
                end
            end
            -- Check for float suffix 'f'
            if pos <= len and str:sub(pos, pos):lower() == "f" then
                has_dot = true
                pos = pos + 1
            end
            local numstr = str:sub(start, pos - 1):gsub("[fF]$", "")
            local val = tonumber(numstr)
            if not val then
                dsl_error("expr: invalid number '" .. numstr .. "' at position " .. start)
            end
            table.insert(tokens, {type = TK.NUM, value = val, is_float = has_dot})

        -- Identifiers and keywords
        elseif ch:match("[%a_]") then
            local start = pos
            while pos <= len and str:sub(pos, pos):match("[%w_]") do
                pos = pos + 1
            end
            table.insert(tokens, {type = TK.IDENT, value = str:sub(start, pos - 1)})

        -- Two-character operators
        elseif pos + 1 <= len then
            local two = str:sub(pos, pos + 1)
            if two == "==" or two == "!=" or two == "<=" or two == ">=" or
               two == "&&" or two == "||" or two == "<<" or two == ">>" then
                table.insert(tokens, {type = TK.OP, value = two})
                pos = pos + 2
            elseif ch:match("[+%-%*/%%&|^~!<>=]") then
                table.insert(tokens, {type = TK.OP, value = ch})
                pos = pos + 1
            elseif ch == "(" then
                table.insert(tokens, {type = TK.LPAREN}); pos = pos + 1
            elseif ch == ")" then
                table.insert(tokens, {type = TK.RPAREN}); pos = pos + 1
            elseif ch == "," then
                table.insert(tokens, {type = TK.COMMA}); pos = pos + 1
            else
                dsl_error("expr: unexpected character '" .. ch .. "' at position " .. pos)
            end
        else
            -- Single character at end of string
            if ch:match("[+%-%*/%%&|^~!<>=]") then
                table.insert(tokens, {type = TK.OP, value = ch})
                pos = pos + 1
            elseif ch == "(" then
                table.insert(tokens, {type = TK.LPAREN}); pos = pos + 1
            elseif ch == ")" then
                table.insert(tokens, {type = TK.RPAREN}); pos = pos + 1
            elseif ch == "," then
                table.insert(tokens, {type = TK.COMMA}); pos = pos + 1
            else
                dsl_error("expr: unexpected character '" .. ch .. "' at position " .. pos)
            end
        end
    end

    table.insert(tokens, {type = TK.EOF})
    return tokens
end

-- ============================================================================
-- PARSER (Pratt / precedence climbing)
-- ============================================================================

local function make_parser(tokens)
    local p = {pos = 1}

    function p:peek()
        return tokens[self.pos]
    end

    function p:advance()
        local t = tokens[self.pos]
        self.pos = self.pos + 1
        return t
    end

    function p:expect(ttype, val)
        local t = self:advance()
        if t.type ~= ttype or (val and t.value ~= val) then
            dsl_error("expr: expected " .. ttype ..
                      (val and (" '" .. val .. "'") or "") ..
                      ", got " .. t.type ..
                      (t.value and (" '" .. tostring(t.value) .. "'") or ""))
        end
        return t
    end

    return p
end

-- Forward declaration
local parse_expr

local function parse_primary(p)
    local tok = p:peek()

    if tok.type == TK.NUM then
        p:advance()
        return num_node(tok.value, tok.is_float)

    elseif tok.type == TK.FIELD then
        p:advance()
        return field_node(tok.value)

    elseif tok.type == TK.IDENT then
        p:advance()
        -- Check if function call
        if p:peek().type == TK.LPAREN then
            p:advance()  -- skip (
            local args = {}
            if p:peek().type ~= TK.RPAREN then
                table.insert(args, parse_expr(p, 0))
                while p:peek().type == TK.COMMA do
                    p:advance()  -- skip ,
                    table.insert(args, parse_expr(p, 0))
                end
            end
            p:expect(TK.RPAREN)

            local info = builtin_funcs[tok.value]
            if not info then
                dsl_error("expr: unknown function '" .. tok.value .. "'")
            end
            if #args ~= info.arity then
                dsl_error("expr: " .. tok.value .. "() expects " ..
                          info.arity .. " argument(s), got " .. #args)
            end
            return call_node(tok.value, args)
        else
            return var_node(tok.value)
        end

    elseif tok.type == TK.LPAREN then
        p:advance()
        local expr = parse_expr(p, 0)
        p:expect(TK.RPAREN)
        return expr

    -- Unary operators
    elseif tok.type == TK.OP and tok.value == "-" then
        p:advance()
        local operand = parse_expr(p, 11)  -- high precedence for unary
        return unop_node("-", operand)

    elseif tok.type == TK.OP and tok.value == "!" then
        p:advance()
        local operand = parse_expr(p, 11)
        return unop_node("!", operand)

    elseif tok.type == TK.OP and tok.value == "~" then
        p:advance()
        local operand = parse_expr(p, 11)
        return unop_node("~", operand)

    else
        dsl_error("expr: unexpected token " .. tok.type ..
                  (tok.value and (" '" .. tostring(tok.value) .. "'") or ""))
    end
end

parse_expr = function(p, min_prec)
    min_prec = min_prec or 0

    local left = parse_primary(p)

    while true do
        local tok = p:peek()
        if tok.type ~= TK.OP then break end

        local info = op_info[tok.value]
        if not info then break end
        if info.prec < min_prec then break end

        p:advance()
        local next_prec = info.assoc == "left" and (info.prec + 1) or info.prec
        local right = parse_expr(p, next_prec)
        left = binop_node(tok.value, left, right)
    end

    return left
end

-- ============================================================================
-- TYPE INFERENCE
-- ============================================================================

local function infer_float(ast, vars)
    if ast.tag == "num" then
        return ast.is_float
    elseif ast.tag == "var" then
        local key = ast.name .. "_is_float"
        return vars[key] or false
    elseif ast.tag == "field" then
        -- Fields default to int unless annotated
        -- Could extend with field type lookup
        return false
    elseif ast.tag == "unop" then
        return infer_float(ast.operand, vars)
    elseif ast.tag == "binop" then
        local info = op_info[ast.op]
        -- Bitwise and logical always produce int
        if info and (info.bitwise or info.logic) then
            return false
        end
        -- Comparison always produces int (0 or 1)
        if info and info.cmp then
            return false
        end
        return infer_float(ast.left, vars) or infer_float(ast.right, vars)
    elseif ast.tag == "call" then
        local info = builtin_funcs[ast.name]
        if info then
            -- If function has no int variant, result is always float
            if not info.fn_i then return true end
            -- Otherwise infer from arguments
            for _, arg in ipairs(ast.args) do
                if infer_float(arg, vars) then return true end
            end
        end
        return false
    end
    return false
end

-- ============================================================================
-- CONSTANT FOLDING
-- ============================================================================

local function fold_constants(ast)
    if ast.tag == "num" or ast.tag == "var" or ast.tag == "field" then
        return ast
    end

    if ast.tag == "unop" then
        local operand = fold_constants(ast.operand)
        if operand.tag == "num" then
            if ast.op == "-" then
                return num_node(-operand.value, operand.is_float)
            elseif ast.op == "!" then
                return num_node(operand.value == 0 and 1 or 0, false)
            elseif ast.op == "~" then
                return num_node(bit.bnot(math.floor(operand.value)), false)
            end
        end
        return unop_node(ast.op, operand)
    end

    if ast.tag == "binop" then
        local left = fold_constants(ast.left)
        local right = fold_constants(ast.right)

        if left.tag == "num" and right.tag == "num" then
            local a, b = left.value, right.value
            local is_f = left.is_float or right.is_float
            local result

            if     ast.op == "+"  then result = a + b
            elseif ast.op == "-"  then result = a - b
            elseif ast.op == "*"  then result = a * b
            elseif ast.op == "/"  and b ~= 0 then result = a / b
            elseif ast.op == "%"  and b ~= 0 then result = a % b
            elseif ast.op == "&"  then result = bit.band(math.floor(a), math.floor(b)); is_f = false
            elseif ast.op == "|"  then result = bit.bor(math.floor(a), math.floor(b));  is_f = false
            elseif ast.op == "^"  then result = bit.bxor(math.floor(a), math.floor(b)); is_f = false
            elseif ast.op == "<<" then result = bit.lshift(math.floor(a), math.floor(b)); is_f = false
            elseif ast.op == ">>" then result = bit.rshift(math.floor(a), math.floor(b)); is_f = false
            elseif ast.op == "==" then result = (a == b) and 1 or 0; is_f = false
            elseif ast.op == "!=" then result = (a ~= b) and 1 or 0; is_f = false
            elseif ast.op == "<"  then result = (a < b) and 1 or 0;  is_f = false
            elseif ast.op == "<=" then result = (a <= b) and 1 or 0; is_f = false
            elseif ast.op == ">"  then result = (a > b) and 1 or 0;  is_f = false
            elseif ast.op == ">=" then result = (a >= b) and 1 or 0; is_f = false
            elseif ast.op == "&&" then result = (a ~= 0 and b ~= 0) and 1 or 0; is_f = false
            elseif ast.op == "||" then result = (a ~= 0 or b ~= 0) and 1 or 0;  is_f = false
            end

            if result then
                if not is_f then result = math.floor(result) end
                return num_node(result, is_f)
            end
        end

        return binop_node(ast.op, left, right)
    end

    if ast.tag == "call" then
        local folded_args = {}
        local all_const = true
        for _, arg in ipairs(ast.args) do
            local fa = fold_constants(arg)
            table.insert(folded_args, fa)
            if fa.tag ~= "num" then all_const = false end
        end

        -- Fold single-arg math functions on constants
        if all_const and #folded_args == 1 then
            local v = folded_args[1].value
            local result
            if     ast.name == "sqrt"  and v >= 0 then result = math.sqrt(v)
            elseif ast.name == "abs"   then result = math.abs(v)
            elseif ast.name == "sin"   then result = math.sin(v)
            elseif ast.name == "cos"   then result = math.cos(v)
            elseif ast.name == "tan"   then result = math.tan(v)
            elseif ast.name == "asin"  then result = math.asin(v)
            elseif ast.name == "acos"  then result = math.acos(v)
            elseif ast.name == "atan"  then result = math.atan(v)
            elseif ast.name == "exp"   then result = math.exp(v)
            elseif ast.name == "log"   then result = math.log(v)
            elseif ast.name == "log10" then result = math.log10(v)
            elseif ast.name == "log2"  then result = math.log(v) / math.log(2)
            elseif ast.name == "sinh"  then result = math.sinh(v)
            elseif ast.name == "cosh"  then result = math.cosh(v)
            elseif ast.name == "tanh"  then result = math.tanh(v)
            elseif ast.name == "neg"   then result = -v
            end
            if result then
                return num_node(result, true)
            end
        end

        -- Fold two-arg math functions on constants
        if all_const and #folded_args == 2 then
            local a, b = folded_args[1].value, folded_args[2].value
            local result
            if     ast.name == "min"   then result = math.min(a, b)
            elseif ast.name == "max"   then result = math.max(a, b)
            elseif ast.name == "pow"   then result = math.pow(a, b)
            elseif ast.name == "atan2" then result = math.atan2(a, b)
            end
            if result then
                return num_node(result, true)
            end
        end

        return call_node(ast.name, folded_args)
    end

    return ast
end

-- ============================================================================
-- AST TO STRING (for debug output)
-- ============================================================================

local function ast_to_string(ast)
    if ast.tag == "num" then
        if ast.is_float then
            return string.format("%.4g", ast.value)
        else
            return tostring(math.floor(ast.value))
        end
    elseif ast.tag == "var" then
        return ast.name
    elseif ast.tag == "field" then
        return "@" .. ast.name
    elseif ast.tag == "unop" then
        return "(" .. ast.op .. ast_to_string(ast.operand) .. ")"
    elseif ast.tag == "binop" then
        return "(" .. ast_to_string(ast.left) .. " " .. ast.op .. " " .. ast_to_string(ast.right) .. ")"
    elseif ast.tag == "call" then
        local arg_strs = {}
        for _, arg in ipairs(ast.args) do
            table.insert(arg_strs, ast_to_string(arg))
        end
        return ast.name .. "(" .. table.concat(arg_strs, ", ") .. ")"
    end
    return "<?>"
end

-- ============================================================================
-- CODE GENERATOR
-- ============================================================================

local function compile_ast(ast, vars, scratch_names, is_pred)
    local scratch_idx = 0
    local scratch_high = 0
    local ops = {}

    local function alloc_scratch()
        scratch_idx = scratch_idx + 1
        if scratch_idx > scratch_high then
            scratch_high = scratch_idx
        end
        local name = scratch_names[scratch_idx]
        if not name then
            dsl_error("expr: ran out of scratch variables (have " ..
                      #scratch_names .. ", need " .. scratch_idx .. ")")
        end
        return name
    end

    local function free_scratch()
        if scratch_idx > 0 then
            scratch_idx = scratch_idx - 1
        end
    end

    -- ========================================================================
    -- ref_for: resolve a name to a parameter-emitting closure
    --
    -- Handles three cases:
    --   1. "@field_name"  -> field_val(field_name)  (blackboard field)
    --   2. "var_name"     -> vars[var_name]          (frame local/scratch)
    --   3. unknown        -> error
    -- ========================================================================
    local function ref_for(name)
        -- Handle @field references (blackboard fields)
        if name:sub(1, 1) == "@" then
            local field_name = name:sub(2)
            return field_val(field_name)
        end
        -- Check frame_vars table
        if vars[name] then
            return vars[name]
        end
        dsl_error("expr: unknown variable '" .. name .. "'")
    end

    local function leaf_ref(node)
        if node.tag == "num" then
            if node.is_float then
                return float_val(node.value)
            else
                if node.value < 0 then
                    return int_val(node.value)
                else
                    return uint_val(node.value)
                end
            end
        elseif node.tag == "var" then
            return ref_for(node.name)
        elseif node.tag == "field" then
            return field_val(node.name)
        end
        return nil
    end

    local function resolve_binop(op, is_f, use_pred)
        local info = op_info[op]
        if not info then return nil end

        if info.logic then
            if use_pred then
                return p_logic_ops[op]
            else
                return logic_ops[op]
            end
        elseif info.cmp then
            if use_pred then
                return is_f and float_p_cmp_ops[op] or int_p_cmp_ops[op]
            else
                return is_f and float_cmp_ops[op] or int_cmp_ops[op]
            end
        elseif info.bitwise then
            return bitwise_ops[op]
        elseif info.arith then
            return is_f and float_arith_ops[op] or int_arith_ops[op]
        end
        return nil
    end

    -- Main recursive emit function
    -- Returns the ref function for where the result is stored
    local function emit(node, dest_name)
        -- Leaf nodes: if no destination specified, return ref directly.
        -- If destination IS specified (top-level assignment like "b = @field"),
        -- we must emit a mov to copy the value.
        local lr = leaf_ref(node)
        if lr then
            if dest_name then
                local dest_ref = ref_for(dest_name)
                table.insert(ops, {fn = "quad_mov", args = {lr, dest_ref}})
                return dest_ref
            end
            return lr
        end

        if node.tag == "unop" then
            local operand_ref = emit(node.operand)
            local scratch = dest_name or alloc_scratch()
            local dest_ref = ref_for(scratch)

            if node.op == "-" then
                local is_f = infer_float(node.operand, vars)
                local fn = is_f and "quad_fneg" or "quad_ineg"
                table.insert(ops, {fn = fn, args = {operand_ref, dest_ref}})
            elseif node.op == "!" then
                table.insert(ops, {fn = "quad_log_not", args = {operand_ref, dest_ref}})
            elseif node.op == "~" then
                table.insert(ops, {fn = "quad_not", args = {operand_ref, dest_ref}})
            else
                dsl_error("expr: unknown unary operator '" .. node.op .. "'")
            end

            if not dest_name then
                -- Result is in scratch, don't free it yet
                -- (caller may need it; freed when parent uses it)
            end
            return dest_ref

        elseif node.tag == "binop" then
            local is_f = infer_float(node, vars)

            -- Determine if we need scratch for intermediates
            local left_is_leaf = (leaf_ref(node.left) ~= nil)
            local right_is_leaf = (leaf_ref(node.right) ~= nil)

            local left_ref, right_ref

            if left_is_leaf and right_is_leaf then
                -- Both leaves: no scratch needed for operands
                left_ref = leaf_ref(node.left)
                right_ref = leaf_ref(node.right)
            elseif left_is_leaf then
                -- Only right needs computation
                right_ref = emit(node.right)
                left_ref = leaf_ref(node.left)
            elseif right_is_leaf then
                -- Only left needs computation
                left_ref = emit(node.left)
                right_ref = leaf_ref(node.right)
            else
                -- Both sides need computation
                left_ref = emit(node.left)
                right_ref = emit(node.right)
            end

            local scratch = dest_name or alloc_scratch()
            local dest_ref = ref_for(scratch)

            local fn = resolve_binop(node.op, is_f, is_pred)
            if not fn then
                dsl_error("expr: no quad op for '" .. node.op .. "'" ..
                          (is_f and " (float)" or " (int)"))
            end

            table.insert(ops, {fn = fn, args = {left_ref, right_ref, dest_ref}})

            -- Free scratch used by children that aren't the dest
            if not left_is_leaf and not dest_name then free_scratch() end
            if not right_is_leaf and not dest_name then free_scratch() end

            return dest_ref

        elseif node.tag == "call" then
            local info = builtin_funcs[node.name]
            local is_f = infer_float(node, vars)

            local arg_refs = {}
            for _, arg in ipairs(node.args) do
                table.insert(arg_refs, emit(arg))
            end

            local scratch = dest_name or alloc_scratch()
            local dest_ref = ref_for(scratch)

            local fn = is_f and info.fn_f or (info.fn_i or info.fn_f)
            if not fn then
                dsl_error("expr: no quad function for " .. node.name ..
                          (is_f and " (float)" or " (int)"))
            end

            -- Build arg list: args..., dest
            local call_args = {}
            for _, r in ipairs(arg_refs) do
                table.insert(call_args, r)
            end
            table.insert(call_args, dest_ref)

            table.insert(ops, {fn = fn, args = call_args})
            return dest_ref
        end

        dsl_error("expr: cannot compile node type '" .. (node.tag or "nil") .. "'")
    end

    return emit, ops, function() return scratch_high end
end

-- ============================================================================
-- COMPOUND ASSIGNMENT DESUGARING
-- ============================================================================

local compound_patterns = {
    {pat = "(%w+)%s*%+=%s*(.+)",  op = "+"},
    {pat = "(%w+)%s*%-=%s*(.+)",  op = "-"},
    {pat = "(%w+)%s*%*=%s*(.+)",  op = "*"},
    {pat = "(%w+)%s*/=%s*(.+)",   op = "/"},
    {pat = "(%w+)%s*%%=%s*(.+)",  op = "%%"},
    {pat = "(%w+)%s*&=%s*(.+)",   op = "&"},
    {pat = "(%w+)%s*|=%s*(.+)",   op = "|"},
    {pat = "(%w+)%s*%^=%s*(.+)",  op = "^"},
    {pat = "(%w+)%s*<<=%s*(.+)",  op = "<<"},
    {pat = "(%w+)%s*>>=%s*(.+)",  op = ">>"},
}

-- Compound assignment patterns that support @field destinations
local compound_patterns_field = {
    {pat = "(@[%w_.]+)%s*%+=%s*(.+)",  op = "+"},
    {pat = "(@[%w_.]+)%s*%-=%s*(.+)",  op = "-"},
    {pat = "(@[%w_.]+)%s*%*=%s*(.+)",  op = "*"},
    {pat = "(@[%w_.]+)%s*/=%s*(.+)",   op = "/"},
    {pat = "(@[%w_.]+)%s*%%=%s*(.+)",  op = "%%"},
    {pat = "(@[%w_.]+)%s*&=%s*(.+)",   op = "&"},
    {pat = "(@[%w_.]+)%s*|=%s*(.+)",   op = "|"},
    {pat = "(@[%w_.]+)%s*%^=%s*(.+)",  op = "^"},
    {pat = "(@[%w_.]+)%s*<<=%s*(.+)",  op = "<<"},
    {pat = "(@[%w_.]+)%s*>>=%s*(.+)",  op = ">>"},
}

local function desugar_compound(expr_str)
    -- Try @field compound patterns first
    for _, cp in ipairs(compound_patterns_field) do
        local dest, body = expr_str:match("^%s*" .. cp.pat .. "%s*$")
        if dest then
            return dest .. " = " .. dest .. " " .. cp.op .. " (" .. body .. ")"
        end
    end
    -- Then try regular variable compound patterns
    for _, cp in ipairs(compound_patterns) do
        local dest, body = expr_str:match("^%s*" .. cp.pat .. "%s*$")
        if dest then
            return dest .. " = " .. dest .. " " .. cp.op .. " (" .. body .. ")"
        end
    end
    return expr_str
end

-- ============================================================================
-- PUBLIC API: frame_vars
-- ============================================================================

function _G.frame_vars(locals, scratch)
    locals = locals or {}
    scratch = scratch or {}

    local vars = {}

    for i, decl in ipairs(locals) do
        local name, type_ann = decl:match("^([%w_]+):?(%w*)$")
        if not name then
            dsl_error("frame_vars: invalid declaration '" .. decl .. "'")
        end
        if vars[name] then
            dsl_error("frame_vars: duplicate name '" .. name .. "'")
        end

        local idx = i - 1
        vars[name] = function() stack_local(idx) end
        vars[name .. "_is_float"] = (type_ann == "float")
        vars[name .. "_type"] = (type_ann ~= "") and type_ann or "int"
    end

    for i, decl in ipairs(scratch) do
        local name, type_ann = decl:match("^([%w_]+):?(%w*)$")
        if not name then
            dsl_error("frame_vars: invalid declaration '" .. decl .. "'")
        end
        if vars[name] then
            dsl_error("frame_vars: duplicate name '" .. name .. "'")
        end

        local offset = i - 1
        vars[name] = function() stack_tos(offset) end
        vars[name .. "_is_float"] = (type_ann == "float")
        vars[name .. "_type"] = (type_ann ~= "") and type_ann or "int"
    end

    -- Store metadata
    vars._locals = locals
    vars._scratch = scratch
    vars._local_count = #locals
    vars._scratch_count = #scratch

    return vars
end

-- ============================================================================
-- PUBLIC API: quad_expr
-- Compile an arithmetic assignment expression into quad operations.
-- Returns a closure that emits DSL nodes when called.
--
-- expr_str: "dest = expression" or compound "dest += expression"
--           dest can be a variable name or @field_name
-- v: frame_vars table
-- scratch: list of scratch variable names (from v) for temporaries
-- ============================================================================

function _G.quad_expr(expr_str, v, scratch)
    scratch = scratch or {}

    -- Desugar compound assignment
    expr_str = desugar_compound(expr_str)

    -- Parse "dest = expr" - dest can be @field.path or variable name
    local dest_name, body = expr_str:match("^%s*(@?[%w_.]+)%s*=%s*(.+)%s*$")
    if not dest_name then
        dsl_error("quad_expr: expected 'dest = expression', got: " .. expr_str)
    end

    local tokens = tokenize(body)
    local parser = make_parser(tokens)
    local ast = parse_expr(parser, 0)

    -- Verify we consumed all tokens
    if parser:peek().type ~= TK.EOF then
        dsl_error("quad_expr: unexpected tokens after expression")
    end

    -- Constant folding
    ast = fold_constants(ast)

    local emit_fn, ops = compile_ast(ast, v, scratch, false)
    emit_fn(ast, dest_name)

    return function()
        for _, op in ipairs(ops) do
            local fn = _G[op.fn]
            if not fn then
                dsl_error("quad_expr: unknown quad function '" .. op.fn .. "'")
            end
            fn(unpack(op.args))()
        end
    end
end

-- ============================================================================
-- PUBLIC API: quad_pred
-- Compile a predicate expression into p_quad operations.
-- Returns a closure suitable for use as a predicate in se_if_then, se_while, etc.
--
-- expr_str: boolean expression like "x > 5 && y <= 10"
-- v: frame_vars table
-- scratch: list of scratch variable names for temporaries
-- ============================================================================

function _G.quad_pred(expr_str, v, scratch)
    scratch = scratch or {}

    local tokens = tokenize(expr_str)
    local parser = make_parser(tokens)
    local ast = parse_expr(parser, 0)

    if parser:peek().type ~= TK.EOF then
        dsl_error("quad_pred: unexpected tokens after expression")
    end

    ast = fold_constants(ast)

    local emit_fn, ops = compile_ast(ast, v, scratch, true)
    emit_fn(ast)

    return function()
        for _, op in ipairs(ops) do
            local fn = _G[op.fn]
            if not fn then
                dsl_error("quad_pred: unknown quad function '" .. op.fn .. "'")
            end
            fn(unpack(op.args))()
        end
    end
end

-- ============================================================================
-- PUBLIC API: quad_multi
-- Compile multiple semicolon-separated assignment statements.
-- Returns a closure that emits all operations when called.
--
-- expr_str: "a = x + 1; b = a * y; result = b - 2"
-- v: frame_vars table
-- scratch: list of scratch variable names
-- ============================================================================

function _G.quad_multi(expr_str, v, scratch)
    scratch = scratch or {}

    local stmts = {}
    for stmt in expr_str:gmatch("[^;]+") do
        local trimmed = stmt:match("^%s*(.-)%s*$")
        if trimmed ~= "" then
            table.insert(stmts, trimmed)
        end
    end

    if #stmts == 0 then
        dsl_error("quad_multi: empty expression")
    end

    local closures = {}
    for _, stmt in ipairs(stmts) do
        table.insert(closures, quad_expr(stmt, v, scratch))
    end

    return function()
        for _, cl in ipairs(closures) do
            cl()
        end
    end
end

-- ============================================================================
-- PUBLIC API: quad_pred_acc
-- Compile multiple conditions with accumulate semantics.
-- Each condition independently tests and increments a counter.
-- Returns a closure that emits p_quad _ACC operations.
--
-- conditions: table of condition strings, e.g. {"x > 5", "y <= 10", "z == 0"}
-- v: frame_vars table
-- count_var: name of the variable to accumulate into (must be in v)
-- ============================================================================

function _G.quad_pred_acc(conditions, v, count_var)
    if type(conditions) ~= "table" or #conditions == 0 then
        dsl_error("quad_pred_acc: conditions must be a non-empty table of strings")
    end
    if not v[count_var] then
        dsl_error("quad_pred_acc: count_var '" .. count_var .. "' not found in vars")
    end

    local compiled = {}

    for _, cond_str in ipairs(conditions) do
        local tokens = tokenize(cond_str)
        local parser = make_parser(tokens)
        local ast = parse_expr(parser, 0)
        ast = fold_constants(ast)

        -- Must be a single comparison
        if ast.tag ~= "binop" or not (op_info[ast.op] and op_info[ast.op].cmp) then
            dsl_error("quad_pred_acc: each condition must be a comparison, got: " .. cond_str)
        end

        local is_f = infer_float(ast, v)

        local function get_leaf_ref(node)
            if node.tag == "num" then
                if node.is_float then return float_val(node.value)
                elseif node.value < 0 then return int_val(node.value)
                else return uint_val(node.value)
                end
            elseif node.tag == "var" then
                if not v[node.name] then
                    dsl_error("quad_pred_acc: unknown variable '" .. node.name .. "'")
                end
                return v[node.name]
            elseif node.tag == "field" then
                return field_val(node.name)
            else
                dsl_error("quad_pred_acc: condition operands must be simple " ..
                          "(variable, field, or number), got complex expression in: " .. cond_str)
            end
        end

        local left_ref = get_leaf_ref(ast.left)
        local right_ref = get_leaf_ref(ast.right)

        local acc_table = is_f and float_p_cmp_acc_ops or int_p_cmp_acc_ops
        local fn_name = acc_table[ast.op]
        if not fn_name then
            dsl_error("quad_pred_acc: no accumulate op for '" .. ast.op .. "'")
        end

        table.insert(compiled, {
            fn_name = fn_name,
            left = left_ref,
            right = right_ref,
        })
    end

    return function()
        -- Zero the counter
        quad_mov(uint_val(0), v[count_var])()

        -- Emit each _ACC operation
        for _, c in ipairs(compiled) do
            local fn = _G[c.fn_name]
            if not fn then
                dsl_error("quad_pred_acc: unknown function '" .. c.fn_name .. "'")
            end
            fn(c.left, c.right, v[count_var])()
        end
    end
end

-- ============================================================================
-- PUBLIC API: quad_expr_debug
-- Same as quad_expr but prints the AST and generated operations.
-- ============================================================================

function _G.quad_expr_debug(expr_str, v, scratch)
    scratch = scratch or {}

    expr_str = desugar_compound(expr_str)

    local dest_name, body = expr_str:match("^%s*(@?[%w_.]+)%s*=%s*(.+)%s*$")
    if not dest_name then
        dsl_error("quad_expr_debug: expected 'dest = expression', got: " .. expr_str)
    end

    local tokens = tokenize(body)
    local parser = make_parser(tokens)
    local ast = parse_expr(parser, 0)

    print("=== quad_expr_debug: " .. expr_str .. " ===")
    print("  AST (before fold): " .. ast_to_string(ast))

    ast = fold_constants(ast)
    print("  AST (after fold):  " .. ast_to_string(ast))

    local emit_fn, ops, get_scratch_high = compile_ast(ast, v, scratch, false)
    emit_fn(ast, dest_name)

    for i, op in ipairs(ops) do
        local args_str = {}
        for _, a in ipairs(op.args) do
            table.insert(args_str, type(a) == "function" and "<ref>" or tostring(a))
        end
        print(string.format("  %d: %s(%s)", i, op.fn, table.concat(args_str, ", ")))
    end
    print("  scratch used: " .. get_scratch_high() .. " / " .. #scratch)
    print("===")

    -- Return the actual working closure
    return quad_expr(expr_str, v, scratch)
end

-- ============================================================================
-- PUBLIC API: quad_pred_debug
-- Same as quad_pred but prints debug info.
-- ============================================================================

function _G.quad_pred_debug(expr_str, v, scratch)
    scratch = scratch or {}

    local tokens = tokenize(expr_str)
    local parser = make_parser(tokens)
    local ast = parse_expr(parser, 0)

    print("=== quad_pred_debug: " .. expr_str .. " ===")
    print("  AST (before fold): " .. ast_to_string(ast))

    ast = fold_constants(ast)
    print("  AST (after fold):  " .. ast_to_string(ast))

    local emit_fn, ops, get_scratch_high = compile_ast(ast, v, scratch, true)
    emit_fn(ast)

    for i, op in ipairs(ops) do
        local args_str = {}
        for _, a in ipairs(op.args) do
            table.insert(args_str, type(a) == "function" and "<ref>" or tostring(a))
        end
        print(string.format("  %d: %s(%s)", i, op.fn, table.concat(args_str, ", ")))
    end
    print("  scratch used: " .. get_scratch_high() .. " / " .. #scratch)
    print("===")

    return quad_pred(expr_str, v, scratch)
end

print("S-Expression compiler loaded (v1.1)")