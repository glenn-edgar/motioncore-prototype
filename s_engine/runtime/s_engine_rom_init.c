// ============================================================================
// s_engine_rom_init.c
// M-port init: wires module fields to const ROM data directly.
// Skips blob parsing, fn-table allocation, hash lookup, builtin registration.
// All fn-ptr arrays come from the linker — --gc-sections drops unreferenced.
// ============================================================================

#include "s_engine_rom.h"
#include "s_engine_exception.h"
#include <string.h>

uint8_t s_engine_init_rom(
    s_expr_module_t* mod,
    const s_engine_rom_t* rom,
    s_expr_allocator_t alloc
) {
    if (!mod) {
        EXCEPTION("s_engine_init_rom: NULL module");
        return S_EXPR_ERR_ALLOC;
    }
    if (!rom) {
        EXCEPTION("s_engine_init_rom: NULL rom");
        return S_EXPR_ERR_NULL_DEF;
    }
    if (!alloc.malloc || !alloc.free) {
        EXCEPTION("s_engine_init_rom: invalid allocator");
        return S_EXPR_ERR_ALLOC;
    }

    memset(mod, 0, sizeof(*mod));

    // s_engine_rom_t prefix is layout-compatible with s_expr_module_def_t.
    // Cast is safe: eval reads only the prefix fields via mod->def.
    mod->def = (const s_expr_module_def_t*)rom;

    // Const fn-ptr arrays from the rom — cast away const since the
    // module struct stores non-const pointers. We never write through them.
    mod->oneshot_fns = (s_expr_oneshot_fn_t*)rom->oneshot_fns;
    mod->main_fns    = (s_expr_main_fn_t*)rom->main_fns;
    mod->pred_fns    = (s_expr_pred_fn_t*)rom->pred_fns;

    mod->alloc = alloc;

    return S_EXPR_ERR_OK;
}
