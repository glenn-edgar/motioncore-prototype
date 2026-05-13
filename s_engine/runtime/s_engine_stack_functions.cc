// ============================================================================
// s_engine_stack_ops.c
// S-Expression Engine Stack-Based Arithmetic Operations
// 
// Oneshot functions that operate on the parameter stack for expression
// evaluation. These provide calculator-style operations within tree nodes.
//
// Stack Notation: [-n, +m] means pop n values, push m values
// Binary ops: -2 is 'a', -1 is 'b', result = a op b
//
// Type Rules:
//   - Both INT/UINT → result is INT
//   - Either FLOAT → promote to float, result is FLOAT
//   - Non-numeric → EXCEPTION (hard crash)
//   - Bitwise ops require integer types
// ============================================================================

#include "s_engine_stack.h"
#include "s_engine_stack_functions.h"
#include "s_engine_exception.h"
#include <math.h>





// ============================================================================
// BASIC ARITHMETIC [-2, +1]
// ============================================================================

void se_stack_add(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_2();
    
    bool use_float = s_expr_stack_isfloat(stack, -1) || s_expr_stack_isfloat(stack, -2);
    
    if (use_float) {
        ct_float_t b = s_expr_stack_tofloat(stack, -1);
        ct_float_t a = s_expr_stack_tofloat(stack, -2);
        s_expr_stack_popn(stack, 2);
        s_expr_stack_push_float(stack, a + b);
    } else {
        ct_int_t b = s_expr_stack_toint(stack, -1);
        ct_int_t a = s_expr_stack_toint(stack, -2);
        s_expr_stack_popn(stack, 2);
        s_expr_stack_push_int(stack, a + b);
    }
}

void se_stack_sub(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_2();
    
    bool use_float = s_expr_stack_isfloat(stack, -1) || s_expr_stack_isfloat(stack, -2);
    
    if (use_float) {
        ct_float_t b = s_expr_stack_tofloat(stack, -1);
        ct_float_t a = s_expr_stack_tofloat(stack, -2);
        s_expr_stack_popn(stack, 2);
        s_expr_stack_push_float(stack, a - b);
    } else {
        ct_int_t b = s_expr_stack_toint(stack, -1);
        ct_int_t a = s_expr_stack_toint(stack, -2);
        s_expr_stack_popn(stack, 2);
        s_expr_stack_push_int(stack, a - b);
    }
}

void se_stack_mul(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_2();
    
    bool use_float = s_expr_stack_isfloat(stack, -1) || s_expr_stack_isfloat(stack, -2);
    
    if (use_float) {
        ct_float_t b = s_expr_stack_tofloat(stack, -1);
        ct_float_t a = s_expr_stack_tofloat(stack, -2);
        s_expr_stack_popn(stack, 2);
        s_expr_stack_push_float(stack, a * b);
    } else {
        ct_int_t b = s_expr_stack_toint(stack, -1);
        ct_int_t a = s_expr_stack_toint(stack, -2);
        s_expr_stack_popn(stack, 2);
        s_expr_stack_push_int(stack, a * b);
    }
}

void se_stack_div(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_2();
    
    // Float division always
    ct_float_t b = s_expr_stack_tofloat(stack, -1);
    ct_float_t a = s_expr_stack_tofloat(stack, -2);
    
    if (b == 0.0) {
        EXCEPTION("division by zero");
        return;
    }
    
    s_expr_stack_popn(stack, 2);
    s_expr_stack_push_float(stack, a / b);
}

void se_stack_mod(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_2();
    
    // Float modulo (fmod)
    ct_float_t b = s_expr_stack_tofloat(stack, -1);
    ct_float_t a = s_expr_stack_tofloat(stack, -2);
    
    if (b == 0.0) {
        EXCEPTION("modulo by zero");
        return;
    }
    
    s_expr_stack_popn(stack, 2);
    s_expr_stack_push_float(stack, fmod(a, b));
}

void se_stack_idiv(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_2();
    
    ct_int_t b = s_expr_stack_toint(stack, -1);
    ct_int_t a = s_expr_stack_toint(stack, -2);
    
    if (b == 0) {
        EXCEPTION("integer division by zero");
        return;
    }
    
    s_expr_stack_popn(stack, 2);
    s_expr_stack_push_int(stack, a / b);
}

void se_stack_imod(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_2();
    
    ct_int_t b = s_expr_stack_toint(stack, -1);
    ct_int_t a = s_expr_stack_toint(stack, -2);
    
    if (b == 0) {
        EXCEPTION("integer modulo by zero");
        return;
    }
    
    s_expr_stack_popn(stack, 2);
    s_expr_stack_push_int(stack, a % b);
}

// ============================================================================
// UNARY ARITHMETIC [-1, +1]
// ============================================================================

void se_stack_neg(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    if (s_expr_stack_isfloat(stack, -1)) {
        ct_float_t val = s_expr_stack_tofloat(stack, -1);
        s_expr_stack_popn(stack, 1);
        s_expr_stack_push_float(stack, -val);
    } else {
        ct_int_t val = s_expr_stack_toint(stack, -1);
        s_expr_stack_popn(stack, 1);
        s_expr_stack_push_int(stack, -val);
    }
}

void se_stack_abs(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    if (s_expr_stack_isfloat(stack, -1)) {
        ct_float_t val = s_expr_stack_tofloat(stack, -1);
        s_expr_stack_popn(stack, 1);
        s_expr_stack_push_float(stack, fabs(val));
    } else {
        ct_int_t val = s_expr_stack_toint(stack, -1);
        s_expr_stack_popn(stack, 1);
        s_expr_stack_push_int(stack, val < 0 ? -val : val);
    }
}

void se_stack_inc(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    if (s_expr_stack_isfloat(stack, -1)) {
        ct_float_t val = s_expr_stack_tofloat(stack, -1);
        s_expr_stack_popn(stack, 1);
        s_expr_stack_push_float(stack, val + 1.0);
    } else {
        ct_int_t val = s_expr_stack_toint(stack, -1);
        s_expr_stack_popn(stack, 1);
        s_expr_stack_push_int(stack, val + 1);
    }
}

void se_stack_dec(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    if (s_expr_stack_isfloat(stack, -1)) {
        ct_float_t val = s_expr_stack_tofloat(stack, -1);
        s_expr_stack_popn(stack, 1);
        s_expr_stack_push_float(stack, val - 1.0);
    } else {
        ct_int_t val = s_expr_stack_toint(stack, -1);
        s_expr_stack_popn(stack, 1);
        s_expr_stack_push_int(stack, val - 1);
    }
}

// ============================================================================
// BITWISE OPERATIONS [-2, +1] - integers only
// ============================================================================

void se_stack_band(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_INTEGER_2();
    
    ct_uint_t b = s_expr_stack_touint(stack, -1);
    ct_uint_t a = s_expr_stack_touint(stack, -2);
    s_expr_stack_popn(stack, 2);
    s_expr_stack_push_int(stack, (ct_int_t)(a & b));
}

void se_stack_bor(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_INTEGER_2();
    
    ct_uint_t b = s_expr_stack_touint(stack, -1);
    ct_uint_t a = s_expr_stack_touint(stack, -2);
    s_expr_stack_popn(stack, 2);
    s_expr_stack_push_int(stack, (ct_int_t)(a | b));
}

void se_stack_bxor(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_INTEGER_2();
    
    ct_uint_t b = s_expr_stack_touint(stack, -1);
    ct_uint_t a = s_expr_stack_touint(stack, -2);
    s_expr_stack_popn(stack, 2);
    s_expr_stack_push_int(stack, (ct_int_t)(a ^ b));
}

void se_stack_shl(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_INTEGER_2();
    
    ct_int_t shift = s_expr_stack_toint(stack, -1);
    ct_uint_t a = s_expr_stack_touint(stack, -2);
    
    if (shift < 0 || shift >= (ct_int_t)(sizeof(ct_uint_t) * 8)) {
        EXCEPTION("shift amount out of range");
        return;
    }
    
    s_expr_stack_popn(stack, 2);
    s_expr_stack_push_int(stack, (ct_int_t)(a << shift));
}

void se_stack_shr(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_INTEGER_2();
    
    ct_int_t shift = s_expr_stack_toint(stack, -1);
    ct_uint_t a = s_expr_stack_touint(stack, -2);
    
    if (shift < 0 || shift >= (ct_int_t)(sizeof(ct_uint_t) * 8)) {
        EXCEPTION("shift amount out of range");
        return;
    }
    
    s_expr_stack_popn(stack, 2);
    // Logical shift right (unsigned)
    s_expr_stack_push_int(stack, (ct_int_t)(a >> shift));
}

void se_stack_sar(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_INTEGER_2();
    
    ct_int_t shift = s_expr_stack_toint(stack, -1);
    ct_int_t a = s_expr_stack_toint(stack, -2);
    
    if (shift < 0 || shift >= (ct_int_t)(sizeof(ct_int_t) * 8)) {
        EXCEPTION("shift amount out of range");
        return;
    }
    
    s_expr_stack_popn(stack, 2);
    // Arithmetic shift right (signed - preserves sign bit)
    s_expr_stack_push_int(stack, a >> shift);
}

// ============================================================================
// UNARY BITWISE [-1, +1] - integers only
// ============================================================================

void se_stack_bnot(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_INTEGER_1();
    
    ct_uint_t val = s_expr_stack_touint(stack, -1);
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_int(stack, (ct_int_t)(~val));
}

// ============================================================================
// COMPARISON [-2, +1] - push 1 (true) or 0 (false) as INT
// ============================================================================

void se_stack_eq(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_2();
    
    bool use_float = s_expr_stack_isfloat(stack, -1) || s_expr_stack_isfloat(stack, -2);
    bool result;
    
    if (use_float) {
        ct_float_t b = s_expr_stack_tofloat(stack, -1);
        ct_float_t a = s_expr_stack_tofloat(stack, -2);
        result = (a == b);
    } else {
        ct_int_t b = s_expr_stack_toint(stack, -1);
        ct_int_t a = s_expr_stack_toint(stack, -2);
        result = (a == b);
    }
    
    s_expr_stack_popn(stack, 2);
    s_expr_stack_push_int(stack, result ? 1 : 0);
}

void se_stack_ne(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_2();
    
    bool use_float = s_expr_stack_isfloat(stack, -1) || s_expr_stack_isfloat(stack, -2);
    bool result;
    
    if (use_float) {
        ct_float_t b = s_expr_stack_tofloat(stack, -1);
        ct_float_t a = s_expr_stack_tofloat(stack, -2);
        result = (a != b);
    } else {
        ct_int_t b = s_expr_stack_toint(stack, -1);
        ct_int_t a = s_expr_stack_toint(stack, -2);
        result = (a != b);
    }
    
    s_expr_stack_popn(stack, 2);
    s_expr_stack_push_int(stack, result ? 1 : 0);
}

void se_stack_lt(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_2();
    
    bool use_float = s_expr_stack_isfloat(stack, -1) || s_expr_stack_isfloat(stack, -2);
    bool result;
    
    if (use_float) {
        ct_float_t b = s_expr_stack_tofloat(stack, -1);
        ct_float_t a = s_expr_stack_tofloat(stack, -2);
        result = (a < b);
    } else {
        ct_int_t b = s_expr_stack_toint(stack, -1);
        ct_int_t a = s_expr_stack_toint(stack, -2);
        result = (a < b);
    }
    
    s_expr_stack_popn(stack, 2);
    s_expr_stack_push_int(stack, result ? 1 : 0);
}

void se_stack_le(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_2();
    
    bool use_float = s_expr_stack_isfloat(stack, -1) || s_expr_stack_isfloat(stack, -2);
    bool result;
    
    if (use_float) {
        ct_float_t b = s_expr_stack_tofloat(stack, -1);
        ct_float_t a = s_expr_stack_tofloat(stack, -2);
        result = (a <= b);
    } else {
        ct_int_t b = s_expr_stack_toint(stack, -1);
        ct_int_t a = s_expr_stack_toint(stack, -2);
        result = (a <= b);
    }
    
    s_expr_stack_popn(stack, 2);
    s_expr_stack_push_int(stack, result ? 1 : 0);
}

void se_stack_gt(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_2();
    
    bool use_float = s_expr_stack_isfloat(stack, -1) || s_expr_stack_isfloat(stack, -2);
    bool result;
    
    if (use_float) {
        ct_float_t b = s_expr_stack_tofloat(stack, -1);
        ct_float_t a = s_expr_stack_tofloat(stack, -2);
        result = (a > b);
    } else {
        ct_int_t b = s_expr_stack_toint(stack, -1);
        ct_int_t a = s_expr_stack_toint(stack, -2);
        result = (a > b);
    }
    
    s_expr_stack_popn(stack, 2);
    s_expr_stack_push_int(stack, result ? 1 : 0);
}

void se_stack_ge(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_2();
    
    bool use_float = s_expr_stack_isfloat(stack, -1) || s_expr_stack_isfloat(stack, -2);
    bool result;
    
    if (use_float) {
        ct_float_t b = s_expr_stack_tofloat(stack, -1);
        ct_float_t a = s_expr_stack_tofloat(stack, -2);
        result = (a >= b);
    } else {
        ct_int_t b = s_expr_stack_toint(stack, -1);
        ct_int_t a = s_expr_stack_toint(stack, -2);
        result = (a >= b);
    }
    
    s_expr_stack_popn(stack, 2);
    s_expr_stack_push_int(stack, result ? 1 : 0);
}

// ============================================================================
// LOGICAL OPERATIONS [-2, +1] - result is INT (0 or 1)
// ============================================================================

void se_stack_and(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_2();
    
    ct_float_t b = s_expr_stack_tofloat(stack, -1);
    ct_float_t a = s_expr_stack_tofloat(stack, -2);
    
    s_expr_stack_popn(stack, 2);
    s_expr_stack_push_int(stack, (a != 0.0 && b != 0.0) ? 1 : 0);
}

void se_stack_or(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_2();
    
    ct_float_t b = s_expr_stack_tofloat(stack, -1);
    ct_float_t a = s_expr_stack_tofloat(stack, -2);
    
    s_expr_stack_popn(stack, 2);
    s_expr_stack_push_int(stack, (a != 0.0 || b != 0.0) ? 1 : 0);
}

// ============================================================================
// UNARY LOGICAL [-1, +1]
// ============================================================================

void se_stack_not(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    ct_float_t val = s_expr_stack_tofloat(stack, -1);
    
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_int(stack, (val == 0.0) ? 1 : 0);
}

// ============================================================================
// MATH FUNCTIONS [-1, +1] - always produce FLOAT
// ============================================================================

void se_stack_sqrt(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    ct_float_t val = s_expr_stack_tofloat(stack, -1);
    
    if (val < 0.0) {
        EXCEPTION("sqrt of negative number");
        return;
    }
    
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_float(stack, sqrt(val));
}

void se_stack_exp(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    ct_float_t val = s_expr_stack_tofloat(stack, -1);
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_float(stack, exp(val));
}

void se_stack_log(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    ct_float_t val = s_expr_stack_tofloat(stack, -1);
    
    if (val <= 0.0) {
        EXCEPTION("log of non-positive number");
        return;
    }
    
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_float(stack, log(val));
}

void se_stack_log10(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    ct_float_t val = s_expr_stack_tofloat(stack, -1);
    
    if (val <= 0.0) {
        EXCEPTION("log10 of non-positive number");
        return;
    }
    
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_float(stack, log10(val));
}

void se_stack_sin(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    ct_float_t val = s_expr_stack_tofloat(stack, -1);
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_float(stack, sin(val));
}

void se_stack_cos(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    ct_float_t val = s_expr_stack_tofloat(stack, -1);
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_float(stack, cos(val));
}

void se_stack_tan(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    ct_float_t val = s_expr_stack_tofloat(stack, -1);
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_float(stack, tan(val));
}

void se_stack_asin(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    ct_float_t val = s_expr_stack_tofloat(stack, -1);
    
    if (val < -1.0 || val > 1.0) {
        EXCEPTION("asin argument out of range [-1, 1]");
        return;
    }
    
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_float(stack, asin(val));
}

void se_stack_acos(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    ct_float_t val = s_expr_stack_tofloat(stack, -1);
    
    if (val < -1.0 || val > 1.0) {
        EXCEPTION("acos argument out of range [-1, 1]");
        return;
    }
    
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_float(stack, acos(val));
}

void se_stack_atan(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    ct_float_t val = s_expr_stack_tofloat(stack, -1);
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_float(stack, atan(val));
}

void se_stack_floor(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    ct_float_t val = s_expr_stack_tofloat(stack, -1);
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_float(stack, floor(val));
}

void se_stack_ceil(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    ct_float_t val = s_expr_stack_tofloat(stack, -1);
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_float(stack, ceil(val));
}

void se_stack_round(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    ct_float_t val = s_expr_stack_tofloat(stack, -1);
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_float(stack, round(val));
}

void se_stack_trunc(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    ct_float_t val = s_expr_stack_tofloat(stack, -1);
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_float(stack, trunc(val));
}

// ============================================================================
// BINARY MATH FUNCTIONS [-2, +1] - always produce FLOAT
// ============================================================================

void se_stack_pow(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_2();
    
    ct_float_t b = s_expr_stack_tofloat(stack, -1);  // exponent
    ct_float_t a = s_expr_stack_tofloat(stack, -2);  // base
    s_expr_stack_popn(stack, 2);
    s_expr_stack_push_float(stack, pow(a, b));
}

void se_stack_atan2(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_2();
    
    ct_float_t x = s_expr_stack_tofloat(stack, -1);
    ct_float_t y = s_expr_stack_tofloat(stack, -2);
    s_expr_stack_popn(stack, 2);
    s_expr_stack_push_float(stack, atan2(y, x));
}

void se_stack_min(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_2();
    
    bool use_float = s_expr_stack_isfloat(stack, -1) || s_expr_stack_isfloat(stack, -2);
    
    if (use_float) {
        ct_float_t b = s_expr_stack_tofloat(stack, -1);
        ct_float_t a = s_expr_stack_tofloat(stack, -2);
        s_expr_stack_popn(stack, 2);
        s_expr_stack_push_float(stack, (a < b) ? a : b);
    } else {
        ct_int_t b = s_expr_stack_toint(stack, -1);
        ct_int_t a = s_expr_stack_toint(stack, -2);
        s_expr_stack_popn(stack, 2);
        s_expr_stack_push_int(stack, (a < b) ? a : b);
    }
}

void se_stack_max(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_2();
    
    bool use_float = s_expr_stack_isfloat(stack, -1) || s_expr_stack_isfloat(stack, -2);
    
    if (use_float) {
        ct_float_t b = s_expr_stack_tofloat(stack, -1);
        ct_float_t a = s_expr_stack_tofloat(stack, -2);
        s_expr_stack_popn(stack, 2);
        s_expr_stack_push_float(stack, (a > b) ? a : b);
    } else {
        ct_int_t b = s_expr_stack_toint(stack, -1);
        ct_int_t a = s_expr_stack_toint(stack, -2);
        s_expr_stack_popn(stack, 2);
        s_expr_stack_push_int(stack, (a > b) ? a : b);
    }
}

// ============================================================================
// TERNARY MATH FUNCTIONS [-3, +1]
// ============================================================================

void se_stack_clamp(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    
    // Check all three are numeric
    if (!s_expr_stack_isnumeric(stack, -1) || 
        !s_expr_stack_isnumeric(stack, -2) || 
        !s_expr_stack_isnumeric(stack, -3)) {
        EXCEPTION("operands must be numeric");
        return;
    }
    
    bool use_float = s_expr_stack_isfloat(stack, -1) || 
                     s_expr_stack_isfloat(stack, -2) || 
                     s_expr_stack_isfloat(stack, -3);
    
    if (use_float) {
        ct_float_t max_val = s_expr_stack_tofloat(stack, -1);
        ct_float_t min_val = s_expr_stack_tofloat(stack, -2);
        ct_float_t val     = s_expr_stack_tofloat(stack, -3);
        s_expr_stack_popn(stack, 3);
        
        if (val < min_val) val = min_val;
        if (val > max_val) val = max_val;
        s_expr_stack_push_float(stack, val);
    } else {
        ct_int_t max_val = s_expr_stack_toint(stack, -1);
        ct_int_t min_val = s_expr_stack_toint(stack, -2);
        ct_int_t val     = s_expr_stack_toint(stack, -3);
        s_expr_stack_popn(stack, 3);
        
        if (val < min_val) val = min_val;
        if (val > max_val) val = max_val;
        s_expr_stack_push_int(stack, val);
    }
}

// ============================================================================
// TYPE CONVERSION [-1, +1]
// ============================================================================

void se_stack_to_int(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    ct_int_t val = s_expr_stack_toint(stack, -1);
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_int(stack, val);
}

void se_stack_to_uint(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    ct_uint_t val = s_expr_stack_touint(stack, -1);
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_uint(stack, val);
}

void se_stack_to_float(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    ct_float_t val = s_expr_stack_tofloat(stack, -1);
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_float(stack, val);
}

// ============================================================================
// CONSTANT PUSH [+1]
// ============================================================================

void se_stack_push_const(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    
    if (param_count < 1) {
        EXCEPTION("push_const requires parameter");
        return;
    }
    
    s_expr_stack_push(stack, &params[0]);
}

// ============================================================================
// IMMEDIATE OPERATIONS [-1, +1]
// Param provides immediate value, operates with stack top
// ============================================================================

// Helper to get immediate value from param as float
static ct_float_t get_imm_float(const s_expr_param_t* p) {
    int type = p->type & S_EXPR_OPCODE_MASK;
    switch (type) {
        case S_EXPR_PARAM_FLOAT: return p->float_val;
        case S_EXPR_PARAM_INT:   return (ct_float_t)p->int_val;
        case S_EXPR_PARAM_UINT:  return (ct_float_t)p->uint_val;
        default: return 0.0;
    }
}

// Helper to get immediate value from param as int
static ct_int_t get_imm_int(const s_expr_param_t* p) {
    int type = p->type & S_EXPR_OPCODE_MASK;
    switch (type) {
        case S_EXPR_PARAM_INT:   return p->int_val;
        case S_EXPR_PARAM_UINT:  return (ct_int_t)p->uint_val;
        case S_EXPR_PARAM_FLOAT: return (ct_int_t)p->float_val;
        default: return 0;
    }
}

// Helper to check if param is float type
static bool is_imm_float(const s_expr_param_t* p) {
    return (p->type & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_FLOAT;
}

void se_stack_addi(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    if (param_count < 1) {
        EXCEPTION("addi requires parameter");
        return;
    }
    
    bool use_float = s_expr_stack_isfloat(stack, -1) || is_imm_float(&params[0]);
    
    if (use_float) {
        ct_float_t a = s_expr_stack_tofloat(stack, -1);
        ct_float_t b = get_imm_float(&params[0]);
        s_expr_stack_popn(stack, 1);
        s_expr_stack_push_float(stack, a + b);
    } else {
        ct_int_t a = s_expr_stack_toint(stack, -1);
        ct_int_t b = get_imm_int(&params[0]);
        s_expr_stack_popn(stack, 1);
        s_expr_stack_push_int(stack, a + b);
    }
}

void se_stack_subi(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    if (param_count < 1) {
        EXCEPTION("subi requires parameter");
        return;
    }
    
    bool use_float = s_expr_stack_isfloat(stack, -1) || is_imm_float(&params[0]);
    
    if (use_float) {
        ct_float_t a = s_expr_stack_tofloat(stack, -1);
        ct_float_t b = get_imm_float(&params[0]);
        s_expr_stack_popn(stack, 1);
        s_expr_stack_push_float(stack, a - b);
    } else {
        ct_int_t a = s_expr_stack_toint(stack, -1);
        ct_int_t b = get_imm_int(&params[0]);
        s_expr_stack_popn(stack, 1);
        s_expr_stack_push_int(stack, a - b);
    }
}

void se_stack_muli(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    if (param_count < 1) {
        EXCEPTION("muli requires parameter");
        return;
    }
    
    bool use_float = s_expr_stack_isfloat(stack, -1) || is_imm_float(&params[0]);
    
    if (use_float) {
        ct_float_t a = s_expr_stack_tofloat(stack, -1);
        ct_float_t b = get_imm_float(&params[0]);
        s_expr_stack_popn(stack, 1);
        s_expr_stack_push_float(stack, a * b);
    } else {
        ct_int_t a = s_expr_stack_toint(stack, -1);
        ct_int_t b = get_imm_int(&params[0]);
        s_expr_stack_popn(stack, 1);
        s_expr_stack_push_int(stack, a * b);
    }
}

void se_stack_divi(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    if (param_count < 1) {
        EXCEPTION("divi requires parameter");
        return;
    }
    
    ct_float_t a = s_expr_stack_tofloat(stack, -1);
    ct_float_t b = get_imm_float(&params[0]);
    
    if (b == 0.0) {
        EXCEPTION("division by zero");
        return;
    }
    
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_float(stack, a / b);
}

void se_stack_modi(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    if (param_count < 1) {
        EXCEPTION("modi requires parameter");
        return;
    }
    
    ct_float_t a = s_expr_stack_tofloat(stack, -1);
    ct_float_t b = get_imm_float(&params[0]);
    
    if (b == 0.0) {
        EXCEPTION("modulo by zero");
        return;
    }
    
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_float(stack, fmod(a, b));
}

void se_stack_shli(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_INTEGER_1();
    
    if (param_count < 1) {
        EXCEPTION("shli requires parameter");
        return;
    }
    
    ct_uint_t a = s_expr_stack_touint(stack, -1);
    ct_int_t shift = get_imm_int(&params[0]);
    
    if (shift < 0 || shift >= (ct_int_t)(sizeof(ct_uint_t) * 8)) {
        EXCEPTION("shift amount out of range");
        return;
    }
    
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_int(stack, (ct_int_t)(a << shift));
}

void se_stack_shri(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_INTEGER_1();
    
    if (param_count < 1) {
        EXCEPTION("shri requires parameter");
        return;
    }
    
    ct_uint_t a = s_expr_stack_touint(stack, -1);
    ct_int_t shift = get_imm_int(&params[0]);
    
    if (shift < 0 || shift >= (ct_int_t)(sizeof(ct_uint_t) * 8)) {
        EXCEPTION("shift amount out of range");
        return;
    }
    
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_int(stack, (ct_int_t)(a >> shift));
}

void se_stack_sari(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_INTEGER_1();
    
    if (param_count < 1) {
        EXCEPTION("sari requires parameter");
        return;
    }
    
    ct_int_t a = s_expr_stack_toint(stack, -1);
    ct_int_t shift = get_imm_int(&params[0]);
    
    if (shift < 0 || shift >= (ct_int_t)(sizeof(ct_int_t) * 8)) {
        EXCEPTION("shift amount out of range");
        return;
    }
    
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_int(stack, a >> shift);
}

void se_stack_bandi(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_INTEGER_1();
    
    if (param_count < 1) {
        EXCEPTION("bandi requires parameter");
        return;
    }
    
    ct_uint_t a = s_expr_stack_touint(stack, -1);
    ct_uint_t b = (ct_uint_t)get_imm_int(&params[0]);
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_int(stack, (ct_int_t)(a & b));
}

void se_stack_bori(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_INTEGER_1();
    
    if (param_count < 1) {
        EXCEPTION("bori requires parameter");
        return;
    }
    
    ct_uint_t a = s_expr_stack_touint(stack, -1);
    ct_uint_t b = (ct_uint_t)get_imm_int(&params[0]);
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_int(stack, (ct_int_t)(a | b));
}

void se_stack_bxori(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_INTEGER_1();
    
    if (param_count < 1) {
        EXCEPTION("bxori requires parameter");
        return;
    }
    
    ct_uint_t a = s_expr_stack_touint(stack, -1);
    ct_uint_t b = (ct_uint_t)get_imm_int(&params[0]);
    s_expr_stack_popn(stack, 1);
    s_expr_stack_push_int(stack, (ct_int_t)(a ^ b));
}

// ============================================================================
// BLACKBOARD FIELD OPERATIONS - LOAD [+1]
// params[0] contains field_offset and field_size
// ============================================================================

void se_stack_load_int(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    
    if (param_count < 1) {
        EXCEPTION("load_int requires field parameter");
        return;
    }
    
    if (!inst->blackboard) {
        EXCEPTION("load_int: NULL blackboard");
        return;
    }
    
    uint16_t offset = params[0].field_offset;
    uint16_t size = params[0].field_size;
    uint8_t* bb = (uint8_t*)inst->blackboard;
    
    ct_int_t val = 0;
    switch (size) {
        case 1: val = *(int8_t*)(bb + offset); break;
        case 2: val = *(int16_t*)(bb + offset); break;
        case 4: val = *(int32_t*)(bb + offset); break;
#if MODULE_IS_64BIT
        case 8: val = *(int64_t*)(bb + offset); break;
#endif
        default:
            EXCEPTION("load_int: invalid field size");
            return;
    }
    
    s_expr_stack_push_int(stack, val);
}

void se_stack_load_uint(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    
    if (param_count < 1) {
        EXCEPTION("load_uint requires field parameter");
        return;
    }
    
    if (!inst->blackboard) {
        EXCEPTION("load_uint: NULL blackboard");
        return;
    }
    
    uint16_t offset = params[0].field_offset;
    uint16_t size = params[0].field_size;
    uint8_t* bb = (uint8_t*)inst->blackboard;
    
    ct_uint_t val = 0;
    switch (size) {
        case 1: val = *(uint8_t*)(bb + offset); break;
        case 2: val = *(uint16_t*)(bb + offset); break;
        case 4: val = *(uint32_t*)(bb + offset); break;
#if MODULE_IS_64BIT
        case 8: val = *(uint64_t*)(bb + offset); break;
#endif
        default:
            EXCEPTION("load_uint: invalid field size");
            return;
    }
    
    s_expr_stack_push_uint(stack, val);
}

void se_stack_load_float(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    
    if (param_count < 1) {
        EXCEPTION("load_float requires field parameter");
        return;
    }
    
    if (!inst->blackboard) {
        EXCEPTION("load_float: NULL blackboard");
        return;
    }
    
    uint16_t offset = params[0].field_offset;
    uint16_t size = params[0].field_size;
    uint8_t* bb = (uint8_t*)inst->blackboard;
    
    ct_float_t val = 0.0;
    switch (size) {
        case 4: val = (ct_float_t)(*(float*)(bb + offset)); break;
#if MODULE_IS_64BIT
        case 8: val = *(double*)(bb + offset); break;
#endif
        default:
            EXCEPTION("load_float: invalid field size");
            return;
    }
    
    s_expr_stack_push_float(stack, val);
}

// ============================================================================
// BLACKBOARD FIELD OPERATIONS - STORE [-1]
// params[0] contains field_offset and field_size
// ============================================================================

void se_stack_store_int(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    if (param_count < 1) {
        EXCEPTION("store_int requires field parameter");
        return;
    }
    
    if (!inst->blackboard) {
        EXCEPTION("store_int: NULL blackboard");
        return;
    }
    
    uint16_t offset = params[0].field_offset;
    uint16_t size = params[0].field_size;
    uint8_t* bb = (uint8_t*)inst->blackboard;
    
    ct_int_t val = s_expr_stack_toint(stack, -1);
    s_expr_stack_popn(stack, 1);
    
    switch (size) {
        case 1: *(int8_t*)(bb + offset) = (int8_t)val; break;
        case 2: *(int16_t*)(bb + offset) = (int16_t)val; break;
        case 4: *(int32_t*)(bb + offset) = (int32_t)val; break;
#if MODULE_IS_64BIT
        case 8: *(int64_t*)(bb + offset) = val; break;
#endif
        default:
            EXCEPTION("store_int: invalid field size");
            return;
    }
}

void se_stack_store_uint(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    if (param_count < 1) {
        EXCEPTION("store_uint requires field parameter");
        return;
    }
    
    if (!inst->blackboard) {
        EXCEPTION("store_uint: NULL blackboard");
        return;
    }
    
    uint16_t offset = params[0].field_offset;
    uint16_t size = params[0].field_size;
    uint8_t* bb = (uint8_t*)inst->blackboard;
    
    ct_uint_t val = s_expr_stack_touint(stack, -1);
    s_expr_stack_popn(stack, 1);
    
    switch (size) {
        case 1: *(uint8_t*)(bb + offset) = (uint8_t)val; break;
        case 2: *(uint16_t*)(bb + offset) = (uint16_t)val; break;
        case 4: *(uint32_t*)(bb + offset) = (uint32_t)val; break;
#if MODULE_IS_64BIT
        case 8: *(uint64_t*)(bb + offset) = val; break;
#endif
        default:
            EXCEPTION("store_uint: invalid field size");
            return;
    }
}

void se_stack_store_float(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    CHECK_NUMERIC_1();
    
    if (param_count < 1) {
        EXCEPTION("store_float requires field parameter");
        return;
    }
    
    if (!inst->blackboard) {
        EXCEPTION("store_float: NULL blackboard");
        return;
    }
    
    uint16_t offset = params[0].field_offset;
    uint16_t size = params[0].field_size;
    uint8_t* bb = (uint8_t*)inst->blackboard;
    
    ct_float_t val = s_expr_stack_tofloat(stack, -1);
    s_expr_stack_popn(stack, 1);
    
    switch (size) {
        case 4: *(float*)(bb + offset) = (float)val; break;
#if MODULE_IS_64BIT
        case 8: *(double*)(bb + offset) = val; break;
#endif
        default:
            EXCEPTION("store_float: invalid field size");
            return;
    }
}

// ============================================================================
// STACK MANIPULATION
// ============================================================================

void se_stack_drop(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    s_expr_stack_popn(stack, 1);
}

void se_stack_drop2(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    s_expr_stack_popn(stack, 2);
}

void se_stack_dropn(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    
    if (param_count < 1) {
        EXCEPTION("dropn requires count parameter");
        return;
    }
    
    uint16_t n = (uint16_t)get_imm_int(&params[0]);
    s_expr_stack_popn(stack, n);
}

void se_stack_dup(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    s_expr_stack_dup(stack);
}

void se_stack_dup2(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    
    // (a b -- a b a b)
    s_expr_stack_pushvalue(stack, -2);  // copy a to top
    s_expr_stack_pushvalue(stack, -2);  // copy b to top (now at -2)
}

void se_stack_swap(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    s_expr_stack_swap(stack);
}

void se_stack_over(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    
    // (a b -- a b a)
    s_expr_stack_pushvalue(stack, -2);
}

void se_stack_rot(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    
    // (a b c -- b c a) : rotate top 3, bringing third to top
    s_expr_stack_rotate(stack, -3, 1);
}

void se_stack_nrot(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    
    // (a b c -- c a b) : reverse rotate, bringing top to third position
    s_expr_stack_rotate(stack, -3, -1);
}

void se_stack_pick(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    
    if (param_count < 1) {
        EXCEPTION("pick requires index parameter");
        return;
    }
    
    // n=0 means top, n=1 means second, etc.
    int n = (int)get_imm_int(&params[0]);
    s_expr_stack_pushvalue(stack, -(n + 1));
}

void se_stack_roll(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    
    if (param_count < 1) {
        EXCEPTION("roll requires count parameter");
        return;
    }
    
    int n = (int)get_imm_int(&params[0]);
    if (n > 0) {
        s_expr_stack_rotate(stack, -n, 1);
    }
}

// ============================================================================
// CONDITIONAL OPERATIONS
// ============================================================================

void se_stack_select(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    
    // (cond a b -- result) : if cond != 0 then a else b
    // Stack: -3=cond, -2=a (true case), -1=b (false case)
    
    if (!s_expr_stack_isnumeric(stack, -3)) {
        EXCEPTION("select: condition must be numeric");
        return;
    }
    
    ct_float_t cond = s_expr_stack_tofloat(stack, -3);
    
    // Get the value we want to keep
    const s_expr_param_t* result;
    if (cond != 0.0) {
        result = s_expr_stack_get(stack, -2);  // true case
    } else {
        result = s_expr_stack_get(stack, -1);  // false case
    }
    
    // Copy result before popping
    s_expr_param_t result_copy = *result;
    
    s_expr_stack_popn(stack, 3);
    s_expr_stack_push(stack, &result_copy);
}

// ============================================================================
// HASH OPERATIONS
// ============================================================================

void se_stack_push_hash(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    
    if (param_count < 1) {
        EXCEPTION("push_hash requires hash parameter");
        return;
    }
    
    s_expr_hash_t hash = params[0].str_hash;
    s_expr_stack_push_hash(stack, hash);
}

void se_stack_hash_eq(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(params); UNUSED(param_count); UNUSED(event_type); UNUSED(event_id); UNUSED(event_data);
    
    GET_STACK(inst);
    
    if (!s_expr_stack_ishash(stack, -1) || !s_expr_stack_ishash(stack, -2)) {
        EXCEPTION("hash_eq: operands must be hashes");
        return;
    }
    
    s_expr_hash_t b = s_expr_stack_tohash(stack, -1);
    s_expr_hash_t a = s_expr_stack_tohash(stack, -2);
    
    s_expr_stack_popn(stack, 2);
    s_expr_stack_push_int(stack, (a == b) ? 1 : 0);
}