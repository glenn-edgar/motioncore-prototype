// ============================================================================
// s_engine_stack.c
// Per-Tree Parameter Stack Implementation
// ============================================================================

#include "s_engine_stack.h"
#include "s_engine_exception.h"
#include <string.h>

// ============================================================================
// TREE INTEGRATION
// ============================================================================

bool s_expr_tree_create_stack(s_expr_tree_instance_t* inst, uint16_t capacity) {
    if (!inst) {
        EXCEPTION("s_expr_tree_create_stack: NULL instance");
        return false;
    }
    
    if (!inst->module) {
        EXCEPTION("s_expr_tree_create_stack: NULL module");
        return false;
    }
    
    if (inst->stack) {
        EXCEPTION("s_expr_tree_create_stack: stack already exists");
        return false;
    }
    
    if (capacity == 0) {
        EXCEPTION("s_expr_tree_create_stack: capacity must be > 0");
        return false;
    }
    
    s_expr_allocator_t* alloc = &inst->module->alloc;
    
    // Allocate stack structure
    s_expr_stack_t* stack = (s_expr_stack_t*)alloc->malloc(
        alloc->ctx, sizeof(s_expr_stack_t)
    );
    if (!stack) {
        EXCEPTION("s_expr_tree_create_stack: failed to allocate stack");
        return false;
    }
    
    // Allocate stack data
    size_t data_size = (size_t)capacity * sizeof(s_expr_param_t);
    stack->data = (s_expr_param_t*)alloc->malloc(alloc->ctx, data_size);
    if (!stack->data) {
        EXCEPTION("s_expr_tree_create_stack: failed to allocate stack data");
        alloc->free(alloc->ctx, stack);
        return false;
    }
    
    stack->capacity = capacity;
    stack->sp = 0;
    
    inst->stack = stack;
    return true;
}

void s_expr_tree_free_stack(s_expr_tree_instance_t* inst) {
    if (!inst || !inst->stack) {
        return;
    }
    
    if (!inst->module) {
        EXCEPTION("s_expr_tree_free_stack: NULL module, cannot free stack");
        return;
    }
    
    s_expr_allocator_t* alloc = &inst->module->alloc;
    
    if (inst->stack->data) {
        alloc->free(alloc->ctx, inst->stack->data);
    }
    alloc->free(alloc->ctx, inst->stack);
    
    inst->stack = NULL;
}

void s_expr_tree_reset_stack(s_expr_tree_instance_t* inst) {
    if (inst && inst->stack) {
        inst->stack->sp = 0;
        inst->stack->frame_count = 0;
    }
}

// ============================================================================
// INDEX CONVERSION
// ============================================================================

int s_expr_stack_absindex(s_expr_stack_t* stack, int idx) {
    if (!stack || stack->sp == 0) return 0;
    
    if (idx > 0) {
        return idx;
    } else if (idx < 0) {
        return (int)stack->sp + idx + 1;
    }
    return 0;
}

static int stack_to_array_idx(s_expr_stack_t* stack, int idx) {
    if (!stack || stack->sp == 0) return -1;
    
    int abs_idx = s_expr_stack_absindex(stack, idx);
    if (abs_idx < 1 || abs_idx > (int)stack->sp) {
        return -1;
    }
    return abs_idx - 1;
}

const s_expr_param_t* s_expr_stack_get(s_expr_stack_t* stack, int idx) {
    int arr_idx = stack_to_array_idx(stack, idx);
    if (arr_idx < 0) return NULL;
    return &stack->data[arr_idx];
}

// ============================================================================
// PUSH OPERATIONS
// ============================================================================

bool s_expr_stack_push(s_expr_stack_t* stack, const s_expr_param_t* param) {
    if (!stack) {
        EXCEPTION("s_expr_stack_push: NULL stack");
        return false;
    }
    if (!param) {
        EXCEPTION("s_expr_stack_push: NULL param");
        return false;
    }
    if (stack->sp >= stack->capacity) {
        EXCEPTION("s_expr_stack_push: overflow");
        return false;
    }
    
    stack->data[stack->sp] = *param;
    stack->sp++;
    return true;
}

bool s_expr_stack_push_int(s_expr_stack_t* stack, ct_int_t val) {
    if (!stack) {
        EXCEPTION("s_expr_stack_push_int: NULL stack");
        return false;
    }
    if (stack->sp >= stack->capacity) {
        EXCEPTION("s_expr_stack_push_int: overflow");
        return false;
    }
    
    s_expr_param_t* p = &stack->data[stack->sp];
    memset(p, 0, sizeof(*p));
    p->type = S_EXPR_PARAM_INT;
    p->int_val = val;
    stack->sp++;
    return true;
}

bool s_expr_stack_push_uint(s_expr_stack_t* stack, ct_uint_t val) {
    if (!stack) {
        EXCEPTION("s_expr_stack_push_uint: NULL stack");
        return false;
    }
    if (stack->sp >= stack->capacity) {
        EXCEPTION("s_expr_stack_push_uint: overflow");
        return false;
    }
    
    s_expr_param_t* p = &stack->data[stack->sp];
    memset(p, 0, sizeof(*p));
    p->type = S_EXPR_PARAM_UINT;
    p->uint_val = val;
    stack->sp++;
    return true;
}

bool s_expr_stack_push_float(s_expr_stack_t* stack, ct_float_t val) {
    if (!stack) {
        EXCEPTION("s_expr_stack_push_float: NULL stack");
        return false;
    }
    if (stack->sp >= stack->capacity) {
        EXCEPTION("s_expr_stack_push_float: overflow");
        return false;
    }
    
    s_expr_param_t* p = &stack->data[stack->sp];
    memset(p, 0, sizeof(*p));
    p->type = S_EXPR_PARAM_FLOAT;
    p->float_val = val;
    stack->sp++;
    return true;
}

bool s_expr_stack_push_hash(s_expr_stack_t* stack, s_expr_hash_t hash) {
    if (!stack) {
        EXCEPTION("s_expr_stack_push_hash: NULL stack");
        return false;
    }
    if (stack->sp >= stack->capacity) {
        EXCEPTION("s_expr_stack_push_hash: overflow");
        return false;
    }
    
    s_expr_param_t* p = &stack->data[stack->sp];
    memset(p, 0, sizeof(*p));
    p->type = S_EXPR_PARAM_STR_HASH;
    p->str_hash = hash;
    stack->sp++;
    return true;
}

bool s_expr_stack_push_ptr(s_expr_stack_t* stack, void* ptr) {
    if (!stack) {
        EXCEPTION("s_expr_stack_push_ptr: NULL stack");
        return false;
    }
    if (stack->sp >= stack->capacity) {
        EXCEPTION("s_expr_stack_push_ptr: overflow");
        return false;
    }
    
    s_expr_param_t* p = &stack->data[stack->sp];
    memset(p, 0, sizeof(*p));
    p->type = S_EXPR_PARAM_SLOT | S_EXPR_FLAG_POINTER;
#if MODULE_IS_64BIT
    p->str_hash = (s_expr_hash_t)(uintptr_t)ptr;
#else
    p->uint_val = (ct_uint_t)(uintptr_t)ptr;
#endif
    stack->sp++;
    return true;
}

bool s_expr_stack_pushvalue(s_expr_stack_t* stack, int idx) {
    if (!stack) {
        EXCEPTION("s_expr_stack_pushvalue: NULL stack");
        return false;
    }
    
    const s_expr_param_t* src = s_expr_stack_get(stack, idx);
    if (!src) {
        EXCEPTION("s_expr_stack_pushvalue: invalid index");
        return false;
    }
    return s_expr_stack_push(stack, src);
}
bool s_expr_stack_poke(s_expr_stack_t* stack, uint16_t offset, const s_expr_param_t* val) {
    if (!stack || !val) return false;
    s_expr_stack_frame_t* frame = &stack->frames[stack->frame_count - 1];
    uint16_t abs_idx = frame->scratch_base + offset;
    if (abs_idx >= stack->capacity) return false;
    stack->data[abs_idx] = *val;
    // Advance sp if needed to cover the scratch slot
    if (abs_idx >= stack->sp) {
        stack->sp = abs_idx + 1;
    }
    return true;
}
// ============================================================================
// POP OPERATIONS
// ============================================================================

const s_expr_param_t* s_expr_stack_pop(s_expr_stack_t* stack) {
    if (!stack) {
        EXCEPTION("s_expr_stack_pop: NULL stack");
        return NULL;
    }
    if (stack->sp == 0) {
        EXCEPTION("s_expr_stack_pop: underflow");
        return NULL;
    }
    
    stack->sp--;
    return &stack->data[stack->sp];
}

void s_expr_stack_popn(s_expr_stack_t* stack, uint16_t n) {
    if (!stack) {
        EXCEPTION("s_expr_stack_popn: NULL stack");
        return;
    }
    
    if (n > stack->sp) {
        stack->sp = 0;
    } else {
        stack->sp -= n;
    }
}

// ============================================================================
// TYPE CHECKING
// ============================================================================

int s_expr_stack_type(s_expr_stack_t* stack, int idx) {
    const s_expr_param_t* p = s_expr_stack_get(stack, idx);
    if (!p) return -1;
    return p->type & S_EXPR_OPCODE_MASK;
}

bool s_expr_stack_isint(s_expr_stack_t* stack, int idx) {
    return s_expr_stack_type(stack, idx) == S_EXPR_PARAM_INT;
}

bool s_expr_stack_isuint(s_expr_stack_t* stack, int idx) {
    return s_expr_stack_type(stack, idx) == S_EXPR_PARAM_UINT;
}

bool s_expr_stack_isfloat(s_expr_stack_t* stack, int idx) {
    return s_expr_stack_type(stack, idx) == S_EXPR_PARAM_FLOAT;
}

bool s_expr_stack_isnumeric(s_expr_stack_t* stack, int idx) {
    int type = s_expr_stack_type(stack, idx);
    return type == S_EXPR_PARAM_INT || 
           type == S_EXPR_PARAM_UINT || 
           type == S_EXPR_PARAM_FLOAT;
}

bool s_expr_stack_ishash(s_expr_stack_t* stack, int idx) {
    return s_expr_stack_type(stack, idx) == S_EXPR_PARAM_STR_HASH;
}

bool s_expr_stack_isptr(s_expr_stack_t* stack, int idx) {
    const s_expr_param_t* p = s_expr_stack_get(stack, idx);
    if (!p) return false;
    return (p->type & S_EXPR_FLAG_POINTER) != 0;
}

bool s_expr_stack_isfield(s_expr_stack_t* stack, int idx) {
    return s_expr_stack_type(stack, idx) == S_EXPR_PARAM_FIELD;
}

bool s_expr_stack_isresult(s_expr_stack_t* stack, int idx) {
    return s_expr_stack_type(stack, idx) == S_EXPR_PARAM_RESULT;
}

// ============================================================================
// VALUE ACCESSORS
// ============================================================================

ct_int_t s_expr_stack_toint(s_expr_stack_t* stack, int idx) {
    const s_expr_param_t* p = s_expr_stack_get(stack, idx);
    if (!p) return 0;
    
    int type = p->type & S_EXPR_OPCODE_MASK;
    switch (type) {
        case S_EXPR_PARAM_INT:
        case S_EXPR_PARAM_RESULT:
            return p->int_val;
        case S_EXPR_PARAM_UINT:
            return (ct_int_t)p->uint_val;
        case S_EXPR_PARAM_FLOAT:
            return (ct_int_t)p->float_val;
        default:
            return 0;
    }
}

ct_uint_t s_expr_stack_touint(s_expr_stack_t* stack, int idx) {
    const s_expr_param_t* p = s_expr_stack_get(stack, idx);
    if (!p) return 0;
    
    int type = p->type & S_EXPR_OPCODE_MASK;
    switch (type) {
        case S_EXPR_PARAM_UINT:
            return p->uint_val;
        case S_EXPR_PARAM_INT:
            return (ct_uint_t)p->int_val;
        case S_EXPR_PARAM_FLOAT:
            return (ct_uint_t)p->float_val;
        default:
            return 0;
    }
}

ct_float_t s_expr_stack_tofloat(s_expr_stack_t* stack, int idx) {
    const s_expr_param_t* p = s_expr_stack_get(stack, idx);
    if (!p) return 0.0f;
    
    int type = p->type & S_EXPR_OPCODE_MASK;
    switch (type) {
        case S_EXPR_PARAM_FLOAT:
            return p->float_val;
        case S_EXPR_PARAM_INT:
            return (ct_float_t)p->int_val;
        case S_EXPR_PARAM_UINT:
            return (ct_float_t)p->uint_val;
        default:
            return 0.0f;
    }
}

s_expr_hash_t s_expr_stack_tohash(s_expr_stack_t* stack, int idx) {
    const s_expr_param_t* p = s_expr_stack_get(stack, idx);
    if (!p) return 0;
    
    if ((p->type & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_STR_HASH) {
        return p->str_hash;
    }
    return 0;
}

void* s_expr_stack_toptr(s_expr_stack_t* stack, int idx) {
    const s_expr_param_t* p = s_expr_stack_get(stack, idx);
    if (!p) return NULL;
    
    if (!(p->type & S_EXPR_FLAG_POINTER)) {
        return NULL;
    }
    
#if MODULE_IS_64BIT
    return (void*)(uintptr_t)p->str_hash;
#else
    return (void*)(uintptr_t)p->uint_val;
#endif
}

ct_float_t s_expr_stack_tonumber(s_expr_stack_t* stack, int idx) {
    return s_expr_stack_tofloat(stack, idx);
}

// ============================================================================
// STACK MANIPULATION
// ============================================================================

uint16_t s_expr_stack_gettop(s_expr_stack_t* stack) {
    return stack ? stack->sp : 0;
}

void s_expr_stack_settop(s_expr_stack_t* stack, int idx) {
    if (!stack) {
        EXCEPTION("s_expr_stack_settop: NULL stack");
        return;
    }
    
    if (idx >= 0) {
        if ((uint16_t)idx < stack->sp) {
            stack->sp = (uint16_t)idx;
        }
    } else {
        int new_top = (int)stack->sp + idx + 1;
        if (new_top < 0) new_top = 0;
        stack->sp = (uint16_t)new_top;
    }
}

bool s_expr_stack_insert(s_expr_stack_t* stack, int idx) {
    if (!stack) {
        EXCEPTION("s_expr_stack_insert: NULL stack");
        return false;
    }
    if (stack->sp == 0) {
        EXCEPTION("s_expr_stack_insert: empty stack");
        return false;
    }
    
    int arr_idx = stack_to_array_idx(stack, idx);
    if (arr_idx < 0) {
        EXCEPTION("s_expr_stack_insert: invalid index");
        return false;
    }
    
    s_expr_param_t top = stack->data[stack->sp - 1];
    
    for (int i = (int)stack->sp - 1; i > arr_idx; i--) {
        stack->data[i] = stack->data[i - 1];
    }
    
    stack->data[arr_idx] = top;
    return true;
}

bool s_expr_stack_remove(s_expr_stack_t* stack, int idx) {
    if (!stack) {
        EXCEPTION("s_expr_stack_remove: NULL stack");
        return false;
    }
    if (stack->sp == 0) {
        EXCEPTION("s_expr_stack_remove: empty stack");
        return false;
    }
    
    int arr_idx = stack_to_array_idx(stack, idx);
    if (arr_idx < 0) {
        EXCEPTION("s_expr_stack_remove: invalid index");
        return false;
    }
    
    for (int i = arr_idx; i < (int)stack->sp - 1; i++) {
        stack->data[i] = stack->data[i + 1];
    }
    
    stack->sp--;
    return true;
}

bool s_expr_stack_replace(s_expr_stack_t* stack, int idx) {
    if (!stack) {
        EXCEPTION("s_expr_stack_replace: NULL stack");
        return false;
    }
    if (stack->sp == 0) {
        EXCEPTION("s_expr_stack_replace: empty stack");
        return false;
    }
    
    int arr_idx = stack_to_array_idx(stack, idx);
    if (arr_idx < 0) {
        EXCEPTION("s_expr_stack_replace: invalid index");
        return false;
    }
    
    stack->data[arr_idx] = stack->data[stack->sp - 1];
    stack->sp--;
    return true;
}

bool s_expr_stack_copy(s_expr_stack_t* stack, int from, int to) {
    if (!stack) {
        EXCEPTION("s_expr_stack_copy: NULL stack");
        return false;
    }
    
    int from_arr = stack_to_array_idx(stack, from);
    int to_arr = stack_to_array_idx(stack, to);
    
    if (from_arr < 0) {
        EXCEPTION("s_expr_stack_copy: invalid 'from' index");
        return false;
    }
    if (to_arr < 0) {
        EXCEPTION("s_expr_stack_copy: invalid 'to' index");
        return false;
    }
    
    stack->data[to_arr] = stack->data[from_arr];
    return true;
}

bool s_expr_stack_rotate(s_expr_stack_t* stack, int idx, int n) {
    if (!stack) {
        EXCEPTION("s_expr_stack_rotate: NULL stack");
        return false;
    }
    if (stack->sp == 0) {
        EXCEPTION("s_expr_stack_rotate: empty stack");
        return false;
    }
    
    int arr_idx = stack_to_array_idx(stack, idx);
    if (arr_idx < 0) {
        EXCEPTION("s_expr_stack_rotate: invalid index");
        return false;
    }
    
    int count = (int)stack->sp - arr_idx;
    if (count <= 1) return true;
    
    n = n % count;
    if (n < 0) n += count;
    if (n == 0) return true;
    
    if (n <= count / 2) {
        for (int r = 0; r < n; r++) {
            s_expr_param_t temp = stack->data[stack->sp - 1];
            for (int i = (int)stack->sp - 1; i > arr_idx; i--) {
                stack->data[i] = stack->data[i - 1];
            }
            stack->data[arr_idx] = temp;
        }
    } else {
        int left_n = count - n;
        for (int r = 0; r < left_n; r++) {
            s_expr_param_t temp = stack->data[arr_idx];
            for (int i = arr_idx; i < (int)stack->sp - 1; i++) {
                stack->data[i] = stack->data[i + 1];
            }
            stack->data[stack->sp - 1] = temp;
        }
    }
    
    return true;
}

bool s_expr_stack_swap(s_expr_stack_t* stack) {
    if (!stack) {
        EXCEPTION("s_expr_stack_swap: NULL stack");
        return false;
    }
    if (stack->sp < 2) {
        EXCEPTION("s_expr_stack_swap: need at least 2 elements");
        return false;
    }
    
    s_expr_param_t temp = stack->data[stack->sp - 1];
    stack->data[stack->sp - 1] = stack->data[stack->sp - 2];
    stack->data[stack->sp - 2] = temp;
    return true;
}

bool s_expr_stack_dup(s_expr_stack_t* stack) {
    if (!stack) {
        EXCEPTION("s_expr_stack_dup: NULL stack");
        return false;
    }
    return s_expr_stack_pushvalue(stack, -1);
}

// ============================================================================
// FRAME OPERATIONS
// ============================================================================

bool s_expr_stack_push_frame(s_expr_stack_t* stack, uint16_t num_params, uint16_t num_locals) {
    if (!stack) {
        EXCEPTION("s_expr_stack_push_frame: NULL stack");
        return false;
    }
    if (stack->frame_count >= S_EXPR_MAX_FRAMES) {
        EXCEPTION("s_expr_stack_push_frame: frame overflow");
        return false;
    }
    if (stack->sp + num_locals > stack->capacity) {
        EXCEPTION("s_expr_stack_push_frame: stack overflow");
        return false;
    }
    if (num_params > stack->sp) {
        EXCEPTION("s_expr_stack_push_frame: not enough params on stack");
        return false;
    }
    
    s_expr_stack_frame_t* frame = &stack->frames[stack->frame_count++];
    frame->base_ptr = stack->sp - num_params;
    frame->num_params = num_params;
    frame->num_locals = num_locals;
    frame->scratch_base = stack->sp + num_locals;
    
    for (uint16_t i = 0; i < num_locals; i++) {
        memset(&stack->data[stack->sp++], 0, sizeof(s_expr_param_t));
    }
    
    return true;
}

void s_expr_stack_pop_frame(s_expr_stack_t* stack) {
    if (!stack || stack->frame_count == 0) return;
    s_expr_stack_frame_t* frame = &stack->frames[--stack->frame_count];
    stack->sp = frame->base_ptr;
}

s_expr_stack_frame_t* s_expr_stack_current_frame(s_expr_stack_t* stack) {
    if (!stack || stack->frame_count == 0) return NULL;
    return &stack->frames[stack->frame_count - 1];
}

const s_expr_param_t* s_expr_stack_get_local(s_expr_stack_t* stack, uint16_t index) {
    if (!stack || stack->frame_count == 0) return NULL;
    s_expr_stack_frame_t* frame = &stack->frames[stack->frame_count - 1];
    if (index >= frame->num_params + frame->num_locals) return NULL;
    return &stack->data[frame->base_ptr + index];
}

bool s_expr_stack_set_local(s_expr_stack_t* stack, uint16_t index, const s_expr_param_t* val) {
    if (!stack || stack->frame_count == 0 || !val) return false;
    s_expr_stack_frame_t* frame = &stack->frames[stack->frame_count - 1];
    if (index >= frame->num_params + frame->num_locals) return false;
    stack->data[frame->base_ptr + index] = *val;
    return true;
}

const s_expr_param_t* s_expr_stack_peek_tos(s_expr_stack_t* stack, uint16_t offset) {
    if (!stack || offset >= stack->sp) return NULL;
    return &stack->data[stack->sp - 1 - offset];
}

uint16_t s_expr_stack_scratch_depth(s_expr_stack_t* stack) {
    if (!stack) return 0;
    if (stack->frame_count == 0) return stack->sp;
    return stack->sp - stack->frames[stack->frame_count - 1].scratch_base;
}