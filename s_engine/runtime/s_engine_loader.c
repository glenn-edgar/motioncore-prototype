// ============================================================================
// s_engine_loader.c
// Binary Module Loader Implementation
// ============================================================================

#include "s_engine_loader.h"
#include "s_engine_exception.h"
#include <string.h>
#include <stdio.h>

// ============================================================================
// ERROR STRINGS
// ============================================================================

const char* s_expr_loader_error_str(uint8_t err) {
    switch (err) {
        case SEXB_ERR_OK:             return "OK";
        case SEXB_ERR_NULL_DATA:      return "Null data pointer";
        case SEXB_ERR_TOO_SMALL:      return "Data too small";
        case SEXB_ERR_BAD_MAGIC:      return "Invalid magic number";
        case SEXB_ERR_BAD_VERSION:    return "Unsupported version";
        case SEXB_ERR_64BIT_MISMATCH: return "64-bit mode mismatch";
        case SEXB_ERR_ALLOC:          return "Allocation failed";
        case SEXB_ERR_CORRUPT:        return "Corrupt binary data";
        case SEXB_ERR_FILE_NOT_FOUND: return "File not found";
        case SEXB_ERR_FILE_READ:      return "File read error";
        default:                      return "Unknown error";
    }
}

// ============================================================================
// VALIDATION
// ============================================================================

bool s_expr_binary_validate(const uint8_t* data, size_t size) {
    if (!data || size < sizeof(sexb_header_t)) {
        return false;
    }
    
    const sexb_header_t* hdr = (const sexb_header_t*)data;
    
    if (hdr->magic != SEXB_MAGIC) {
        return false;
    }
    
    if (hdr->version != SEXB_VERSION) {
        return false;
    }
    
    bool is_64bit = (hdr->flags & SEXB_FLAG_64BIT) != 0;
    if (is_64bit != (MODULE_IS_64BIT != 0)) {
        return false;
    }
    
    if (hdr->total_size > size) {
        return false;
    }
    
    return true;
}

s_expr_hash_t s_expr_binary_get_hash(const uint8_t* data, size_t size) {
    if (!data || size < sizeof(sexb_header_t)) {
        return 0;
    }
    
    const sexb_header_t* hdr = (const sexb_header_t*)data;
    
    if (hdr->magic != SEXB_MAGIC) {
        return 0;
    }
    
    return hdr->name_hash;
}

// ============================================================================
// STRING PARSING (length-prefixed, null-terminated, 4-byte aligned)
// ============================================================================

static const char* parse_string(
    const uint8_t* data,
    size_t data_size,
    uint32_t* offset
) {
    if (*offset + 2 > data_size) {
        return NULL;
    }
    
    uint16_t len = *(uint16_t*)(data + *offset);
    *offset += 2;
    
    if (*offset + len + 1 > data_size) {
        return NULL;
    }
    
    const char* str = (const char*)(data + *offset);
    
    // Skip string + null + padding to 4-byte boundary
    uint32_t total = 2 + len + 1;
    uint32_t padding = (4 - (total % 4)) % 4;
    *offset += len + 1 + padding;
    
    return str;
}

// ============================================================================
// FILE READING
// ============================================================================

uint8_t* s_expr_read_file(
    const char* filepath,
    size_t* out_size,
    uint8_t* out_error,
    s_expr_allocator_t alloc
) {
    if (out_size) *out_size = 0;
    if (out_error) *out_error = SEXB_ERR_OK;
    
    if (!filepath) {
        EXCEPTION("s_expr_read_file: NULL filepath");
        if (out_error) *out_error = SEXB_ERR_NULL_DATA;
        return NULL;
    }
    
    if (!alloc.malloc || !alloc.free) {
        EXCEPTION("s_expr_read_file: invalid allocator");
        if (out_error) *out_error = SEXB_ERR_ALLOC;
        return NULL;
    }
    
    // Open file
    FILE* fp = fopen(filepath, "rb");
    if (!fp) {
        EXCEPTION("s_expr_read_file: cannot open file");
        if (out_error) *out_error = SEXB_ERR_FILE_NOT_FOUND;
        return NULL;
    }
    
    // Get file size
    if (fseek(fp, 0, SEEK_END) != 0) {
        EXCEPTION("s_expr_read_file: seek failed");
        fclose(fp);
        if (out_error) *out_error = SEXB_ERR_FILE_READ;
        return NULL;
    }
    
    long file_size = ftell(fp);
    if (file_size < 0) {
        EXCEPTION("s_expr_read_file: ftell failed");
        fclose(fp);
        if (out_error) *out_error = SEXB_ERR_FILE_READ;
        return NULL;
    }
    
    if (fseek(fp, 0, SEEK_SET) != 0) {
        EXCEPTION("s_expr_read_file: seek to start failed");
        fclose(fp);
        if (out_error) *out_error = SEXB_ERR_FILE_READ;
        return NULL;
    }
    
    // Allocate buffer
    uint8_t* buffer = (uint8_t*)alloc.malloc(alloc.ctx, (size_t)file_size);
    if (!buffer) {
        EXCEPTION("s_expr_read_file: allocation failed");
        fclose(fp);
        if (out_error) *out_error = SEXB_ERR_ALLOC;
        return NULL;
    }
    
    // Read file
    size_t bytes_read = fread(buffer, 1, (size_t)file_size, fp);
    fclose(fp);
    
    if (bytes_read != (size_t)file_size) {
        EXCEPTION("s_expr_read_file: incomplete read");
        alloc.free(alloc.ctx, buffer);
        if (out_error) *out_error = SEXB_ERR_FILE_READ;
        return NULL;
    }
    
    if (out_size) *out_size = (size_t)file_size;
    return buffer;
}

// ============================================================================
// LOAD FROM FILE
// ============================================================================

s_expr_loaded_module_t* s_expr_load_from_file(
    const char* filepath,
    s_expr_allocator_t alloc
) {
    uint8_t error = SEXB_ERR_OK;
    size_t size = 0;
    
    // Read file into memory
    uint8_t* data = s_expr_read_file(filepath, &size, &error, alloc);
    if (!data) {
        return NULL;
    }
    
    // Parse binary (takes ownership of data)
    s_expr_loaded_module_t* loaded = s_expr_load_binary(data, size, alloc, true);
    
    if (!loaded) {
        // Load failed, free the data we read
        alloc.free(alloc.ctx, data);
        return NULL;
    }
    
    return loaded;
}

// ============================================================================
// LOAD FROM MEMORY
// NOTE: Validation failures before the loaded module is allocated return NULL
// with no structured error code. The EXCEPTION() macro fires on each path.
// Callers that need to distinguish failure reasons should call
// s_expr_binary_validate() first.
// ============================================================================

s_expr_loaded_module_t* s_expr_load_binary(
    const uint8_t* data,
    size_t size,
    s_expr_allocator_t alloc,
    bool binary_owned
) {
    // Validate
    if (!data) {
        EXCEPTION("s_expr_load_binary: NULL data");
        return NULL;
    }
    
    if (size < sizeof(sexb_header_t) + sizeof(sexb_directory_t)) {
        EXCEPTION("s_expr_load_binary: data too small");
        return NULL;
    }
    
    const sexb_header_t* hdr = (const sexb_header_t*)data;
    
    if (hdr->magic != SEXB_MAGIC) {
        EXCEPTION("s_expr_load_binary: invalid magic");
        return NULL;
    }
    
    if (hdr->version != SEXB_VERSION) {
        EXCEPTION("s_expr_load_binary: unsupported version");
        return NULL;
    }
    
    bool is_64bit = (hdr->flags & SEXB_FLAG_64BIT) != 0;
    if (is_64bit != (MODULE_IS_64BIT != 0)) {
        EXCEPTION("s_expr_load_binary: 64-bit mode mismatch");
        return NULL;
    }
    
    if (!alloc.malloc || !alloc.free) {
        EXCEPTION("s_expr_load_binary: invalid allocator");
        return NULL;
    }
    
    // Allocate loaded module structure
    s_expr_loaded_module_t* loaded = (s_expr_loaded_module_t*)alloc.malloc(
        alloc.ctx, sizeof(s_expr_loaded_module_t)
    );
    if (!loaded) {
        EXCEPTION("s_expr_load_binary: failed to allocate loaded module");
        return NULL;
    }
    
    memset(loaded, 0, sizeof(*loaded));
    loaded->alloc = alloc;
    loaded->binary_data = data;
    loaded->binary_size = size;
    loaded->binary_owned = binary_owned;
    loaded->error_code = SEXB_ERR_OK;
    
    // Get directory
    const sexb_directory_t* dir = (const sexb_directory_t*)(data + sizeof(sexb_header_t));
    
    // Initialize def
    s_expr_module_def_t* def = &loaded->def;
    def->name_hash = hdr->name_hash;
    def->is_64bit = is_64bit;
    def->tree_count = hdr->tree_count;
    def->record_count = hdr->record_count;
    def->oneshot_count = hdr->oneshot_count;
    def->main_count = hdr->main_count;
    def->pred_count = hdr->pred_count;
    def->string_count = hdr->string_count;
    def->const_count = hdr->const_count;
    
    // ========== ALLOCATE TREES ==========
    if (hdr->tree_count > 0) {
        size_t tree_size = hdr->tree_count * sizeof(s_expr_tree_def_t);
        loaded->trees = (s_expr_tree_def_t*)alloc.malloc(alloc.ctx, tree_size);
        if (!loaded->trees) {
            EXCEPTION("s_expr_load_binary: failed to allocate trees");
            loaded->error_code = SEXB_ERR_ALLOC;
            s_expr_unload_module(loaded);
            return NULL;
        }
        memset(loaded->trees, 0, tree_size);
        def->trees = loaded->trees;
        
        const sexb_tree_entry_t* tree_entries = 
            (const sexb_tree_entry_t*)(data + dir->tree_offset);
        
        for (uint16_t i = 0; i < hdr->tree_count; i++) {
            loaded->trees[i].name_hash = tree_entries[i].name_hash;
            loaded->trees[i].record_hash = tree_entries[i].record_hash;
            loaded->trees[i].func_node_count = tree_entries[i].node_count;
            loaded->trees[i].pointer_count = tree_entries[i].pointer_count;
            loaded->trees[i].defaults_index = tree_entries[i].defaults_index;
            loaded->trees[i].param_count = tree_entries[i].param_count;
            
            // Point directly into binary for params (zero-copy)
            loaded->trees[i].params = 
                (const s_expr_param_t*)(data + tree_entries[i].param_offset);
            
            // Track max values
            if (tree_entries[i].node_count > def->max_func_node_count) {
                def->max_func_node_count = tree_entries[i].node_count;
            }
            if (tree_entries[i].pointer_count > def->max_pointer_count) {
                def->max_pointer_count = tree_entries[i].pointer_count;
            }
            if (tree_entries[i].param_count > def->max_param_count) {
                def->max_param_count = tree_entries[i].param_count;
            }
        }
    }
    
    // ========== ALLOCATE RECORDS ==========
    if (hdr->record_count > 0) {
        size_t record_size = hdr->record_count * sizeof(s_expr_record_desc_t);
        loaded->records = (s_expr_record_desc_t*)alloc.malloc(alloc.ctx, record_size);
        if (!loaded->records) {
            EXCEPTION("s_expr_load_binary: failed to allocate records");
            loaded->error_code = SEXB_ERR_ALLOC;
            s_expr_unload_module(loaded);
            return NULL;
        }
        memset(loaded->records, 0, record_size);
        def->records = loaded->records;
        
        // Count total fields
        const sexb_record_entry_t* record_entries = 
            (const sexb_record_entry_t*)(data + dir->record_offset);
        
        uint32_t total_fields = 0;
        for (uint16_t i = 0; i < hdr->record_count; i++) {
            total_fields += record_entries[i].field_count;
        }
        
        // Allocate all fields in one block
        if (total_fields > 0) {
            size_t field_size = total_fields * sizeof(s_expr_field_desc_t);
            loaded->fields = (s_expr_field_desc_t*)alloc.malloc(alloc.ctx, field_size);
            if (!loaded->fields) {
                EXCEPTION("s_expr_load_binary: failed to allocate fields");
                loaded->error_code = SEXB_ERR_ALLOC;
                s_expr_unload_module(loaded);
                return NULL;
            }
            memset(loaded->fields, 0, field_size);
        }
        
        // Parse records and fields
        uint32_t field_idx = 0;
        for (uint16_t i = 0; i < hdr->record_count; i++) {
            loaded->records[i].name_hash = record_entries[i].name_hash;
            loaded->records[i].total_size = record_entries[i].size;
            loaded->records[i].field_count = record_entries[i].field_count;
            loaded->records[i].fields = &loaded->fields[field_idx];
            
            // Parse fields for this record
            const sexb_field_entry_t* field_entries = 
                (const sexb_field_entry_t*)(data + record_entries[i].field_table_offset);
            
            for (uint16_t j = 0; j < record_entries[i].field_count; j++) {
                loaded->fields[field_idx].name_hash = field_entries[j].name_hash;
                loaded->fields[field_idx].offset = field_entries[j].offset;
                loaded->fields[field_idx].size = field_entries[j].size;
                field_idx++;
            }
        }
    }
    
    // ========== ALLOCATE STRING TABLE ==========
    if (hdr->string_count > 0) {
        size_t str_table_size = hdr->string_count * sizeof(const char*);
        loaded->string_table = (const char**)alloc.malloc(alloc.ctx, str_table_size);
        if (!loaded->string_table) {
            EXCEPTION("s_expr_load_binary: failed to allocate string table");
            loaded->error_code = SEXB_ERR_ALLOC;
            s_expr_unload_module(loaded);
            return NULL;
        }
        memset(loaded->string_table, 0, str_table_size);
        def->string_table = loaded->string_table;
        
        // Parse strings (point directly into binary)
        uint32_t str_offset = dir->string_offset;
        for (uint16_t i = 0; i < hdr->string_count; i++) {
            loaded->string_table[i] = parse_string(data, size, &str_offset);
            if (!loaded->string_table[i]) {
                EXCEPTION("s_expr_load_binary: failed to parse string");
                loaded->error_code = SEXB_ERR_CORRUPT;
                s_expr_unload_module(loaded);
                return NULL;
            }
        }
    }
    
    // ========== ALLOCATE CONSTANTS ==========
    if (hdr->const_count > 0) {
        size_t const_table_size = hdr->const_count * sizeof(const void*);
        loaded->constants = (const void**)alloc.malloc(alloc.ctx, const_table_size);
        if (!loaded->constants) {
            EXCEPTION("s_expr_load_binary: failed to allocate constants");
            loaded->error_code = SEXB_ERR_ALLOC;
            s_expr_unload_module(loaded);
            return NULL;
        }
        memset(loaded->constants, 0, const_table_size);
        def->constants = loaded->constants;
        
        // Parse constants (point directly into binary)
        const sexb_const_entry_t* const_entries = 
            (const sexb_const_entry_t*)(data + dir->const_offset);
        
        for (uint16_t i = 0; i < hdr->const_count; i++) {
            loaded->constants[i] = data + const_entries[i].data_offset;
        }
    }
    
    // ========== ALLOCATE FUNCTION HASH ARRAYS ==========
    const uint32_t* func_hashes = (const uint32_t*)(data + dir->func_hash_offset);
    uint32_t hash_idx = 0;
    
    if (hdr->oneshot_count > 0) {
        size_t hash_size = hdr->oneshot_count * sizeof(s_expr_hash_t);
        loaded->oneshot_hashes = (s_expr_hash_t*)alloc.malloc(alloc.ctx, hash_size);
        if (!loaded->oneshot_hashes) {
            EXCEPTION("s_expr_load_binary: failed to allocate oneshot hashes");
            loaded->error_code = SEXB_ERR_ALLOC;
            s_expr_unload_module(loaded);
            return NULL;
        }
        for (uint16_t i = 0; i < hdr->oneshot_count; i++) {
            loaded->oneshot_hashes[i] = func_hashes[hash_idx++];
        }
        def->oneshot_hashes = loaded->oneshot_hashes;
    }
    
    if (hdr->main_count > 0) {
        size_t hash_size = hdr->main_count * sizeof(s_expr_hash_t);
        loaded->main_hashes = (s_expr_hash_t*)alloc.malloc(alloc.ctx, hash_size);
        if (!loaded->main_hashes) {
            EXCEPTION("s_expr_load_binary: failed to allocate main hashes");
            loaded->error_code = SEXB_ERR_ALLOC;
            s_expr_unload_module(loaded);
            return NULL;
        }
        for (uint16_t i = 0; i < hdr->main_count; i++) {
            loaded->main_hashes[i] = func_hashes[hash_idx++];
        }
        def->main_hashes = loaded->main_hashes;
    }
    
    if (hdr->pred_count > 0) {
        size_t hash_size = hdr->pred_count * sizeof(s_expr_hash_t);
        loaded->pred_hashes = (s_expr_hash_t*)alloc.malloc(alloc.ctx, hash_size);
        if (!loaded->pred_hashes) {
            EXCEPTION("s_expr_load_binary: failed to allocate pred hashes");
            loaded->error_code = SEXB_ERR_ALLOC;
            s_expr_unload_module(loaded);
            return NULL;
        }
        for (uint16_t i = 0; i < hdr->pred_count; i++) {
            loaded->pred_hashes[i] = func_hashes[hash_idx++];
        }
        def->pred_hashes = loaded->pred_hashes;
    }
    
    return loaded;
}

// ============================================================================
// UNLOAD
// ============================================================================

void s_expr_unload_module(s_expr_loaded_module_t* loaded) {
    if (!loaded) return;
    
    s_expr_allocator_t alloc = loaded->alloc;
    
    if (loaded->trees) {
        alloc.free(alloc.ctx, loaded->trees);
    }
    
    if (loaded->records) {
        alloc.free(alloc.ctx, loaded->records);
    }
    
    if (loaded->fields) {
        alloc.free(alloc.ctx, loaded->fields);
    }
    
    if (loaded->oneshot_hashes) {
        alloc.free(alloc.ctx, loaded->oneshot_hashes);
    }
    
    if (loaded->main_hashes) {
        alloc.free(alloc.ctx, loaded->main_hashes);
    }
    
    if (loaded->pred_hashes) {
        alloc.free(alloc.ctx, loaded->pred_hashes);
    }
    
    if (loaded->string_table) {
        alloc.free(alloc.ctx, loaded->string_table);
    }
    
    if (loaded->constants) {
        alloc.free(alloc.ctx, loaded->constants);
    }
    
    if (loaded->binary_owned && loaded->binary_data) {
        alloc.free(alloc.ctx, (void*)loaded->binary_data);
    }
    
    alloc.free(alloc.ctx, loaded);
}