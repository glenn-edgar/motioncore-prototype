--============================================================================
-- se_quad_ops.lua
-- SE_QUAD_OP table, se_quad emitter, value-ref helpers, and all quad_*
-- convenience wrappers (integer/float arithmetic, bitwise, comparison,
-- logical, move, math, trig, hyperbolic, min/max)
--============================================================================

--============================================================================
-- VALUE REFERENCE HELPERS
-- Closures that emit a single parameter for use with se_quad
--============================================================================
register_builtin("SE_QUAD")

function stack_push_ref()
    return function() stack_push() end
end

function stack_pop_ref()
    return function() stack_pop() end
end

function local_ref(idx)
    return function() stack_local(idx) end
end

function tos_ref(offset)
    return function() stack_tos(offset) end
end

function int_val(v)
    return function() int(v) end
end

function uint_val(v)
    return function() uint(v) end
end

function float_val(v)
    return function() flt(v) end
end

function field_val(name)
    return function() field_ref(name) end
end

function const_val(name)
    return function() const_ref(name) end
end

function hash_val(s)
    return function() str_hash(s) end
end

function null_val()
    return function() null_param() end
end

--============================================================================
-- OPCODE TABLE
--============================================================================

SE_QUAD_OP = {
    -- Integer Arithmetic
    IADD         = 0x00,
    ISUB         = 0x01,
    IMUL         = 0x02,
    IDIV         = 0x03,
    IMOD         = 0x04,
    INEG         = 0x05,

    -- Float Arithmetic
    FADD         = 0x08,
    FSUB         = 0x09,
    FMUL         = 0x0A,
    FDIV         = 0x0B,
    FMOD         = 0x0C,
    FNEG         = 0x0D,

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

    -- Move
    MOVE         = 0x40,

    -- Float Math Functions
    FSQRT        = 0x50,
    FPOW         = 0x51,
    FEXP         = 0x52,
    FLOG         = 0x53,
    FLOG10       = 0x54,
    FLOG2        = 0x55,
    FABS         = 0x56,

    -- Trigonometric (float, radians)
    FSIN         = 0x58,
    FCOS         = 0x59,
    FTAN         = 0x5A,
    FASIN        = 0x5B,
    FACOS        = 0x5C,
    FATAN        = 0x5D,
    FATAN2       = 0x5E,

    -- Hyperbolic (float)
    FSINH        = 0x60,
    FCOSH        = 0x61,
    FTANH        = 0x62,

    -- Integer Math
    IABS         = 0x68,
    IMIN         = 0x69,
    IMAX         = 0x6A,

    -- Float Min/Max
    FMIN         = 0x6C,
    FMAX         = 0x6D,
    MOV          = 0x6E,
}

-- Lookup set for compile-time validation
local SE_QUAD_OP_SET = {}
for name, val in pairs(SE_QUAD_OP) do
    SE_QUAD_OP_SET[val] = name
end

--============================================================================
-- CORE EMITTER
--============================================================================

function se_quad(opcode, src1_fn, src2_fn, dest_fn)
    if type(opcode) ~= "number" then
        dsl_error("se_quad: opcode must be a number")
    end
    if not SE_QUAD_OP_SET[opcode] then
        dsl_error("se_quad: unknown opcode 0x" .. string.format("%02X", opcode) ..
                  " - not a valid SE_QUAD_OP")
    end
    if type(src1_fn) ~= "function" then
        dsl_error("se_quad: src1 must be a function emitting a parameter")
    end
    if type(src2_fn) ~= "function" then
        dsl_error("se_quad: src2 must be a function emitting a parameter")
    end
    if type(dest_fn) ~= "function" then
        dsl_error("se_quad: dest must be a function emitting a parameter")
    end

    local c = o_call("SE_QUAD")
        uint(opcode)
        src1_fn()
        src2_fn()
        dest_fn()
    end_call(c)
end

--============================================================================
-- NULL helper for unary ops
--============================================================================
local function _null() null_param() end

--============================================================================
-- INTEGER ARITHMETIC
--============================================================================

function quad_iadd(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.IADD, src1_fn, src2_fn, dest_fn) end
end

function quad_isub(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.ISUB, src1_fn, src2_fn, dest_fn) end
end

function quad_imul(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.IMUL, src1_fn, src2_fn, dest_fn) end
end

function quad_idiv(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.IDIV, src1_fn, src2_fn, dest_fn) end
end

function quad_imod(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.IMOD, src1_fn, src2_fn, dest_fn) end
end

function quad_ineg(src_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.INEG, src_fn, _null, dest_fn) end
end

--============================================================================
-- FLOAT ARITHMETIC
--============================================================================

function quad_fadd(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FADD, src1_fn, src2_fn, dest_fn) end
end

function quad_fsub(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FSUB, src1_fn, src2_fn, dest_fn) end
end

function quad_fmul(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FMUL, src1_fn, src2_fn, dest_fn) end
end

function quad_fdiv(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FDIV, src1_fn, src2_fn, dest_fn) end
end

function quad_fmod(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FMOD, src1_fn, src2_fn, dest_fn) end
end

function quad_fneg(src_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FNEG, src_fn, _null, dest_fn) end
end

--============================================================================
-- BITWISE OPERATIONS
--============================================================================

function quad_and(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.BIT_AND, src1_fn, src2_fn, dest_fn) end
end

function quad_or(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.BIT_OR, src1_fn, src2_fn, dest_fn) end
end

function quad_xor(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.BIT_XOR, src1_fn, src2_fn, dest_fn) end
end

function quad_not(src_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.BIT_NOT, src_fn, _null, dest_fn) end
end

function quad_shl(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.BIT_SHL, src1_fn, src2_fn, dest_fn) end
end

function quad_shr(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.BIT_SHR, src1_fn, src2_fn, dest_fn) end
end

--============================================================================
-- INTEGER COMPARISON
--============================================================================

function quad_ieq(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.ICMP_EQ, src1_fn, src2_fn, dest_fn) end
end

function quad_ine(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.ICMP_NE, src1_fn, src2_fn, dest_fn) end
end

function quad_ilt(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.ICMP_LT, src1_fn, src2_fn, dest_fn) end
end

function quad_ile(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.ICMP_LE, src1_fn, src2_fn, dest_fn) end
end

function quad_igt(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.ICMP_GT, src1_fn, src2_fn, dest_fn) end
end

function quad_ige(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.ICMP_GE, src1_fn, src2_fn, dest_fn) end
end

--============================================================================
-- FLOAT COMPARISON
--============================================================================

function quad_feq(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FCMP_EQ, src1_fn, src2_fn, dest_fn) end
end

function quad_fne(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FCMP_NE, src1_fn, src2_fn, dest_fn) end
end

function quad_flt(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FCMP_LT, src1_fn, src2_fn, dest_fn) end
end

function quad_fle(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FCMP_LE, src1_fn, src2_fn, dest_fn) end
end

function quad_fgt(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FCMP_GT, src1_fn, src2_fn, dest_fn) end
end

function quad_fge(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FCMP_GE, src1_fn, src2_fn, dest_fn) end
end

--============================================================================
-- LOGICAL OPERATIONS
--============================================================================

function quad_log_and(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.LOG_AND, src1_fn, src2_fn, dest_fn) end
end

function quad_log_or(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.LOG_OR, src1_fn, src2_fn, dest_fn) end
end

function quad_log_not(src_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.LOG_NOT, src_fn, _null, dest_fn) end
end

function quad_log_nand(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.LOG_NAND, src1_fn, src2_fn, dest_fn) end
end

function quad_log_nor(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.LOG_NOR, src1_fn, src2_fn, dest_fn) end
end

function quad_log_xor(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.LOG_XOR, src1_fn, src2_fn, dest_fn) end
end

--============================================================================
-- MOVE
--============================================================================

function quad_mov(src_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.MOVE, src_fn, _null, dest_fn) end
end

--============================================================================
-- FLOAT MATH FUNCTIONS
--============================================================================

function quad_sqrt(src_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FSQRT, src_fn, _null, dest_fn) end
end

function quad_pow(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FPOW, src1_fn, src2_fn, dest_fn) end
end

function quad_exp(src_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FEXP, src_fn, _null, dest_fn) end
end

function quad_log(src_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FLOG, src_fn, _null, dest_fn) end
end

function quad_log10(src_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FLOG10, src_fn, _null, dest_fn) end
end

function quad_log2(src_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FLOG2, src_fn, _null, dest_fn) end
end

function quad_fabs(src_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FABS, src_fn, _null, dest_fn) end
end

--============================================================================
-- TRIGONOMETRIC
--============================================================================

function quad_sin(src_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FSIN, src_fn, _null, dest_fn) end
end

function quad_cos(src_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FCOS, src_fn, _null, dest_fn) end
end

function quad_tan(src_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FTAN, src_fn, _null, dest_fn) end
end

function quad_asin(src_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FASIN, src_fn, _null, dest_fn) end
end

function quad_acos(src_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FACOS, src_fn, _null, dest_fn) end
end

function quad_atan(src_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FATAN, src_fn, _null, dest_fn) end
end

function quad_atan2(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FATAN2, src1_fn, src2_fn, dest_fn) end
end

--============================================================================
-- HYPERBOLIC
--============================================================================

function quad_sinh(src_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FSINH, src_fn, _null, dest_fn) end
end

function quad_cosh(src_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FCOSH, src_fn, _null, dest_fn) end
end

function quad_tanh(src_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FTANH, src_fn, _null, dest_fn) end
end

--============================================================================
-- INTEGER MATH
--============================================================================

function quad_iabs(src_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.IABS, src_fn, _null, dest_fn) end
end

function quad_imin(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.IMIN, src1_fn, src2_fn, dest_fn) end
end

function quad_imax(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.IMAX, src1_fn, src2_fn, dest_fn) end
end

--============================================================================
-- FLOAT MIN/MAX
--============================================================================

function quad_fmin(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FMIN, src1_fn, src2_fn, dest_fn) end
end

function quad_fmax(src1_fn, src2_fn, dest_fn)
    return function() se_quad(SE_QUAD_OP.FMAX, src1_fn, src2_fn, dest_fn) end
end