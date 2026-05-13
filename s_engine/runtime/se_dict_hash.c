// ============================================================================
// se_dict_hash.c (FIXED)
// Hash-Based Dictionary Implementation
// ============================================================================

#include "se_dict_hash.h"
#include <string.h>

// ============================================================================
// INDEX TO HASH CONVERSION
// ============================================================================

static const s_expr_hash_t index_hash_table[16] = {
    0x350CA8AF,  // hash("0")
    0x340CA71C,  // hash("1")
    0x370CABD5,  // hash("2")
    0x360CAA42,  // hash("3")
    0x310CA263,  // hash("4")
    0x300CA0D0,  // hash("5")
    0x330CA589,  // hash("6")
    0x320CA3F6,  // hash("7")
    0x3D0CB547,  // hash("8")
    0x3C0CB3B4,  // hash("9")
    0x1BEB2A44,  // hash("10")
    0x1CEB2BD7,  // hash("11")
    0x1DEB2D6A,  // hash("12")
    0x1EEB2EFD,  // hash("13")
    0x17EB23F8,  // hash("14")
    0x18EB258B,  // hash("15")
};
s_expr_hash_t se_dicth_index_hash(uint16_t index) {
    if (index < 16) {
        return index_hash_table[index];
    }
    
    char buf[8];
    int len = 0;
    uint16_t n = index;
    
    do {
        buf[len++] = '0' + (n % 10);
        n /= 10;
    } while (n > 0 && len < 7);
    
    s_expr_hash_t hash = FNV_OFFSET;
    for (int i = len - 1; i >= 0; i--) {
        hash ^= (uint8_t)buf[i];
        hash *= FNV_PRIME;
    }
    return hash;
}

// ============================================================================
// HELPER: Skip a single value (handles nested structures)
// Returns pointer to next param after the value
// ============================================================================

static const s_expr_param_t* skip_value(const s_expr_param_t* p, const s_expr_param_t* end) {
    if (!p || p >= end) return NULL;
    
    uint8_t opcode = p->type & S_EXPR_OPCODE_MASK;
    
    // If it's an open token with brace_idx, use it to skip
    if (opcode == S_EXPR_PARAM_OPEN_DICT ||
        opcode == S_EXPR_PARAM_OPEN_ARRAY ||
        opcode == S_EXPR_PARAM_OPEN_TUPLE ||
        opcode == S_EXPR_PARAM_OPEN ||
        opcode == S_EXPR_PARAM_OPEN_CALL) {
        // brace_idx points to matching close
        return p + p->brace_idx + 1;
    }
    
    // Simple value - just advance by one
    return p + 1;
}

// ============================================================================
// SINGLE-LEVEL DICTIONARY LOOKUP
// 
// Dictionary structure:
//   OPEN_DICT (brace_idx points to CLOSE_DICT)
//     OPEN_KEY (str_hash = key hash)
//       <value>
//     CLOSE_KEY
//     OPEN_KEY ...
//     ...
//   CLOSE_DICT
// ============================================================================

const s_expr_param_t* se_dicth_find(
    const s_expr_param_t* dict,
    s_expr_hash_t key_hash
) {
    if (!dict) return NULL;
    
    uint8_t opcode = dict->type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_OPEN_DICT) return NULL;
    
    // Get dictionary end using brace_idx on OPEN_DICT
    const s_expr_param_t* dict_end = dict + dict->brace_idx;
    const s_expr_param_t* p = dict + 1;  // Skip OPEN_DICT
    
    while (p < dict_end) {
        opcode = p->type & S_EXPR_OPCODE_MASK;
        
        if (opcode == S_EXPR_PARAM_OPEN_KEY) {
            // Key hash is in str_hash field
            s_expr_hash_t this_key = p->str_hash;
            
            // Value is next param after OPEN_KEY
            const s_expr_param_t* value = p + 1;
            
            if (this_key == key_hash) {
                // Found it - return pointer to value
                return value;
            }
            
            // Skip the value
            p = skip_value(value, dict_end);
            if (!p) return NULL;
            
            // Now p should point to CLOSE_KEY - skip it
            if (p < dict_end && (p->type & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_CLOSE_KEY) {
                p++;
            }
        }
        else if (opcode == S_EXPR_PARAM_CLOSE_DICT) {
            break;
        }
        else {
            // Unexpected - skip it
            p++;
        }
    }
    
    return NULL;
}

// ============================================================================
// ARRAY ACCESS
// ============================================================================

uint16_t se_dicth_array_count(const s_expr_param_t* array) {
    if (!array) return 0;
    
    uint8_t opcode = array->type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_OPEN_ARRAY) return 0;
    
    const s_expr_param_t* array_end = array + array->brace_idx;
    const s_expr_param_t* p = array + 1;
    
    uint16_t count = 0;
    while (p < array_end) {
        opcode = p->type & S_EXPR_OPCODE_MASK;
        
        if (opcode == S_EXPR_PARAM_CLOSE_ARRAY) {
            break;
        }
        
        // Skip this element
        p = skip_value(p, array_end);
        if (!p) break;
        
        count++;
    }
    
    return count;
}

const s_expr_param_t* se_dicth_array_get(
    const s_expr_param_t* array,
    uint16_t index
) {
    if (!array) return NULL;
    
    uint8_t opcode = array->type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_OPEN_ARRAY) return NULL;
    
    const s_expr_param_t* array_end = array + array->brace_idx;
    const s_expr_param_t* p = array + 1;
    
    uint16_t current = 0;
    while (p < array_end) {
        opcode = p->type & S_EXPR_OPCODE_MASK;
        
        if (opcode == S_EXPR_PARAM_CLOSE_ARRAY) {
            break;
        }
        
        if (current == index) {
            return p;
        }
        
        // Skip this element
        p = skip_value(p, array_end);
        if (!p) break;
        
        current++;
    }
    
    return NULL;
}

// ============================================================================
// PATH RESOLUTION
// ============================================================================

const s_expr_param_t* se_dicth_resolve(
    const s_expr_param_t* dict,
    const s_expr_hash_t* path_hashes,
    uint16_t path_depth,
    se_pathh_context_t* ctx
) {
    if (ctx) {
        ctx->result = NULL;
        ctx->status = SE_PATHH_OK;
        ctx->depth = 0;
        ctx->failed_hash = 0;
    }
    
    if (!dict) {
        if (ctx) ctx->status = SE_PATHH_NULL_DICT;
        return NULL;
    }
    if (!path_hashes) {
        if (ctx) ctx->status = SE_PATHH_NULL_PATH;
        return NULL;
    }
    if (path_depth == 0) {
        if (ctx) ctx->status = SE_PATHH_ZERO_DEPTH;
        return NULL;
    }
    
    const s_expr_param_t* current = dict;
    
    for (uint16_t i = 0; i < path_depth; i++) {
        if (!current) {
            if (ctx) {
                ctx->status = SE_PATHH_NOT_FOUND;
                ctx->depth = i;
                ctx->failed_hash = path_hashes[i];
            }
            return NULL;
        }
        
        uint8_t opcode = current->type & S_EXPR_OPCODE_MASK;
        
        if (opcode == S_EXPR_PARAM_OPEN_DICT) {
            current = se_dicth_find(current, path_hashes[i]);
            if (!current) {
                if (ctx) {
                    ctx->status = SE_PATHH_NOT_FOUND;
                    ctx->depth = i;
                    ctx->failed_hash = path_hashes[i];
                }
                return NULL;
            }
        }
        else if (opcode == S_EXPR_PARAM_OPEN_ARRAY) {
            uint16_t index = UINT16_MAX;
            
            for (uint16_t idx = 0; idx < 256; idx++) {
                if (se_dicth_index_hash(idx) == path_hashes[i]) {
                    index = idx;
                    break;
                }
            }
            
            if (index == UINT16_MAX) {
                if (ctx) {
                    ctx->status = SE_PATHH_INVALID_INDEX;
                    ctx->depth = i;
                    ctx->failed_hash = path_hashes[i];
                }
                return NULL;
            }
            
            current = se_dicth_array_get(current, index);
            if (!current) {
                if (ctx) {
                    ctx->status = SE_PATHH_INVALID_INDEX;
                    ctx->depth = i;
                    ctx->failed_hash = path_hashes[i];
                }
                return NULL;
            }
        }
        else {
            if (ctx) {
                ctx->status = SE_PATHH_TYPE_MISMATCH;
                ctx->depth = i;
                ctx->failed_hash = path_hashes[i];
            }
            return NULL;
        }
    }
    
    if (ctx) {
        ctx->result = current;
        ctx->status = SE_PATHH_OK;
        ctx->depth = path_depth;
    }
    
    return current;
}

// ============================================================================
// TYPED VALUE EXTRACTION
// ============================================================================

ct_int_t se_dicth_get_int(
    const s_expr_param_t* dict,
    const s_expr_hash_t* path_hashes,
    uint16_t path_depth,
    ct_int_t default_val
) {
    const s_expr_param_t* p = se_dicth_resolve(dict, path_hashes, path_depth, NULL);
    return se_dicth_param_int(p, default_val);
}

ct_uint_t se_dicth_get_uint(
    const s_expr_param_t* dict,
    const s_expr_hash_t* path_hashes,
    uint16_t path_depth,
    ct_uint_t default_val
) {
    const s_expr_param_t* p = se_dicth_resolve(dict, path_hashes, path_depth, NULL);
    return se_dicth_param_uint(p, default_val);
}

ct_float_t se_dicth_get_float(
    const s_expr_param_t* dict,
    const s_expr_hash_t* path_hashes,
    uint16_t path_depth,
    ct_float_t default_val
) {
    const s_expr_param_t* p = se_dicth_resolve(dict, path_hashes, path_depth, NULL);
    return se_dicth_param_float(p, default_val);
}

bool se_dicth_get_bool(
    const s_expr_param_t* dict,
    const s_expr_hash_t* path_hashes,
    uint16_t path_depth,
    bool default_val
) {
    const s_expr_param_t* p = se_dicth_resolve(dict, path_hashes, path_depth, NULL);
    return se_dicth_param_bool(p, default_val);
}

s_expr_hash_t se_dicth_get_hash(
    const s_expr_param_t* dict,
    const s_expr_hash_t* path_hashes,
    uint16_t path_depth,
    const s_expr_module_def_t* module_def,
    s_expr_hash_t default_val
) {
    se_pathh_context_t ctx;
    se_pathh_context_init(&ctx);
    
    const s_expr_param_t* value = se_dicth_resolve(dict, path_hashes, path_depth, &ctx);
    if (!value) return default_val;
    
    uint8_t opcode = value->type & S_EXPR_OPCODE_MASK;
    
    // Already a hash
    if (opcode == S_EXPR_PARAM_STR_HASH) {
        return value->str_hash;
    }
    
    // String index - look up and hash
    if (opcode == S_EXPR_PARAM_STR_IDX && module_def) {
        if (value->str_index < module_def->string_count) {
            const char* str = module_def->string_table[value->str_index];
            if (str) {
                return s_expr_hash(str);
            }
        }
    }
    
    // Integer - return as hash
    if (opcode == S_EXPR_PARAM_INT || opcode == S_EXPR_PARAM_UINT) {
        return (s_expr_hash_t)value->uint_val;
    }
    
    return default_val;
}

const s_expr_param_t* se_dicth_get_dict(
    const s_expr_param_t* dict,
    const s_expr_hash_t* path_hashes,
    uint16_t path_depth
) {
    const s_expr_param_t* p = se_dicth_resolve(dict, path_hashes, path_depth, NULL);
    if (p && se_dicth_is_dict(p)) return p;
    return NULL;
}

const s_expr_param_t* se_dicth_get_array(
    const s_expr_param_t* dict,
    const s_expr_hash_t* path_hashes,
    uint16_t path_depth
) {
    const s_expr_param_t* p = se_dicth_resolve(dict, path_hashes, path_depth, NULL);
    if (p && se_dicth_is_array(p)) return p;
    return NULL;
}

const s_expr_param_t* se_dicth_get_callable(
    const s_expr_param_t* dict,
    const s_expr_hash_t* path_hashes,
    uint16_t path_depth
) {
    const s_expr_param_t* p = se_dicth_resolve(dict, path_hashes, path_depth, NULL);
    if (p && se_dicth_is_callable(p)) return p;
    return NULL;
}

bool se_dicth_get_string(
    const s_expr_param_t* dict,
    const s_expr_hash_t* path_hashes,
    uint16_t path_depth,
    uint16_t* out_str_index,
    uint16_t* out_str_len
) {
    const s_expr_param_t* p = se_dicth_resolve(dict, path_hashes, path_depth, NULL);
    if (!p) return false;
    
    if ((p->type & S_EXPR_OPCODE_MASK) != S_EXPR_PARAM_STR_IDX) return false;
    
    if (out_str_index) *out_str_index = p->str_index;
    if (out_str_len) *out_str_len = p->str_len;
    return true;
}

// ============================================================================
// DICTIONARY ITERATION
// ============================================================================

void se_dicth_iter_init(se_dicth_iter_t* iter, const s_expr_param_t* dict) {
    if (!iter) return;
    
    iter->dict = dict;
    iter->index = 0;
    
    if (!dict || (dict->type & S_EXPR_OPCODE_MASK) != S_EXPR_PARAM_OPEN_DICT) {
        iter->current = NULL;
        iter->dict_end = NULL;
        return;
    }
    
    iter->dict_end = dict + dict->brace_idx;
    iter->current = dict + 1;
}

bool se_dicth_iter_next(
    se_dicth_iter_t* iter,
    s_expr_hash_t* out_key_hash,
    const s_expr_param_t** out_value
) {
    if (!iter || !iter->current || iter->current >= iter->dict_end) {
        return false;
    }
    
    while (iter->current < iter->dict_end) {
        uint8_t opcode = iter->current->type & S_EXPR_OPCODE_MASK;
        
        if (opcode == S_EXPR_PARAM_OPEN_KEY) {
            if (out_key_hash) *out_key_hash = iter->current->str_hash;
            
            const s_expr_param_t* value = iter->current + 1;
            if (out_value) *out_value = value;
            
            // Skip value
            const s_expr_param_t* after_value = skip_value(value, iter->dict_end);
            if (after_value && (after_value->type & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_CLOSE_KEY) {
                iter->current = after_value + 1;
            } else {
                iter->current = after_value ? after_value : iter->dict_end;
            }
            
            iter->index++;
            return true;
        }
        else if (opcode == S_EXPR_PARAM_CLOSE_DICT) {
            break;
        }
        else {
            iter->current++;
        }
    }
    
    return false;
}

void se_dicth_iter_reset(se_dicth_iter_t* iter) {
    if (!iter || !iter->dict) return;
    iter->current = iter->dict + 1;
    iter->index = 0;
}

// ============================================================================
// ARRAY ITERATION
// ============================================================================

void se_arrayh_iter_init(se_arrayh_iter_t* iter, const s_expr_param_t* array) {
    if (!iter) return;
    
    iter->array = array;
    iter->index = 0;
    
    if (!array || (array->type & S_EXPR_OPCODE_MASK) != S_EXPR_PARAM_OPEN_ARRAY) {
        iter->current = NULL;
        iter->array_end = NULL;
        return;
    }
    
    iter->array_end = array + array->brace_idx;
    iter->current = array + 1;
}

bool se_arrayh_iter_next(
    se_arrayh_iter_t* iter,
    const s_expr_param_t** out_value,
    uint16_t* out_index
) {
    if (!iter || !iter->current || iter->current >= iter->array_end) {
        return false;
    }
    
    uint8_t opcode = iter->current->type & S_EXPR_OPCODE_MASK;
    
    if (opcode == S_EXPR_PARAM_CLOSE_ARRAY) {
        return false;
    }
    
    if (out_value) *out_value = iter->current;
    if (out_index) *out_index = iter->index;
    
    iter->current = skip_value(iter->current, iter->array_end);
    if (!iter->current) iter->current = iter->array_end;
    
    iter->index++;
    
    return true;
}

void se_arrayh_iter_reset(se_arrayh_iter_t* iter) {
    if (!iter || !iter->array) return;
    iter->current = iter->array + 1;
    iter->index = 0;
}

// ============================================================================
// DEBUG HELPERS
// ============================================================================

const char* se_pathh_status_name(se_pathh_status_t status) {
    switch (status) {
        case SE_PATHH_OK:            return "OK";
        case SE_PATHH_NOT_FOUND:     return "NOT_FOUND";
        case SE_PATHH_TYPE_MISMATCH: return "TYPE_MISMATCH";
        case SE_PATHH_INVALID_INDEX: return "INVALID_INDEX";
        case SE_PATHH_NULL_DICT:     return "NULL_DICT";
        case SE_PATHH_NULL_PATH:     return "NULL_PATH";
        case SE_PATHH_ZERO_DEPTH:    return "ZERO_DEPTH";
        default:                     return "UNKNOWN";
    }
}