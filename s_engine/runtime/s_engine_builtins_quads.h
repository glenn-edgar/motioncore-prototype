// ============================================================================
// SE_QUAD - Three-address instruction: dest = op(src1, src2)
// params[0] = opcode (uint)
// params[1] = src1
// params[2] = src2 (or null_param for unary ops)
// params[3] = dest
// ============================================================================

// Opcode definitions
#define SE_QUAD_IADD      0x00
#define SE_QUAD_ISUB      0x01
#define SE_QUAD_IMUL      0x02
#define SE_QUAD_IDIV      0x03
#define SE_QUAD_IMOD      0x04
#define SE_QUAD_INEG      0x05

#define SE_QUAD_FADD      0x08
#define SE_QUAD_FSUB      0x09
#define SE_QUAD_FMUL      0x0A
#define SE_QUAD_FDIV      0x0B
#define SE_QUAD_FMOD      0x0C
#define SE_QUAD_FNEG      0x0D

#define SE_QUAD_BIT_AND   0x10
#define SE_QUAD_BIT_OR    0x11
#define SE_QUAD_BIT_XOR   0x12
#define SE_QUAD_BIT_NOT   0x13
#define SE_QUAD_BIT_SHL   0x14
#define SE_QUAD_BIT_SHR   0x15

#define SE_QUAD_ICMP_EQ   0x20
#define SE_QUAD_ICMP_NE   0x21
#define SE_QUAD_ICMP_LT   0x22
#define SE_QUAD_ICMP_LE   0x23
#define SE_QUAD_ICMP_GT   0x24
#define SE_QUAD_ICMP_GE   0x25

#define SE_QUAD_FCMP_EQ   0x28
#define SE_QUAD_FCMP_NE   0x29
#define SE_QUAD_FCMP_LT   0x2A
#define SE_QUAD_FCMP_LE   0x2B
#define SE_QUAD_FCMP_GT   0x2C
#define SE_QUAD_FCMP_GE   0x2D

#define SE_QUAD_LOG_AND   0x30
#define SE_QUAD_LOG_OR    0x31
#define SE_QUAD_LOG_NOT   0x32
#define SE_QUAD_LOG_NAND  0x33
#define SE_QUAD_LOG_NOR   0x34
#define SE_QUAD_LOG_XOR   0x35

#define SE_QUAD_MOVE      0x40

#define SE_QUAD_FSQRT     0x50
#define SE_QUAD_FPOW      0x51
#define SE_QUAD_FEXP      0x52
#define SE_QUAD_FLOG      0x53
#define SE_QUAD_FLOG10    0x54
#define SE_QUAD_FLOG2     0x55
#define SE_QUAD_FABS      0x56

#define SE_QUAD_FSIN      0x58
#define SE_QUAD_FCOS      0x59
#define SE_QUAD_FTAN      0x5A
#define SE_QUAD_FASIN     0x5B
#define SE_QUAD_FACOS     0x5C
#define SE_QUAD_FATAN     0x5D
#define SE_QUAD_FATAN2    0x5E

#define SE_QUAD_FSINH     0x60
#define SE_QUAD_FCOSH     0x61
#define SE_QUAD_FTANH     0x62

#define SE_QUAD_IABS      0x68
#define SE_QUAD_IMIN      0x69
#define SE_QUAD_IMAX      0x6A

#define SE_QUAD_FMIN      0x6C
#define SE_QUAD_FMAX      0x6D
#define SE_QUAD_MOV       0x6E

static ct_int_t quad_read_int(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* p
) {
    uint8_t opcode = p->type & S_EXPR_OPCODE_MASK;
    
    switch (opcode) {
        case S_EXPR_PARAM_INT:
            return p->int_val;
        case S_EXPR_PARAM_UINT:
            return (ct_int_t)p->uint_val;
        case S_EXPR_PARAM_FLOAT:
            return (ct_int_t)p->float_val;
        case S_EXPR_PARAM_FIELD:
            if (inst->blackboard) {
                uint8_t* src = (uint8_t*)inst->blackboard + p->field_offset;
                if (p->field_size == 4) return *(int32_t*)src;
                if (p->field_size == 8) return *(ct_int_t*)src;
            }
            return 0;
        case S_EXPR_PARAM_STACK_TOS: {
            const s_expr_param_t* sp = s_expr_stack_peek_tos(inst->stack, p->stack_offset);
            return sp ? s_expr_param_int(sp) : 0;
        }
        case S_EXPR_PARAM_STACK_LOCAL: {
            const s_expr_param_t* sp = s_expr_stack_get_local(inst->stack, p->stack_offset);
           
            return sp ? s_expr_param_int(sp) : 0;
        }
        case S_EXPR_PARAM_STACK_POP: {
            const s_expr_param_t *sp;
            sp = s_expr_stack_pop(inst->stack);
            return s_expr_param_int(sp);
        }
        case S_EXPR_PARAM_CONST_REF:
            if (inst->module && inst->module->def && p->const_index < inst->module->def->const_count) {
                const void* cdata = inst->module->def->constants[p->const_index];
                if (cdata && p->const_size >= 4) return *(const int32_t*)cdata;
            }
            return 0;
        default:
            return 0;
    }
}

static ct_float_t quad_read_float(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* p
) {
    uint8_t opcode = p->type & S_EXPR_OPCODE_MASK;
    
    switch (opcode) {
        case S_EXPR_PARAM_FLOAT:
            return p->float_val;
        case S_EXPR_PARAM_INT:
            return (ct_float_t)p->int_val;
        case S_EXPR_PARAM_UINT:
            return (ct_float_t)p->uint_val;
        case S_EXPR_PARAM_FIELD:
            if (inst->blackboard) {
                uint8_t* src = (uint8_t*)inst->blackboard + p->field_offset;
                if (p->field_size == 4) return *(float*)src;
                if (p->field_size == 8) return *(double*)src;
            }
            return 0.0f;
        case S_EXPR_PARAM_STACK_TOS: {
            const s_expr_param_t* sp = s_expr_stack_peek_tos(inst->stack, p->stack_offset);
            return sp ? s_expr_param_float(sp) : 0.0f;
        }
        case S_EXPR_PARAM_STACK_LOCAL: {
            const s_expr_param_t* sp = s_expr_stack_get_local(inst->stack, p->stack_offset);
            return sp ? s_expr_param_float(sp) : 0.0f;
        }
        case S_EXPR_PARAM_STACK_POP: {
            const s_expr_param_t *sp;
            sp = s_expr_stack_pop(inst->stack);
            return s_expr_param_float(sp);
        }
        case S_EXPR_PARAM_CONST_REF:
            if (inst->module && inst->module->def && p->const_index < inst->module->def->const_count) {
                const void* cdata = inst->module->def->constants[p->const_index];
                if (cdata && p->const_size == 4) return *(const float*)cdata;
                if (cdata && p->const_size == 8) return (ct_float_t)*(const double*)cdata;
            }
            return 0.0f;
        default:
            return 0.0f;
    }
}
// ============================================================================
// Helper: Write integer result to dest
// ============================================================================
static void quad_write_int(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* dest,
    ct_int_t val
) {
    uint8_t opcode = dest->type & S_EXPR_OPCODE_MASK;
    
    switch (opcode) {
        case S_EXPR_PARAM_FIELD:
            if (inst->blackboard) {
                uint8_t* dst = (uint8_t*)inst->blackboard + dest->field_offset;
                if (dest->field_size == 4) *(int32_t*)dst = (int32_t)val;
                else if (dest->field_size == 8) *(ct_int_t*)dst = val;
            }
            break;
        case S_EXPR_PARAM_STACK_TOS:
        case S_EXPR_PARAM_STACK_LOCAL: {
            s_expr_param_t p;
            memset(&p, 0, sizeof(p));
            p.type = S_EXPR_PARAM_INT;
            p.int_val = val;
            
            if (opcode == S_EXPR_PARAM_STACK_LOCAL) {
                s_expr_stack_set_local(inst->stack, dest->stack_offset, &p);
            } else {
                s_expr_stack_poke(inst->stack, dest->stack_offset, &p);
            }
            break;
        }
        case S_EXPR_PARAM_STACK_PUSH: {
            s_expr_param_t p;
            memset(&p, 0, sizeof(p));
            p.type = S_EXPR_PARAM_INT;
            p.int_val = val;
            s_expr_stack_push(inst->stack, &p);
            break;
        }
        default:
            break;
    }
}
// ============================================================================
// Helper: Write float result to dest
// ============================================================================
static void quad_write_float(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* dest,
    ct_float_t val
) {
    uint8_t opcode = dest->type & S_EXPR_OPCODE_MASK;
    
    switch (opcode) {
        case S_EXPR_PARAM_FIELD:
            if (inst->blackboard) {
                uint8_t* dst = (uint8_t*)inst->blackboard + dest->field_offset;
                if (dest->field_size == 4) *(float*)dst = (float)val;
                else if (dest->field_size == 8) *(double*)dst = (double)val;
            }
            break;
        case S_EXPR_PARAM_STACK_TOS:
        case S_EXPR_PARAM_STACK_LOCAL: {
            s_expr_param_t p;
            memset(&p, 0, sizeof(p));
            p.type = S_EXPR_PARAM_FLOAT;
            p.float_val = val;
            if (opcode == S_EXPR_PARAM_STACK_LOCAL) {
                s_expr_stack_set_local(inst->stack, dest->stack_offset, &p);
            } else {
                s_expr_stack_poke(inst->stack, dest->stack_offset, &p);
            }
            break;
        }
        case S_EXPR_PARAM_STACK_PUSH: {
            s_expr_param_t p;
            memset(&p, 0, sizeof(p));
            p.type = S_EXPR_PARAM_FLOAT;
            p.float_val = val;
            s_expr_stack_push(inst->stack, &p);
            break;
        }
        default:
            break;
    }
}
// ============================================================================
// SE_QUAD Implementation
// ============================================================================
void se_quad(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t*   params,
    uint16_t                param_count,
    s_expr_event_type_t     event_type,
    uint16_t                event_id,
    void*                   event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (!inst || param_count < 4) return;
    
    uint32_t opcode = params[0].uint_val;
    const s_expr_param_t* src1 = &params[1];
    const s_expr_param_t* src2 = &params[2];
    const s_expr_param_t* dest = &params[3];
    
    ct_int_t i1, i2;
    ct_float_t f1, f2;
    
    
    switch (opcode) {
        // Integer Arithmetic
        case SE_QUAD_IADD:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, i1 + i2);
            break;
        case SE_QUAD_ISUB:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, i1 - i2);
            break;
        case SE_QUAD_IMUL:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, i1 * i2);
            break;
        case SE_QUAD_IDIV:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, i2 != 0 ? i1 / i2 : 0);
            break;
        case SE_QUAD_IMOD:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, i2 != 0 ? i1 % i2 : 0);
            break;
        case SE_QUAD_INEG:
            i1 = quad_read_int(inst, src1);
            quad_write_int(inst, dest, -i1);
            break;
            
        // Float Arithmetic
        case SE_QUAD_FADD:
            f1 = quad_read_float(inst, src1);
            f2 = quad_read_float(inst, src2);
            quad_write_float(inst, dest, f1 + f2);
            break;
        case SE_QUAD_FSUB:
            f1 = quad_read_float(inst, src1);
            f2 = quad_read_float(inst, src2);
            quad_write_float(inst, dest, f1 - f2);
            break;
        case SE_QUAD_FMUL:
            f1 = quad_read_float(inst, src1);
            f2 = quad_read_float(inst, src2);
            quad_write_float(inst, dest, f1 * f2);
            break;
        case SE_QUAD_FDIV:
            f1 = quad_read_float(inst, src1);
            f2 = quad_read_float(inst, src2);
            quad_write_float(inst, dest, f2 != 0.0f ? f1 / f2 : 0.0f);
            break;
        case SE_QUAD_FMOD:
            f1 = quad_read_float(inst, src1);
            f2 = quad_read_float(inst, src2);
            quad_write_float(inst, dest, f2 != 0.0f ? fmodf(f1, f2) : 0.0f);
            break;
        case SE_QUAD_FNEG:
            f1 = quad_read_float(inst, src1);
            quad_write_float(inst, dest, -f1);
            break;
            
        // Bitwise
        case SE_QUAD_BIT_AND:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, i1 & i2);
            break;
        case SE_QUAD_BIT_OR:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, i1 | i2);
            break;
        case SE_QUAD_BIT_XOR:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, i1 ^ i2);
            break;
        case SE_QUAD_BIT_NOT:
            i1 = quad_read_int(inst, src1);
            quad_write_int(inst, dest, ~i1);
            break;
        case SE_QUAD_BIT_SHL:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, i1 << i2);
            break;
        case SE_QUAD_BIT_SHR:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, i1 >> i2);
            break;
            
        // Integer Comparison
        case SE_QUAD_ICMP_EQ:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, i1 == i2 ? 1 : 0);
            break;
        case SE_QUAD_ICMP_NE:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, i1 != i2 ? 1 : 0);
            break;
        case SE_QUAD_ICMP_LT:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, i1 < i2 ? 1 : 0);
            break;
        case SE_QUAD_ICMP_LE:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, i1 <= i2 ? 1 : 0);
            break;
        case SE_QUAD_ICMP_GT:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, i1 > i2 ? 1 : 0);
            break;
        case SE_QUAD_ICMP_GE:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, i1 >= i2 ? 1 : 0);
            break;
            
        // Float Comparison
        case SE_QUAD_FCMP_EQ:
            f1 = quad_read_float(inst, src1);
            f2 = quad_read_float(inst, src2);
            quad_write_int(inst, dest, f1 == f2 ? 1 : 0);
            break;
        case SE_QUAD_FCMP_NE:
            f1 = quad_read_float(inst, src1);
            f2 = quad_read_float(inst, src2);
            quad_write_int(inst, dest, f1 != f2 ? 1 : 0);
            break;
        case SE_QUAD_FCMP_LT:
            f1 = quad_read_float(inst, src1);
            f2 = quad_read_float(inst, src2);
            quad_write_int(inst, dest, f1 < f2 ? 1 : 0);
            break;
        case SE_QUAD_FCMP_LE:
            f1 = quad_read_float(inst, src1);
            f2 = quad_read_float(inst, src2);
            quad_write_int(inst, dest, f1 <= f2 ? 1 : 0);
            break;
        case SE_QUAD_FCMP_GT:
            f1 = quad_read_float(inst, src1);
            f2 = quad_read_float(inst, src2);
            quad_write_int(inst, dest, f1 > f2 ? 1 : 0);
            break;
        case SE_QUAD_FCMP_GE:
            f1 = quad_read_float(inst, src1);
            f2 = quad_read_float(inst, src2);
            quad_write_int(inst, dest, f1 >= f2 ? 1 : 0);
            break;
            
        // Logical
        case SE_QUAD_LOG_AND:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, (i1 && i2) ? 1 : 0);
            break;
        case SE_QUAD_LOG_OR:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, (i1 || i2) ? 1 : 0);
            break;
        case SE_QUAD_LOG_NOT:
            i1 = quad_read_int(inst, src1);
            quad_write_int(inst, dest, !i1 ? 1 : 0);
            break;
        case SE_QUAD_LOG_NAND:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, !(i1 && i2) ? 1 : 0);
            break;
        case SE_QUAD_LOG_NOR:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, !(i1 || i2) ? 1 : 0);
            break;
        case SE_QUAD_LOG_XOR:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, (!i1 != !i2) ? 1 : 0);
            break;
            
        // Move
        case SE_QUAD_MOVE:
        case SE_QUAD_MOV:
            i1 = quad_read_int(inst, src1);
            quad_write_int(inst, dest, i1);
            break;
            
        // Float Math Functions
        case SE_QUAD_FSQRT:
            f1 = quad_read_float(inst, src1);
            quad_write_float(inst, dest, sqrtf(f1));
            break;
        case SE_QUAD_FPOW:
            f1 = quad_read_float(inst, src1);
            f2 = quad_read_float(inst, src2);
            quad_write_float(inst, dest, powf(f1, f2));
            break;
        case SE_QUAD_FEXP:
            f1 = quad_read_float(inst, src1);
            quad_write_float(inst, dest, expf(f1));
            break;
        case SE_QUAD_FLOG:
            f1 = quad_read_float(inst, src1);
            quad_write_float(inst, dest, logf(f1));
            break;
        case SE_QUAD_FLOG10:
            f1 = quad_read_float(inst, src1);
            quad_write_float(inst, dest, log10f(f1));
            break;
        case SE_QUAD_FLOG2:
            f1 = quad_read_float(inst, src1);
            quad_write_float(inst, dest, log2f(f1));
            break;
        case SE_QUAD_FABS:
            f1 = quad_read_float(inst, src1);
            quad_write_float(inst, dest, fabsf(f1));
            break;
            
        // Trigonometric
        case SE_QUAD_FSIN:
            f1 = quad_read_float(inst, src1);
            quad_write_float(inst, dest, sinf(f1));
            break;
        case SE_QUAD_FCOS:
            f1 = quad_read_float(inst, src1);
            quad_write_float(inst, dest, cosf(f1));
            break;
        case SE_QUAD_FTAN:
            f1 = quad_read_float(inst, src1);
            quad_write_float(inst, dest, tanf(f1));
            break;
        case SE_QUAD_FASIN:
            f1 = quad_read_float(inst, src1);
            quad_write_float(inst, dest, asinf(f1));
            break;
        case SE_QUAD_FACOS:
            f1 = quad_read_float(inst, src1);
            quad_write_float(inst, dest, acosf(f1));
            break;
        case SE_QUAD_FATAN:
            f1 = quad_read_float(inst, src1);
            quad_write_float(inst, dest, atanf(f1));
            break;
        case SE_QUAD_FATAN2:
            f1 = quad_read_float(inst, src1);
            f2 = quad_read_float(inst, src2);
            quad_write_float(inst, dest, atan2f(f1, f2));
            break;
            
        // Hyperbolic
        case SE_QUAD_FSINH:
            f1 = quad_read_float(inst, src1);
            quad_write_float(inst, dest, sinhf(f1));
            break;
        case SE_QUAD_FCOSH:
            f1 = quad_read_float(inst, src1);
            quad_write_float(inst, dest, coshf(f1));
            break;
        case SE_QUAD_FTANH:
            f1 = quad_read_float(inst, src1);
            quad_write_float(inst, dest, tanhf(f1));
            break;
            
        // Integer Math
        case SE_QUAD_IABS:
            i1 = quad_read_int(inst, src1);
            quad_write_int(inst, dest, i1 < 0 ? -i1 : i1);
            break;
        case SE_QUAD_IMIN:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, i1 < i2 ? i1 : i2);
            break;
        case SE_QUAD_IMAX:
            i1 = quad_read_int(inst, src1);
            i2 = quad_read_int(inst, src2);
            quad_write_int(inst, dest, i1 > i2 ? i1 : i2);
            break;
            
        // Float Min/Max
        case SE_QUAD_FMIN:
            f1 = quad_read_float(inst, src1);
            f2 = quad_read_float(inst, src2);
            quad_write_float(inst, dest, f1 < f2 ? f1 : f2);
            break;
        case SE_QUAD_FMAX:
            f1 = quad_read_float(inst, src1);
            f2 = quad_read_float(inst, src2);
            quad_write_float(inst, dest, f1 > f2 ? f1 : f2);
            break;
            
        default:
            break;
    }
}



static void copy_return_vars_and_pop(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* return_list
) {
    if (!inst || !inst->stack) return;
    
    uint8_t opcode = return_list->type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_OPEN) {
        s_expr_stack_pop_frame(inst->stack);
        return;
    }
    
    uint16_t list_len = return_list->brace_idx;
    const s_expr_param_t* p = return_list + 1;
    const s_expr_param_t* end = return_list + list_len;
    
    // Read return values into temp buffer before destroying frame
    s_expr_param_t temp[16];
    uint16_t return_count = 0;
    
    while (p < end && return_count < 16) {
        uint8_t p_opcode = p->type & S_EXPR_OPCODE_MASK;
        if (p_opcode == S_EXPR_PARAM_UINT || p_opcode == S_EXPR_PARAM_INT) {
            uint16_t local_idx = (uint16_t)p->uint_val;
            const s_expr_param_t* local = s_expr_stack_get_local(inst->stack, local_idx);
            if (local) {
                temp[return_count++] = *local;
            }
        }
        p++;
    }
    
    // Pop frame
    s_expr_stack_pop_frame(inst->stack);
    
    // Push return values from temp buffer
    for (uint16_t i = 0; i < return_count; i++) {
        s_expr_stack_push(inst->stack, &temp[i]);
    }
}

// ============================================================================
// SE_STACK_FRAME_INSTANCE - Stack Frame Management
// 
// Creates a stack frame for local variables and manages frame lifecycle.
// Pushes frame on INIT, pops frame on TERMINATE/DISABLE.
//
// params[0] = num_params (uint) - parameters already on stack
// params[1] = num_locals (uint) - local variable slots to allocate
// params[2] = scratch_depth (uint) - max scratch space (for validation)
// params[3] = list of return var indices to copy back before pop
//
// State machine:
//   0 = not initialized
//   1 = frame active
//
// This node does NOT execute children - it only manages the frame.
// The se_call wrapper uses se_sequence_once to execute body functions.
//
// Lifecycle:
//   INIT      -> push frame, return PIPELINE_CONTINUE
//   TICK      -> return PIPELINE_CONTINUE (frame stays active)
//   TERMINATE -> copy return vars, pop frame, return PIPELINE_CONTINUE
// ============================================================================

static s_expr_result_t se_stack_frame_instance(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (!inst || !inst->stack || param_count < 4) {
        EXCEPTION("SE_STACK_FRAME_INSTANCE: invalid parameters");
        return SE_PIPELINE_DISABLE;
    }
    
    uint16_t num_params = (uint16_t)params[0].uint_val;
    uint16_t num_locals = (uint16_t)params[1].uint_val;
    // params[2] = scratch_depth (compile-time validation only)
    // params[3] = return_vars list
    
    uint8_t state = s_expr_get_state(inst);
    
    // =========================================================================
    // TERMINATE EVENT - copy return vars, pop frame
    // =========================================================================
    if (event_type == SE_EVENT_TERMINATE) {
        if (state == 1) {
            copy_return_vars_and_pop(inst, &params[3]);
        }
        s_expr_set_state(inst, 0);
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // INIT EVENT - validate param count, push frame
    // =========================================================================
    if (event_type == SE_EVENT_INIT) {
        // Pop passed param count from stack (pushed by se_call wrapper)
        const s_expr_param_t* count_param = s_expr_stack_pop(inst->stack);
        if (!count_param) {
            EXCEPTION("SE_STACK_FRAME_INSTANCE: missing param count on stack");
            return SE_PIPELINE_TERMINATE;
        }
        
        uint16_t passed_params = (uint16_t)count_param->uint_val;
        
        if (passed_params != num_params) {
            EXCEPTION("SE_STACK_FRAME_INSTANCE: param mismatch");
            return SE_PIPELINE_TERMINATE;
        }
        
        if (!s_expr_stack_push_frame(inst->stack, num_params, num_locals)) {
            EXCEPTION("SE_STACK_FRAME_INSTANCE: failed to push frame");
            return SE_PIPELINE_TERMINATE;
        }
        
        s_expr_set_state(inst, 1);
        return SE_PIPELINE_CONTINUE;
    }
    
    // =========================================================================
    // TICK EVENT - frame stays active, do nothing
    // =========================================================================
    return SE_PIPELINE_CONTINUE;
}
/*
 * se_quad.c
 * SE_QUAD predicate function implementation
 * 
 * Three-address instruction: dest = op(src1, src2)
 * Returns: predicate true (non-zero dest) or false (zero dest)
 *
 * Parameter layout:
 *   param[0]: uint  - opcode (SE_QUAD_OP enum)
 *   param[1]: any   - src1
 *   param[2]: any   - src2 (null_param for unary ops)
 *   param[3]: any   - dest (must be writable: stack_local or stack_tos)
 */

 // ============================================================================
// SE_QUAD_PRED - Predicate version of SE_QUAD
// Three-address instruction: dest = op(src1, src2)
// Returns: true if dest is non-zero, false if dest is zero
//
// params[0] = opcode (uint)
// params[1] = src1
// params[2] = src2 (or null_param for unary ops)
// params[3] = dest
// ============================================================================

// Bitwise
#define SE_P_QUAD_BIT_AND      0x10
#define SE_P_QUAD_BIT_OR       0x11
#define SE_P_QUAD_BIT_XOR      0x12
#define SE_P_QUAD_BIT_NOT      0x13
#define SE_P_QUAD_BIT_SHL      0x14
#define SE_P_QUAD_BIT_SHR      0x15

// Integer Comparison
#define SE_P_QUAD_ICMP_EQ      0x20
#define SE_P_QUAD_ICMP_NE      0x21
#define SE_P_QUAD_ICMP_LT      0x22
#define SE_P_QUAD_ICMP_LE      0x23
#define SE_P_QUAD_ICMP_GT      0x24
#define SE_P_QUAD_ICMP_GE      0x25

// Float Comparison
#define SE_P_QUAD_FCMP_EQ      0x28
#define SE_P_QUAD_FCMP_NE      0x29
#define SE_P_QUAD_FCMP_LT      0x2A
#define SE_P_QUAD_FCMP_LE      0x2B
#define SE_P_QUAD_FCMP_GT      0x2C
#define SE_P_QUAD_FCMP_GE      0x2D

// Logical
#define SE_P_QUAD_LOG_AND      0x30
#define SE_P_QUAD_LOG_OR       0x31
#define SE_P_QUAD_LOG_NOT      0x32
#define SE_P_QUAD_LOG_NAND     0x33
#define SE_P_QUAD_LOG_NOR      0x34
#define SE_P_QUAD_LOG_XOR      0x35

// Integer Comparison + Accumulate (dest += result)
#define SE_P_QUAD_ICMP_EQ_ACC  0x40
#define SE_P_QUAD_ICMP_NE_ACC  0x41
#define SE_P_QUAD_ICMP_LT_ACC  0x42
#define SE_P_QUAD_ICMP_LE_ACC  0x43
#define SE_P_QUAD_ICMP_GT_ACC  0x44
#define SE_P_QUAD_ICMP_GE_ACC  0x45

// Float Comparison + Accumulate (dest += result)
#define SE_P_QUAD_FCMP_EQ_ACC  0x48
#define SE_P_QUAD_FCMP_NE_ACC  0x49
#define SE_P_QUAD_FCMP_LT_ACC  0x4A
#define SE_P_QUAD_FCMP_LE_ACC  0x4B
#define SE_P_QUAD_FCMP_GT_ACC  0x4C
#define SE_P_QUAD_FCMP_GE_ACC  0x4D

static bool se_p_quad(
    s_expr_tree_instance_t* inst,
    const s_expr_param_t* params,
    uint16_t param_count,
    s_expr_event_type_t event_type,
    uint16_t event_id,
    void* event_data
) {
    UNUSED(event_type);
    UNUSED(event_id);
    UNUSED(event_data);
    
    if (!inst || param_count < 4) return false;
    
    uint32_t opcode = params[0].uint_val;
    const s_expr_param_t* src1 = &params[1];
    const s_expr_param_t* src2 = &params[2];
    const s_expr_param_t* dest = &params[3];
    
    ct_int_t result = 0;
    
    ct_int_t s1_i = quad_read_int(inst, src1);
    ct_int_t s2_i = quad_read_int(inst, src2);
    ct_float_t s1_f = quad_read_float(inst, src1);
    ct_float_t s2_f = quad_read_float(inst, src2);
    
    switch (opcode) {
        // Bitwise
        case SE_P_QUAD_BIT_AND:  result = s1_i & s2_i;                    break;
        case SE_P_QUAD_BIT_OR:   result = s1_i | s2_i;                    break;
        case SE_P_QUAD_BIT_XOR:  result = s1_i ^ s2_i;                    break;
        case SE_P_QUAD_BIT_NOT:  result = ~s1_i;                          break;
        case SE_P_QUAD_BIT_SHL:  result = s1_i << (s2_i & 0x1F);         break;
        case SE_P_QUAD_BIT_SHR:  result = (ct_uint_t)s1_i >> (s2_i & 0x1F); break;
            
        // Integer Comparison
        case SE_P_QUAD_ICMP_EQ:  result = (s1_i == s2_i) ? 1 : 0;        break;
        case SE_P_QUAD_ICMP_NE:  result = (s1_i != s2_i) ? 1 : 0;        break;
        case SE_P_QUAD_ICMP_LT:  result = (s1_i <  s2_i) ? 1 : 0;        break;
        case SE_P_QUAD_ICMP_LE:  result = (s1_i <= s2_i) ? 1 : 0;        break;
        case SE_P_QUAD_ICMP_GT:  result = (s1_i >  s2_i) ? 1 : 0;        break;
        case SE_P_QUAD_ICMP_GE:  result = (s1_i >= s2_i) ? 1 : 0;        break;
            
        // Float Comparison
        case SE_P_QUAD_FCMP_EQ:  result = (s1_f == s2_f) ? 1 : 0;        break;
        case SE_P_QUAD_FCMP_NE:  result = (s1_f != s2_f) ? 1 : 0;        break;
        case SE_P_QUAD_FCMP_LT:  result = (s1_f <  s2_f) ? 1 : 0;        break;
        case SE_P_QUAD_FCMP_LE:  result = (s1_f <= s2_f) ? 1 : 0;        break;
        case SE_P_QUAD_FCMP_GT:  result = (s1_f >  s2_f) ? 1 : 0;        break;
        case SE_P_QUAD_FCMP_GE:  result = (s1_f >= s2_f) ? 1 : 0;        break;
            
        // Logical
        case SE_P_QUAD_LOG_AND:  result = (s1_i && s2_i) ? 1 : 0;        break;
        case SE_P_QUAD_LOG_OR:   result = (s1_i || s2_i) ? 1 : 0;        break;
        case SE_P_QUAD_LOG_NOT:  result = (!s1_i) ? 1 : 0;               break;
        case SE_P_QUAD_LOG_NAND: result = !(s1_i && s2_i) ? 1 : 0;       break;
        case SE_P_QUAD_LOG_NOR:  result = !(s1_i || s2_i) ? 1 : 0;       break;
        case SE_P_QUAD_LOG_XOR:  result = (!s1_i != !s2_i) ? 1 : 0;      break;

// Integer Comparison + Accumulate
        case SE_P_QUAD_ICMP_EQ_ACC: {
            ct_int_t cmp = (s1_i == s2_i) ? 1 : 0;
            quad_write_int(inst, dest, quad_read_int(inst, dest) + cmp);
            return (cmp != 0);
        }
        case SE_P_QUAD_ICMP_NE_ACC: {
            ct_int_t cmp = (s1_i != s2_i) ? 1 : 0;
            quad_write_int(inst, dest, quad_read_int(inst, dest) + cmp);
            return (cmp != 0);
        }
        case SE_P_QUAD_ICMP_LT_ACC: {
            ct_int_t cmp = (s1_i < s2_i) ? 1 : 0;
            quad_write_int(inst, dest, quad_read_int(inst, dest) + cmp);
            return (cmp != 0);
        }
        case SE_P_QUAD_ICMP_LE_ACC: {
            ct_int_t cmp = (s1_i <= s2_i) ? 1 : 0;
            quad_write_int(inst, dest, quad_read_int(inst, dest) + cmp);
            return (cmp != 0);
        }
        case SE_P_QUAD_ICMP_GT_ACC: {
            ct_int_t cmp = (s1_i > s2_i) ? 1 : 0;
            quad_write_int(inst, dest, quad_read_int(inst, dest) + cmp);
            return (cmp != 0);
        }
        case SE_P_QUAD_ICMP_GE_ACC: {
            ct_int_t cmp = (s1_i >= s2_i) ? 1 : 0;
            quad_write_int(inst, dest, quad_read_int(inst, dest) + cmp);
            return (cmp != 0);
        }

        // Float Comparison + Accumulate
        case SE_P_QUAD_FCMP_EQ_ACC: {
            ct_int_t cmp = (s1_f == s2_f) ? 1 : 0;
            quad_write_int(inst, dest, quad_read_int(inst, dest) + cmp);
            return (cmp != 0);
        }
        case SE_P_QUAD_FCMP_NE_ACC: {
            ct_int_t cmp = (s1_f != s2_f) ? 1 : 0;
            quad_write_int(inst, dest, quad_read_int(inst, dest) + cmp);
            return (cmp != 0);
        }
        case SE_P_QUAD_FCMP_LT_ACC: {
            ct_int_t cmp = (s1_f < s2_f) ? 1 : 0;
            quad_write_int(inst, dest, quad_read_int(inst, dest) + cmp);
            return (cmp != 0);
        }
        case SE_P_QUAD_FCMP_LE_ACC: {
            ct_int_t cmp = (s1_f <= s2_f) ? 1 : 0;
            quad_write_int(inst, dest, quad_read_int(inst, dest) + cmp);
            return (cmp != 0);
        }
        case SE_P_QUAD_FCMP_GT_ACC: {
            ct_int_t cmp = (s1_f > s2_f) ? 1 : 0;
            quad_write_int(inst, dest, quad_read_int(inst, dest) + cmp);
            return (cmp != 0);
        }
        case SE_P_QUAD_FCMP_GE_ACC: {
            ct_int_t cmp = (s1_f >= s2_f) ? 1 : 0;
            quad_write_int(inst, dest, quad_read_int(inst, dest) + cmp);
            return (cmp != 0);
        }
            
        default:
            EXCEPTION("SE_P_QUAD: unknown opcode 0x%02X");
            return false;
    }
    
    quad_write_int(inst, dest, result);
    
    return (result != 0);
}
