--============================================================================
-- se_p_quad_ops.lua
-- Predicate quad operations: SE_P_QUAD_OP table, se_p_quad emitter,
-- all p_* boolean/comparison/accumulate wrappers, range checks
--============================================================================
register_builtin("SE_P_QUAD")
SE_P_QUAD_OP = {
    -- Bitwise (integer only)
    BIT_AND      = 0x10,
    BIT_OR       = 0x11,
    BIT_XOR      = 0x12,
    BIT_NOT      = 0x13,
    BIT_SHL      = 0x14,
    BIT_SHR      = 0x15,

    -- Integer Comparison (dest = 1 or 0)
    ICMP_EQ      = 0x20,
    ICMP_NE      = 0x21,
    ICMP_LT      = 0x22,
    ICMP_LE      = 0x23,
    ICMP_GT      = 0x24,
    ICMP_GE      = 0x25,

    -- Float Comparison (dest = 1 or 0)
    FCMP_EQ      = 0x28,
    FCMP_NE      = 0x29,
    FCMP_LT      = 0x2A,
    FCMP_LE      = 0x2B,
    FCMP_GT      = 0x2C,
    FCMP_GE      = 0x2D,

    -- Logical (dest = 1 or 0)
    LOG_AND      = 0x30,
    LOG_OR       = 0x31,
    LOG_NOT      = 0x32,
    LOG_NAND     = 0x33,
    LOG_NOR      = 0x34,
    LOG_XOR      = 0x35,

    -- Integer Comparison + Accumulate (dest += result)
    ICMP_EQ_ACC  = 0x40,
    ICMP_NE_ACC  = 0x41,
    ICMP_LT_ACC  = 0x42,
    ICMP_LE_ACC  = 0x43,
    ICMP_GT_ACC  = 0x44,
    ICMP_GE_ACC  = 0x45,

    -- Float Comparison + Accumulate (dest += result)
    FCMP_EQ_ACC  = 0x48,
    FCMP_NE_ACC  = 0x49,
    FCMP_LT_ACC  = 0x4A,
    FCMP_LE_ACC  = 0x4B,
    FCMP_GT_ACC  = 0x4C,
    FCMP_GE_ACC  = 0x4D,
}

local SE_P_QUAD_OP_SET = {}
for name, val in pairs(SE_P_QUAD_OP) do
    SE_P_QUAD_OP_SET[val] = name
end

--============================================================================
-- CORE EMITTER
--============================================================================

function se_p_quad(opcode, src1_fn, src2_fn, dest_fn)
    if type(opcode) ~= "number" then
        dsl_error("se_p_quad: opcode must be a number")
    end
    if not SE_P_QUAD_OP_SET[opcode] then
        dsl_error("se_p_quad: unknown opcode 0x" .. string.format("%02X", opcode) ..
                  " - not a valid SE_P_QUAD_OP")
    end
    if type(src1_fn) ~= "function" then
        dsl_error("se_p_quad: src1 must be a function emitting a parameter")
    end
    if type(src2_fn) ~= "function" then
        dsl_error("se_p_quad: src2 must be a function emitting a parameter")
    end
    if type(dest_fn) ~= "function" then
        dsl_error("se_p_quad: dest must be a function emitting a parameter")
    end

    local c = p_call("SE_P_QUAD")
        uint(opcode)
        src1_fn()
        src2_fn()
        dest_fn()
    end_call(c)
end

local function _null() null_param() end

--============================================================================
-- BITWISE
--============================================================================

function p_bit_and(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.BIT_AND, src1_fn, src2_fn, dest_fn) end
end

function p_bit_or(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.BIT_OR, src1_fn, src2_fn, dest_fn) end
end

function p_bit_xor(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.BIT_XOR, src1_fn, src2_fn, dest_fn) end
end

function p_bit_not(src1_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.BIT_NOT, src1_fn, _null, dest_fn) end
end

function p_bit_shl(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.BIT_SHL, src1_fn, src2_fn, dest_fn) end
end

function p_bit_shr(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.BIT_SHR, src1_fn, src2_fn, dest_fn) end
end

--============================================================================
-- INTEGER COMPARISON
--============================================================================

function p_icmp_eq(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.ICMP_EQ, src1_fn, src2_fn, dest_fn) end
end

function p_icmp_ne(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.ICMP_NE, src1_fn, src2_fn, dest_fn) end
end

function p_icmp_lt(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.ICMP_LT, src1_fn, src2_fn, dest_fn) end
end

function p_icmp_le(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.ICMP_LE, src1_fn, src2_fn, dest_fn) end
end

function p_icmp_gt(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.ICMP_GT, src1_fn, src2_fn, dest_fn) end
end

function p_icmp_ge(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.ICMP_GE, src1_fn, src2_fn, dest_fn) end
end

--============================================================================
-- FLOAT COMPARISON
--============================================================================

function p_fcmp_eq(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.FCMP_EQ, src1_fn, src2_fn, dest_fn) end
end

function p_fcmp_ne(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.FCMP_NE, src1_fn, src2_fn, dest_fn) end
end

function p_fcmp_lt(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.FCMP_LT, src1_fn, src2_fn, dest_fn) end
end

function p_fcmp_le(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.FCMP_LE, src1_fn, src2_fn, dest_fn) end
end

function p_fcmp_gt(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.FCMP_GT, src1_fn, src2_fn, dest_fn) end
end

function p_fcmp_ge(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.FCMP_GE, src1_fn, src2_fn, dest_fn) end
end

--============================================================================
-- LOGICAL
--============================================================================

function p_log_and(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.LOG_AND, src1_fn, src2_fn, dest_fn) end
end

function p_log_or(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.LOG_OR, src1_fn, src2_fn, dest_fn) end
end

function p_log_not(src1_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.LOG_NOT, src1_fn, _null, dest_fn) end
end

function p_log_nand(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.LOG_NAND, src1_fn, src2_fn, dest_fn) end
end

function p_log_nor(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.LOG_NOR, src1_fn, src2_fn, dest_fn) end
end

function p_log_xor(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.LOG_XOR, src1_fn, src2_fn, dest_fn) end
end

--============================================================================
-- INTEGER COMPARISON + ACCUMULATE
--============================================================================

function p_icmp_eq_acc(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.ICMP_EQ_ACC, src1_fn, src2_fn, dest_fn) end
end

function p_icmp_ne_acc(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.ICMP_NE_ACC, src1_fn, src2_fn, dest_fn) end
end

function p_icmp_lt_acc(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.ICMP_LT_ACC, src1_fn, src2_fn, dest_fn) end
end

function p_icmp_le_acc(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.ICMP_LE_ACC, src1_fn, src2_fn, dest_fn) end
end

function p_icmp_gt_acc(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.ICMP_GT_ACC, src1_fn, src2_fn, dest_fn) end
end

function p_icmp_ge_acc(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.ICMP_GE_ACC, src1_fn, src2_fn, dest_fn) end
end

--============================================================================
-- FLOAT COMPARISON + ACCUMULATE
--============================================================================

function p_fcmp_eq_acc(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.FCMP_EQ_ACC, src1_fn, src2_fn, dest_fn) end
end

function p_fcmp_ne_acc(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.FCMP_NE_ACC, src1_fn, src2_fn, dest_fn) end
end

function p_fcmp_lt_acc(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.FCMP_LT_ACC, src1_fn, src2_fn, dest_fn) end
end

function p_fcmp_le_acc(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.FCMP_LE_ACC, src1_fn, src2_fn, dest_fn) end
end

function p_fcmp_gt_acc(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.FCMP_GT_ACC, src1_fn, src2_fn, dest_fn) end
end

function p_fcmp_ge_acc(src1_fn, src2_fn, dest_fn)
    return function() se_p_quad(SE_P_QUAD_OP.FCMP_GE_ACC, src1_fn, src2_fn, dest_fn) end
end

--============================================================================
-- RANGE CHECKS (composite: two comparisons + logical AND)
--============================================================================

function p_icmp_in_range(src_fn, low_fn, high_fn, dest_fn, scratch1_fn, scratch2_fn)
    return function()
        p_icmp_le(low_fn, src_fn, scratch1_fn)()
        p_icmp_le(src_fn, high_fn, scratch2_fn)()
        p_log_and(scratch1_fn, scratch2_fn, dest_fn)()
    end
end

function p_fcmp_in_range(src_fn, low_fn, high_fn, dest_fn, scratch1_fn, scratch2_fn)
    return function()
        p_fcmp_le(low_fn, src_fn, scratch1_fn)()
        p_fcmp_le(src_fn, high_fn, scratch2_fn)()
        p_log_and(scratch1_fn, scratch2_fn, dest_fn)()
    end
end