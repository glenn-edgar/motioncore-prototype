// ============================================================================
// s_engine_init.h
// High-level S-Expression Engine Initialization API
// 
// The engine manages module initialization and function registration.
// Tree instances are created by the engine but OWNED BY THE CALLER.
// ============================================================================

#ifndef S_ENGINE_INIT_H
#define S_ENGINE_INIT_H


#ifdef __cplusplus
extern "C" {
#endif

#include "s_engine_types.h"
#include "s_engine_module.h"
#include "s_engine_loader.h"


// ============================================================================
// ENGINE HANDLE - Module management only
// ============================================================================

typedef struct {
    s_expr_module_t           module;       // Initialized module
    s_expr_loaded_module_t*   loaded;       // Binary loader result (NULL if from def)
    s_expr_allocator_t        alloc;        // Allocator (ctx = external handle)
    void*                     user_ctx;     // External handle (e.g., cfl_runtime_handle_t*)
    uint8_t                   error_code;   // Last error
} s_engine_handle_t;

// Get current time using allocator's time function
static inline double s_engine_get_time(s_engine_handle_t* handle) {
    if (handle && handle->alloc.get_time) {
        return handle->alloc.get_time(handle->alloc.ctx);
    }
    return 0.0;
}

// Get current time from tree instance (for use in main functions)
// Tree must have been created by s_engine_create_tree
static inline double s_expr_tree_get_time(s_expr_tree_instance_t* inst) {
    if (inst && inst->module && inst->module->alloc.get_time) {
        return inst->module->alloc.get_time(inst->module->alloc.ctx);
    }
    return 0.0;
}

typedef void (*s_engine_user_register_fn)(s_engine_handle_t* engine);

// ============================================================================
// DEBUG CALLBACK TYPEDEF
// ============================================================================

typedef void (*s_engine_debug_callback_fn)(s_expr_tree_instance_t* inst, const char* msg);


bool s_engine_load_from_file(
    s_engine_handle_t* engine,
    s_expr_allocator_t* alloc,
    const char* filepath,
    s_engine_debug_callback_fn debug_cb,
    size_t user_fn_count,
    s_engine_user_register_fn* user_fns
);

bool s_engine_load_from_rom(
    s_engine_handle_t* engine,
    s_expr_allocator_t* alloc,
    const uint8_t* binary_data,
    size_t binary_size,
    s_engine_debug_callback_fn debug_cb,
    size_t user_fn_count,
    s_engine_user_register_fn* user_fns
);


// ============================================================================
// INITIALIZATION
// 
// user_ctx is passed to all created trees via s_expr_tree_set_user_ctx()
// alloc.ctx can be the same as user_ctx for unified memory management
// ============================================================================

// Initialize from binary data in ROM/flash (data must remain valid)
uint8_t s_engine_init_from_rom(
    s_engine_handle_t* handle,
    const uint8_t* binary_data,
    size_t binary_size,
    s_expr_allocator_t alloc,
    void* user_ctx
);

// Initialize from binary file (loaded into RAM, owned by engine)
uint8_t s_engine_init_from_file(
    s_engine_handle_t* handle,
    const char* filepath,
    s_expr_allocator_t alloc,
    void* user_ctx
);

// Initialize from compile-time module definition
uint8_t s_engine_init_from_def(
    s_engine_handle_t* handle,
    const s_expr_module_def_t* def,
    s_expr_allocator_t alloc,
    void* user_ctx
);

// ============================================================================
// FUNCTION REGISTRATION
// ============================================================================

void s_engine_register_oneshot(s_engine_handle_t* handle, const s_expr_fn_table_t* table);
void s_engine_register_main(s_engine_handle_t* handle, const s_expr_fn_table_t* table);
void s_engine_register_pred(s_engine_handle_t* handle, const s_expr_fn_table_t* table);

// Register built-in functions (SE_PIPELINE, SE_PRED_AND, etc.)
void s_engine_register_builtins(s_engine_handle_t* handle);

// Validate all required functions are registered
uint8_t s_engine_validate(s_engine_handle_t* handle);

// ============================================================================
// TREE CREATION
// 
// Trees are created using the engine's module and user_ctx.
// CALLER OWNS THE RETURNED TREE and must free it via s_expr_tree_free().
// ============================================================================

// Create tree by index - caller owns result
s_expr_tree_instance_t* s_engine_create_tree(
    s_engine_handle_t* handle,
    uint16_t tree_index,
    uint32_t node_id
);

// Create tree by name hash - caller owns result
s_expr_tree_instance_t* s_engine_create_tree_by_hash(
    s_engine_handle_t* handle,
    s_expr_hash_t name_hash,
    uint32_t node_id
);

// ============================================================================
// CLEANUP
// 
// Frees module and loaded binary.
// Does NOT free trees - caller must free trees before calling this.
// ============================================================================

void s_engine_free(s_engine_handle_t* handle);

// ============================================================================
// ACCESSORS
// ============================================================================

static inline void* s_engine_get_user_ctx(s_engine_handle_t* handle) {
    return handle ? handle->user_ctx : NULL;
}

static inline void s_engine_set_user_ctx(s_engine_handle_t* handle, void* ctx) {
    if (handle) handle->user_ctx = ctx;
}

static inline s_expr_module_t* s_engine_get_module(s_engine_handle_t* handle) {
    return handle ? &handle->module : NULL;
}

static inline s_expr_allocator_t* s_engine_get_alloc(s_engine_handle_t* handle) {
    return handle ? &handle->alloc : NULL;
}

// Number of tree definitions in module
static inline uint16_t s_engine_tree_def_count(s_engine_handle_t* handle) {
    return (handle && handle->module.def) ? handle->module.def->tree_count : 0;
}

// Get tree definition hash by index
static inline s_expr_hash_t s_engine_tree_def_hash(s_engine_handle_t* handle, uint16_t idx) {
    if (!handle || !handle->module.def || idx >= handle->module.def->tree_count) return 0;
    return handle->module.def->trees[idx].name_hash;
}

// Error string
static inline const char* s_engine_error_str(s_engine_handle_t* handle) {
    if (!handle) return "NULL handle";
    if (handle->error_code != 0) {
        return s_expr_loader_error_str(handle->error_code);
    }
    return s_expr_error_str(handle->module.error_code);
}

#ifdef __cplusplus
}
#endif

#endif // S_ENGINE_INIT_H