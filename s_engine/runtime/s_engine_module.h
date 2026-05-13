// ============================================================================
// s_engine_module.h
// S-Expression Module Management API
// ============================================================================

#ifndef S_ENGINE_MODULE_H
#define S_ENGINE_MODULE_H



#ifdef __cplusplus
extern "C" {
#endif
#include "s_engine_types.h"
// ============================================================================
// MODULE LIFECYCLE
// ============================================================================

uint8_t s_expr_module_init(
    s_expr_module_t* mod,
    const s_expr_module_def_t* def,
    s_expr_allocator_t alloc
);

void s_expr_module_register_oneshot(s_expr_module_t* mod, const s_expr_fn_table_t* table);
void s_expr_module_register_main(s_expr_module_t* mod, const s_expr_fn_table_t* table);
void s_expr_module_register_pred(s_expr_module_t* mod, const s_expr_fn_table_t* table);

uint8_t s_expr_module_validate(s_expr_module_t* mod);
void s_expr_module_free(s_expr_module_t* mod);

void s_expr_module_set_debug(s_expr_module_t* mod, s_expr_debug_fn_t fn);

// ----------------------------------------------------------------------------
// Helper for user C functions that want to emit a log line. Equivalent to
// the DSL se_log() oneshot from inside C code. Prepends "[<uptime_ms>] "
// to match the format DSL se_log produces (see s_engine_builtins_oneshot.h).
//
// No-op when no debug_fn is registered (silent on bare M-port until a
// debug_packet_fn or equivalent is registered).
//
// Caveat: format-free. If you need formatting, snprintf into a local buf
// first then pass the result. Don't add an s_engine_logf variant — %f in
// format strings would pull in newlib float printf (~3 KB flash on M-port).
// ----------------------------------------------------------------------------
#include <stdio.h>
static inline void s_engine_log(s_expr_tree_instance_t* inst, const char* msg) {
    if (!inst || !inst->module || !inst->module->debug_fn || !msg) return;
    // Timestamp uses alloc.get_time_ms (uint32, no float math). NULL = 0.
    // Intentional: no fallback to get_time*1000.0 — the compiler keeps that
    // dead branch's __aeabi_dmul/ddiv in the binary even when unreachable.
    uint32_t uptime_ms = inst->module->alloc.get_time_ms
                            ? inst->module->alloc.get_time_ms(inst->module->alloc.ctx)
                            : 0;
    char buf[256];
    snprintf(buf, sizeof(buf), "[%lu] %s", (unsigned long)uptime_ms, msg);
    inst->module->debug_fn(inst, buf);
}
void s_expr_module_set_error(s_expr_module_t* mod, s_expr_error_fn_t fn);
void s_expr_module_set_pools(s_expr_module_t* mod, void** pools, uint16_t count);

const char* s_expr_error_str(uint8_t error_code);

// ============================================================================
// TREE INSTANCE LIFECYCLE
// ============================================================================

s_expr_tree_instance_t* s_expr_tree_create(
    s_expr_module_t* mod,
    uint16_t tree_index,
    uint32_t ct_node_id
);

s_expr_tree_instance_t* s_expr_tree_create_by_hash(
    s_expr_module_t* mod,
    s_expr_hash_t name_hash,
    uint32_t ct_node_id
);

void s_expr_tree_free(s_expr_tree_instance_t* inst);

void s_expr_tree_bind_blackboard(
    s_expr_tree_instance_t* inst,
    void* blackboard,
    uint16_t size
);

void s_expr_tree_set_user_ctx(s_expr_tree_instance_t* inst, void* ctx);
void* s_expr_tree_get_user_ctx(s_expr_tree_instance_t* inst);

// ============================================================================
// BLACKBOARD USER ACCESS APIs
// For external entities to read/write blackboard fields
// ============================================================================

// Get raw pointer to blackboard
void* s_expr_tree_get_blackboard(s_expr_tree_instance_t* inst);
uint16_t s_expr_tree_get_blackboard_size(s_expr_tree_instance_t* inst);

// Get field pointer by offset (from DSL field_offset)
void* s_expr_blackboard_get_field_ptr(s_expr_tree_instance_t* inst, uint16_t field_offset);

// Get field pointer by hash (runtime lookup)
void* s_expr_blackboard_get_field_by_hash(s_expr_tree_instance_t* inst, s_expr_hash_t field_hash);

// Typed accessors (by field hash)
bool s_expr_blackboard_set_int(s_expr_tree_instance_t* inst, s_expr_hash_t field_hash, int32_t value);
int32_t s_expr_blackboard_get_int(s_expr_tree_instance_t* inst, s_expr_hash_t field_hash, int32_t default_value);
bool s_expr_blackboard_set_float(s_expr_tree_instance_t* inst, s_expr_hash_t field_hash, float value);
float s_expr_blackboard_get_float(s_expr_tree_instance_t* inst, s_expr_hash_t field_hash, float default_value);

// String-based accessors (calculates hash internally - convenience for external code)
void* s_expr_blackboard_get_field_by_string(s_expr_tree_instance_t* inst, const char* field_name);
bool s_expr_blackboard_set_int_by_string(s_expr_tree_instance_t* inst, const char* field_name, int32_t value);
int32_t s_expr_blackboard_get_int_by_string(s_expr_tree_instance_t* inst, const char* field_name, int32_t default_value);
bool s_expr_blackboard_set_float_by_string(s_expr_tree_instance_t* inst, const char* field_name, float value);
float s_expr_blackboard_get_float_by_string(s_expr_tree_instance_t* inst, const char* field_name, float default_value);
bool s_expr_blackboard_set_uint_by_string(s_expr_tree_instance_t* inst, const char* field_name, uint32_t value);
uint32_t s_expr_blackboard_get_uint_by_string(s_expr_tree_instance_t* inst, const char* field_name, uint32_t default_value);

// ============================================================================
// NODE STATE ACCESS (current node during callback)
// ============================================================================

uint8_t s_expr_node_get_flags(s_expr_tree_instance_t* inst);
void s_expr_node_set_user_flags(s_expr_tree_instance_t* inst, uint8_t flags);

uint8_t s_expr_node_get_state(s_expr_tree_instance_t* inst);
void s_expr_node_set_state(s_expr_tree_instance_t* inst, uint8_t state);

uint16_t s_expr_node_get_user_data(s_expr_tree_instance_t* inst);
void s_expr_node_set_user_data(s_expr_tree_instance_t* inst, uint16_t data);

// ============================================================================
// POINTER ARRAY ACCESS (for pt_m_call)
// These error if called outside a pt_m_call context
// ============================================================================

bool s_expr_is_pointer_call(s_expr_tree_instance_t* inst);
void* s_expr_get_field_ptr(s_expr_tree_instance_t* inst, const s_expr_param_t* field_param);
void* s_expr_pointer_alloc(s_expr_tree_instance_t* inst, uint16_t param_index, size_t size);
void s_expr_pointer_free(s_expr_tree_instance_t* inst, uint16_t param_index);

// Typed access to pointer slots (stores pointer, u64, or f64)
void* s_expr_get_ptr(s_expr_tree_instance_t* inst, uint16_t param_index);
void s_expr_set_ptr(s_expr_tree_instance_t* inst, uint16_t param_index, void* ptr);
uint64_t s_expr_get_u64(s_expr_tree_instance_t* inst);
void s_expr_set_u64(s_expr_tree_instance_t* inst, uint64_t val);
double s_expr_get_f64(s_expr_tree_instance_t* inst);
void s_expr_set_f64(s_expr_tree_instance_t* inst, double val);
s_expr_slot_t* s_expr_get_pointer_slot(s_expr_tree_instance_t* inst, uint16_t param_index);
// ============================================================================
// STRING TABLE ACCESS
// ============================================================================

const char* s_expr_get_string(s_expr_tree_instance_t* inst, const s_expr_param_t* param);
// ============================================================================
// BLACKBOARD ACCESS
// ============================================================================



// ============================================================================
// POOL ACCESS (legacy)
// ============================================================================

void* s_expr_get_slot_ptr(s_expr_tree_instance_t* inst, const s_expr_param_t* slot_param, size_t elem_size);

// ============================================================================
// SLOT ACCESS (external initialization)
// ============================================================================

// Direct slot access
s_expr_slot_t* s_expr_tree_get_slot(s_expr_tree_instance_t* inst, uint16_t index);
uint16_t s_expr_tree_get_slot_count(s_expr_tree_instance_t* inst);

// Slot inspection
uint8_t s_expr_tree_get_slot_flags(s_expr_tree_instance_t* inst, uint16_t index);
bool s_expr_tree_slot_is_allocated(s_expr_tree_instance_t* inst, uint16_t index);
bool s_expr_tree_slot_is_external(s_expr_tree_instance_t* inst, uint16_t index);
bool s_expr_tree_slot_has_ptr(s_expr_tree_instance_t* inst, uint16_t index);

// Slot pointer access
void* s_expr_tree_slot_get_ptr(s_expr_tree_instance_t* inst, uint16_t index);
void  s_expr_tree_slot_set_ptr(s_expr_tree_instance_t* inst, uint16_t index, void* ptr);

// Slot allocation
void* s_expr_tree_slot_alloc(s_expr_tree_instance_t* inst, uint16_t index, size_t size);
void  s_expr_tree_slot_free(s_expr_tree_instance_t* inst, uint16_t index);

#define S_EXPR_GET_SLOT(inst, param, type) \
    ((type*)s_expr_get_slot_ptr((inst), (param), sizeof(type)))

// ============================================================================
// PARAMETER NAVIGATION
// ============================================================================

static inline uint16_t s_expr_skip_param(const s_expr_param_t* params, uint16_t idx) {
    uint8_t opcode = params[idx].type & S_EXPR_OPCODE_MASK;
    
    if (opcode == S_EXPR_PARAM_OPEN || 
        opcode == S_EXPR_PARAM_OPEN_CALL ||
        opcode == S_EXPR_PARAM_OPEN_DICT ||
        opcode == S_EXPR_PARAM_OPEN_ARRAY ||
        opcode == S_EXPR_PARAM_OPEN_TUPLE ||
        opcode == S_EXPR_PARAM_OPEN_KEY) {
        return idx + params[idx].brace_idx + 1;
    }
    return idx + 1;
}

static inline const s_expr_param_t* s_expr_brace_contents(
    const s_expr_param_t* params,
    uint16_t open_idx,
    uint16_t* out_count
) {
    uint16_t close_idx = open_idx + params[open_idx].brace_idx;
    *out_count = close_idx - open_idx - 1;
    return &params[open_idx + 1];
}

static inline const s_expr_param_t* s_expr_call_func(
    const s_expr_param_t* params,
    uint16_t open_idx
) {
    return &params[open_idx + 1];
}

static inline const s_expr_param_t* s_expr_call_args(
    const s_expr_param_t* params,
    uint16_t open_idx,
    uint16_t* out_count
) {
    uint16_t close_idx = open_idx + params[open_idx].brace_idx;
    *out_count = (close_idx > open_idx + 2) ? (close_idx - open_idx - 2) : 0;
    return (*out_count > 0) ? &params[open_idx + 2] : NULL;
}

// ============================================================================
// MODULE ACCESSORS
// ============================================================================

static inline uint16_t s_expr_module_tree_count(const s_expr_module_t* mod) {
    return (mod && mod->def) ? mod->def->tree_count : 0;
}

static inline s_expr_hash_t s_expr_module_tree_hash(const s_expr_module_t* mod, uint16_t idx) {
    if (!mod || !mod->def || idx >= mod->def->tree_count) return 0;
    return mod->def->trees[idx].name_hash;
}

// ============================================================================
// TREE INSTANCE ACCESSORS
// ============================================================================

static inline s_expr_hash_t s_expr_tree_name_hash(const s_expr_tree_instance_t* inst) {
    return (inst && inst->tree) ? inst->tree->name_hash : 0;
}

static inline uint16_t s_expr_tree_node_count(const s_expr_tree_instance_t* inst) {
    return inst ? inst->node_count : 0;
}

static inline const s_expr_param_t* s_expr_tree_params(const s_expr_tree_instance_t* inst) {
    return (inst && inst->tree) ? inst->tree->params : NULL;
}

static inline uint16_t s_expr_tree_param_count(const s_expr_tree_instance_t* inst) {
    return (inst && inst->tree) ? inst->tree->param_count : 0;
}

// ============================================================================
// FUNCTION TABLE UTILITIES
// ============================================================================

void s_expr_build_fn_table(
    const s_expr_fn_entry_named_t* named,
    s_expr_fn_entry_t* out,
    uint16_t count
);

void* s_expr_lookup_func(
    const s_expr_fn_table_t* table,
    s_expr_hash_t hash
);

#ifdef __cplusplus
}
#endif

#endif // S_ENGINE_MODULE_H