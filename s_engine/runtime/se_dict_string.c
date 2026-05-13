// ============================================================================
// se_dict_string.c (FIXED)
// String-Based Dictionary Implementation
// ============================================================================

#include "se_dict_string.h"
#include <string.h>
#include <stdio.h>
// ============================================================================
// PATH SEGMENT UTILITIES
// ============================================================================

bool se_dicts_is_numeric(const char* segment, uint16_t len, uint16_t* out_index) {
    if (len == 0) return false;
    
    uint32_t value = 0;
    for (uint16_t i = 0; i < len; i++) {
        char c = segment[i];
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
        if (value > 65535) return false;
    }
    
    if (out_index) *out_index = (uint16_t)value;
    return true;
}

// ============================================================================
// STRING TABLE LOOKUP
// ============================================================================

static const char* get_string_from_table(
    const s_expr_module_def_t* module_def,
    uint16_t str_index
) {
    if (!module_def || !module_def->string_table) return NULL;
    if (str_index >= module_def->string_count) return NULL;
    return module_def->string_table[str_index];
}

// ============================================================================
// HELPER: Skip a single value (handles nested structures)
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
        return p + p->brace_idx + 1;
    }
    
    // Simple value - just advance by one
    return p + 1;
}

// ============================================================================
// SINGLE-LEVEL DICTIONARY LOOKUP BY STRING
// ============================================================================

const s_expr_param_t* se_dicts_find(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* key,
    uint16_t key_len
) {
    (void)module_def;
    
    if (!dict || !key) return NULL;
    
    uint8_t opcode = dict->type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_OPEN_DICT) return NULL;
    
    s_expr_hash_t target_hash = se_dicts_hash_segment(key, key_len);
    
    const s_expr_param_t* dict_end = dict + dict->brace_idx;
    const s_expr_param_t* p = dict + 1;
   
    while (p < dict_end) {
        opcode = p->type & S_EXPR_OPCODE_MASK;
        
        if (opcode == S_EXPR_PARAM_OPEN_KEY) {
            if (p->str_hash == target_hash) {
                return p + 1;  // Return value (next param)
            }
            
            // Skip value
            const s_expr_param_t* value = p + 1;
            p = skip_value(value, dict_end);
            if (!p) return NULL;
            
            // Skip CLOSE_KEY
            if (p < dict_end && (p->type & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_CLOSE_KEY) {
                p++;
            }
        }
        else if (opcode == S_EXPR_PARAM_CLOSE_DICT) {
            break;
        }
        else {
            p++;
        }
    }
    
    return NULL;
}

// ============================================================================
// SINGLE-LEVEL LOOKUP BY HASH (fallback)
// ============================================================================

const s_expr_param_t* se_dicts_find_hash(
    const s_expr_param_t* dict,
    s_expr_hash_t key_hash
) {
    if (!dict) return NULL;
    
    uint8_t opcode = dict->type & S_EXPR_OPCODE_MASK;
    if (opcode != S_EXPR_PARAM_OPEN_DICT) return NULL;
    
    const s_expr_param_t* dict_end = dict + dict->brace_idx;
    const s_expr_param_t* p = dict + 1;
    
    while (p < dict_end) {
        opcode = p->type & S_EXPR_OPCODE_MASK;
        
        if (opcode == S_EXPR_PARAM_OPEN_KEY) {
        
            if (p->str_hash == key_hash) {
                return p + 1;
            }
            
            const s_expr_param_t* value = p + 1;
            p = skip_value(value, dict_end);
            if (!p) return NULL;
            
            if (p < dict_end && (p->type & S_EXPR_OPCODE_MASK) == S_EXPR_PARAM_CLOSE_KEY) {
                p++;
            }
        }
        else if (opcode == S_EXPR_PARAM_CLOSE_DICT) {
            break;
        }
        else {
            p++;
        }
    }
    
    return NULL;
}

// ============================================================================
// ARRAY ACCESS
// ============================================================================

uint16_t se_dicts_array_count(const s_expr_param_t* array) {
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
        
        p = skip_value(p, array_end);
        if (!p) break;
        
        count++;
    }
    
    return count;
}

const s_expr_param_t* se_dicts_array_get(
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
        
        p = skip_value(p, array_end);
        if (!p) break;
        
        current++;
    }
    
    return NULL;
}

// ============================================================================
// PATH RESOLUTION
// ============================================================================

const s_expr_param_t* se_dicts_resolve(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path,
    se_paths_context_t* ctx
) {
    if (ctx) {
        ctx->result = NULL;
        ctx->status = SE_PATHS_OK;
        ctx->depth = 0;
        ctx->failed_segment_start = 0;
        ctx->failed_segment_len = 0;
        ctx->failed_hash = 0;
    }
    
    if (!dict) {
        if (ctx) ctx->status = SE_PATHS_NULL_DICT;
        return NULL;
    }
    if (!path || path[0] == '\0') {
        if (ctx) ctx->status = SE_PATHS_NULL_PATH;
        return NULL;
    }
    
    const s_expr_param_t* current = dict;
    uint16_t segment_start = 0;
    uint16_t pos = 0;
    uint16_t depth = 0;
    
    while (1) {
        char c = path[pos];
        
        if (c == '.' || c == '\0') {
            uint16_t segment_len = pos - segment_start;
            
            if (segment_len == 0) {
                if (ctx) {
                    ctx->status = SE_PATHS_INVALID_PATH;
                    ctx->depth = depth;
                    ctx->failed_segment_start = segment_start;
                    ctx->failed_segment_len = 0;
                }
                return NULL;
            }
            
            if (!current) {
                if (ctx) {
                    ctx->status = SE_PATHS_NOT_FOUND;
                    ctx->depth = depth;
                    ctx->failed_segment_start = segment_start;
                    ctx->failed_segment_len = segment_len;
                    ctx->failed_hash = se_dicts_hash_segment(path + segment_start, segment_len);
                }
                return NULL;
            }
            
            uint8_t opcode = current->type & S_EXPR_OPCODE_MASK;
            
            if (opcode == S_EXPR_PARAM_OPEN_DICT) {
                current = se_dicts_find(current, module_def, path + segment_start, segment_len);
                if (!current) {
                    if (ctx) {
                        ctx->status = SE_PATHS_NOT_FOUND;
                        ctx->depth = depth;
                        ctx->failed_segment_start = segment_start;
                        ctx->failed_segment_len = segment_len;
                        ctx->failed_hash = se_dicts_hash_segment(path + segment_start, segment_len);
                    }
                    return NULL;
                }
            }
            else if (opcode == S_EXPR_PARAM_OPEN_ARRAY) {
                uint16_t index;
                if (!se_dicts_is_numeric(path + segment_start, segment_len, &index)) {
                    if (ctx) {
                        ctx->status = SE_PATHS_INVALID_INDEX;
                        ctx->depth = depth;
                        ctx->failed_segment_start = segment_start;
                        ctx->failed_segment_len = segment_len;
                    }
                    return NULL;
                }
                
                current = se_dicts_array_get(current, index);
                if (!current) {
                    if (ctx) {
                        ctx->status = SE_PATHS_INVALID_INDEX;
                        ctx->depth = depth;
                        ctx->failed_segment_start = segment_start;
                        ctx->failed_segment_len = segment_len;
                    }
                    return NULL;
                }
            }
            else {
                if (ctx) {
                    ctx->status = SE_PATHS_TYPE_MISMATCH;
                    ctx->depth = depth;
                    ctx->failed_segment_start = segment_start;
                    ctx->failed_segment_len = segment_len;
                    ctx->failed_hash = se_dicts_hash_segment(path + segment_start, segment_len);
                }
                return NULL;
            }
            
            depth++;
            segment_start = pos + 1;
            
            if (c == '\0') {
                break;
            }
        }
        
        pos++;
    }
    
    if (ctx) {
        ctx->result = current;
        ctx->status = SE_PATHS_OK;
        ctx->depth = depth;
    }
    
    return current;
}

// ============================================================================
// TYPED VALUE EXTRACTION (rest unchanged)
// ============================================================================

ct_int_t se_dicts_get_int(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path,
    ct_int_t default_val
) {
    const s_expr_param_t* p = se_dicts_resolve(dict, module_def, path, NULL);
    return se_dicts_param_int(p, default_val);
}

ct_uint_t se_dicts_get_uint(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path,
    ct_uint_t default_val
) {
    const s_expr_param_t* p = se_dicts_resolve(dict, module_def, path, NULL);
    return se_dicts_param_uint(p, default_val);
}

ct_float_t se_dicts_get_float(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path,
    ct_float_t default_val
) {
    const s_expr_param_t* p = se_dicts_resolve(dict, module_def, path, NULL);
    return se_dicts_param_float(p, default_val);
}

bool se_dicts_get_bool(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path,
    bool default_val
) {
    const s_expr_param_t* p = se_dicts_resolve(dict, module_def, path, NULL);
    return se_dicts_param_bool(p, default_val);
}

s_expr_hash_t se_dicts_get_hash(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path,
    s_expr_hash_t default_val
) {
    se_paths_context_t ctx;
    se_paths_context_init(&ctx);
    
    const s_expr_param_t* value = se_dicts_resolve(dict, module_def, path, &ctx);
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

const s_expr_param_t* se_dicts_get_dict(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path
) {
    const s_expr_param_t* p = se_dicts_resolve(dict, module_def, path, NULL);
    if (p && se_dicts_is_dict(p)) return p;
    return NULL;
}

const s_expr_param_t* se_dicts_get_array(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path
) {
    const s_expr_param_t* p = se_dicts_resolve(dict, module_def, path, NULL);
    if (p && se_dicts_is_array(p)) return p;
    return NULL;
}

const s_expr_param_t* se_dicts_get_callable(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path
) {
    const s_expr_param_t* p = se_dicts_resolve(dict, module_def, path, NULL);
    if (p && se_dicts_is_callable(p)) return p;
    return NULL;
}

bool se_dicts_get_string(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path,
    uint16_t* out_str_index,
    uint16_t* out_str_len
) {
    const s_expr_param_t* p = se_dicts_resolve(dict, module_def, path, NULL);
    if (!p) return false;
    
    if ((p->type & S_EXPR_OPCODE_MASK) != S_EXPR_PARAM_STR_IDX) return false;
    
    if (out_str_index) *out_str_index = p->str_index;
    if (out_str_len) *out_str_len = p->str_len;
    return true;
}

const char* se_dicts_get_string_ptr(
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def,
    const char* path
) {
    uint16_t str_index;
    if (!se_dicts_get_string(dict, module_def, path, &str_index, NULL)) {
        return NULL;
    }
    return get_string_from_table(module_def, str_index);
}

// ============================================================================
// ITERATION (rest unchanged from before - with same skip_value fix)
// ============================================================================

void se_dicts_iter_init(
    se_dicts_iter_t* iter,
    const s_expr_param_t* dict,
    const s_expr_module_def_t* module_def
) {
    if (!iter) return;
    
    iter->dict = dict;
    iter->module_def = module_def;
    iter->index = 0;
    
    if (!dict || (dict->type & S_EXPR_OPCODE_MASK) != S_EXPR_PARAM_OPEN_DICT) {
        iter->current = NULL;
        iter->dict_end = NULL;
        return;
    }
    
    iter->dict_end = dict + dict->brace_idx;
    iter->current = dict + 1;
}

bool se_dicts_iter_next(
    se_dicts_iter_t* iter,
    const char** out_key_str,
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
            
            if (out_key_str) {
                *out_key_str = NULL;
            }
            
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

void se_dicts_iter_reset(se_dicts_iter_t* iter) {
    if (!iter || !iter->dict) return;
    iter->current = iter->dict + 1;
    iter->index = 0;
}

void se_arrays_iter_init(se_arrays_iter_t* iter, const s_expr_param_t* array) {
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

bool se_arrays_iter_next(
    se_arrays_iter_t* iter,
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

void se_arrays_iter_reset(se_arrays_iter_t* iter) {
    if (!iter || !iter->array) return;
    iter->current = iter->array + 1;
    iter->index = 0;
}

const char* se_paths_status_name(se_paths_status_t status) {
    switch (status) {
        case SE_PATHS_OK:            return "OK";
        case SE_PATHS_NOT_FOUND:     return "NOT_FOUND";
        case SE_PATHS_TYPE_MISMATCH: return "TYPE_MISMATCH";
        case SE_PATHS_INVALID_INDEX: return "INVALID_INDEX";
        case SE_PATHS_INVALID_PATH:  return "INVALID_PATH";
        case SE_PATHS_NULL_DICT:     return "NULL_DICT";
        case SE_PATHS_NULL_PATH:     return "NULL_PATH";
        case SE_PATHS_NULL_MODULE:   return "NULL_MODULE";
        default:                     return "UNKNOWN";
    }
}