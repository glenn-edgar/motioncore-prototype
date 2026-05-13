// ============================================================================
// INTERNAL: Function registry storage
// NOTE: These are file-scope globals. s_expr_module_init() zeroes them,
// so only one module can be active per process. This is intentional for
// single-module embedded targets. For multi-module server deployments,
// these would need to become per-module.
// ============================================================================
#include "s_engine_module.h"
#include "s_engine_stack.h"
#include "s_engine_exception.h"
#include "s_engine_event_queue.h"
#include <string.h>
#include <stdio.h>

// ============================================================================
// INTERNAL: Function registry storage
// ============================================================================

#define MAX_REGISTRY_TABLES 8

typedef struct {
    const s_expr_fn_table_t* tables[MAX_REGISTRY_TABLES];
    uint16_t count;
} s_expr_registry_t;

static s_expr_registry_t oneshot_registry;
static s_expr_registry_t main_registry;
static s_expr_registry_t pred_registry;

// ============================================================================
// INTERNAL: Lookup function by hash
// ============================================================================

static void* lookup_by_hash(const s_expr_registry_t* reg, s_expr_hash_t hash) {
    if (!reg) return NULL;
    
    for (uint16_t t = 0; t < reg->count; t++) {
        const s_expr_fn_table_t* table = reg->tables[t];
        if (!table || !table->entries) continue;
        
        for (uint16_t i = 0; i < table->count; i++) {
            if (table->entries[i].hash == hash) {
                return table->entries[i].fn_ptr;
            }
        }
    }
    return NULL;
}

// ============================================================================
// INTERNAL: Find record descriptor by hash
// ============================================================================

static const s_expr_record_desc_t* find_record_by_hash(
    const s_expr_module_def_t* def,
    s_expr_hash_t record_hash
) {
    if (!def || !def->records || record_hash == 0) return NULL;
    
    for (uint16_t i = 0; i < def->record_count; i++) {
        if (def->records[i].name_hash == record_hash) {
            return &def->records[i];
        }
    }
    return NULL;
}

// ============================================================================
// FUNCTION TABLE UTILITIES
// ============================================================================

void* s_expr_lookup_func(
    const s_expr_fn_table_t* table,
    s_expr_hash_t hash
) {
    if (!table || !table->entries) {
        EXCEPTION("s_expr_lookup_func: NULL table or entries");
        return NULL;
    }
    
    for (uint16_t i = 0; i < table->count; i++) {
        if (table->entries[i].hash == hash) {
            return table->entries[i].fn_ptr;
        }
    }
    return NULL;  // Not found is not an error
}

void s_expr_build_fn_table(
    const s_expr_fn_entry_named_t* named,
    s_expr_fn_entry_t* entries,
    uint16_t count
) {
    if (!named || !entries) {
        EXCEPTION("s_expr_build_fn_table: NULL named or entries");
        return;
    }
    for (uint16_t i = 0; i < count; i++) {
        if (!named[i].name) {
            EXCEPTION("s_expr_build_fn_table: NULL function name");
            return;
        }
        entries[i].hash = s_expr_hash(named[i].name);
        entries[i].fn_ptr = named[i].fn_ptr;
    }
}

// ============================================================================
// ERROR STRING
// ============================================================================

const char* s_expr_error_str(uint8_t error_code) {
    switch (error_code) {
        case S_EXPR_ERR_OK:               return "OK";
        case S_EXPR_ERR_ALLOC:            return "Allocation failed";
        case S_EXPR_ERR_NULL_DEF:         return "Null module definition";
        case S_EXPR_ERR_64BIT_MISMATCH:   return "64-bit mode mismatch";
        case S_EXPR_ERR_ONESHOT_NOT_FOUND: return "Oneshot function not found";
        case S_EXPR_ERR_MAIN_NOT_FOUND:   return "Main function not found";
        case S_EXPR_ERR_PRED_NOT_FOUND:   return "Predicate function not found";
        case S_EXPR_ERR_INVALID_TREE:     return "Invalid tree index";
        case S_EXPR_ERR_NOT_POINTER_CALL: return "Pointer access outside pt_m_call";
        case S_EXPR_ERR_POINTER_INDEX:    return "Pointer index out of range";
        case S_EXPR_ERR_NO_BLACKBOARD:    return "No blackboard bound";
        default:                          return "Unknown error";
    }
}

// ============================================================================
// MODULE INIT
// ============================================================================

uint8_t s_expr_module_init(
    s_expr_module_t* mod,
    const s_expr_module_def_t* def,
    s_expr_allocator_t alloc
) {
    if (!mod) {
        EXCEPTION("s_expr_module_init: NULL module");
        return S_EXPR_ERR_ALLOC;
    }
    
    memset(mod, 0, sizeof(*mod));
    memset(&oneshot_registry, 0, sizeof(oneshot_registry));
    memset(&main_registry, 0, sizeof(main_registry));
    memset(&pred_registry, 0, sizeof(pred_registry));
    
    mod->alloc = alloc;
    
    if (!def) {
        EXCEPTION("s_expr_module_init: NULL module definition");
        mod->error_code = S_EXPR_ERR_NULL_DEF;
        return S_EXPR_ERR_NULL_DEF;
    }
    
    if (def->is_64bit != (MODULE_IS_64BIT != 0)) {
        EXCEPTION("s_expr_module_init: 64-bit mode mismatch between module and definition");
        mod->error_code = S_EXPR_ERR_64BIT_MISMATCH;
        return S_EXPR_ERR_64BIT_MISMATCH;
    }
    
    if (!alloc.malloc || !alloc.free) {
        EXCEPTION("s_expr_module_init: NULL allocator functions");
        mod->error_code = S_EXPR_ERR_ALLOC;
        return S_EXPR_ERR_ALLOC;
    }
    
    mod->def = def;
    
    if (def->oneshot_count > 0) {
        size_t size = def->oneshot_count * sizeof(s_expr_oneshot_fn_t);
        mod->oneshot_fns = (s_expr_oneshot_fn_t*)alloc.malloc(alloc.ctx, size);
        if (!mod->oneshot_fns) {
            EXCEPTION("s_expr_module_init: failed to allocate oneshot function table");
            mod->def = NULL;
            mod->error_code = S_EXPR_ERR_ALLOC;
            return S_EXPR_ERR_ALLOC;
        }
        memset(mod->oneshot_fns, 0, size);
    }
    
    if (def->main_count > 0) {
        size_t size = def->main_count * sizeof(s_expr_main_fn_t);
        mod->main_fns = (s_expr_main_fn_t*)alloc.malloc(alloc.ctx, size);
        if (!mod->main_fns) {
            EXCEPTION("s_expr_module_init: failed to allocate main function table");
            if (mod->oneshot_fns) {
                alloc.free(alloc.ctx, mod->oneshot_fns);
                mod->oneshot_fns = NULL;
            }
            mod->def = NULL;
            mod->error_code = S_EXPR_ERR_ALLOC;
            return S_EXPR_ERR_ALLOC;
        }
        memset(mod->main_fns, 0, size);
    }
    
    if (def->pred_count > 0) {
        size_t size = def->pred_count * sizeof(s_expr_pred_fn_t);
        mod->pred_fns = (s_expr_pred_fn_t*)alloc.malloc(alloc.ctx, size);
        if (!mod->pred_fns) {
            EXCEPTION("s_expr_module_init: failed to allocate predicate function table");
            if (mod->main_fns) {
                alloc.free(alloc.ctx, mod->main_fns);
                mod->main_fns = NULL;
            }
            if (mod->oneshot_fns) {
                alloc.free(alloc.ctx, mod->oneshot_fns);
                mod->oneshot_fns = NULL;
            }
            mod->def = NULL;
            mod->error_code = S_EXPR_ERR_ALLOC;
            return S_EXPR_ERR_ALLOC;
        }
        memset(mod->pred_fns, 0, size);
    }
    
    mod->error_code = S_EXPR_ERR_OK;
    return S_EXPR_ERR_OK;
}
// ============================================================================
// FUNCTION REGISTRATION
// ============================================================================

void s_expr_module_register_oneshot(s_expr_module_t* mod, const s_expr_fn_table_t* table) {
    if (!mod) {
        EXCEPTION("s_expr_module_register_oneshot: NULL module");
        return;
    }
    if (!table) {
        EXCEPTION("s_expr_module_register_oneshot: NULL table");
        return;
    }
    if (!mod->def) {
        EXCEPTION("s_expr_module_register_oneshot: module not initialized");
        return;
    }
    if (!mod->def->oneshot_hashes || mod->def->oneshot_count == 0) return;
    if (!mod->oneshot_fns) {
        EXCEPTION("s_expr_module_register_oneshot: oneshot_fns not allocated");
        return;
    }
    
    if (oneshot_registry.count >= MAX_REGISTRY_TABLES) {
        EXCEPTION("s_expr_module_register_oneshot: registry full");
        return;
    }
    oneshot_registry.tables[oneshot_registry.count++] = table;
    
    for (uint16_t i = 0; i < mod->def->oneshot_count; i++) {
        if (mod->oneshot_fns[i]) continue;
        
        s_expr_hash_t needed_hash = mod->def->oneshot_hashes[i];
        for (uint16_t j = 0; j < table->count; j++) {
            if (table->entries[j].hash == needed_hash) {
                mod->oneshot_fns[i] = (s_expr_oneshot_fn_t)table->entries[j].fn_ptr;
                break;
            }
        }
    }
}

void s_expr_module_register_main(s_expr_module_t* mod, const s_expr_fn_table_t* table) {
    if (!mod) {
        EXCEPTION("s_expr_module_register_main: NULL module");
        return;
    }
    if (!table) {
        EXCEPTION("s_expr_module_register_main: NULL table");
        return;
    }
    if (!mod->def) {
        EXCEPTION("s_expr_module_register_main: module not initialized");
        return;
    }
    if (!mod->def->main_hashes || mod->def->main_count == 0) return;
    if (!mod->main_fns) {
        EXCEPTION("s_expr_module_register_main: main_fns not allocated");
        return;
    }
    
    if (main_registry.count >= MAX_REGISTRY_TABLES) {
        EXCEPTION("s_expr_module_register_main: registry full");
        return;
    }
    main_registry.tables[main_registry.count++] = table;
    
    for (uint16_t i = 0; i < mod->def->main_count; i++) {
        if (mod->main_fns[i]) continue;
        
        s_expr_hash_t needed_hash = mod->def->main_hashes[i];
        for (uint16_t j = 0; j < table->count; j++) {
            if (table->entries[j].hash == needed_hash) {
                mod->main_fns[i] = (s_expr_main_fn_t)table->entries[j].fn_ptr;
                break;
            }
        }
    }
}

void s_expr_module_register_pred(s_expr_module_t* mod, const s_expr_fn_table_t* table) {
    if (!mod) {
        EXCEPTION("s_expr_module_register_pred: NULL module");
        return;
    }
    if (!table) {
        EXCEPTION("s_expr_module_register_pred: NULL table");
        return;
    }
    if (!mod->def) {
        EXCEPTION("s_expr_module_register_pred: module not initialized");
        return;
    }
    if (!mod->def->pred_hashes || mod->def->pred_count == 0) return;
    if (!mod->pred_fns) {
        EXCEPTION("s_expr_module_register_pred: pred_fns not allocated");
        return;
    }
    
    if (pred_registry.count >= MAX_REGISTRY_TABLES) {
        EXCEPTION("s_expr_module_register_pred: registry full");
        return;
    }
    pred_registry.tables[pred_registry.count++] = table;
    
    for (uint16_t i = 0; i < mod->def->pred_count; i++) {
        if (mod->pred_fns[i]) continue;
        
        s_expr_hash_t needed_hash = mod->def->pred_hashes[i];
        for (uint16_t j = 0; j < table->count; j++) {
            if (table->entries[j].hash == needed_hash) {
                mod->pred_fns[i] = (s_expr_pred_fn_t)table->entries[j].fn_ptr;
                break;
            }
        }
    }
}

uint8_t s_expr_module_validate(s_expr_module_t* mod) {
    if (!mod) {
        EXCEPTION("s_expr_module_validate: NULL module");
        return S_EXPR_ERR_NULL_DEF;
    }
    if (!mod->def) {
        EXCEPTION("s_expr_module_validate: module not initialized");
        return S_EXPR_ERR_NULL_DEF;
    }
    
    const s_expr_module_def_t* def = mod->def;
    uint16_t missing_count = 0;
    uint8_t first_error = S_EXPR_ERR_OK;
    
    // Check oneshot functions
    for (uint16_t i = 0; i < def->oneshot_count; i++) {
        if (!mod->oneshot_fns[i]) {
            s_expr_hash_t hash = def->oneshot_hashes[i];
            mod->oneshot_fns[i] = (s_expr_oneshot_fn_t)lookup_by_hash(&oneshot_registry, hash);
            if (!mod->oneshot_fns[i]) {
                printf("Missing oneshot function: hash=0x%08X index=%d\n", (uint32_t)hash, i);
                if (first_error == S_EXPR_ERR_OK) {
                    first_error = S_EXPR_ERR_ONESHOT_NOT_FOUND;
                    mod->error_code = S_EXPR_ERR_ONESHOT_NOT_FOUND;
                    mod->error_index = i;
                    mod->error_hash = hash;
                }
                missing_count++;
            }
        }
    }
    
    // Check main functions
    for (uint16_t i = 0; i < def->main_count; i++) {
        if (!mod->main_fns[i]) {
            s_expr_hash_t hash = def->main_hashes[i];
            mod->main_fns[i] = (s_expr_main_fn_t)lookup_by_hash(&main_registry, hash);
            if (!mod->main_fns[i]) {
                printf("Missing main function: hash=0x%08X index=%d\n", (uint32_t)hash, i);
                if (first_error == S_EXPR_ERR_OK) {
                    first_error = S_EXPR_ERR_MAIN_NOT_FOUND;
                    mod->error_code = S_EXPR_ERR_MAIN_NOT_FOUND;
                    mod->error_index = i;
                    mod->error_hash = hash;
                }
                missing_count++;
            }
        }
    }
    
    // Check predicate functions
    for (uint16_t i = 0; i < def->pred_count; i++) {
        if (!mod->pred_fns[i]) {
            s_expr_hash_t hash = def->pred_hashes[i];
            mod->pred_fns[i] = (s_expr_pred_fn_t)lookup_by_hash(&pred_registry, hash);
            if (!mod->pred_fns[i]) {
                printf("Missing predicate function: hash=0x%08X index=%d\n", (uint32_t)hash, i);
                if (first_error == S_EXPR_ERR_OK) {
                    first_error = S_EXPR_ERR_PRED_NOT_FOUND;
                    mod->error_code = S_EXPR_ERR_PRED_NOT_FOUND;
                    mod->error_index = i;
                    mod->error_hash = hash;
                }
                missing_count++;
            }
        }
    }
    
    // Report summary and throw exception if errors
    if (missing_count > 0) {
        printf("Validation failed: %d missing function(s)\n", missing_count);
        EXCEPTION("s_expr_module_validate: missing functions");
        return first_error;
    }
    
    mod->error_code = S_EXPR_ERR_OK;
    return S_EXPR_ERR_OK;
}

// ============================================================================
// MODULE FREE
// ============================================================================

void s_expr_module_free(s_expr_module_t* mod) {
    if (!mod) return;
    
    s_expr_allocator_t* alloc = &mod->alloc;
    
    if (mod->oneshot_fns && alloc->free) {
        alloc->free(alloc->ctx, mod->oneshot_fns);
        mod->oneshot_fns = NULL;
    }
    
    if (mod->main_fns && alloc->free) {
        alloc->free(alloc->ctx, mod->main_fns);
        mod->main_fns = NULL;
    }
    
    if (mod->pred_fns && alloc->free) {
        alloc->free(alloc->ctx, mod->pred_fns);
        mod->pred_fns = NULL;
    }
    
    mod->def = NULL;
}

// ============================================================================
// SET CALLBACKS
// ============================================================================

void s_expr_module_set_debug(s_expr_module_t* mod, s_expr_debug_fn_t fn) {
    if (!mod) {
        EXCEPTION("s_expr_module_set_debug: NULL module");
        return;
    }
    mod->debug_fn = fn;
}

void s_expr_module_set_error(s_expr_module_t* mod, s_expr_error_fn_t fn) {
    if (!mod) {
        EXCEPTION("s_expr_module_set_error: NULL module");
        return;
    }
    mod->error_fn = fn;
}

void s_expr_module_set_pools(s_expr_module_t* mod, void** pools, uint16_t count) {
    if (!mod) {
        EXCEPTION("s_expr_module_set_pools: NULL module");
        return;
    }
    mod->pool_table = pools;
    mod->pool_count = count;
}

// ============================================================================
// TREE INSTANCE CREATE
// ============================================================================

s_expr_tree_instance_t* s_expr_tree_create(
    s_expr_module_t* mod,
    uint16_t tree_index,
    uint32_t ct_node_id
) {
    if (!mod) {
        EXCEPTION("s_expr_tree_create: NULL module");
        return NULL;
    }
    if (!mod->def) {
        EXCEPTION("s_expr_tree_create: module not initialized");
        return NULL;
    }
    if (tree_index >= mod->def->tree_count) {
        EXCEPTION("s_expr_tree_create: tree_index out of range");
        return NULL;
    }
    
    const s_expr_tree_def_t* tree_def = &mod->def->trees[tree_index];
    s_expr_allocator_t* alloc = &mod->alloc;
    
    if (!alloc->malloc) {
        EXCEPTION("s_expr_tree_create: NULL allocator");
        return NULL;
    }
    
    // Allocate instance structure
    s_expr_tree_instance_t* inst = (s_expr_tree_instance_t*)alloc->malloc(
        alloc->ctx, sizeof(s_expr_tree_instance_t)
    );
    if (!inst) {
        EXCEPTION("s_expr_tree_create: failed to allocate tree instance");
        return NULL;
    }
    
    memset(inst, 0, sizeof(*inst));
    inst->stack = NULL;
    
    inst->ct_node_id = ct_node_id;
    inst->module = mod;
    inst->tree = tree_def;
    inst->tree_index = tree_index;
    inst->node_count = tree_def->func_node_count;
    inst->pointer_count = tree_def->pointer_count;
    inst->event_queue_head = 0;
    inst->event_queue_count = 0;
    
    // Allocate node states
    if (tree_def->func_node_count > 0) {
        size_t node_size = tree_def->func_node_count * sizeof(s_expr_node_state_t);
        inst->node_states = (s_expr_node_state_t*)alloc->malloc(alloc->ctx, node_size);
        if (!inst->node_states) {
            EXCEPTION("s_expr_tree_create: failed to allocate node_states");
            alloc->free(alloc->ctx, inst);
            return NULL;
        }
        
        for (uint16_t i = 0; i < tree_def->func_node_count; i++) {
            inst->node_states[i].flags = S_EXPR_NODE_FLAG_ACTIVE;
            inst->node_states[i].state = 0;
            inst->node_states[i].user_data = 0;
        }
    }
    
    // Allocate pointer/slot array and flags (single allocation block)
    if (tree_def->pointer_count > 0) {
        size_t slot_size = tree_def->pointer_count * sizeof(s_expr_slot_t);
        inst->pointer_array = (s_expr_slot_t*)alloc->malloc(alloc->ctx, slot_size);
        if (!inst->pointer_array) {
            EXCEPTION("s_expr_tree_create: failed to allocate pointer_array");
            if (inst->node_states) alloc->free(alloc->ctx, inst->node_states);
            alloc->free(alloc->ctx, inst);
            return NULL;
        }
        memset(inst->pointer_array, 0, slot_size);
        
        // Allocate parallel flags array
        inst->slot_flags = (uint8_t*)alloc->malloc(alloc->ctx, tree_def->pointer_count);
        if (!inst->slot_flags) {
            EXCEPTION("s_expr_tree_create: failed to allocate slot_flags");
            alloc->free(alloc->ctx, inst->pointer_array);
            if (inst->node_states) alloc->free(alloc->ctx, inst->node_states);
            alloc->free(alloc->ctx, inst);
            return NULL;
        }
        memset(inst->slot_flags, S_EXPR_SLOT_FLAG_NONE, tree_def->pointer_count);
    }
    
    // Auto-allocate blackboard if tree uses a record
    if (tree_def->record_hash != 0) {
        const s_expr_record_desc_t* record = find_record_by_hash(mod->def, tree_def->record_hash);
        if (record && record->total_size > 0) {
            inst->blackboard = alloc->malloc(alloc->ctx, record->total_size);
            if (!inst->blackboard) {
                EXCEPTION("s_expr_tree_create: failed to allocate blackboard");
                if (inst->slot_flags) alloc->free(alloc->ctx, inst->slot_flags);
                if (inst->pointer_array) alloc->free(alloc->ctx, inst->pointer_array);
                if (inst->node_states) alloc->free(alloc->ctx, inst->node_states);
                alloc->free(alloc->ctx, inst);
                return NULL;
            }
            
            if (tree_def->defaults_index != S_EXPR_NO_DEFAULTS && 
                tree_def->defaults_index < mod->def->const_count &&
                mod->def->constants) {
                const void* defaults = mod->def->constants[tree_def->defaults_index];
                if (defaults) {
                    memcpy(inst->blackboard, defaults, record->total_size);
                } else {
                    memset(inst->blackboard, 0, record->total_size);
                }
            } else {
                memset(inst->blackboard, 0, record->total_size);
            }
            inst->blackboard_size = record->total_size;
            inst->blackboard_owned = true;
        }
    }
    
    return inst;
}

s_expr_tree_instance_t* s_expr_tree_create_by_hash(
    s_expr_module_t* mod,
    s_expr_hash_t name_hash,
    uint32_t ct_node_id
) {
    if (!mod) {
        EXCEPTION("s_expr_tree_create_by_hash: NULL module");
        return NULL;
    }
    if (!mod->def) {
        EXCEPTION("s_expr_tree_create_by_hash: module not initialized");
        return NULL;
    }
    
    for (uint16_t i = 0; i < mod->def->tree_count; i++) {
        if (mod->def->trees[i].name_hash == name_hash) {
            return s_expr_tree_create(mod, i, ct_node_id);
        }
    }
    
    EXCEPTION("s_expr_tree_create_by_hash: tree not found");
    return NULL;
}

// ============================================================================
// TREE INSTANCE FREE
// ============================================================================

void s_expr_tree_free(s_expr_tree_instance_t* inst) {
    if (!inst) return;
    
    s_expr_module_t* mod = inst->module;
    if (!mod) {
        EXCEPTION("s_expr_tree_free: NULL module in instance");
        return;
    }
    
    s_expr_allocator_t* alloc = &mod->alloc;
    if (!alloc->free) {
        EXCEPTION("s_expr_tree_free: NULL free function");
        return;
    }
    
    // Free allocated slot pointers
    if (inst->pointer_array && inst->slot_flags) {
        for (uint16_t i = 0; i < inst->pointer_count; i++) {
            if ((inst->slot_flags[i] & S_EXPR_SLOT_FLAG_ALLOCATED) && 
                inst->pointer_array[i].ptr) {
                alloc->free(alloc->ctx, inst->pointer_array[i].ptr);
            }
        }
    }
    
    // Free stack if present
    s_expr_tree_free_stack(inst);
    
    // Free event queue if present
    inst->event_queue_count = 0;
    inst->event_queue_head = 0;
    
    if (inst->slot_flags) {
        alloc->free(alloc->ctx, inst->slot_flags);
    }
    
    if (inst->pointer_array) {
        alloc->free(alloc->ctx, inst->pointer_array);
    }
    
    if (inst->node_states) {
        alloc->free(alloc->ctx, inst->node_states);
    }
    
    if (inst->blackboard && inst->blackboard_owned) {
        alloc->free(alloc->ctx, inst->blackboard);
    }
    
    alloc->free(alloc->ctx, inst);
}

// ============================================================================
// BLACKBOARD BINDING
// ============================================================================

void s_expr_tree_bind_blackboard(
    s_expr_tree_instance_t* inst,
    void* blackboard,
    uint16_t size
) {
    if (!inst) {
        EXCEPTION("s_expr_tree_bind_blackboard: NULL instance");
        return;
    }
    
    // Free existing auto-allocated blackboard
    if (inst->blackboard && inst->blackboard_owned && inst->module) {
        if (inst->module->alloc.free) {
            inst->module->alloc.free(inst->module->alloc.ctx, inst->blackboard);
        }
    }
    
    inst->blackboard = blackboard;
    inst->blackboard_size = size;
    inst->blackboard_owned = false;
}

// ============================================================================
// USER CONTEXT
// ============================================================================

void s_expr_tree_set_user_ctx(s_expr_tree_instance_t* inst, void* ctx) {
    if (!inst) {
        EXCEPTION("s_expr_tree_set_user_ctx: NULL instance");
        return;
    }
    inst->user_ctx = ctx;
}

void* s_expr_tree_get_user_ctx(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_tree_get_user_ctx: NULL instance");
        return NULL;
    }
    return inst->user_ctx;
}

// ============================================================================
// BLACKBOARD USER ACCESS APIs
// ============================================================================

void* s_expr_tree_get_blackboard(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_tree_get_blackboard: NULL instance");
        return NULL;
    }
    return inst->blackboard;
}

uint16_t s_expr_tree_get_blackboard_size(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_tree_get_blackboard_size: NULL instance");
        return 0;
    }
    return inst->blackboard_size;
}

void* s_expr_blackboard_get_field_ptr(
    s_expr_tree_instance_t* inst,
    uint16_t field_offset
) {
    if (!inst) {
        EXCEPTION("s_expr_blackboard_get_field_ptr: NULL instance");
        return NULL;
    }
    if (!inst->blackboard) {
        EXCEPTION("s_expr_blackboard_get_field_ptr: no blackboard bound");
        return NULL;
    }
    if (field_offset >= inst->blackboard_size) {
        EXCEPTION("s_expr_blackboard_get_field_ptr: field_offset out of range");
        return NULL;
    }
    return (uint8_t*)inst->blackboard + field_offset;
}

void* s_expr_blackboard_get_field_by_hash(
    s_expr_tree_instance_t* inst,
    s_expr_hash_t field_hash
) {
    if (!inst) {
        EXCEPTION("s_expr_blackboard_get_field_by_hash: NULL instance");
        return NULL;
    }
    if (!inst->blackboard) {
        EXCEPTION("s_expr_blackboard_get_field_by_hash: no blackboard bound");
        return NULL;
    }
    if (!inst->module || !inst->module->def) {
        EXCEPTION("s_expr_blackboard_get_field_by_hash: invalid module");
        return NULL;
    }
    if (!inst->tree || inst->tree->record_hash == 0) {
        EXCEPTION("s_expr_blackboard_get_field_by_hash: tree has no record");
        return NULL;
    }
    
    const s_expr_record_desc_t* record = find_record_by_hash(
        inst->module->def, inst->tree->record_hash
    );
    if (!record || !record->fields) {
        EXCEPTION("s_expr_blackboard_get_field_by_hash: record not found");
        return NULL;
    }
    
    for (uint16_t i = 0; i < record->field_count; i++) {
        if (record->fields[i].name_hash == field_hash) {
            uint16_t offset = record->fields[i].offset;
            if (offset >= inst->blackboard_size) {
                EXCEPTION("s_expr_blackboard_get_field_by_hash: field offset exceeds blackboard size");
                return NULL;
            }
            return (uint8_t*)inst->blackboard + offset;
        }
    }
    
    EXCEPTION("s_expr_blackboard_get_field_by_hash: field not found");
    return NULL;
}

bool s_expr_blackboard_set_int(
    s_expr_tree_instance_t* inst,
    s_expr_hash_t field_hash,
    int32_t value
) {
    int32_t* ptr = (int32_t*)s_expr_blackboard_get_field_by_hash(inst, field_hash);
    if (!ptr) return false;
    *ptr = value;
    return true;
}

int32_t s_expr_blackboard_get_int(
    s_expr_tree_instance_t* inst,
    s_expr_hash_t field_hash,
    int32_t default_value
) {
    int32_t* ptr = (int32_t*)s_expr_blackboard_get_field_by_hash(inst, field_hash);
    return ptr ? *ptr : default_value;
}

bool s_expr_blackboard_set_float(
    s_expr_tree_instance_t* inst,
    s_expr_hash_t field_hash,
    float value
) {
    float* ptr = (float*)s_expr_blackboard_get_field_by_hash(inst, field_hash);
    if (!ptr) return false;
    *ptr = value;
    return true;
}

float s_expr_blackboard_get_float(
    s_expr_tree_instance_t* inst,
    s_expr_hash_t field_hash,
    float default_value
) {
    float* ptr = (float*)s_expr_blackboard_get_field_by_hash(inst, field_hash);
    return ptr ? *ptr : default_value;
}

// ============================================================================
// BLACKBOARD ACCESS BY STRING
// ============================================================================

void* s_expr_blackboard_get_field_by_string(
    s_expr_tree_instance_t* inst,
    const char* field_name
) {
    if (!field_name) {
        EXCEPTION("s_expr_blackboard_get_field_by_string: NULL field_name");
        return NULL;
    }
    return s_expr_blackboard_get_field_by_hash(inst, s_expr_hash(field_name));
}

bool s_expr_blackboard_set_int_by_string(
    s_expr_tree_instance_t* inst,
    const char* field_name,
    int32_t value
) {
    if (!field_name) {
        EXCEPTION("s_expr_blackboard_set_int_by_string: NULL field_name");
        return false;
    }
    return s_expr_blackboard_set_int(inst, s_expr_hash(field_name), value);
}

int32_t s_expr_blackboard_get_int_by_string(
    s_expr_tree_instance_t* inst,
    const char* field_name,
    int32_t default_value
) {
    if (!field_name) {
        EXCEPTION("s_expr_blackboard_get_int_by_string: NULL field_name");
        return default_value;
    }
    return s_expr_blackboard_get_int(inst, s_expr_hash(field_name), default_value);
}

bool s_expr_blackboard_set_float_by_string(
    s_expr_tree_instance_t* inst,
    const char* field_name,
    float value
) {
    if (!field_name) {
        EXCEPTION("s_expr_blackboard_set_float_by_string: NULL field_name");
        return false;
    }
    return s_expr_blackboard_set_float(inst, s_expr_hash(field_name), value);
}

float s_expr_blackboard_get_float_by_string(
    s_expr_tree_instance_t* inst,
    const char* field_name,
    float default_value
) {
    if (!field_name) {
        EXCEPTION("s_expr_blackboard_get_float_by_string: NULL field_name");
        return default_value;
    }
    return s_expr_blackboard_get_float(inst, s_expr_hash(field_name), default_value);
}

bool s_expr_blackboard_set_uint_by_string(
    s_expr_tree_instance_t* inst,
    const char* field_name,
    uint32_t value
) {
    if (!field_name) {
        EXCEPTION("s_expr_blackboard_set_uint_by_string: NULL field_name");
        return false;
    }
    uint32_t* ptr = (uint32_t*)s_expr_blackboard_get_field_by_hash(inst, s_expr_hash(field_name));
    if (!ptr) return false;
    *ptr = value;
    return true;
}

uint32_t s_expr_blackboard_get_uint_by_string(
    s_expr_tree_instance_t* inst,
    const char* field_name,
    uint32_t default_value
) {
    if (!field_name) {
        EXCEPTION("s_expr_blackboard_get_uint_by_string: NULL field_name");
        return default_value;
    }
    uint32_t* ptr = (uint32_t*)s_expr_blackboard_get_field_by_hash(inst, s_expr_hash(field_name));
    return ptr ? *ptr : default_value;
}

// ============================================================================
// INTERNAL: Get current node state
// ============================================================================

static inline s_expr_node_state_t* get_current_state(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("get_current_state: NULL instance");
        return NULL;
    }
    if (inst->current_node_index >= inst->node_count) {
        EXCEPTION("get_current_state: current_node_index out of range");
        return NULL;
    }
    return &inst->node_states[inst->current_node_index];
}

// ============================================================================
// NODE STATE ACCESS
// ============================================================================

uint8_t s_expr_node_get_flags(s_expr_tree_instance_t* inst) {
    s_expr_node_state_t* state = get_current_state(inst);
    return state ? state->flags : 0;
}

void s_expr_node_set_user_flags(s_expr_tree_instance_t* inst, uint8_t flags) {
    s_expr_node_state_t* state = get_current_state(inst);
    if (state) {
        state->flags = (state->flags & S_EXPR_NODE_FLAGS_SYSTEM) | (flags & S_EXPR_NODE_FLAGS_USER);
    }
}

uint8_t s_expr_node_get_state(s_expr_tree_instance_t* inst) {
    s_expr_node_state_t* state = get_current_state(inst);
    return state ? state->state : 0;
}

void s_expr_node_set_state(s_expr_tree_instance_t* inst, uint8_t st) {
    s_expr_node_state_t* state = get_current_state(inst);
    if (state) state->state = st;
}

uint16_t s_expr_node_get_user_data(s_expr_tree_instance_t* inst) {
    s_expr_node_state_t* state = get_current_state(inst);
    return state ? state->user_data : 0;
}

void s_expr_node_set_user_data(s_expr_tree_instance_t* inst, uint16_t data) {
    s_expr_node_state_t* state = get_current_state(inst);
    if (state) state->user_data = data;
}

// ============================================================================
// POINTER SLOT ACCESS (for pt_m_call)
// ============================================================================

bool s_expr_is_pointer_call(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_is_pointer_call: NULL instance");
        return false;
    }
    return inst->in_pointer_call;
}

s_expr_slot_t* s_expr_get_pointer_slot(s_expr_tree_instance_t* inst, uint16_t param_index) {
    if (!inst) {
        EXCEPTION("s_expr_get_pointer_slot: NULL instance");
        return NULL;
    }
    
    if (!inst->in_pointer_call) {
        EXCEPTION("s_expr_get_pointer_slot: called outside pt_m_call context - use pt_m_call() in DSL");
        return NULL;
    }
    
    uint16_t ptr_idx = inst->pointer_base + param_index;
    
    if (ptr_idx >= inst->pointer_count) {
        EXCEPTION("s_expr_get_pointer_slot: index out of range");
        return NULL;
    }
    
    return &inst->pointer_array[ptr_idx];
}

void* s_expr_pointer_alloc(s_expr_tree_instance_t* inst, uint16_t param_index, size_t size) {
    s_expr_slot_t* slot = s_expr_get_pointer_slot(inst, param_index);
    if (!slot) return NULL;
    
    if (!inst->module) {
        EXCEPTION("s_expr_pointer_alloc: NULL module");
        return NULL;
    }
    
    uint16_t ptr_idx = inst->pointer_base + param_index;
    
    // Free existing allocated pointer
    if ((inst->slot_flags[ptr_idx] & S_EXPR_SLOT_FLAG_ALLOCATED) && slot->ptr) {
        inst->module->alloc.free(inst->module->alloc.ctx, slot->ptr);
    }
    
    slot->ptr = inst->module->alloc.malloc(inst->module->alloc.ctx, size);
    if (!slot->ptr) {
        EXCEPTION("s_expr_pointer_alloc: allocation failed");
        inst->slot_flags[ptr_idx] = S_EXPR_SLOT_FLAG_NONE;
        return NULL;
    }
    
    memset(slot->ptr, 0, size);
    inst->slot_flags[ptr_idx] = S_EXPR_SLOT_FLAG_ALLOCATED;
    return slot->ptr;
}

void s_expr_pointer_free(s_expr_tree_instance_t* inst, uint16_t param_index) {
    s_expr_slot_t* slot = s_expr_get_pointer_slot(inst, param_index);
    if (!slot || !slot->ptr) return;
    
    if (!inst->module) {
        EXCEPTION("s_expr_pointer_free: NULL module");
        return;
    }
    
    uint16_t ptr_idx = inst->pointer_base + param_index;
    
    if (!(inst->slot_flags[ptr_idx] & S_EXPR_SLOT_FLAG_ALLOCATED)) {
        return;  // Not allocated by us - don't free
    }
    
    inst->module->alloc.free(inst->module->alloc.ctx, slot->ptr);
    slot->ptr = NULL;
    inst->slot_flags[ptr_idx] = S_EXPR_SLOT_FLAG_NONE;
}

void* s_expr_get_ptr(s_expr_tree_instance_t* inst, uint16_t param_index) {
    s_expr_slot_t* slot = s_expr_get_pointer_slot(inst, param_index);
    return slot ? slot->ptr : NULL;
}

void s_expr_set_ptr(s_expr_tree_instance_t* inst, uint16_t param_index, void* ptr) {
    s_expr_slot_t* slot = s_expr_get_pointer_slot(inst, param_index);
    if (slot) {
        uint16_t ptr_idx = inst->pointer_base + param_index;
        
        // Free existing allocated pointer
        if ((inst->slot_flags[ptr_idx] & S_EXPR_SLOT_FLAG_ALLOCATED) && slot->ptr) {
            inst->module->alloc.free(inst->module->alloc.ctx, slot->ptr);
        }
        
        slot->ptr = ptr;
        inst->slot_flags[ptr_idx] = S_EXPR_SLOT_FLAG_EXTERNAL;
    }
}

// ============================================================================
// SLOT ACCESSORS (u64, i64, f64, ptr) - use union directly
// ============================================================================

uint64_t s_expr_get_u64(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_get_u64: NULL instance");
        return 0;
    }
    if (!inst->in_pointer_call) {
        EXCEPTION("s_expr_get_u64: called outside pt_m_call context - use pt_m_call() in DSL");
        return 0;
    }
    if (inst->pointer_base >= inst->pointer_count) {
        EXCEPTION("s_expr_get_u64: pointer_base out of range");
        return 0;
    }
    if (!inst->pointer_array) {
        EXCEPTION("s_expr_get_u64: NULL pointer_array");
        return 0;
    }
    return inst->pointer_array[inst->pointer_base].u64;
}

void s_expr_set_u64(s_expr_tree_instance_t* inst, uint64_t val) {
    if (!inst) {
        EXCEPTION("s_expr_set_u64: NULL instance");
        return;
    }
    if (!inst->in_pointer_call) {
        EXCEPTION("s_expr_set_u64: called outside pt_m_call context - use pt_m_call() in DSL");
        return;
    }
    if (inst->pointer_base >= inst->pointer_count) {
        EXCEPTION("s_expr_set_u64: pointer_base out of range");
        return;
    }
    if (!inst->pointer_array) {
        EXCEPTION("s_expr_set_u64: NULL pointer_array");
        return;
    }
    inst->pointer_array[inst->pointer_base].u64 = val;
}

int64_t s_expr_get_i64(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_get_i64: NULL instance");
        return 0;
    }
    if (!inst->in_pointer_call) {
        EXCEPTION("s_expr_get_i64: called outside pt_m_call context - use pt_m_call() in DSL");
        return 0;
    }
    if (inst->pointer_base >= inst->pointer_count) {
        EXCEPTION("s_expr_get_i64: pointer_base out of range");
        return 0;
    }
    if (!inst->pointer_array) {
        EXCEPTION("s_expr_get_i64: NULL pointer_array");
        return 0;
    }
    return inst->pointer_array[inst->pointer_base].i64;
}

void s_expr_set_i64(s_expr_tree_instance_t* inst, int64_t val) {
    if (!inst) {
        EXCEPTION("s_expr_set_i64: NULL instance");
        return;
    }
    if (!inst->in_pointer_call) {
        EXCEPTION("s_expr_set_i64: called outside pt_m_call context - use pt_m_call() in DSL");
        return;
    }
    if (inst->pointer_base >= inst->pointer_count) {
        EXCEPTION("s_expr_set_i64: pointer_base out of range");
        return;
    }
    if (!inst->pointer_array) {
        EXCEPTION("s_expr_set_i64: NULL pointer_array");
        return;
    }
    inst->pointer_array[inst->pointer_base].i64 = val;
}

double s_expr_get_f64(s_expr_tree_instance_t* inst) {
    if (!inst) {
        EXCEPTION("s_expr_get_f64: NULL instance");
        return 0.0;
    }
    if (!inst->in_pointer_call) {
        EXCEPTION("s_expr_get_f64: called outside pt_m_call context - use pt_m_call() in DSL");
        return 0.0;
    }
    if (inst->pointer_base >= inst->pointer_count) {
        EXCEPTION("s_expr_get_f64: pointer_base out of range");
        return 0.0;
    }
    if (!inst->pointer_array) {
        EXCEPTION("s_expr_get_f64: NULL pointer_array");
        return 0.0;
    }
    return inst->pointer_array[inst->pointer_base].f64;
}

void s_expr_set_f64(s_expr_tree_instance_t* inst, double val) {
    if (!inst) {
        EXCEPTION("s_expr_set_f64: NULL instance");
        return;
    }
    if (!inst->in_pointer_call) {
        EXCEPTION("s_expr_set_f64: called outside pt_m_call context - use pt_m_call() in DSL");
        return;
    }
    if (inst->pointer_base >= inst->pointer_count) {
        EXCEPTION("s_expr_set_f64: pointer_base out of range");
        return;
    }
    if (!inst->pointer_array) {
        EXCEPTION("s_expr_set_f64: NULL pointer_array");
        return;
    }
    inst->pointer_array[inst->pointer_base].f64 = val;
}

// ============================================================================
// BLACKBOARD ACCESS (from params)
// ============================================================================

void* s_expr_get_field_ptr(s_expr_tree_instance_t* inst, const s_expr_param_t* field_param) {
    if (!inst) {
        EXCEPTION("s_expr_get_field_ptr: NULL instance");
        return NULL;
    }
    if (!field_param) {
        EXCEPTION("s_expr_get_field_ptr: NULL field_param");
        return NULL;
    }
    
    if ((field_param->type & S_EXPR_OPCODE_MASK) != S_EXPR_PARAM_FIELD) {
        EXCEPTION("s_expr_get_field_ptr: param is not a FIELD type");
        return NULL;
    }
    
    if (!inst->blackboard) {
        EXCEPTION("s_expr_get_field_ptr: no blackboard bound");
        return NULL;
    }
    
    uint16_t offset = field_param->field_offset;
    
    if (offset + field_param->field_size > inst->blackboard_size) {
        EXCEPTION("s_expr_get_field_ptr: field exceeds blackboard size");
        return NULL;
    }
    
    return (uint8_t*)inst->blackboard + offset;
}

// ============================================================================
// POOL ACCESS (legacy)
// ============================================================================

void* s_expr_get_slot_ptr(s_expr_tree_instance_t* inst, const s_expr_param_t* slot_param, size_t elem_size) {
    if (!inst) {
        EXCEPTION("s_expr_get_slot_ptr: NULL instance");
        return NULL;
    }
    if (!slot_param) {
        EXCEPTION("s_expr_get_slot_ptr: NULL slot_param");
        return NULL;
    }
    if (!inst->module) {
        EXCEPTION("s_expr_get_slot_ptr: NULL module");
        return NULL;
    }
    if (!inst->module->pool_table) {
        EXCEPTION("s_expr_get_slot_ptr: no pool table set");
        return NULL;
    }
    
    if ((slot_param->type & S_EXPR_OPCODE_MASK) != S_EXPR_PARAM_SLOT) {
        EXCEPTION("s_expr_get_slot_ptr: param is not a SLOT type");
        return NULL;
    }
    
    uint16_t pool_id = slot_param->pool_id;
    uint16_t slot_idx = slot_param->slot_index;
    
    if (pool_id >= inst->module->pool_count) {
        EXCEPTION("s_expr_get_slot_ptr: pool_id out of range");
        return NULL;
    }
    
    uint8_t* pool = (uint8_t*)inst->module->pool_table[pool_id];
    if (!pool) {
        EXCEPTION("s_expr_get_slot_ptr: pool is NULL");
        return NULL;
    }
    
    return pool + (slot_idx * elem_size);
}

// ============================================================================
// STRING TABLE ACCESS
// ============================================================================

const char* s_expr_get_string(s_expr_tree_instance_t* inst, const s_expr_param_t* param) {
    if (!inst) {
        EXCEPTION("s_expr_get_string: NULL instance");
        return NULL;
    }
    if (!param) {
        EXCEPTION("s_expr_get_string: NULL param");
        return NULL;
    }
    if ((param->type & S_EXPR_OPCODE_MASK) != S_EXPR_PARAM_STR_IDX) {
        EXCEPTION("s_expr_get_string: param is not STR_IDX type");
        return NULL;
    }
    if (!inst->module || !inst->module->def) {
        EXCEPTION("s_expr_get_string: invalid module");
        return NULL;
    }
    if (!inst->module->def->string_table) {
        EXCEPTION("s_expr_get_string: no string table");
        return NULL;
    }
    if (param->str_index >= inst->module->def->string_count) {
        EXCEPTION("s_expr_get_string: string index out of range");
        return NULL;
    }
    return inst->module->def->string_table[param->str_index];
}

// ============================================================================
// SLOT ACCESS (external initialization)
// ============================================================================

s_expr_slot_t* s_expr_tree_get_slot(s_expr_tree_instance_t* inst, uint16_t index) {
    if (!inst) {
        EXCEPTION("s_expr_tree_get_slot: NULL instance");
        return NULL;
    }
    if (index >= inst->pointer_count) {
        EXCEPTION("s_expr_tree_get_slot: index out of range");
        return NULL;
    }
    if (!inst->pointer_array) {
        EXCEPTION("s_expr_tree_get_slot: NULL pointer_array");
        return NULL;
    }
    return &inst->pointer_array[index];
}

uint16_t s_expr_tree_get_slot_count(s_expr_tree_instance_t* inst) {
    if (!inst) return 0;
    return inst->pointer_count;
}

uint8_t s_expr_tree_get_slot_flags(s_expr_tree_instance_t* inst, uint16_t index) {
    if (!inst || index >= inst->pointer_count || !inst->slot_flags) {
        return S_EXPR_SLOT_FLAG_NONE;
    }
    return inst->slot_flags[index];
}

bool s_expr_tree_slot_is_allocated(s_expr_tree_instance_t* inst, uint16_t index) {
    return (s_expr_tree_get_slot_flags(inst, index) & S_EXPR_SLOT_FLAG_ALLOCATED) != 0;
}

bool s_expr_tree_slot_is_external(s_expr_tree_instance_t* inst, uint16_t index) {
    return (s_expr_tree_get_slot_flags(inst, index) & S_EXPR_SLOT_FLAG_EXTERNAL) != 0;
}

bool s_expr_tree_slot_has_ptr(s_expr_tree_instance_t* inst, uint16_t index) {
    s_expr_slot_t* slot = s_expr_tree_get_slot(inst, index);
    return slot && slot->ptr != NULL;
}

void* s_expr_tree_slot_get_ptr(s_expr_tree_instance_t* inst, uint16_t index) {
    s_expr_slot_t* slot = s_expr_tree_get_slot(inst, index);
    return slot ? slot->ptr : NULL;
}

void s_expr_tree_slot_set_ptr(s_expr_tree_instance_t* inst, uint16_t index, void* ptr) {
    if (!inst || index >= inst->pointer_count) {
        EXCEPTION("s_expr_tree_slot_set_ptr: invalid index");
        return;
    }
    if (!inst->pointer_array || !inst->slot_flags) {
        EXCEPTION("s_expr_tree_slot_set_ptr: NULL pointer_array or slot_flags");
        return;
    }
    
    s_expr_slot_t* slot = &inst->pointer_array[index];
    
    // Free existing allocated pointer
    if ((inst->slot_flags[index] & S_EXPR_SLOT_FLAG_ALLOCATED) && slot->ptr) {
        if (inst->module && inst->module->alloc.free) {
            inst->module->alloc.free(inst->module->alloc.ctx, slot->ptr);
        }
    }
    
    slot->ptr = ptr;
    inst->slot_flags[index] = S_EXPR_SLOT_FLAG_EXTERNAL;
}

void* s_expr_tree_slot_alloc(s_expr_tree_instance_t* inst, uint16_t index, size_t size) {
    if (!inst || index >= inst->pointer_count) {
        EXCEPTION("s_expr_tree_slot_alloc: invalid index");
        return NULL;
    }
    if (!inst->module) {
        EXCEPTION("s_expr_tree_slot_alloc: NULL module");
        return NULL;
    }
    if (!inst->pointer_array || !inst->slot_flags) {
        EXCEPTION("s_expr_tree_slot_alloc: NULL pointer_array or slot_flags");
        return NULL;
    }
    
    s_expr_slot_t* slot = &inst->pointer_array[index];
    
    // Free existing allocated pointer
    if ((inst->slot_flags[index] & S_EXPR_SLOT_FLAG_ALLOCATED) && slot->ptr) {
        inst->module->alloc.free(inst->module->alloc.ctx, slot->ptr);
    }
    
    slot->ptr = inst->module->alloc.malloc(inst->module->alloc.ctx, size);
    if (!slot->ptr) {
        EXCEPTION("s_expr_tree_slot_alloc: allocation failed");
        inst->slot_flags[index] = S_EXPR_SLOT_FLAG_NONE;
        return NULL;
    }
    
    memset(slot->ptr, 0, size);
    inst->slot_flags[index] = S_EXPR_SLOT_FLAG_ALLOCATED;
    return slot->ptr;
}

void s_expr_tree_slot_free(s_expr_tree_instance_t* inst, uint16_t index) {
    if (!inst || index >= inst->pointer_count) {
        EXCEPTION("s_expr_tree_slot_free: invalid index");
        return;
    }
    if (!inst->slot_flags) {
        EXCEPTION("s_expr_tree_slot_free: NULL slot_flags");
        return;
    }
    
    if (!(inst->slot_flags[index] & S_EXPR_SLOT_FLAG_ALLOCATED)) {
        return;  // Not allocated - nothing to free
    }
    
    if (inst->pointer_array[index].ptr) {
        if (inst->module && inst->module->alloc.free) {
            inst->module->alloc.free(inst->module->alloc.ctx, inst->pointer_array[index].ptr);
        }
        inst->pointer_array[index].ptr = NULL;
    }
    
    inst->slot_flags[index] = S_EXPR_SLOT_FLAG_NONE;
}