// ============================================================================
// s_engine_loader.h
// Binary Module Loader
// ============================================================================

#ifndef S_ENGINE_LOADER_H
#define S_ENGINE_LOADER_H



#ifdef __cplusplus
extern "C" {
#endif
#include "s_engine_types.h"
// ============================================================================
// BINARY FORMAT CONSTANTS (must match s_expr_dsl.lua)
// ============================================================================

#define SEXB_MAGIC        0x42584553   // "SEXB"
#define SEXB_VERSION      0x0503       // v5.1

#define SEXB_FLAG_64BIT   0x0001
#define SEXB_FLAG_DEBUG   0x0002

// ============================================================================
// BINARY HEADER STRUCTURE (32 bytes)
// ============================================================================

typedef struct __attribute__((packed)) {
    uint32_t magic;           // 0x42584553 "SEXB"
    uint16_t version;         // 0x0501
    uint16_t flags;           // SEXB_FLAG_*
    uint32_t name_hash;       // Module name hash
    uint16_t tree_count;
    uint16_t record_count;
    uint16_t string_count;
    uint16_t const_count;
    uint16_t oneshot_count;
    uint16_t main_count;
    uint16_t pred_count;
    uint16_t reserved;
    uint32_t total_size;
} sexb_header_t;

_Static_assert(sizeof(sexb_header_t) == 32, "header should be 32 bytes");

// ============================================================================
// BINARY DIRECTORY (32 bytes - 8 offsets)
// ============================================================================

typedef struct __attribute__((packed)) {
    uint32_t tree_offset;
    uint32_t record_offset;
    uint32_t field_offset;
    uint32_t string_offset;
    uint32_t const_offset;
    uint32_t const_data_offset;
    uint32_t func_hash_offset;
    uint32_t params_offset;
} sexb_directory_t;

_Static_assert(sizeof(sexb_directory_t) == 32, "directory should be 32 bytes");

// ============================================================================
// BINARY TREE ENTRY (20 bytes)
// ============================================================================

// Change from 20 bytes to 24 bytes
typedef struct __attribute__((packed)) {
    uint32_t name_hash;
    uint32_t record_hash;
    uint16_t node_count;
    uint16_t pointer_count;
    uint16_t defaults_index;    // NEW: 0xFFFF = no defaults
    uint16_t reserved;
    uint32_t param_offset;
    uint16_t param_count;
    uint16_t reserved2;
} sexb_tree_entry_t;

_Static_assert(sizeof(sexb_tree_entry_t) == 24, "tree entry should be 24 bytes");
// ============================================================================
// BINARY RECORD ENTRY (12 bytes)
// ============================================================================

typedef struct __attribute__((packed)) {
    uint32_t name_hash;
    uint16_t field_count;
    uint16_t size;
    uint32_t field_table_offset;
} sexb_record_entry_t;

_Static_assert(sizeof(sexb_record_entry_t) == 12, "record entry should be 12 bytes");

// ============================================================================
// BINARY FIELD ENTRY (12 bytes)
// ============================================================================

typedef struct __attribute__((packed)) {
    uint32_t name_hash;
    uint8_t  type_tag;
    uint8_t  flags;
    uint16_t offset;
    uint16_t size;
    uint16_t aux;          // array_len, target_record_idx, or embedded_record_idx
} sexb_field_entry_t;

_Static_assert(sizeof(sexb_field_entry_t) == 12, "field entry should be 12 bytes");

// ============================================================================
// BINARY CONSTANT ENTRY (12 bytes)
// ============================================================================

typedef struct __attribute__((packed)) {
    uint32_t name_hash;
    uint16_t record_index;
    uint16_t data_size;
    uint32_t data_offset;
} sexb_const_entry_t;

_Static_assert(sizeof(sexb_const_entry_t) == 12, "const entry should be 12 bytes");

// ============================================================================
// LOADED MODULE (owns allocated memory)
// ============================================================================

typedef struct {
    // The module definition (points into allocated data)
    s_expr_module_def_t def;
    
    // Allocated arrays
    s_expr_tree_def_t*    trees;
    s_expr_record_desc_t* records;
    s_expr_field_desc_t*  fields;
    s_expr_hash_t*        oneshot_hashes;
    s_expr_hash_t*        main_hashes;
    s_expr_hash_t*        pred_hashes;
    const char**          string_table;
    const void**          constants;
    
    // Binary data reference
    const uint8_t*        binary_data;
    size_t                binary_size;
    bool                  binary_owned;   // If true, free binary on unload
    
    // Allocator used (for cleanup)
    s_expr_allocator_t    alloc;
    
    // Error info
    uint8_t               error_code;
} s_expr_loaded_module_t;

// ============================================================================
// ERROR CODES
// ============================================================================

#define SEXB_ERR_OK              0
#define SEXB_ERR_NULL_DATA       1
#define SEXB_ERR_TOO_SMALL       2
#define SEXB_ERR_BAD_MAGIC       3
#define SEXB_ERR_BAD_VERSION     4
#define SEXB_ERR_64BIT_MISMATCH  5
#define SEXB_ERR_ALLOC           6
#define SEXB_ERR_CORRUPT         7
#define SEXB_ERR_FILE_NOT_FOUND  8
#define SEXB_ERR_FILE_READ       9

const char* s_expr_loader_error_str(uint8_t err);

// ============================================================================
// VALIDATION API
// ============================================================================

// Validate binary without loading
bool s_expr_binary_validate(const uint8_t* data, size_t size);

// Get module name hash without full load
s_expr_hash_t s_expr_binary_get_hash(const uint8_t* data, size_t size);

// ============================================================================
// LOADER API - FROM MEMORY
// ============================================================================

// Load binary into module definition
// Returns pointer to loaded structure, or NULL on error
// For ROM binary: binary_owned = false, data must remain valid
// For RAM binary: binary_owned = true, loader will free data on unload
s_expr_loaded_module_t* s_expr_load_binary(
    const uint8_t* data,
    size_t size,
    s_expr_allocator_t alloc,
    bool binary_owned
);

// Convenience: load from ROM (data stays valid, not freed)
static inline s_expr_loaded_module_t* s_expr_load_from_rom(
    const uint8_t* data,
    size_t size,
    s_expr_allocator_t alloc
) {
    return s_expr_load_binary(data, size, alloc, false);
}

// ============================================================================
// LOADER API - FROM FILE
// ============================================================================

// Load binary from file into memory, then parse
// Returns loaded module or NULL on error
// File data is owned by loader (binary_owned = true)
s_expr_loaded_module_t* s_expr_load_from_file(
    const char* filepath,
    s_expr_allocator_t alloc
);

// Load raw binary data from file (helper)
// Returns allocated buffer, caller owns memory
// Sets *out_size to file size, sets *out_error to error code
uint8_t* s_expr_read_file(
    const char* filepath,
    size_t* out_size,
    uint8_t* out_error,
    s_expr_allocator_t alloc
);

// ============================================================================
// UNLOAD API
// ============================================================================

// Free loaded module and all allocated memory
void s_expr_unload_module(s_expr_loaded_module_t* loaded);

#ifdef __cplusplus
}
#endif

#endif // S_ENGINE_LOADER_H