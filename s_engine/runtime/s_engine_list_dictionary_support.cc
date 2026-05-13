#include "s_engine_types.h"
#include "s_engine_module.h"
#include "s_engine_eval.h"
#include "s_engine_exception.h"
#include "s_engine_node.h"
#include "s_engine_list_dictionary_support.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// DICTIONARY SUPPORT FUNCTIONS
// ============================================================================

const s_expr_param_t* s_expr_key_contents(
    const s_expr_param_t* key_param,
    uint16_t* content_count
) {
    uint8_t opcode = key_param->type & S_EXPR_OPCODE_MASK;
    
    if (opcode != S_EXPR_PARAM_OPEN_KEY) {
        if (content_count) *content_count = 0;
        return NULL;
    }
    
    // Content starts after OPEN_KEY
    const s_expr_param_t* content = key_param + 1;
    
    // Linear scan to find CLOSE_KEY (OPEN_KEY stores hash, not brace_idx)
    uint16_t count = 0;
    const s_expr_param_t* p = content;
    
    while (count < 10000) {
        opcode = p->type & S_EXPR_OPCODE_MASK;
        if (opcode == S_EXPR_PARAM_CLOSE_KEY) {
            break;
        }
        count++;
        p++;
    }
    
    if (content_count) *content_count = count;
    return content;
}

const s_expr_param_t* s_expr_dict_find_key(
    const s_expr_param_t* dict_param,
    uint32_t key_hash
) {
    uint8_t opcode = dict_param->type & S_EXPR_OPCODE_MASK;
    
    if (opcode != S_EXPR_PARAM_OPEN_DICT) {
        return NULL;
    }
    
    uint16_t dict_size = dict_param->brace_idx;
    const s_expr_param_t* dict_end = dict_param + dict_size;
    const s_expr_param_t* p = dict_param + 1;
    
    while (p < dict_end) {
        opcode = p->type & S_EXPR_OPCODE_MASK;
        
        if (opcode == S_EXPR_PARAM_OPEN_KEY) {
            if (p->str_hash == key_hash) {
                return p + 1;  // Return content after OPEN_KEY
            }
            // Skip past this key's contents, tracking nesting depth
            int depth = 1;
            p++;
            while (p < dict_end && depth > 0) {
                uint8_t op = p->type & S_EXPR_OPCODE_MASK;
                if (op == S_EXPR_PARAM_OPEN_KEY) {
                    depth++;
                } else if (op == S_EXPR_PARAM_CLOSE_KEY) {
                    depth--;
                }
                p++;
            }
        } else if (opcode == S_EXPR_PARAM_CLOSE_DICT) {
            break;
        } else {
            p++;
        }
    }
    
    return NULL;
}
const s_expr_param_t* s_expr_dict_find_int_key(
    const s_expr_param_t* dict_param,
    int32_t key_val
) {
    uint8_t opcode = dict_param->type & S_EXPR_OPCODE_MASK;
    
    if (opcode != S_EXPR_PARAM_OPEN_DICT) {
        return NULL;
    }
    
    uint16_t dict_size = dict_param->brace_idx;
    const s_expr_param_t* dict_end = dict_param + dict_size;
    const s_expr_param_t* p = dict_param + 1;
    
    while (p < dict_end) {
        opcode = p->type & S_EXPR_OPCODE_MASK;
        
        if (opcode == S_EXPR_PARAM_OPEN_KEY) {
            const s_expr_param_t* key_param = p + 1;
            uint8_t key_opcode = key_param->type & S_EXPR_OPCODE_MASK;
            
            if (key_opcode == S_EXPR_PARAM_INT || key_opcode == S_EXPR_PARAM_UINT) {
                if ((int32_t)key_param->int_val == key_val) {
                    return key_param + 1;  // Return content after the int key
                }
            }
            
            // Skip past this key's contents, tracking nesting depth
            int depth = 1;
            p++;
            while (p < dict_end && depth > 0) {
                uint8_t op = p->type & S_EXPR_OPCODE_MASK;
                if (op == S_EXPR_PARAM_OPEN_KEY) {
                    depth++;
                } else if (op == S_EXPR_PARAM_CLOSE_KEY) {
                    depth--;
                }
                p++;
            }
        } else if (opcode == S_EXPR_PARAM_CLOSE_DICT) {
            break;
        } else {
            p++;
        }
    }
    
    return NULL;
}

// ============================================================================
// TUPLE/ARRAY/DICT CONTENT ACCESSORS
// ============================================================================

const s_expr_param_t* s_expr_tuple_contents(
    const s_expr_param_t* tuple_param,
    uint16_t* content_count
) {
    if (!S_EXPR_PARAM_IS_OPEN_TUPLE(tuple_param->type)) {
        if (content_count) *content_count = 0;
        return NULL;
    }
    
    uint16_t brace_idx = tuple_param->brace_idx;
    if (brace_idx <= 1) {
        if (content_count) *content_count = 0;
        return NULL;
    }
    
    if (content_count) *content_count = brace_idx - 1;
    return tuple_param + 1;
}

const s_expr_param_t* s_expr_array_contents(
    const s_expr_param_t* array_param,
    uint16_t* content_count
) {
    if (!S_EXPR_PARAM_IS_OPEN_ARRAY(array_param->type)) {
        if (content_count) *content_count = 0;
        return NULL;
    }
    
    uint16_t brace_idx = array_param->brace_idx;
    if (brace_idx <= 1) {
        if (content_count) *content_count = 0;
        return NULL;
    }
    
    if (content_count) *content_count = brace_idx - 1;
    return array_param + 1;
}

const s_expr_param_t* s_expr_dict_contents(
    const s_expr_param_t* dict_param,
    uint16_t* content_count
) {
    if (!S_EXPR_PARAM_IS_OPEN_DICT(dict_param->type)) {
        if (content_count) *content_count = 0;
        return NULL;
    }
    
    uint16_t brace_idx = dict_param->brace_idx;
    if (brace_idx <= 1) {
        if (content_count) *content_count = 0;
        return NULL;
    }
    
    if (content_count) *content_count = brace_idx - 1;
    return dict_param + 1;
}

const s_expr_param_t* s_expr_list_contents(
    const s_expr_param_t* list_param,
    uint16_t* content_count
) {
    if (!S_EXPR_PARAM_IS_OPEN(list_param->type)) {
        if (content_count) *content_count = 0;
        return NULL;
    }
    
    uint16_t brace_idx = list_param->brace_idx;
    if (brace_idx <= 1) {
        if (content_count) *content_count = 0;
        return NULL;
    }
    
    if (content_count) *content_count = brace_idx - 1;
    return list_param + 1;
}

// ============================================================================
// ALIST (Association List) SUPPORT
// Lisp-style: ((key1 . val1) (key2 . val2) ...)
// Represented as list of tuples where tuple[0] = key hash, tuple[1] = value
// ============================================================================

// Find value in alist by key hash
// Returns pointer to value param, or NULL if not found
const s_expr_param_t* s_expr_alist_find(
    const s_expr_param_t* alist,
    uint32_t key_hash
) {
    if (!alist || !S_EXPR_PARAM_IS_OPEN(alist->type)) {
        return NULL;
    }
    
    uint16_t list_count;
    const s_expr_param_t* list_items = s_expr_list_contents(alist, &list_count);
    if (!list_items) return NULL;
    
    const s_expr_param_t* p = list_items;
    const s_expr_param_t* list_end = alist + alist->brace_idx;
    
    while (p < list_end) {
        if (S_EXPR_PARAM_IS_OPEN_TUPLE(p->type)) {
            uint16_t tuple_count;
            const s_expr_param_t* tuple_items = s_expr_tuple_contents(p, &tuple_count);
            
            if (tuple_items && tuple_count >= 2) {
                if (tuple_items[0].str_hash == key_hash) {
                    return &tuple_items[1];  // Return value
                }
            }
            p += p->brace_idx + 1;
        } else if (S_EXPR_PARAM_IS_CLOSE(p->type)) {
            break;
        } else {
            p++;
        }
    }
    
    return NULL;
}

// Typed accessors with defaults
int32_t s_expr_alist_int(
    const s_expr_param_t* alist,
    uint32_t key_hash,
    int32_t default_val
) {
    const s_expr_param_t* val = s_expr_alist_find(alist, key_hash);
    if (!val) return default_val;
    
    uint8_t opcode = val->type & S_EXPR_OPCODE_MASK;
    if (opcode == S_EXPR_PARAM_INT || opcode == S_EXPR_PARAM_UINT) {
        return val->int_val;
    }
    return default_val;
}

uint32_t s_expr_alist_uint(
    const s_expr_param_t* alist,
    uint32_t key_hash,
    uint32_t default_val
) {
    const s_expr_param_t* val = s_expr_alist_find(alist, key_hash);
    if (!val) return default_val;
    
    uint8_t opcode = val->type & S_EXPR_OPCODE_MASK;
    if (opcode == S_EXPR_PARAM_INT || opcode == S_EXPR_PARAM_UINT) {
        return val->uint_val;
    }
    return default_val;
}

float s_expr_alist_float(
    const s_expr_param_t* alist,
    uint32_t key_hash,
    float default_val
) {
    const s_expr_param_t* val = s_expr_alist_find(alist, key_hash);
    if (!val) return default_val;
    
    uint8_t opcode = val->type & S_EXPR_OPCODE_MASK;
    if (opcode == S_EXPR_PARAM_FLOAT) {
        return val->float_val;
    }
    // Allow int->float conversion
    if (opcode == S_EXPR_PARAM_INT) {
        return (float)val->int_val;
    }
    return default_val;
}

s_expr_hash_t s_expr_alist_hash(
    const s_expr_param_t* alist,
    uint32_t key_hash,
    s_expr_hash_t default_val
) {
    const s_expr_param_t* val = s_expr_alist_find(alist, key_hash);
    if (!val) return default_val;
    
    uint8_t opcode = val->type & S_EXPR_OPCODE_MASK;
    if (opcode == S_EXPR_PARAM_STR_HASH) {
        return val->str_hash;
    }
    return default_val;
}

const char* s_expr_alist_str(
    const s_expr_module_def_t* def,
    const s_expr_param_t* alist,
    uint32_t key_hash,
    const char* default_val
) {
    if (!def || !def->string_table) return default_val;
    
    const s_expr_param_t* val = s_expr_alist_find(alist, key_hash);
    if (!val) return default_val;
    
    uint8_t opcode = val->type & S_EXPR_OPCODE_MASK;
    if (opcode == S_EXPR_PARAM_STR_IDX) {
        if (val->str_index < def->string_count) {
            return def->string_table[val->str_index];
        }
    }
    return default_val;
}

bool s_expr_alist_bool(
    const s_expr_param_t* alist,
    uint32_t key_hash,
    bool default_val
) {
    const s_expr_param_t* val = s_expr_alist_find(alist, key_hash);
    if (!val) return default_val;
    
    uint8_t opcode = val->type & S_EXPR_OPCODE_MASK;
    if (opcode == S_EXPR_PARAM_INT || opcode == S_EXPR_PARAM_UINT) {
        return val->int_val != 0;
    }
    return default_val;
}

// ============================================================================
// PLIST (Property List) SUPPORT
// Flat alternating: (key1 val1 key2 val2 ...)
// Keys are str_hash, values follow immediately
// ============================================================================

// Find value in plist by key hash
// Returns pointer to value param, or NULL if not found
const s_expr_param_t* s_expr_plist_find(
    const s_expr_param_t* plist,
    uint32_t key_hash
) {
    if (!plist || !S_EXPR_PARAM_IS_OPEN(plist->type)) {
        return NULL;
    }
    
    uint16_t list_count;
    const s_expr_param_t* list_items = s_expr_list_contents(plist, &list_count);
    if (!list_items || list_count < 2) return NULL;
    
    const s_expr_param_t* p = list_items;
    const s_expr_param_t* list_end = plist + plist->brace_idx;
    
    while (p < list_end - 1) {  // Need at least 2 elements (key + value)
        uint8_t opcode = p->type & S_EXPR_OPCODE_MASK;
        
        if (opcode == S_EXPR_PARAM_CLOSE) {
            break;
        }
        
        // Check if this is our key
        if (opcode == S_EXPR_PARAM_STR_HASH && p->str_hash == key_hash) {
            return p + 1;  // Return next element (value)
        }
        
        // Skip key and value (2 elements)
        p += 2;
    }
    
    return NULL;
}

// Typed accessors with defaults
int32_t s_expr_plist_int(
    const s_expr_param_t* plist,
    uint32_t key_hash,
    int32_t default_val
) {
    const s_expr_param_t* val = s_expr_plist_find(plist, key_hash);
    if (!val) return default_val;
    
    uint8_t opcode = val->type & S_EXPR_OPCODE_MASK;
    if (opcode == S_EXPR_PARAM_INT || opcode == S_EXPR_PARAM_UINT) {
        return val->int_val;
    }
    return default_val;
}

uint32_t s_expr_plist_uint(
    const s_expr_param_t* plist,
    uint32_t key_hash,
    uint32_t default_val
) {
    const s_expr_param_t* val = s_expr_plist_find(plist, key_hash);
    if (!val) return default_val;
    
    uint8_t opcode = val->type & S_EXPR_OPCODE_MASK;
    if (opcode == S_EXPR_PARAM_INT || opcode == S_EXPR_PARAM_UINT) {
        return val->uint_val;
    }
    return default_val;
}

float s_expr_plist_float(
    const s_expr_param_t* plist,
    uint32_t key_hash,
    float default_val
) {
    const s_expr_param_t* val = s_expr_plist_find(plist, key_hash);
    if (!val) return default_val;
    
    uint8_t opcode = val->type & S_EXPR_OPCODE_MASK;
    if (opcode == S_EXPR_PARAM_FLOAT) {
        return val->float_val;
    }
    if (opcode == S_EXPR_PARAM_INT) {
        return (float)val->int_val;
    }
    return default_val;
}

s_expr_hash_t s_expr_plist_hash(
    const s_expr_param_t* plist,
    uint32_t key_hash,
    s_expr_hash_t default_val
) {
    const s_expr_param_t* val = s_expr_plist_find(plist, key_hash);
    if (!val) return default_val;
    
    uint8_t opcode = val->type & S_EXPR_OPCODE_MASK;
    if (opcode == S_EXPR_PARAM_STR_HASH) {
        return val->str_hash;
    }
    return default_val;
}

const char* s_expr_plist_str(
    const s_expr_module_def_t* def,
    const s_expr_param_t* plist,
    uint32_t key_hash,
    const char* default_val
) {
    if (!def || !def->string_table) return default_val;
    
    const s_expr_param_t* val = s_expr_plist_find(plist, key_hash);
    if (!val) return default_val;
    
    uint8_t opcode = val->type & S_EXPR_OPCODE_MASK;
    if (opcode == S_EXPR_PARAM_STR_IDX) {
        if (val->str_index < def->string_count) {
            return def->string_table[val->str_index];
        }
    }
    return default_val;
}

bool s_expr_plist_bool(
    const s_expr_param_t* plist,
    uint32_t key_hash,
    bool default_val
) {
    const s_expr_param_t* val = s_expr_plist_find(plist, key_hash);
    if (!val) return default_val;
    
    uint8_t opcode = val->type & S_EXPR_OPCODE_MASK;
    if (opcode == S_EXPR_PARAM_INT || opcode == S_EXPR_PARAM_UINT) {
        return val->int_val != 0;
    }
    return default_val;
}