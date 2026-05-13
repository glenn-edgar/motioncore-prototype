// ============================================================================
// s_engine_rom.h
// M-port ROM handle: const-data-only module, linker-resolved fn pointers.
// Replaces the s_engine_load_from_rom blob path for embedded M-class targets.
// See memory/s_engine_m_port_architecture_2026-05-12.md.
// ============================================================================

#ifndef S_ENGINE_ROM_H
#define S_ENGINE_ROM_H

#include "s_engine_types.h"
#include "s_engine_module.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// s_engine_rom_t — M-port ROM handle.
//
// LAYOUT-COMPATIBLE PREFIX with s_expr_module_def_t (first 18 fields match
// exactly). Engine eval reads only the def-prefix fields via a cast in
// s_engine_init_rom. Extension fields (oneshot/main/pred_fns + bump size)
// are M-port-only and provide pre-resolved fn pointers — no hash lookup,
// no malloc'd parallel arrays.
// ============================================================================

typedef struct s_engine_rom {
    // === s_expr_module_def_t prefix (do not reorder) ===
    s_expr_hash_t name_hash;
    const s_expr_tree_def_t* trees;
    uint16_t tree_count;
    bool     is_64bit;

    const s_expr_hash_t* oneshot_hashes;   // NULL on M-port (decision #3)
    const s_expr_hash_t* main_hashes;
    const s_expr_hash_t* pred_hashes;

    uint16_t oneshot_count;
    uint16_t main_count;
    uint16_t pred_count;

    uint16_t max_func_node_count;
    uint16_t max_pointer_count;
    uint16_t max_param_count;

    const s_expr_record_desc_t* records;
    uint16_t record_count;

    const char* const* string_table;
    uint16_t string_count;
    const void* const* constants;
    uint16_t const_count;
    // === end s_expr_module_def_t prefix ===

    // === M-port extension: linker-resolved fn-ptr arrays ===
    const s_expr_oneshot_fn_t* oneshot_fns;
    const s_expr_main_fn_t*    main_fns;
    const s_expr_pred_fn_t*    pred_fns;

    uint16_t bump_buffer_size;   // per-tree-instance (TODO: DSL-computed)
} s_engine_rom_t;

// ============================================================================
// M-port initialization.
// Wires module->def to the rom (prefix cast) + module->{oneshot,main,pred}_fns
// to the rom's const fn-ptr arrays. NO malloc, NO blob parsing,
// NO s_engine_register_builtins. Linker has already resolved fn pointers.
// ============================================================================

uint8_t s_engine_init_rom(
    s_expr_module_t* mod,
    const s_engine_rom_t* rom,
    s_expr_allocator_t alloc
);

#ifdef __cplusplus
}
#endif

#endif // S_ENGINE_ROM_H
