// ============================================================================
// se_dict_hash.h
// Hash-Based Dictionary Path Resolution (for json_hash dictionaries)
// 
// Keys are stored as FNV-1a hashes for fastest lookup.
// Paths are pre-computed arrays of hashes - no string parsing at runtime.
//
// Usage:
//   const s_expr_param_t* val = se_dicth_resolve(dict, SE_PATH_H("hw", "gpio"), &ctx);
//   ct_int_t mode = se_dicth_get_int(dict, SE_PATH_H("hw", "gpio", "mode"), 0);
// ============================================================================

#ifndef SE_DICT_HASH_H
#define SE_DICT_HASH_H

#include "s_engine_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// PATH RESOLUTION STATUS
// ============================================================================

typedef enum {
    SE_PATHH_OK = 0,              // Success
    SE_PATHH_NOT_FOUND,           // Key not found at some level
    SE_PATHH_TYPE_MISMATCH,       // Expected dict/array, got scalar value
    SE_PATHH_INVALID_INDEX,       // Array index out of bounds
    SE_PATHH_NULL_DICT,           // Null dictionary pointer passed
    SE_PATHH_NULL_PATH,           // Null path pointer passed
    SE_PATHH_ZERO_DEPTH,          // Zero path depth passed
} se_pathh_status_t;

// ============================================================================
// PATH RESOLUTION CONTEXT
// ============================================================================

typedef struct {
    const s_expr_param_t* result;      // Final resolved param (NULL on error)
    se_pathh_status_t status;          // Resolution status code
    uint16_t depth;                    // Depth reached before failure (0-based)
    s_expr_hash_t failed_hash;         // Hash of failed segment
} se_pathh_context_t;

// Initialize context
static inline void se_pathh_context_init(se_pathh_context_t* ctx) {
    if (ctx) {
        ctx->result = NULL;
        ctx->status = SE_PATHH_OK;
        ctx->depth = 0;
        ctx->failed_hash = 0;
    }
}

// ============================================================================
// KEY HASH MACRO
// ============================================================================

#define SE_KEYH(str) s_expr_hash(str)

// ============================================================================
// PATH CONVENIENCE MACROS
// Usage: se_dicth_resolve(dict, SE_PATH_H("key1", "key2"), &ctx)
// ============================================================================

#define SE_PATH_H(...) \
    ((const s_expr_hash_t[]){ SE_PP_MAP(SE_KEYH, __VA_ARGS__) }), \
    SE_PP_NARG(__VA_ARGS__)

// Preprocessor helpers for SE_PATH_H
#define SE_PP_NARG(...) SE_PP_NARG_(__VA_ARGS__, SE_PP_RSEQ())
#define SE_PP_NARG_(...) SE_PP_ARG_N(__VA_ARGS__)
#define SE_PP_ARG_N(_1,_2,_3,_4,_5,_6,_7,_8,N,...) N
#define SE_PP_RSEQ() 8,7,6,5,4,3,2,1,0

#define SE_PP_MAP(f, ...) SE_PP_MAP_(SE_PP_NARG(__VA_ARGS__), f, __VA_ARGS__)
#define SE_PP_MAP_(N, f, ...) SE_PP_MAP__(N, f, __VA_ARGS__)
#define SE_PP_MAP__(N, f, ...) SE_PP_MAP_##N(f, __VA_ARGS__)
#define SE_PP_MAP_1(f, a) f(a)
#define SE_PP_MAP_2(f, a, ...) f(a), SE_PP_MAP_1(f, __VA_ARGS__)
#define SE_PP_MAP_3(f, a, ...) f(a), SE_PP_MAP_2(f, __VA_ARGS__)
#define SE_PP_MAP_4(f, a, ...) f(a), SE_PP_MAP_3(f, __VA_ARGS__)
#define SE_PP_MAP_5(f, a, ...) f(a), SE_PP_MAP_4(f, __VA_ARGS__)
#define SE_PP_MAP_6(f, a, ...) f(a), SE_PP_MAP_5(f, __VA_ARGS__)
#define SE_PP_MAP_7(f, a, ...) f(a), SE_PP_MAP_6(f, __VA_ARGS__)
#define SE_PP_MAP_8(f, a, ...) f(a), SE_PP_MAP_7(f, __VA_ARGS__)

// Fixed-depth macros (simpler, guaranteed to work)
#define SE_PATH_H1(a)           ((const s_expr_hash_t[]){ SE_KEYH(a) }), 1
#define SE_PATH_H2(a,b)         ((const s_expr_hash_t[]){ SE_KEYH(a), SE_KEYH(b) }), 2
#define SE_PATH_H3(a,b,c)       ((const s_expr_hash_t[]){ SE_KEYH(a), SE_KEYH(b), SE_KEYH(c) }), 3
#define SE_PATH_H4(a,b,c,d)     ((const s_expr_hash_t[]){ SE_KEYH(a), SE_KEYH(b), SE_KEYH(c), SE_KEYH(d) }), 4
#define SE_PATH_H5(a,b,c,d,e)   ((const s_expr_hash_t[]){ SE_KEYH(a), SE_KEYH(b), SE_KEYH(c), SE_KEYH(d), SE_KEYH(e) }), 5

// Array index as hash (for mixed dict/array paths)
#define SE_IDXH(n) se_dicth_index_hash(n)

// ============================================================================
// CORE RESOLUTION
// ============================================================================

// Resolve path using array of pre-computed key hashes
const s_expr_param_t* se_dicth_resolve(
    const s_expr_param_t* dict,
    const s_expr_hash_t* path_hashes,
    uint16_t path_depth,
    se_pathh_context_t* ctx
);

// ============================================================================
// SINGLE-LEVEL LOOKUP
// ============================================================================

// Find key in dictionary by hash
const s_expr_param_t* se_dicth_find(
    const s_expr_param_t* dict,
    s_expr_hash_t key_hash
);

// Get array element by index
const s_expr_param_t* se_dicth_array_get(
    const s_expr_param_t* array,
    uint16_t index
);

// Get array element count
uint16_t se_dicth_array_count(const s_expr_param_t* array);

// Convert numeric index to hash (for array access in paths)
s_expr_hash_t se_dicth_index_hash(uint16_t index);

// ============================================================================
// TYPED VALUE EXTRACTION
// ============================================================================

ct_int_t se_dicth_get_int(
    const s_expr_param_t* dict,
    const s_expr_hash_t* path_hashes,
    uint16_t path_depth,
    ct_int_t default_val
);

ct_uint_t se_dicth_get_uint(
    const s_expr_param_t* dict,
    const s_expr_hash_t* path_hashes,
    uint16_t path_depth,
    ct_uint_t default_val
);

ct_float_t se_dicth_get_float(
    const s_expr_param_t* dict,
    const s_expr_hash_t* path_hashes,
    uint16_t path_depth,
    ct_float_t default_val
);

bool se_dicth_get_bool(
    const s_expr_param_t* dict,
    const s_expr_hash_t* path_hashes,
    uint16_t path_depth,
    bool default_val
);

s_expr_hash_t se_dicth_get_hash(
    const s_expr_param_t* dict,
    const s_expr_hash_t* path_hashes,
    uint16_t path_depth,
    const s_expr_module_def_t* module_def,
    s_expr_hash_t default_val
);
const s_expr_param_t* se_dicth_get_dict(
    const s_expr_param_t* dict,
    const s_expr_hash_t* path_hashes,
    uint16_t path_depth
);

const s_expr_param_t* se_dicth_get_array(
    const s_expr_param_t* dict,
    const s_expr_hash_t* path_hashes,
    uint16_t path_depth
);

const s_expr_param_t* se_dicth_get_callable(
    const s_expr_param_t* dict,
    const s_expr_hash_t* path_hashes,
    uint16_t path_depth
);

bool se_dicth_get_string(
    const s_expr_param_t* dict,
    const s_expr_hash_t* path_hashes,
    uint16_t path_depth,
    uint16_t* out_str_index,
    uint16_t* out_str_len
);

// ============================================================================
// DICTIONARY ITERATION
// ============================================================================

typedef struct {
    const s_expr_param_t* dict;
    const s_expr_param_t* current;
    const s_expr_param_t* dict_end;
    uint16_t index;
} se_dicth_iter_t;

void se_dicth_iter_init(se_dicth_iter_t* iter, const s_expr_param_t* dict);

bool se_dicth_iter_next(
    se_dicth_iter_t* iter,
    s_expr_hash_t* out_key_hash,
    const s_expr_param_t** out_value
);

void se_dicth_iter_reset(se_dicth_iter_t* iter);

// ============================================================================
// ARRAY ITERATION
// ============================================================================

typedef struct {
    const s_expr_param_t* array;
    const s_expr_param_t* current;
    const s_expr_param_t* array_end;
    uint16_t index;
} se_arrayh_iter_t;

void se_arrayh_iter_init(se_arrayh_iter_t* iter, const s_expr_param_t* array);

bool se_arrayh_iter_next(
    se_arrayh_iter_t* iter,
    const s_expr_param_t** out_value,
    uint16_t* out_index
);

void se_arrayh_iter_reset(se_arrayh_iter_t* iter);

// ============================================================================
// PARAM TYPE CHECKS
// ============================================================================

static inline bool se_dicth_is_dict(const s_expr_param_t* p) {
    return p && (p->type & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_OPEN_DICT;
}

static inline bool se_dicth_is_array(const s_expr_param_t* p) {
    return p && (p->type & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_OPEN_ARRAY;
}

static inline bool se_dicth_is_callable(const s_expr_param_t* p) {
    return p && (p->type & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_OPEN_CALL;
}

static inline bool se_dicth_is_scalar(const s_expr_param_t* p) {
    if (!p) return false;
    uint8_t op = p->type & S_EXPR_OPCODE_MASK;
    return op == S_EXPR_PARAM_INT || op == S_EXPR_PARAM_UINT ||
           op == S_EXPR_PARAM_FLOAT || op == S_EXPR_PARAM_STR_HASH;
}

// ============================================================================
// PARAM VALUE EXTRACTION
// ============================================================================

static inline ct_int_t se_dicth_param_int(const s_expr_param_t* p, ct_int_t def) {
    if (!p) return def;
    uint8_t op = p->type & S_EXPR_OPCODE_MASK;
    if (op == S_EXPR_PARAM_INT) return p->int_val;
    if (op == S_EXPR_PARAM_UINT) return (ct_int_t)p->uint_val;
    if (op == S_EXPR_PARAM_FLOAT) return (ct_int_t)p->float_val;
    return def;
}

static inline ct_uint_t se_dicth_param_uint(const s_expr_param_t* p, ct_uint_t def) {
    if (!p) return def;
    uint8_t op = p->type & S_EXPR_OPCODE_MASK;
    if (op == S_EXPR_PARAM_UINT) return p->uint_val;
    if (op == S_EXPR_PARAM_INT) return (ct_uint_t)p->int_val;
    if (op == S_EXPR_PARAM_FLOAT) return (ct_uint_t)p->float_val;
    return def;
}

static inline ct_float_t se_dicth_param_float(const s_expr_param_t* p, ct_float_t def) {
    if (!p) return def;
    uint8_t op = p->type & S_EXPR_OPCODE_MASK;
    if (op == S_EXPR_PARAM_FLOAT) return p->float_val;
    if (op == S_EXPR_PARAM_INT) return (ct_float_t)p->int_val;
    if (op == S_EXPR_PARAM_UINT) return (ct_float_t)p->uint_val;
    return def;
}

static inline bool se_dicth_param_bool(const s_expr_param_t* p, bool def) {
    if (!p) return def;
    uint8_t op = p->type & S_EXPR_OPCODE_MASK;
    if (op == S_EXPR_PARAM_INT) return p->int_val != 0;
    if (op == S_EXPR_PARAM_UINT) return p->uint_val != 0;
    if (op == S_EXPR_PARAM_FLOAT) return p->float_val != 0.0;
    return def;
}

static inline s_expr_hash_t se_dicth_param_hash(const s_expr_param_t* p, s_expr_hash_t def) {
    if (!p) return def;
    if ((p->type & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_STR_HASH) return p->str_hash;
    return def;
}

// ============================================================================
// BLACKBOARD INTEGRATION
// ============================================================================

static inline const s_expr_param_t* se_dicth_from_blackboard(
    const void* blackboard,
    uint16_t field_offset
) {
    if (!blackboard) return NULL;
    const uint64_t* ptr = (const uint64_t*)((const uint8_t*)blackboard + field_offset);
    return (const s_expr_param_t*)(uintptr_t)*ptr;
}

static inline const s_expr_param_t* se_dicth_from_instance(
    const s_expr_tree_instance_t* inst,
    uint16_t field_offset
) {
    if (!inst) return NULL;
    return se_dicth_from_blackboard(inst->blackboard, field_offset);
}

// ============================================================================
// DEBUG HELPERS
// ============================================================================

const char* se_pathh_status_name(se_pathh_status_t status);

#ifdef __cplusplus
}
#endif

#endif // SE_DICT_HASH_H