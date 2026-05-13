#ifndef S_ENGINE_LIST_DICTIONARY_SUPPORT_H
#define S_ENGINE_LIST_DICTIONARY_SUPPORT_H

#ifdef __cplusplus
extern "C" {
#endif
#include "s_engine_types.h"


const s_expr_param_t* s_expr_dict_find_int_key(
    const s_expr_param_t* dict_param,
    int32_t key_val
);

const s_expr_param_t* s_expr_dict_find_key(
    const s_expr_param_t* dict_param,
    uint32_t key_hash
);

// ============================================================================
// STRUCTURE CONTENT ACCESSORS
// Returns pointer to first element inside, sets content_count
// Returns NULL if not valid or empty
// ============================================================================

const s_expr_param_t* s_expr_key_contents(
    const s_expr_param_t* key_param,
    uint16_t* content_count
);

const s_expr_param_t* s_expr_tuple_contents(
    const s_expr_param_t* tuple_param,
    uint16_t* content_count
);

const s_expr_param_t* s_expr_array_contents(
    const s_expr_param_t* array_param,
    uint16_t* content_count
);

const s_expr_param_t* s_expr_dict_contents(
    const s_expr_param_t* dict_param,
    uint16_t* content_count
);

const s_expr_param_t* s_expr_list_contents(
    const s_expr_param_t* list_param,
    uint16_t* content_count
);
// ============================================================================
// ALIST (Association List) SUPPORT
// ============================================================================

const s_expr_param_t* s_expr_alist_find(
    const s_expr_param_t* alist,
    uint32_t key_hash
);

int32_t s_expr_alist_int(
    const s_expr_param_t* alist,
    uint32_t key_hash,
    int32_t default_val
);

uint32_t s_expr_alist_uint(
    const s_expr_param_t* alist,
    uint32_t key_hash,
    uint32_t default_val
);

float s_expr_alist_float(
    const s_expr_param_t* alist,
    uint32_t key_hash,
    float default_val
);

s_expr_hash_t s_expr_alist_hash(
    const s_expr_param_t* alist,
    uint32_t key_hash,
    s_expr_hash_t default_val
);

const char* s_expr_alist_str(
    const s_expr_module_def_t* def,
    const s_expr_param_t* alist,
    uint32_t key_hash,
    const char* default_val
);

bool s_expr_alist_bool(
    const s_expr_param_t* alist,
    uint32_t key_hash,
    bool default_val
);
// ============================================================================
// PLIST (Property List) SUPPORT
// ============================================================================

const s_expr_param_t* s_expr_plist_find(
    const s_expr_param_t* plist,
    uint32_t key_hash
);

int32_t s_expr_plist_int(
    const s_expr_param_t* plist,
    uint32_t key_hash,
    int32_t default_val
);

uint32_t s_expr_plist_uint(
    const s_expr_param_t* plist,
    uint32_t key_hash,
    uint32_t default_val
);

float s_expr_plist_float(
    const s_expr_param_t* plist,
    uint32_t key_hash,
    float default_val
);

s_expr_hash_t s_expr_plist_hash(
    const s_expr_param_t* plist,
    uint32_t key_hash,
    s_expr_hash_t default_val
);

const char* s_expr_plist_str(
    const s_expr_module_def_t* def,
    const s_expr_param_t* plist,
    uint32_t key_hash,
    const char* default_val
);

bool s_expr_plist_bool(
    const s_expr_param_t* plist,
    uint32_t key_hash,
    bool default_val
);
#ifdef __cplusplus
}
#endif
#endif