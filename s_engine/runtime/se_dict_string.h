// ============================================================================
// se_dict_string.h
// String-Based Dictionary Path Resolution (for json dictionaries)
// 
// Keys are stored as string table indices for human-readable debugging.
// Paths are dot-separated strings parsed at runtime.
//
// Usage:
//   const s_expr_param_t* val = se_dicts_resolve(dict, mod, "hw.gpio.mode", &ctx);
//   ct_int_t mode = se_dicts_get_int(dict, mod, "hw.gpio.mode", 0);
// ============================================================================

#ifndef SE_DICT_STRING_H
#define SE_DICT_STRING_H

#include "s_engine_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// PATH RESOLUTION STATUS
// ============================================================================

typedef enum {
    SE_PATHS_OK = 0,              // Success
    SE_PATHS_NOT_FOUND,           // Key not found at some level
    SE_PATHS_TYPE_MISMATCH,       // Expected dict/array, got scalar value
    SE_PATHS_INVALID_INDEX,       // Array index out of bounds or non-numeric
    SE_PATHS_INVALID_PATH,        // Malformed path (empty segment, etc.)
    SE_PATHS_NULL_DICT,           // Null dictionary pointer passed
    SE_PATHS_NULL_PATH,           // Null path string passed
    SE_PATHS_NULL_MODULE,         // Null module_def (needed for string lookup)
} se_paths_status_t;

// ============================================================================
// PATH RESOLUTION CONTEXT
// ============================================================================

typedef struct {
    const s_expr_param_t* result;      // Final resolved param (NULL on error)
    se_paths_status_t status;          // Resolution status code
    uint16_t depth;                    // Depth reached before failure (0-based)
    uint16_t failed_segment_start;     // Offset into path where failure occurred
    uint16_t failed_segment_len;       // Length of failed segment
    s_expr_hash_t failed_hash;         // Hash of failed segment (for debugging)
} se_paths_context_t;

// Initialize context
static inline void se_paths_context_init(se_paths_context_t* ctx) {
    if (ctx) {
        ctx->result = NULL;
        ctx->status = SE_PATHS_OK;
        ctx->depth = 0;
        ctx->failed_segment_start = 0;
        ctx->failed_segment_len = 0;
        ctx->failed_hash = 0;
    }
}

// ============================================================================
// HASH COMPUTATION HELPER
// ============================================================================

static inline s_expr_hash_t se_dicts_hash_segment(const char* str, uint16_t len) {
    s_expr_hash_t hash = FNV_OFFSET;
    for (uint16_t i = 0; i < len; i++) {
        hash ^= (uint8_t)str[i];
        hash *= FNV_PRIME;
    }
    return hash;
}

// ============================================================================
// CORE RESOLUTION
// ============================================================================

// Resolve dot-separated path string
// module_def required for string table lookup
const s_expr_param_t* se_dicts_resolve(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path,
    se_paths_context_t* ctx
);

// ============================================================================
// SINGLE-LEVEL LOOKUP
// ============================================================================

// Find key in dictionary by string comparison
const s_expr_param_t* se_dicts_find(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* key,
    uint16_t key_len
);

// Find key in dictionary by hash (fallback for hash-keyed dicts)
const s_expr_param_t* se_dicts_find_hash(
    const s_expr_param_t* dict,
    s_expr_hash_t key_hash
);

// Get array element by index
const s_expr_param_t* se_dicts_array_get(
    const s_expr_param_t* array,
    uint16_t index
);

// Get array element count
uint16_t se_dicts_array_count(const s_expr_param_t* array);

// ============================================================================
// PATH SEGMENT UTILITIES
// ============================================================================

// Check if segment is numeric (for array indexing)
// Returns true if numeric, fills out_index with parsed value
bool se_dicts_is_numeric(const char* segment, uint16_t len, uint16_t* out_index);

// ============================================================================
// TYPED VALUE EXTRACTION
// ============================================================================

ct_int_t se_dicts_get_int(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path,
    ct_int_t default_val
);

ct_uint_t se_dicts_get_uint(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path,
    ct_uint_t default_val
);

ct_float_t se_dicts_get_float(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path,
    ct_float_t default_val
);

bool se_dicts_get_bool(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path,
    bool default_val
);

s_expr_hash_t se_dicts_get_hash(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path,
    s_expr_hash_t default_val
);

const s_expr_param_t* se_dicts_get_dict(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path
);

const s_expr_param_t* se_dicts_get_array(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path
);

const s_expr_param_t* se_dicts_get_callable(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path
);

bool se_dicts_get_string(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path,
    uint16_t* out_str_index,
    uint16_t* out_str_len
);

// Get actual string pointer (requires module_def for string table)
const char* se_dicts_get_string_ptr(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path
);

// ============================================================================
// DICTIONARY ITERATION
// ============================================================================

typedef struct {
    const s_expr_param_t* dict;
    const s_expr_param_t* current;
    const s_expr_param_t* dict_end;
    const s_expr_module_def_t* module_def;
    uint16_t index;
} se_dicts_iter_t;

void se_dicts_iter_init(
    se_dicts_iter_t* iter,
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def
);

// Get next key-value pair
// out_key_str receives pointer to key string (from string table)
// out_key_hash receives key hash
// out_value receives pointer to value param
bool se_dicts_iter_next(
    se_dicts_iter_t* iter,
    const char** out_key_str,
    s_expr_hash_t* out_key_hash,
    const s_expr_param_t** out_value
);

void se_dicts_iter_reset(se_dicts_iter_t* iter);

// ============================================================================
// ARRAY ITERATION
// ============================================================================

typedef struct {
    const s_expr_param_t* array;
    const s_expr_param_t* current;
    const s_expr_param_t* array_end;
    uint16_t index;
} se_arrays_iter_t;

void se_arrays_iter_init(se_arrays_iter_t* iter, const s_expr_param_t* array);

bool se_arrays_iter_next(
    se_arrays_iter_t* iter,
    const s_expr_param_t** out_value,
    uint16_t* out_index
);

void se_arrays_iter_reset(se_arrays_iter_t* iter);

// ============================================================================
// PARAM TYPE CHECKS
// ============================================================================

static inline bool se_dicts_is_dict(const s_expr_param_t* p) {
    return p && (p->type & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_OPEN_DICT;
}

static inline bool se_dicts_is_array(const s_expr_param_t* p) {
    return p && (p->type & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_OPEN_ARRAY;
}

static inline bool se_dicts_is_callable(const s_expr_param_t* p) {
    return p && (p->type & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_OPEN_CALL;
}

// ============================================================================
// PARAM VALUE EXTRACTION
// ============================================================================

static inline ct_int_t se_dicts_param_int(const s_expr_param_t* p, ct_int_t def) {
    if (!p) return def;
    uint8_t op = p->type & S_EXPR_OPCODE_MASK;
    if (op == S_EXPR_PARAM_INT) return p->int_val;
    if (op == S_EXPR_PARAM_UINT) return (ct_int_t)p->uint_val;
    if (op == S_EXPR_PARAM_FLOAT) return (ct_int_t)p->float_val;
    return def;
}

static inline ct_uint_t se_dicts_param_uint(const s_expr_param_t* p, ct_uint_t def) {
    if (!p) return def;
    uint8_t op = p->type & S_EXPR_OPCODE_MASK;
    if (op == S_EXPR_PARAM_UINT) return p->uint_val;
    if (op == S_EXPR_PARAM_INT) return (ct_uint_t)p->int_val;
    if (op == S_EXPR_PARAM_FLOAT) return (ct_uint_t)p->float_val;
    return def;
}

static inline ct_float_t se_dicts_param_float(const s_expr_param_t* p, ct_float_t def) {
    if (!p) return def;
    uint8_t op = p->type & S_EXPR_OPCODE_MASK;
    if (op == S_EXPR_PARAM_FLOAT) return p->float_val;
    if (op == S_EXPR_PARAM_INT) return (ct_float_t)p->int_val;
    if (op == S_EXPR_PARAM_UINT) return (ct_float_t)p->uint_val;
    return def;
}

static inline bool se_dicts_param_bool(const s_expr_param_t* p, bool def) {
    if (!p) return def;
    uint8_t op = p->type & S_EXPR_OPCODE_MASK;
    if (op == S_EXPR_PARAM_INT) return p->int_val != 0;
    if (op == S_EXPR_PARAM_UINT) return p->uint_val != 0;
    if (op == S_EXPR_PARAM_FLOAT) return p->float_val != 0.0;
    return def;
}

static inline s_expr_hash_t se_dicts_param_hash(const s_expr_param_t* p, s_expr_hash_t def) {
    if (!p) return def;
    if ((p->type & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_STR_HASH) return p->str_hash;
    return def;
}

// ============================================================================
// BLACKBOARD INTEGRATION
// ============================================================================

static inline const s_expr_param_t* se_dicts_from_blackboard(
    const void* blackboard,
    uint16_t field_offset
) {
    if (!blackboard) return NULL;
    const uint64_t* ptr = (const uint64_t*)((const uint8_t*)blackboard + field_offset);
    return (const s_expr_param_t*)(uintptr_t)*ptr;
}

static inline const s_expr_param_t* se_dicts_from_instance(
    const s_expr_tree_instance_t* inst,
    uint16_t field_offset
) {
    if (!inst) return NULL;
    return se_dicts_from_blackboard(inst->blackboard, field_offset);
}

// ============================================================================
// DEBUG HELPERS
// ============================================================================

const char* se_paths_status_name(se_paths_status_t status);

#ifdef __cplusplus
}
#endif

#endif // SE_DICT_STRING_H