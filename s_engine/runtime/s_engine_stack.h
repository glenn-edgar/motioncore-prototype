// ============================================================================
// s_engine_stack.h
// Per-Tree Parameter Stack Interface
// ============================================================================

#ifndef S_ENGINE_STACK_H
#define S_ENGINE_STACK_H

#include "s_engine_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GET_STACK(inst) \
    s_expr_stack_t* stack = (inst) ? (inst)->stack : NULL; \
    if (!stack) { EXCEPTION("NULL stack"); return; }

#define CHECK_NUMERIC_2() \
    if (!s_expr_stack_isnumeric(stack, -1) || !s_expr_stack_isnumeric(stack, -2)) { \
        EXCEPTION("operands must be numeric"); return; \
    }

#define CHECK_NUMERIC_1() \
    if (!s_expr_stack_isnumeric(stack, -1)) { \
        EXCEPTION("operand must be numeric"); return; \
    }

#define CHECK_INTEGER_2() \
    if (s_expr_stack_isfloat(stack, -1) || s_expr_stack_isfloat(stack, -2)) { \
        EXCEPTION("operands must be integer"); return; \
    } \
    if (!s_expr_stack_isnumeric(stack, -1) || !s_expr_stack_isnumeric(stack, -2)) { \
        EXCEPTION("operands must be numeric"); return; \
    }

#define CHECK_INTEGER_1() \
    if (s_expr_stack_isfloat(stack, -1)) { \
        EXCEPTION("operand must be integer"); return; \
    } \
    if (!s_expr_stack_isnumeric(stack, -1)) { \
        EXCEPTION("operand must be numeric"); return; \
    }

// ============================================================================
// STACK FRAME STRUCTURE
// ============================================================================

typedef struct {
    uint16_t base_ptr;
    uint16_t num_params;
    uint16_t num_locals;
    uint16_t scratch_base;
} s_expr_stack_frame_t;

#define S_EXPR_MAX_FRAMES 16

struct s_expr_stack {
    s_expr_param_t*       data;      // s_expr_param_t, not s_expr_slot_t
    uint16_t              capacity;
    uint16_t              sp;        // use sp, not top
    
    s_expr_stack_frame_t  frames[S_EXPR_MAX_FRAMES];
    uint8_t               frame_count;
};

// ============================================================================
// TREE INTEGRATION
// ============================================================================

bool s_expr_tree_create_stack(s_expr_tree_instance_t* inst, uint16_t capacity);
void s_expr_tree_free_stack(s_expr_tree_instance_t* inst);
void s_expr_tree_reset_stack(s_expr_tree_instance_t* inst);

// ============================================================================
// INDEX CONVERSION (Lua-style: positive from bottom, negative from top)
// ============================================================================

int s_expr_stack_absindex(s_expr_stack_t* stack, int idx);
const s_expr_param_t* s_expr_stack_get(s_expr_stack_t* stack, int idx);

// ============================================================================
// PUSH OPERATIONS
// ============================================================================

bool s_expr_stack_push(s_expr_stack_t* stack, const s_expr_param_t* param);
bool s_expr_stack_push_int(s_expr_stack_t* stack, ct_int_t val);
bool s_expr_stack_push_uint(s_expr_stack_t* stack, ct_uint_t val);
bool s_expr_stack_push_float(s_expr_stack_t* stack, ct_float_t val);
bool s_expr_stack_push_hash(s_expr_stack_t* stack, s_expr_hash_t hash);
bool s_expr_stack_push_ptr(s_expr_stack_t* stack, void* ptr);
bool s_expr_stack_pushvalue(s_expr_stack_t* stack, int idx);

// ============================================================================
// POP OPERATIONS
// ============================================================================

const s_expr_param_t* s_expr_stack_pop(s_expr_stack_t* stack);
void s_expr_stack_popn(s_expr_stack_t* stack, uint16_t n);

// ============================================================================
// TYPE CHECKING
// ============================================================================

int  s_expr_stack_type(s_expr_stack_t* stack, int idx);
bool s_expr_stack_isint(s_expr_stack_t* stack, int idx);
bool s_expr_stack_isuint(s_expr_stack_t* stack, int idx);
bool s_expr_stack_isfloat(s_expr_stack_t* stack, int idx);
bool s_expr_stack_isnumeric(s_expr_stack_t* stack, int idx);
bool s_expr_stack_ishash(s_expr_stack_t* stack, int idx);
bool s_expr_stack_isptr(s_expr_stack_t* stack, int idx);
bool s_expr_stack_isfield(s_expr_stack_t* stack, int idx);
bool s_expr_stack_isresult(s_expr_stack_t* stack, int idx);

// ============================================================================
// VALUE ACCESSORS
// ============================================================================

ct_int_t      s_expr_stack_toint(s_expr_stack_t* stack, int idx);
ct_uint_t     s_expr_stack_touint(s_expr_stack_t* stack, int idx);
ct_float_t    s_expr_stack_tofloat(s_expr_stack_t* stack, int idx);
s_expr_hash_t s_expr_stack_tohash(s_expr_stack_t* stack, int idx);
void*         s_expr_stack_toptr(s_expr_stack_t* stack, int idx);
ct_float_t    s_expr_stack_tonumber(s_expr_stack_t* stack, int idx);

// ============================================================================
// STACK MANIPULATION
// ============================================================================

uint16_t s_expr_stack_gettop(s_expr_stack_t* stack);
void     s_expr_stack_settop(s_expr_stack_t* stack, int idx);
bool     s_expr_stack_insert(s_expr_stack_t* stack, int idx);
bool     s_expr_stack_remove(s_expr_stack_t* stack, int idx);
bool     s_expr_stack_replace(s_expr_stack_t* stack, int idx);
bool     s_expr_stack_copy(s_expr_stack_t* stack, int from, int to);
bool     s_expr_stack_rotate(s_expr_stack_t* stack, int idx, int n);
bool     s_expr_stack_swap(s_expr_stack_t* stack);
bool     s_expr_stack_dup(s_expr_stack_t* stack);
bool     s_expr_stack_poke(s_expr_stack_t* stack, uint16_t offset, const s_expr_param_t* val);
// ============================================================================
// FRAME OPERATIONS
// ============================================================================

bool s_expr_stack_push_frame(s_expr_stack_t* stack, uint16_t num_params, uint16_t num_locals);
void s_expr_stack_pop_frame(s_expr_stack_t* stack);
s_expr_stack_frame_t* s_expr_stack_current_frame(s_expr_stack_t* stack);
const s_expr_param_t* s_expr_stack_get_local(s_expr_stack_t* stack, uint16_t index);
bool s_expr_stack_set_local(s_expr_stack_t* stack, uint16_t index, const s_expr_param_t* val);
const s_expr_param_t* s_expr_stack_peek_tos(s_expr_stack_t* stack, uint16_t offset);
uint16_t s_expr_stack_scratch_depth(s_expr_stack_t* stack);

#ifdef __cplusplus
}
#endif

#endif // S_ENGINE_STACK_H